/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "audio_usb.h"

#include "usb_audio.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/time.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define USB_AUDIO_NS_PER_SECOND INT64_C(1000000000)

typedef struct USBAudioCallbackFrame
{
  const struct LGA_USBState    * state;
  struct USBAudioCallbackFrame * previous;
}
USBAudioCallbackFrame;

typedef struct USBAudioCallbackWaiter
{
  struct USBAudioCallbackWaiter * next;
  LGEvent                       * event;
  unsigned int                    depth;
}
USBAudioCallbackWaiter;

typedef struct USBAudioCallbackWaitQueue
{
  LG_Lock                 lock;
  atomic_uint             count;
  USBAudioCallbackWaiter * waiters;
}
USBAudioCallbackWaitQueue;

typedef struct USBAudioCallbackGate
{
  atomic_uint                inFlight;
  USBAudioCallbackWaitQueue  wait;
}
USBAudioCallbackGate;

typedef struct USBAudioEventTarget
{
  const LG_AudioEventOps   * events;
  void                     * opaque;
  uint32_t                   attachmentGeneration;
  USBAudioCallbackFrame      frame;
  USBAudioCallbackFrame   ** frames;
  USBAudioCallbackGate     * gate;
}
USBAudioEventTarget;

typedef struct USBAudioOperationWaiter
{
  struct USBAudioOperationWaiter * next;
  LGEvent                        * event;
}
USBAudioOperationWaiter;

typedef struct USBAudioOperationQueue
{
  LG_Lock                    lock;
  bool                       active;
  USBAudioOperationWaiter  * head;
  USBAudioOperationWaiter  * tail;
}
USBAudioOperationQueue;

struct LGA_USBState
{
  LG_USBAudio * device;
  LG_USBRedir * redir;

  LG_Lock                    statusLock;
  atomic_bool                available;
  uint32_t                   statusGeneration;
  LG_AudioStatusFn           statusCallback;
  void                     * statusOpaque;
  atomic_uint                statusInFlight;
  USBAudioCallbackWaitQueue  statusWait;
  USBAudioOperationQueue     statusOperation;

  LG_Lock                    stateLock;
  LGEvent                  * detachEvent;
  bool                       attached;
  bool                       detaching;
  uint32_t                   attachmentGeneration;
  const LG_AudioEventOps   * events;
  void                     * eventOpaque;

  LG_AudioFormat             playbackFormat;
  uint32_t                   playbackGeneration;
  LG_AudioFormat             recordFormat;
  uint32_t                   recordGeneration;
  uint32_t                   generationSerial;
  int64_t                    playbackClockOrigin;
  atomic_uint_fast64_t       playbackPosition;
  atomic_uint                playbackDeliveryGeneration;
  atomic_uint                recordDeliveryGeneration;
  alignas(64) USBAudioCallbackGate playbackGate;
  alignas(64) USBAudioCallbackGate recordGate;
  USBAudioOperationQueue     recordOperation;
};

static _Thread_local USBAudioCallbackFrame * l_playbackFrames;
static _Thread_local USBAudioCallbackFrame * l_recordFrames;
static _Thread_local USBAudioCallbackFrame * l_statusFrames;

static const LG_AudioFormat l_formatTemplate =
{
  .sampleFormat = LG_AUDIO_FMT_S24_LE,
  .sampleRate   = LG_USB_AUDIO_DEFAULT_SAMPLE_RATE,
};

static const LG_AudioChannel l_uacChannels[] =
{
  LG_AUDIO_CH_FRONT_LEFT,
  LG_AUDIO_CH_FRONT_RIGHT,
  LG_AUDIO_CH_FRONT_CENTER,
  LG_AUDIO_CH_LFE,
  LG_AUDIO_CH_REAR_LEFT,
  LG_AUDIO_CH_REAR_RIGHT,
  LG_AUDIO_CH_FRONT_LEFT_CENTER,
  LG_AUDIO_CH_FRONT_RIGHT_CENTER,
  LG_AUDIO_CH_REAR_CENTER,
  LG_AUDIO_CH_SIDE_LEFT,
  LG_AUDIO_CH_SIDE_RIGHT,
};

static void setStreamFormat(LG_AudioFormat * format,
    uint32_t sampleRate, uint32_t channelMask)
{
  *format            = l_formatTemplate;
  format->sampleRate = sampleRate;

  for (size_t bit = 0;
      bit < sizeof(l_uacChannels) / sizeof(*l_uacChannels); ++bit)
  {
    if (!(channelMask & (UINT32_C(1) << bit)))
      continue;

    format->channels[format->channelCount++] = l_uacChannels[bit];
  }
}

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static unsigned int callbackDepth(
    const USBAudioCallbackFrame * frame, const LGA_USBState * state)
{
  unsigned int depth = 0;
  while (frame)
  {
    if (frame->state == state)
      ++depth;
    frame = frame->previous;
  }
  return depth;
}

static LGEvent * createWaitEvent(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  if (!event)
    DEBUG_FATAL("Failed to create USB audio wait event");
  return event;
}

static void waitEvent(LGEvent * event)
{
  if (!lgWaitEvent(event, TIMEOUT_INFINITE))
    DEBUG_FATAL("Failed to wait for USB audio event");
}

static void signalWaitEvent(LGEvent * event)
{
  if (!lgSignalEvent(event))
    DEBUG_FATAL("Failed to signal USB audio event");
}

static void signalCallbackWaiters(
    USBAudioCallbackWaitQueue * queue, unsigned int remaining)
{
  if (!atomic_load_explicit(&queue->count, memory_order_seq_cst))
    return;

  LG_LOCK(queue->lock);
  USBAudioCallbackWaiter ** link = &queue->waiters;
  while (*link)
  {
    USBAudioCallbackWaiter * waiter = *link;
    if (remaining > waiter->depth)
    {
      link = &waiter->next;
      continue;
    }

    *link = waiter->next;
    atomic_fetch_sub_explicit(
        &queue->count, 1, memory_order_release);
    signalWaitEvent(waiter->event);
  }
  LG_UNLOCK(queue->lock);
}

static void beginCallback(LGA_USBState * state,
    USBAudioCallbackFrame * frame, USBAudioCallbackFrame ** frames,
    atomic_uint * inFlight)
{
  atomic_fetch_add_explicit(inFlight, 1, memory_order_seq_cst);
  frame->state    = state;
  frame->previous = *frames;
  *frames         = frame;
}

static void endCallback(USBAudioCallbackFrame * frame,
    USBAudioCallbackFrame ** frames, atomic_uint * inFlight,
    USBAudioCallbackWaitQueue * waitQueue)
{
  *frames = frame->previous;
  const unsigned int previous = atomic_fetch_sub_explicit(
      inFlight, 1, memory_order_seq_cst);
  DEBUG_ASSERT(previous > 0);
  signalCallbackWaiters(waitQueue, previous - 1);
}

static void waitCallbacks(LGA_USBState * state,
    USBAudioCallbackFrame * frames, atomic_uint * inFlight,
    USBAudioCallbackWaitQueue * waitQueue)
{
  const unsigned int depth = callbackDepth(frames, state);
  if (atomic_load_explicit(inFlight, memory_order_seq_cst) <= depth)
    return;

  USBAudioCallbackWaiter waiter =
  {
    .event = createWaitEvent(),
    .depth = depth,
  };

  bool queued = false;
  LG_LOCK(waitQueue->lock);

  /* Publish the waiter before rechecking inFlight. This prevents the final
   * callback from taking the lock-free path while a waiter is being queued. */
  atomic_fetch_add_explicit(
      &waitQueue->count, 1, memory_order_seq_cst);
  if (atomic_load_explicit(inFlight, memory_order_seq_cst) > depth)
  {
    waiter.next        = waitQueue->waiters;
    waitQueue->waiters = &waiter;
    queued = true;
  }
  else
    atomic_fetch_sub_explicit(
        &waitQueue->count, 1, memory_order_seq_cst);

  LG_UNLOCK(waitQueue->lock);

  if (queued)
  {
    waitEvent(waiter.event);

    /* The signaler owns the waiter until it releases the queue lock. */
    LG_LOCK(waitQueue->lock);
    LG_UNLOCK(waitQueue->lock);
  }

  lgFreeEvent(waiter.event);
}

static LG_AudioClock makePlaybackClock(
    const LGA_USBState * state, uint64_t position)
{
  /* USB redirection carries no publisher timestamp or USB frame number.
   * Preserve the sample timeline, but do not claim a measured source rate. */
  const uint32_t sampleRate = state->playbackFormat.sampleRate;
  const int64_t elapsed =
    position / sampleRate * USB_AUDIO_NS_PER_SECOND +
    position % sampleRate * USB_AUDIO_NS_PER_SECOND / sampleRate;

  return (LG_AudioClock)
  {
    .position = position,
    .time     = state->playbackClockOrigin + elapsed,
    .rate     = 0.0,
    .stable   = false,
  };
}

/* stateLock must be held while admitting a control event. The data event
 * path performs the equivalent admission using deliveryGeneration. */
static bool beginEventNL(
    LGA_USBState * state, USBAudioEventTarget * target,
    USBAudioCallbackFrame ** frames, USBAudioCallbackGate * gate)
{
  if (!state->attached || !state->events)
    return false;

  target->events               = state->events;
  target->opaque               = state->eventOpaque;
  target->attachmentGeneration = state->attachmentGeneration;
  target->frames               = frames;
  target->gate                 = gate;
  beginCallback(
      state, &target->frame, frames, &gate->inFlight);
  return true;
}

static bool beginPlaybackEventNL(
    LGA_USBState * state, USBAudioEventTarget * target)
{
  return beginEventNL(
      state, target, &l_playbackFrames, &state->playbackGate);
}

static bool beginRecordEventNL(
    LGA_USBState * state, USBAudioEventTarget * target)
{
  return beginEventNL(
      state, target, &l_recordFrames, &state->recordGate);
}

static bool attachmentCurrentNL(const LGA_USBState * state,
    const USBAudioEventTarget * target)
{
  return state->attached &&
    state->attachmentGeneration == target->attachmentGeneration &&
    state->events                == target->events &&
    state->eventOpaque           == target->opaque;
}

static bool playbackEventCurrentNL(const LGA_USBState * state,
    const USBAudioEventTarget * target, uint32_t streamGeneration)
{
  return attachmentCurrentNL(state, target) &&
    state->playbackGeneration == streamGeneration;
}

static bool recordEventCurrentNL(const LGA_USBState * state,
    const USBAudioEventTarget * target, uint32_t streamGeneration)
{
  return attachmentCurrentNL(state, target) &&
    state->recordGeneration == streamGeneration;
}

static void endEvent(USBAudioEventTarget * target)
{
  endCallback(&target->frame, target->frames,
      &target->gate->inFlight, &target->gate->wait);
}

static void waitPlaybackEvents(LGA_USBState * state)
{
  waitCallbacks(state, l_playbackFrames,
      &state->playbackGate.inFlight, &state->playbackGate.wait);
}

static void waitRecordEvents(LGA_USBState * state)
{
  waitCallbacks(state, l_recordFrames,
      &state->recordGate.inFlight, &state->recordGate.wait);
}

static bool beginOperation(LGA_USBState * state,
    USBAudioOperationQueue * operation,
    const USBAudioCallbackFrame * callbackFrames)
{
  /* Reentrant control must run inline: the current owner may be waiting for
   * this callback to finish before it can release the operation. */
  if (callbackDepth(callbackFrames, state))
    return false;

  USBAudioOperationWaiter waiter =
  {
    .event = createWaitEvent(),
  };

  bool queued = false;
  LG_LOCK(operation->lock);
  if (operation->active)
  {
    if (operation->tail)
      operation->tail->next = &waiter;
    else
      operation->head = &waiter;
    operation->tail = &waiter;
    queued = true;
  }
  else
    operation->active = true;
  LG_UNLOCK(operation->lock);

  if (queued)
  {
    waitEvent(waiter.event);

    /* The previous owner owns the waiter until it releases the queue lock. */
    LG_LOCK(operation->lock);
    LG_UNLOCK(operation->lock);
  }

  lgFreeEvent(waiter.event);
  return true;
}

static void endOperation(
    USBAudioOperationQueue * operation, bool owner)
{
  if (!owner)
    return;

  LG_LOCK(operation->lock);
  USBAudioOperationWaiter * waiter = operation->head;
  if (waiter)
  {
    operation->head = waiter->next;
    if (!operation->head)
      operation->tail = NULL;
    signalWaitEvent(waiter->event);
  }
  else
    operation->active = false;
  LG_UNLOCK(operation->lock);
}

static void beginStatusCallback(
    LGA_USBState * state, USBAudioCallbackFrame * frame)
{
  beginCallback(
      state, frame, &l_statusFrames, &state->statusInFlight);
}

static void endStatusCallback(
    LGA_USBState * state, USBAudioCallbackFrame * frame)
{
  endCallback(frame, &l_statusFrames,
      &state->statusInFlight, &state->statusWait);
}

static void waitStatusCallbacks(LGA_USBState * state)
{
  waitCallbacks(state, l_statusFrames,
      &state->statusInFlight, &state->statusWait);
}

/* Returns with stateLock held. A callback must not wait for a detach which
 * may itself be waiting for that callback to return. */
static bool lockStateAfterDetach(LGA_USBState * state)
{
  for (;;)
  {
    LG_LOCK(state->stateLock);
    if (!state->detaching)
      return true;
    LG_UNLOCK(state->stateLock);

    if (callbackDepth(l_playbackFrames, state) ||
        callbackDepth(l_recordFrames, state))
      return false;
    waitEvent(state->detachEvent);
  }
}

static void usbPlaybackStart(
    void * opaque, uint32_t sampleRate, uint32_t channelMask)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;
  bool                dispatch;
  uint32_t            generation;
  LG_AudioClock       clock;

  LG_LOCK(state->stateLock);
  if (state->playbackGeneration)
  {
    LG_UNLOCK(state->stateLock);
    return;
  }

  setStreamFormat(&state->playbackFormat, sampleRate, channelMask);
  generation = state->generationSerial =
    nextGeneration(state->generationSerial);
  state->playbackGeneration  = generation;
  state->playbackClockOrigin = (int64_t)nanotime();
  atomic_store_explicit(
      &state->playbackPosition, 0, memory_order_relaxed);
  atomic_store_explicit(
      &state->playbackDeliveryGeneration, 0, memory_order_seq_cst);
  clock    = makePlaybackClock(state, 0);
  const bool admitted = beginPlaybackEventNL(state, &target);
  dispatch = admitted && target.events->playbackStart;
  if (admitted && !dispatch)
    endEvent(&target);
  LG_UNLOCK(state->stateLock);

  if (!dispatch)
    return;

  target.events->playbackStart(
      target.opaque, generation, &state->playbackFormat, &clock);

  LG_LOCK(state->stateLock);
  if (playbackEventCurrentNL(state, &target, generation))
    atomic_store_explicit(&state->playbackDeliveryGeneration,
        generation, memory_order_seq_cst);
  LG_UNLOCK(state->stateLock);
  endEvent(&target);
}

static void usbPlaybackStop(void * opaque)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;

  LG_LOCK(state->stateLock);
  const uint32_t generation = state->playbackGeneration;
  if (!generation)
  {
    LG_UNLOCK(state->stateLock);
    return;
  }

  state->playbackGeneration = 0;
  atomic_store_explicit(
      &state->playbackDeliveryGeneration, 0, memory_order_seq_cst);
  const bool admitted = beginPlaybackEventNL(state, &target);
  LG_UNLOCK(state->stateLock);

  waitPlaybackEvents(state);

  if (!admitted)
    return;

  LG_LOCK(state->stateLock);
  const bool dispatch = attachmentCurrentNL(state, &target) &&
    target.events->playbackStop;
  LG_UNLOCK(state->stateLock);

  if (dispatch)
    target.events->playbackStop(target.opaque, generation);
  endEvent(&target);
}

static void usbPlaybackData(
    void * opaque, const void * data, size_t frames)
{
  LGA_USBState * state = opaque;
  USBAudioCallbackFrame frame;
  beginCallback(state, &frame, &l_playbackFrames,
      &state->playbackGate.inFlight);

  const uint64_t position = atomic_fetch_add_explicit(
      &state->playbackPosition, frames, memory_order_relaxed);
  const uint32_t generation = atomic_load_explicit(
      &state->playbackDeliveryGeneration, memory_order_seq_cst);
  if (generation)
  {
    const LG_AudioEventOps * events = state->events;
    void                   * target = state->eventOpaque;
    if (events && events->playbackData)
    {
      const LG_AudioClock clock = makePlaybackClock(state, position);
      events->playbackData(
          target, generation, data, frames, &clock);
    }
  }
  endCallback(&frame, &l_playbackFrames,
      &state->playbackGate.inFlight, &state->playbackGate.wait);
}

static void usbRecordStart(
    void * opaque, uint32_t sampleRate, uint32_t channelMask)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;
  LG_AudioFormat      format;
  bool                dispatch;
  uint32_t            generation;
  const bool recordOwner = beginOperation(
      state, &state->recordOperation, l_recordFrames);

  LG_LOCK(state->stateLock);
  setStreamFormat(&state->recordFormat, sampleRate, channelMask);
  format     = state->recordFormat;
  generation = state->recordGeneration;
  if (!generation)
  {
    generation = state->generationSerial =
      nextGeneration(state->generationSerial);
    state->recordGeneration = generation;
  }
  atomic_store_explicit(
      &state->recordDeliveryGeneration, 0, memory_order_seq_cst);
  const bool admitted = beginRecordEventNL(state, &target);
  dispatch = admitted && target.events->recordStart;
  if (dispatch)
    atomic_store_explicit(&state->recordDeliveryGeneration,
        generation, memory_order_seq_cst);
  if (admitted && !dispatch)
    endEvent(&target);
  LG_UNLOCK(state->stateLock);

  if (!dispatch)
  {
    endOperation(&state->recordOperation, recordOwner);
    return;
  }

  target.events->recordStart(
      target.opaque, generation, &format);
  endEvent(&target);
  endOperation(&state->recordOperation, recordOwner);
}

static void usbRecordStop(void * opaque)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;
  const bool recordOwner = beginOperation(
      state, &state->recordOperation, l_recordFrames);

  LG_LOCK(state->stateLock);
  const uint32_t generation = state->recordGeneration;
  if (!generation)
  {
    LG_UNLOCK(state->stateLock);
    endOperation(&state->recordOperation, recordOwner);
    return;
  }

  state->recordGeneration = 0;
  atomic_store_explicit(
      &state->recordDeliveryGeneration, 0, memory_order_seq_cst);
  const bool admitted = beginRecordEventNL(state, &target);
  LG_UNLOCK(state->stateLock);

  /* A reentrant stop cannot quiesce callbacks for the same reason that it
   * cannot wait for ownership above. */
  waitRecordEvents(state);

  if (!admitted)
  {
    endOperation(&state->recordOperation, recordOwner);
    return;
  }

  LG_LOCK(state->stateLock);
  const bool dispatch = attachmentCurrentNL(state, &target) &&
    target.events->recordStop;
  LG_UNLOCK(state->stateLock);

  if (dispatch)
    target.events->recordStop(target.opaque, generation);
  endEvent(&target);
  endOperation(&state->recordOperation, recordOwner);
}

static const LG_USBAudioEventOps l_usbAudioEvents =
{
  .playbackStart = usbPlaybackStart,
  .playbackStop  = usbPlaybackStop,
  .playbackData  = usbPlaybackData,
  .recordStart   = usbRecordStart,
  .recordStop    = usbRecordStop,
};

static void usbSetAvailable(void * opaque, bool available)
{
  LGA_USBState * state = opaque;
  const bool statusOwner = beginOperation(
      state, &state->statusOperation, l_statusFrames);

  LG_LOCK(state->statusLock);
  const bool changed = atomic_load_explicit(
      &state->available, memory_order_relaxed) != available;
  if (changed)
  {
    atomic_store_explicit(
        &state->available, available, memory_order_release);
    state->statusGeneration =
      nextGeneration(state->statusGeneration);
  }

  const LG_AudioStatusFn callback = state->statusCallback;
  void * callbackOpaque           = state->statusOpaque;
  const LG_AudioStatus status =
  {
    .available  = available,
    .generation = state->statusGeneration,
  };
  USBAudioCallbackFrame frame;
  if (changed && callback)
    beginStatusCallback(state, &frame);
  LG_UNLOCK(state->statusLock);

  if (changed && callback)
  {
    callback(callbackOpaque, &status);
    endStatusCallback(state, &frame);
  }
  endOperation(&state->statusOperation, statusOwner);
}

static void usbSetStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  LGA_USBState * state = opaque;
  const bool statusOwner = beginOperation(
      state, &state->statusOperation, l_statusFrames);

  LG_LOCK(state->statusLock);
  state->statusCallback = callback;
  state->statusOpaque   = callbackOpaque;
  const LG_AudioStatus status =
  {
    .available  = atomic_load_explicit(
        &state->available, memory_order_acquire),
    .generation = state->statusGeneration,
  };
  USBAudioCallbackFrame frame;
  if (callback)
    beginStatusCallback(state, &frame);
  LG_UNLOCK(state->statusLock);

  if (callback)
  {
    callback(callbackOpaque, &status);
    endStatusCallback(state, &frame);
  }
  else
    waitStatusCallbacks(state);
  endOperation(&state->statusOperation, statusOwner);
}

static bool usbAttach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  LGA_USBState * state = opaque;
  if (!events || !atomic_load_explicit(
        &state->available, memory_order_acquire))
    return false;

  USBAudioEventTarget playbackTarget;
  USBAudioEventTarget recordTarget;
  LG_AudioFormat      playbackFormat;
  LG_AudioFormat      recordFormat;
  LG_AudioClock       playbackClock;
  uint32_t            attachmentGeneration;
  uint32_t            playbackGeneration;
  uint32_t            recordGeneration;
  bool                playbackDispatch;
  bool                recordDispatch;
  bool                playbackAdmitted;
  bool                recordAdmitted;

  if (!lockStateAfterDetach(state))
    return false;

  if (state->attached || !atomic_load_explicit(
        &state->available, memory_order_acquire))
  {
    LG_UNLOCK(state->stateLock);
    return false;
  }

  state->attached             = true;
  state->attachmentGeneration =
    nextGeneration(state->attachmentGeneration);
  state->events               = events;
  state->eventOpaque          = eventOpaque;
  attachmentGeneration        = state->attachmentGeneration;
  playbackGeneration          = state->playbackGeneration;
  recordGeneration            = state->recordGeneration;
  playbackDispatch = playbackGeneration && events->playbackStart;
  recordDispatch   = recordGeneration && events->recordStart;
  playbackAdmitted = playbackDispatch &&
    beginPlaybackEventNL(state, &playbackTarget);
  playbackDispatch = playbackDispatch && playbackAdmitted;
  recordAdmitted   = false;
  if (playbackDispatch)
  {
    playbackFormat = state->playbackFormat;
    playbackClock = makePlaybackClock(state, atomic_load_explicit(
        &state->playbackPosition, memory_order_relaxed));
  }
  lgUsbRedir_setPlugged(state->redir, true);
  LG_UNLOCK(state->stateLock);

  if (playbackDispatch)
  {
    playbackTarget.events->playbackStart(
        playbackTarget.opaque, playbackGeneration,
        &playbackFormat, &playbackClock);

    LG_LOCK(state->stateLock);
    if (playbackEventCurrentNL(
          state, &playbackTarget, playbackGeneration))
      atomic_store_explicit(&state->playbackDeliveryGeneration,
          playbackGeneration, memory_order_seq_cst);
    LG_UNLOCK(state->stateLock);
  }
  if (playbackAdmitted)
    endEvent(&playbackTarget);

  if (recordDispatch)
  {
    const bool recordOwner = beginOperation(
        state, &state->recordOperation, l_recordFrames);

    LG_LOCK(state->stateLock);
    recordDispatch = state->attached &&
      state->attachmentGeneration == attachmentGeneration &&
      state->events                == events &&
      state->eventOpaque           == eventOpaque &&
      state->recordGeneration      == recordGeneration;
    recordAdmitted = recordDispatch &&
      beginRecordEventNL(state, &recordTarget);
    recordDispatch = recordAdmitted &&
      recordEventCurrentNL(state, &recordTarget, recordGeneration);
    if (recordDispatch)
    {
      recordFormat = state->recordFormat;
      atomic_store_explicit(&state->recordDeliveryGeneration,
          recordGeneration, memory_order_seq_cst);
    }
    LG_UNLOCK(state->stateLock);

    if (recordDispatch)
      recordTarget.events->recordStart(
          recordTarget.opaque, recordGeneration, &recordFormat);

    if (recordAdmitted)
      endEvent(&recordTarget);
    endOperation(&state->recordOperation, recordOwner);
  }

  return true;
}

static void usbDetach(void * opaque)
{
  LGA_USBState * state = opaque;

  if (!lockStateAfterDetach(state))
    return;

  state->detaching = true;
  lgResetEvent(state->detachEvent);
  state->attached  = false;
  state->attachmentGeneration =
    nextGeneration(state->attachmentGeneration);
  const uint32_t attachmentGeneration =
    state->attachmentGeneration;
  atomic_store_explicit(
      &state->playbackDeliveryGeneration, 0, memory_order_seq_cst);
  atomic_store_explicit(
      &state->recordDeliveryGeneration, 0, memory_order_seq_cst);
  lgUsbRedir_setPlugged(state->redir, false);
  LG_UNLOCK(state->stateLock);

  waitPlaybackEvents(state);
  waitRecordEvents(state);

  LG_LOCK(state->stateLock);
  if (!state->attached &&
      state->attachmentGeneration == attachmentGeneration)
  {
    state->events      = NULL;
    state->eventOpaque = NULL;
    state->detaching   = false;
    signalWaitEvent(state->detachEvent);
  }
  LG_UNLOCK(state->stateLock);
}

static bool usbClockFeedback(void * opaque, uint32_t generation,
    const LG_AudioClock * playbackClock, double targetRate)
{
  LGA_USBState * state = opaque;

  LG_LOCK(state->stateLock);
  if (!state->attached ||
      state->playbackGeneration != generation ||
      !lgUsbAudio_feedbackActive(state->device))
  {
    LG_UNLOCK(state->stateLock);
    return false;
  }

  const double nominalRate = state->playbackFormat.sampleRate;
  const double rate = playbackClock &&
      targetRate >= nominalRate * 0.995 &&
      targetRate <= nominalRate * 1.005 ?
    targetRate : nominalRate;
  lgUsbAudio_setFeedbackRate(state->device, rate);
  LG_UNLOCK(state->stateLock);
  return true;
}

static bool usbRecordData(void * opaque, uint32_t generation,
    const void * data, size_t frames,
    const LG_AudioClock * sourceClock)
{
  LGA_USBState * state = opaque;
  USBAudioCallbackFrame frame;
  beginCallback(state, &frame, &l_recordFrames,
      &state->recordGate.inFlight);

  const bool valid = generation && generation == atomic_load_explicit(
      &state->recordDeliveryGeneration, memory_order_seq_cst);
  const bool result = valid &&
    lgUsbAudio_recordData(
        state->device, data, frames, sourceClock);

  endCallback(&frame, &l_recordFrames,
      &state->recordGate.inFlight, &state->recordGate.wait);
  return result;
}

const LG_AudioOps LGA_USB =
{
  .name              = "USB Audio",
  .setStatusListener = usbSetStatusListener,
  .attach            = usbAttach,
  .detach            = usbDetach,
  .recordData        = usbRecordData,
  .clockFeedback     = usbClockFeedback,
};

LGA_USBState * lgaUsb_create(bool debug)
{
  LGA_USBState * state = aligned_alloc(
      alignof(LGA_USBState), sizeof(*state));
  if (!state)
    return NULL;
  memset(state, 0, sizeof(*state));

  LG_LOCK_INIT(state->statusLock);
  LG_LOCK_INIT(state->statusWait.lock);
  LG_LOCK_INIT(state->statusOperation.lock);
  LG_LOCK_INIT(state->stateLock);
  LG_LOCK_INIT(state->playbackGate.wait.lock);
  LG_LOCK_INIT(state->recordGate.wait.lock);
  LG_LOCK_INIT(state->recordOperation.lock);
  atomic_init(&state->available, false);
  atomic_init(&state->playbackPosition, 0);
  atomic_init(&state->playbackDeliveryGeneration, 0);
  atomic_init(&state->recordDeliveryGeneration, 0);
  atomic_init(&state->playbackGate.inFlight, 0);
  atomic_init(&state->recordGate.inFlight, 0);
  atomic_init(&state->statusInFlight, 0);
  atomic_init(&state->playbackGate.wait.count, 0);
  atomic_init(&state->recordGate.wait.count, 0);
  atomic_init(&state->statusWait.count, 0);
  state->playbackFormat = l_formatTemplate;
  state->recordFormat   = l_formatTemplate;

  state->detachEvent = lgCreateEvent(false, 0);
  if (!state->detachEvent || !lgSignalEvent(state->detachEvent))
  {
    if (state->detachEvent)
      lgFreeEvent(state->detachEvent);
    free(state);
    return NULL;
  }

  state->device = lgUsbAudio_create(&l_usbAudioEvents, state, debug);
  if (!state->device)
  {
    lgFreeEvent(state->detachEvent);
    free(state);
    return NULL;
  }

  state->redir = lgUsbRedir_create(
      lgUsbAudio_deviceOps(), state->device, usbSetAvailable, state);
  if (!state->redir)
  {
    lgUsbAudio_destroy(state->device);
    lgFreeEvent(state->detachEvent);
    free(state);
    return NULL;
  }

  return state;
}

void lgaUsb_destroy(LGA_USBState * state)
{
  if (!state)
    return;

  usbSetStatusListener(state, NULL, NULL);
  usbDetach(state);
  lgUsbRedir_destroy(state->redir);
  lgUsbAudio_destroy(state->device);
  lgFreeEvent(state->detachEvent);
  LG_LOCK_FREE(state->recordOperation.lock);
  LG_LOCK_FREE(state->recordGate.wait.lock);
  LG_LOCK_FREE(state->playbackGate.wait.lock);
  LG_LOCK_FREE(state->stateLock);
  LG_LOCK_FREE(state->statusOperation.lock);
  LG_LOCK_FREE(state->statusWait.lock);
  LG_LOCK_FREE(state->statusLock);
  free(state);
}

LG_USBRedir * lgaUsb_redir(LGA_USBState * state)
{
  return state ? state->redir : NULL;
}

uint64_t lgaUsb_processDelayNs(const LGA_USBState * state)
{
  return state ? lgUsbAudio_processDelayNs(state->device) : UINT64_MAX;
}
