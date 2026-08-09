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

#include "common/locking.h"
#include "common/time.h"

#include <stdatomic.h>
#include <stdlib.h>

#define USB_AUDIO_NS_PER_SECOND INT64_C(1000000000)

typedef struct USBAudioCallbackFrame
{
  const struct LGA_USBState    * state;
  struct USBAudioCallbackFrame * previous;
}
USBAudioCallbackFrame;

typedef struct USBAudioEventTarget
{
  const LG_AudioEventOps * events;
  void                   * opaque;
  uint32_t                 attachmentGeneration;
  USBAudioCallbackFrame    frame;
}
USBAudioEventTarget;

struct LGA_USBState
{
  LG_USBAudio * device;
  LG_USBRedir * redir;

  LG_Lock                  statusLock;
  atomic_bool              available;
  uint32_t                 statusGeneration;
  LG_AudioStatusFn         statusCallback;
  void                   * statusOpaque;
  atomic_uint              statusInFlight;
  atomic_uint_fast64_t     statusNextTicket;
  atomic_uint_fast64_t     statusServingTicket;

  LG_Lock                  stateLock;
  bool                     attached;
  bool                     detaching;
  uint32_t                 attachmentGeneration;
  const LG_AudioEventOps * events;
  void                   * eventOpaque;

  LG_AudioFormat           streamFormat;
  uint32_t                 streamGeneration;
  uint32_t                 generationSerial;
  int64_t                  clockOrigin;
  atomic_uint_fast64_t     position;
  atomic_uint              deliveryGeneration;
  atomic_uint              inFlight;
};

static _Thread_local USBAudioCallbackFrame * l_eventFrames;
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
    USBAudioCallbackFrame ** frames, atomic_uint * inFlight)
{
  *frames = frame->previous;
  atomic_fetch_sub_explicit(inFlight, 1, memory_order_seq_cst);
}

static LG_AudioClock makeClock(
    const LGA_USBState * state, uint64_t position)
{
  /* USB redirection carries no publisher timestamp or USB frame number.
   * Preserve the sample timeline, but do not claim a measured source rate. */
  const uint32_t sampleRate = state->streamFormat.sampleRate;
  const int64_t elapsed =
    position / sampleRate * USB_AUDIO_NS_PER_SECOND +
    position % sampleRate * USB_AUDIO_NS_PER_SECOND / sampleRate;

  return (LG_AudioClock)
  {
    .position = position,
    .time     = state->clockOrigin + elapsed,
    .rate     = 0.0,
    .stable   = false,
  };
}

/* stateLock must be held while admitting a control event. The data event
 * path performs the equivalent admission using deliveryGeneration. */
static bool beginEventNL(
    LGA_USBState * state, USBAudioEventTarget * target)
{
  if (!state->attached || !state->events)
    return false;

  target->events               = state->events;
  target->opaque               = state->eventOpaque;
  target->attachmentGeneration = state->attachmentGeneration;
  beginCallback(
      state, &target->frame, &l_eventFrames, &state->inFlight);
  return true;
}

static bool attachmentCurrentNL(const LGA_USBState * state,
    const USBAudioEventTarget * target)
{
  return state->attached &&
    state->attachmentGeneration == target->attachmentGeneration &&
    state->events                == target->events &&
    state->eventOpaque           == target->opaque;
}

static bool eventCurrentNL(const LGA_USBState * state,
    const USBAudioEventTarget * target, uint32_t streamGeneration)
{
  return attachmentCurrentNL(state, target) &&
    state->streamGeneration == streamGeneration;
}

static void endEvent(LGA_USBState * state, USBAudioEventTarget * target)
{
  endCallback(&target->frame, &l_eventFrames, &state->inFlight);
}

static void waitEvents(const LGA_USBState * state)
{
  const unsigned int depth = callbackDepth(l_eventFrames, state);
  while (atomic_load_explicit(
        &state->inFlight, memory_order_seq_cst) > depth)
    ;
}

static bool beginStatusOperation(LGA_USBState * state)
{
  if (callbackDepth(l_statusFrames, state))
    return false;

  const uint_fast64_t ticket = atomic_fetch_add_explicit(
      &state->statusNextTicket, 1, memory_order_relaxed);
  while (atomic_load_explicit(
        &state->statusServingTicket, memory_order_acquire) != ticket)
    ;
  return true;
}

static void endStatusOperation(LGA_USBState * state, bool owner)
{
  if (owner)
    atomic_fetch_add_explicit(
        &state->statusServingTicket, 1, memory_order_release);
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
  endCallback(frame, &l_statusFrames, &state->statusInFlight);
}

static void waitStatusCallbacks(const LGA_USBState * state)
{
  const unsigned int depth = callbackDepth(l_statusFrames, state);
  while (atomic_load_explicit(
        &state->statusInFlight, memory_order_acquire) > depth)
    ;
}

static void usbAudioStart(
    void * opaque, uint32_t sampleRate, uint32_t channelMask)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;
  bool                dispatch;
  uint32_t            generation;
  LG_AudioClock       clock;

  LG_LOCK(state->stateLock);
  if (state->streamGeneration)
  {
    LG_UNLOCK(state->stateLock);
    return;
  }

  setStreamFormat(&state->streamFormat, sampleRate, channelMask);
  generation = state->generationSerial =
    nextGeneration(state->generationSerial);
  state->streamGeneration = generation;
  state->clockOrigin = (int64_t)nanotime();
  atomic_store_explicit(&state->position, 0, memory_order_relaxed);
  atomic_store_explicit(
      &state->deliveryGeneration, 0, memory_order_seq_cst);
  clock    = makeClock(state, 0);
  const bool admitted = beginEventNL(state, &target);
  dispatch = admitted && target.events->playbackStart;
  if (admitted && !dispatch)
    endEvent(state, &target);
  LG_UNLOCK(state->stateLock);

  if (!dispatch)
    return;

  target.events->playbackStart(
      target.opaque, generation, &state->streamFormat, &clock);

  LG_LOCK(state->stateLock);
  if (eventCurrentNL(state, &target, generation))
    atomic_store_explicit(&state->deliveryGeneration,
        generation, memory_order_seq_cst);
  LG_UNLOCK(state->stateLock);
  endEvent(state, &target);
}

static void usbAudioStop(void * opaque)
{
  LGA_USBState * state = opaque;
  USBAudioEventTarget target;

  LG_LOCK(state->stateLock);
  const uint32_t generation = state->streamGeneration;
  if (!generation)
  {
    LG_UNLOCK(state->stateLock);
    return;
  }

  state->streamGeneration = 0;
  atomic_store_explicit(
      &state->deliveryGeneration, 0, memory_order_seq_cst);
  const bool admitted = beginEventNL(state, &target);
  LG_UNLOCK(state->stateLock);

  waitEvents(state);

  if (!admitted)
    return;

  LG_LOCK(state->stateLock);
  const bool dispatch = attachmentCurrentNL(state, &target) &&
    target.events->playbackStop;
  LG_UNLOCK(state->stateLock);

  if (dispatch)
    target.events->playbackStop(target.opaque, generation);
  endEvent(state, &target);
}

static void usbAudioData(void * opaque, const void * data, size_t frames)
{
  LGA_USBState * state = opaque;
  USBAudioCallbackFrame frame;
  beginCallback(state, &frame, &l_eventFrames, &state->inFlight);

  const uint64_t position = atomic_fetch_add_explicit(
      &state->position, frames, memory_order_relaxed);
  const uint32_t generation = atomic_load_explicit(
      &state->deliveryGeneration, memory_order_seq_cst);
  if (generation)
  {
    const LG_AudioEventOps * events = state->events;
    void                   * target = state->eventOpaque;
    if (events && events->playbackData)
    {
      const LG_AudioClock clock = makeClock(state, position);
      events->playbackData(
          target, generation, data, frames, &clock);
    }
  }
  endCallback(&frame, &l_eventFrames, &state->inFlight);
}

static const LG_USBAudioEventOps l_usbAudioEvents =
{
  .start = usbAudioStart,
  .stop  = usbAudioStop,
  .data  = usbAudioData,
};

static void usbSetAvailable(void * opaque, bool available)
{
  LGA_USBState * state = opaque;
  const bool statusOwner = beginStatusOperation(state);

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
  endStatusOperation(state, statusOwner);
}

static void usbSetStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  LGA_USBState * state = opaque;
  const bool statusOwner = beginStatusOperation(state);

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
  endStatusOperation(state, statusOwner);
}

static bool usbAttach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  LGA_USBState * state = opaque;
  if (!events || !atomic_load_explicit(
        &state->available, memory_order_acquire))
    return false;

  USBAudioEventTarget target;
  LG_AudioClock       clock;
  uint32_t            generation;
  bool                dispatch;

  for (;;)
  {
    LG_LOCK(state->stateLock);
    if (!state->detaching)
      break;
    LG_UNLOCK(state->stateLock);
    if (callbackDepth(l_eventFrames, state))
      return false;
  }

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
  generation                  = state->streamGeneration;
  dispatch = generation && events->playbackStart &&
    beginEventNL(state, &target);
  if (dispatch)
    clock = makeClock(state, atomic_load_explicit(
        &state->position, memory_order_relaxed));
  lgUsbRedir_setPlugged(state->redir, true);
  LG_UNLOCK(state->stateLock);

  if (dispatch)
  {
    target.events->playbackStart(
        target.opaque, generation, &state->streamFormat, &clock);

    LG_LOCK(state->stateLock);
    if (eventCurrentNL(state, &target, generation))
      atomic_store_explicit(&state->deliveryGeneration,
          generation, memory_order_seq_cst);
    LG_UNLOCK(state->stateLock);
    endEvent(state, &target);
  }

  return true;
}

static void usbDetach(void * opaque)
{
  LGA_USBState * state = opaque;

  for (;;)
  {
    LG_LOCK(state->stateLock);
    if (!state->detaching)
      break;
    LG_UNLOCK(state->stateLock);
    if (callbackDepth(l_eventFrames, state))
      return;
  }

  state->detaching = true;
  state->attached  = false;
  state->attachmentGeneration =
    nextGeneration(state->attachmentGeneration);
  const uint32_t attachmentGeneration =
    state->attachmentGeneration;
  atomic_store_explicit(
      &state->deliveryGeneration, 0, memory_order_seq_cst);
  lgUsbRedir_setPlugged(state->redir, false);
  LG_UNLOCK(state->stateLock);

  waitEvents(state);

  LG_LOCK(state->stateLock);
  if (!state->attached &&
      state->attachmentGeneration == attachmentGeneration)
  {
    state->events      = NULL;
    state->eventOpaque = NULL;
    state->detaching   = false;
  }
  LG_UNLOCK(state->stateLock);
}

static bool usbClockFeedback(void * opaque, uint32_t generation,
    const LG_AudioClock * playbackClock, double targetRate)
{
  LGA_USBState * state = opaque;

  LG_LOCK(state->stateLock);
  if (!state->attached || state->streamGeneration != generation)
  {
    LG_UNLOCK(state->stateLock);
    return false;
  }

  const double nominalRate = state->streamFormat.sampleRate;
  const double rate = playbackClock &&
      targetRate >= nominalRate * 0.995 &&
      targetRate <= nominalRate * 1.005 ?
    targetRate : nominalRate;
  lgUsbAudio_setFeedbackRate(state->device, rate);
  LG_UNLOCK(state->stateLock);
  return true;
}

const LG_AudioOps LGA_USB =
{
  .name              = "USB Audio",
  .setStatusListener = usbSetStatusListener,
  .attach            = usbAttach,
  .detach            = usbDetach,
  .recordData        = NULL,
  .clockFeedback     = usbClockFeedback,
};

LGA_USBState * lgaUsb_create(void)
{
  LGA_USBState * state = calloc(1, sizeof(*state));
  if (!state)
    return NULL;

  LG_LOCK_INIT(state->statusLock);
  LG_LOCK_INIT(state->stateLock);
  atomic_init(&state->available, false);
  atomic_init(&state->position, 0);
  atomic_init(&state->deliveryGeneration, 0);
  atomic_init(&state->inFlight, 0);
  atomic_init(&state->statusInFlight, 0);
  atomic_init(&state->statusNextTicket, 0);
  atomic_init(&state->statusServingTicket, 0);
  state->streamFormat = l_formatTemplate;

  state->device = lgUsbAudio_create(&l_usbAudioEvents, state);
  if (!state->device)
  {
    free(state);
    return NULL;
  }

  state->redir = lgUsbRedir_create(
      lgUsbAudio_deviceOps(), state->device, usbSetAvailable, state);
  if (!state->redir)
  {
    lgUsbAudio_destroy(state->device);
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
  LG_LOCK_FREE(state->stateLock);
  LG_LOCK_FREE(state->statusLock);
  free(state);
}

LG_USBRedir * lgaUsb_redir(LGA_USBState * state)
{
  return state ? state->redir : NULL;
}
