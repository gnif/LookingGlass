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

#include "audio.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SPICE_AUDIO_TIMESTAMP_DISCONTINUITY_MS 2000

typedef struct SpiceAudioStream
{
  bool           active;
  uint32_t       generation;
  LG_AudioFormat format;

  bool     volumeValid;
  uint8_t  volumeChannels;
  uint16_t volume[LG_AUDIO_MAX_CHANNELS];
  bool     muteValid;
  bool     mute;
}
SpiceAudioStream;

typedef struct SpiceAudioEventTarget
{
  const LG_AudioEventOps * events;
  void                   * opaque;
  uint32_t                 generation;
}
SpiceAudioEventTarget;

typedef struct SpiceAudioCallbackWaiter
{
  struct SpiceAudioCallbackWaiter * next;
  LGEvent                         * event;
  unsigned int                      depth;
}
SpiceAudioCallbackWaiter;

typedef struct SpiceAudioCallbackWaitQueue
{
  LG_Lock                    lock;
  atomic_uint                count;
  SpiceAudioCallbackWaiter * waiters;
}
SpiceAudioCallbackWaitQueue;

struct SpiceAudio
{
  LG_RWLock lock;

  bool             available;
  uint32_t         statusGeneration;
  LG_AudioStatusFn statusCallback;
  void           * statusOpaque;
  atomic_uint      statusInFlight;
  SpiceAudioCallbackWaitQueue statusWait;

  const LG_AudioEventOps * events;
  void                   * eventOpaque;
  uint32_t                 eventGeneration;
  atomic_uint              inFlight;
  SpiceAudioCallbackWaitQueue eventWait;

  SpiceAudioStream playback;
  SpiceAudioStream record;

  bool     playbackClockValid;
  uint32_t playbackMediaTime;
  int64_t  playbackTime;
  uint64_t playbackPosition;
  LG_AudioClock playbackClock;
};

static _Thread_local unsigned int l_eventDepth;
static _Thread_local unsigned int l_statusDepth;

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static LGEvent * createWaitEvent(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  if (!event)
    DEBUG_FATAL("Failed to create SPICE audio wait event");
  return event;
}

static void waitEvent(LGEvent * event)
{
  if (!lgWaitEvent(event, TIMEOUT_INFINITE))
    DEBUG_FATAL("Failed to wait for SPICE audio event");
}

static void signalWaitEvent(LGEvent * event)
{
  if (!lgSignalEvent(event))
    DEBUG_FATAL("Failed to signal SPICE audio event");
}

static void signalCallbackWaiters(
    SpiceAudioCallbackWaitQueue * queue, unsigned int remaining)
{
  if (!atomic_load_explicit(&queue->count, memory_order_acquire))
    return;

  LG_LOCK(queue->lock);
  SpiceAudioCallbackWaiter ** link = &queue->waiters;
  while (*link)
  {
    SpiceAudioCallbackWaiter * waiter = *link;
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

static void endCallback(atomic_uint * inFlight,
    SpiceAudioCallbackWaitQueue * waitQueue)
{
  const unsigned int previous = atomic_fetch_sub_explicit(
      inFlight, 1, memory_order_release);
  DEBUG_ASSERT(previous > 0);
  signalCallbackWaiters(waitQueue, previous - 1);
}

static void waitCallbacks(atomic_uint * inFlight,
    SpiceAudioCallbackWaitQueue * waitQueue, unsigned int depth)
{
  if (atomic_load_explicit(inFlight, memory_order_acquire) <= depth)
    return;

  SpiceAudioCallbackWaiter waiter =
  {
    .event = createWaitEvent(),
    .depth = depth,
  };

  bool queued = false;
  LG_LOCK(waitQueue->lock);
  if (atomic_load_explicit(inFlight, memory_order_acquire) > depth)
  {
    waiter.next        = waitQueue->waiters;
    waitQueue->waiters = &waiter;
    atomic_fetch_add_explicit(
        &waitQueue->count, 1, memory_order_release);
    queued = true;
  }
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

/* The instance lock must be held exclusively while admitting a callback so
 * detach cannot invalidate the target between the snapshot and in-flight
 * increment. */
static bool beginEventNL(SpiceAudio * audio,
    SpiceAudioEventTarget * target)
{
  if (!audio->events)
    return false;

  target->events     = audio->events;
  target->opaque     = audio->eventOpaque;
  target->generation = audio->eventGeneration;
  atomic_fetch_add_explicit(&audio->inFlight, 1, memory_order_relaxed);
  ++l_eventDepth;
  return true;
}

static bool playbackEventCurrent(SpiceAudio * audio,
    const SpiceAudioEventTarget * target, uint32_t generation, bool active)
{
  LG_LOCK_SHARED(audio->lock);
  const bool result =
    target->events             == audio->events &&
    target->opaque             == audio->eventOpaque &&
    target->generation         == audio->eventGeneration &&
    audio->playback.generation == generation &&
    audio->playback.active     == active;
  LG_UNLOCK_SHARED(audio->lock);
  return result;
}

static bool recordEventCurrent(SpiceAudio * audio,
    const SpiceAudioEventTarget * target, uint32_t generation, bool active)
{
  LG_LOCK_SHARED(audio->lock);
  const bool result =
    target->events           == audio->events &&
    target->opaque           == audio->eventOpaque &&
    target->generation       == audio->eventGeneration &&
    audio->record.generation == generation &&
    audio->record.active     == active;
  LG_UNLOCK_SHARED(audio->lock);
  return result;
}

static void endEvent(SpiceAudio * audio)
{
  --l_eventDepth;
  endCallback(&audio->inFlight, &audio->eventWait);
}

static bool sampleFormat(PSAudioFormat source,
    LG_AudioSampleFormat * format)
{
  switch (source)
  {
    case PS_AUDIO_FMT_S16:
      *format = LG_AUDIO_FMT_S16_LE;
      return true;

    default:
      return false;
  }
}

static void channelLayout(LG_AudioFormat * format)
{
  memset(format->channels, 0, sizeof(format->channels));

  switch (format->channelCount)
  {
    case 1:
      format->channels[0] = LG_AUDIO_CH_MONO;
      break;

    case 2:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      break;

    case 3:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_FRONT_CENTER;
      break;

    case 4:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_REAR_LEFT;
      format->channels[3] = LG_AUDIO_CH_REAR_RIGHT;
      break;

    case 5:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_FRONT_CENTER;
      format->channels[3] = LG_AUDIO_CH_REAR_LEFT;
      format->channels[4] = LG_AUDIO_CH_REAR_RIGHT;
      break;

    case 6:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_FRONT_CENTER;
      format->channels[3] = LG_AUDIO_CH_LFE;
      format->channels[4] = LG_AUDIO_CH_REAR_LEFT;
      format->channels[5] = LG_AUDIO_CH_REAR_RIGHT;
      break;

    case 7:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_FRONT_CENTER;
      format->channels[3] = LG_AUDIO_CH_LFE;
      format->channels[4] = LG_AUDIO_CH_REAR_LEFT;
      format->channels[5] = LG_AUDIO_CH_REAR_RIGHT;
      format->channels[6] = LG_AUDIO_CH_REAR_CENTER;
      break;

    case 8:
      format->channels[0] = LG_AUDIO_CH_FRONT_LEFT;
      format->channels[1] = LG_AUDIO_CH_FRONT_RIGHT;
      format->channels[2] = LG_AUDIO_CH_FRONT_CENTER;
      format->channels[3] = LG_AUDIO_CH_LFE;
      format->channels[4] = LG_AUDIO_CH_REAR_LEFT;
      format->channels[5] = LG_AUDIO_CH_REAR_RIGHT;
      format->channels[6] = LG_AUDIO_CH_SIDE_LEFT;
      format->channels[7] = LG_AUDIO_CH_SIDE_RIGHT;
      break;

    default:
      break;
  }
}

static bool makeFormat(int channels, int sampleRate, PSAudioFormat source,
    LG_AudioFormat * format)
{
  if (channels < 1 || channels > LG_AUDIO_MAX_CHANNELS || sampleRate < 1 ||
      !sampleFormat(source, &format->sampleFormat))
    return false;

  format->sampleRate   = sampleRate;
  format->channelCount = channels;
  channelLayout(format);
  return true;
}

static size_t sampleSize(LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8:     return 1;
    case LG_AUDIO_FMT_S16_LE: return 2;
    case LG_AUDIO_FMT_S24_LE: return 3;
    case LG_AUDIO_FMT_S32_LE: return 4;
    case LG_AUDIO_FMT_F32_LE: return 4;
    case LG_AUDIO_FMT_F32_NE: return 4;
    case LG_AUDIO_FMT_F64_LE: return 8;
  }

  return 0;
}

static LG_AudioClock playbackClockNL(SpiceAudio * audio, uint32_t time)
{
  bool discontinuity = false;

  if (!audio->playbackClockValid)
  {
    audio->playbackClockValid = true;
    audio->playbackMediaTime  = time;
    audio->playbackTime       = 0;
  }
  else
  {
    const int32_t delta = (int32_t)(time - audio->playbackMediaTime);
    audio->playbackMediaTime = time;
    if (delta < 0 || delta > SPICE_AUDIO_TIMESTAMP_DISCONTINUITY_MS)
    {
      audio->playbackTime = 0;
      discontinuity = true;
    }
    else
      audio->playbackTime += (int64_t)delta * 1000000;
  }

  audio->playbackClock = (LG_AudioClock)
  {
    .position      = audio->playbackPosition,
    .time          = audio->playbackTime,
    .rate          = 0.0,
    .stable        = !discontinuity,
    .discontinuity = discontinuity,
  };
  return audio->playbackClock;
}

static void spiceSetStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  SpiceAudio * audio = opaque;
  LG_AudioStatus status;

  LG_LOCK_EXCLUSIVE(audio->lock);
  audio->statusCallback = callback;
  audio->statusOpaque   = callbackOpaque;
  status = (LG_AudioStatus)
  {
    .available  = audio->available,
    .generation = audio->statusGeneration,
  };
  if (callback)
  {
    atomic_fetch_add_explicit(
        &audio->statusInFlight, 1, memory_order_relaxed);
    ++l_statusDepth;
  }
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (callback)
  {
    callback(callbackOpaque, &status);
    --l_statusDepth;
    endCallback(&audio->statusInFlight, &audio->statusWait);
  }
  else
    waitCallbacks(&audio->statusInFlight,
        &audio->statusWait, l_statusDepth);
}

static bool spiceAttach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  SpiceAudio * audio = opaque;
  if (!events)
    return false;

  SpiceAudioEventTarget target;
  SpiceAudioStream playback;
  SpiceAudioStream record;
  LG_AudioClock playbackClock;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return false;
  }

  audio->events          = events;
  audio->eventOpaque     = eventOpaque;
  audio->eventGeneration =
    nextGeneration(audio->eventGeneration);
  playback      = audio->playback;
  record        = audio->record;
  playbackClock = audio->playbackClock;
  dispatch      = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return true;

  if (playback.active)
  {
    if (playbackEventCurrent(audio, &target, playback.generation, true) &&
        target.events->playbackStart)
    {
      target.events->playbackStart(target.opaque,
          playback.generation, &playback.format, &playbackClock);
      if (playback.volumeValid &&
          playbackEventCurrent(audio, &target, playback.generation, true) &&
          target.events->playbackVolume)
        target.events->playbackVolume(target.opaque,
            playback.generation,
            playback.volumeChannels, playback.volume);
      if (playback.muteValid &&
          playbackEventCurrent(audio, &target, playback.generation, true) &&
          target.events->playbackMute)
        target.events->playbackMute(target.opaque,
            playback.generation, playback.mute);
    }
  }

  if (record.active)
  {
    if (recordEventCurrent(audio, &target, record.generation, true) &&
        target.events->recordStart)
    {
      target.events->recordStart(target.opaque,
          record.generation, &record.format);
      if (record.volumeValid &&
          recordEventCurrent(audio, &target, record.generation, true) &&
          target.events->recordVolume)
        target.events->recordVolume(target.opaque, record.generation,
            record.volumeChannels, record.volume);
      if (record.muteValid &&
          recordEventCurrent(audio, &target, record.generation, true) &&
          target.events->recordMute)
        target.events->recordMute(target.opaque,
            record.generation, record.mute);
    }
  }

  endEvent(audio);
  return true;
}

static void spiceDetach(void * opaque)
{
  SpiceAudio * audio = opaque;
  LG_LOCK_EXCLUSIVE(audio->lock);
  audio->events          = NULL;
  audio->eventOpaque     = NULL;
  audio->eventGeneration =
    nextGeneration(audio->eventGeneration);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  waitCallbacks(&audio->inFlight, &audio->eventWait, l_eventDepth);
}

static bool spiceRecordData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  SpiceAudio * audio = opaque;
  size_t size  = 0;
  bool   valid = false;

  LG_LOCK_SHARED(audio->lock);
  if (audio->available && audio->events && audio->record.active &&
      generation == audio->record.generation)
  {
    const size_t bytesPerSample =
      sampleSize(audio->record.format.sampleFormat);
    const size_t channels = audio->record.format.channelCount;
    if (bytesPerSample && frames <= SIZE_MAX / bytesPerSample / channels &&
        (frames == 0 || data))
    {
      size = frames * bytesPerSample * channels;
      valid = true;
    }
  }
  LG_UNLOCK_SHARED(audio->lock);

  return valid && purespice_writeAudio((void *)data, size, 0);
}

static const LG_AudioOps l_spiceAudioOps =
{
  .name              = "SPICE",
  .setStatusListener = spiceSetStatusListener,
  .attach            = spiceAttach,
  .detach            = spiceDetach,
  .recordData        = spiceRecordData,
  .clockFeedback     = NULL,
};

bool spiceAudio_init(SpiceAudio ** result)
{
  if (!result)
    return false;

  SpiceAudio * audio = calloc(1, sizeof(*audio));
  if (!audio)
    return false;

  LG_RWLOCK_INIT(audio->lock);
  atomic_init(&audio->statusInFlight, 0);
  LG_LOCK_INIT(audio->statusWait.lock);
  atomic_init(&audio->statusWait.count, 0);
  atomic_init(&audio->inFlight, 0);
  LG_LOCK_INIT(audio->eventWait.lock);
  atomic_init(&audio->eventWait.count, 0);

  *result = audio;
  return true;
}

void spiceAudio_free(SpiceAudio ** audio)
{
  if (!audio || !*audio)
    return;

  SpiceAudio * instance = *audio;
  spiceAudio_setAvailable(instance, false);
  spiceSetStatusListener(instance, NULL, NULL);
  spiceDetach(instance);

  LG_LOCK_FREE(instance->statusWait.lock);
  LG_LOCK_FREE(instance->eventWait.lock);
  LG_RWLOCK_FREE(instance->lock);
  free(instance);
  *audio = NULL;
}

const LG_AudioOps * spiceAudio_getOps(void)
{
  return &l_spiceAudioOps;
}

void spiceAudio_setAvailable(SpiceAudio * audio, bool available)
{
  LG_AudioStatusFn callback;
  void * callbackOpaque;
  LG_AudioStatus status;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (audio->available == available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  audio->available = available;
  audio->statusGeneration =
    nextGeneration(audio->statusGeneration);
  if (!available)
  {
    audio->playback.active = false;
    audio->record.active   = false;

    audio->playbackClockValid = false;
    audio->playbackPosition   = 0;
    audio->playbackClock      = (LG_AudioClock) { 0 };

    audio->playback.volumeValid = false;
    audio->playback.muteValid   = false;

    audio->record.volumeValid = false;
    audio->record.muteValid   = false;
  }

  callback       = audio->statusCallback;
  callbackOpaque = audio->statusOpaque;
  status = (LG_AudioStatus)
  {
    .available  = available,
    .generation = audio->statusGeneration,
  };
  if (callback)
  {
    atomic_fetch_add_explicit(
        &audio->statusInFlight, 1, memory_order_relaxed);
    ++l_statusDepth;
  }
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (callback)
  {
    callback(callbackOpaque, &status);
    --l_statusDepth;
    endCallback(&audio->statusInFlight, &audio->statusWait);
  }
}

void spiceAudio_playbackStart(SpiceAudio * audio,
    int channels, int sampleRate, PSAudioFormat sourceFormat, uint32_t time)
{
  LG_AudioFormat format;
  if (!makeFormat(channels, sampleRate, sourceFormat, &format))
  {
    DEBUG_ERROR("Invalid SPICE playback format: %d channels, %d Hz, %d",
        channels, sampleRate, sourceFormat);
    return;
  }

  SpiceAudioEventTarget target;
  SpiceAudioStream playback;
  LG_AudioClock clock;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  audio->playback.active     = true;
  audio->playback.generation =
    nextGeneration(audio->playback.generation);
  audio->playback.format     = format;
  audio->playbackClockValid = false;
  audio->playbackPosition   = 0;
  clock = playbackClockNL(audio, time);
  clock.stable        = false;
  clock.discontinuity = true;
  audio->playbackClock = clock;
  playback = audio->playback;
  dispatch = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (playbackEventCurrent(audio, &target, playback.generation, true) &&
      target.events->playbackStart)
  {
    target.events->playbackStart(target.opaque,
        playback.generation, &playback.format, &clock);
    if (playback.volumeValid &&
        playbackEventCurrent(audio, &target, playback.generation, true) &&
        target.events->playbackVolume)
      target.events->playbackVolume(target.opaque,
          playback.generation, playback.volumeChannels, playback.volume);
    if (playback.muteValid &&
        playbackEventCurrent(audio, &target, playback.generation, true) &&
        target.events->playbackMute)
      target.events->playbackMute(target.opaque,
          playback.generation, playback.mute);
  }
  endEvent(audio);
}

void spiceAudio_playbackStop(SpiceAudio * audio)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->playback.active)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  generation                = audio->playback.generation;
  audio->playback.active    = false;
  audio->playbackClockValid = false;
  dispatch = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->playbackStop &&
      playbackEventCurrent(audio, &target, generation, false))
    target.events->playbackStop(target.opaque, generation);
  endEvent(audio);
}

void spiceAudio_playbackVolume(SpiceAudio * audio,
    int channels, const uint16_t volume[])
{
  if (channels < 1 || channels > LG_AUDIO_MAX_CHANNELS || !volume)
    return;

  SpiceAudioEventTarget target;
  uint32_t generation;
  uint16_t snapshot[LG_AUDIO_MAX_CHANNELS];
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  memcpy(audio->playback.volume, volume,
      (size_t)channels * sizeof(*volume));
  audio->playback.volumeChannels = channels;
  audio->playback.volumeValid    = true;
  generation = audio->playback.generation;
  memcpy(snapshot, volume, (size_t)channels * sizeof(*volume));
  dispatch   = audio->playback.active && beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->playbackVolume &&
      playbackEventCurrent(audio, &target, generation, true))
    target.events->playbackVolume(
        target.opaque, generation, channels, snapshot);
  endEvent(audio);
}

void spiceAudio_playbackMute(SpiceAudio * audio, bool mute)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  audio->playback.mute      = mute;
  audio->playback.muteValid = true;
  generation = audio->playback.generation;
  dispatch   = audio->playback.active && beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->playbackMute &&
      playbackEventCurrent(audio, &target, generation, true))
    target.events->playbackMute(target.opaque, generation, mute);
  endEvent(audio);
}

void spiceAudio_playbackData(SpiceAudio * audio,
    uint8_t * data, size_t size, uint32_t time)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  size_t frames;
  LG_AudioClock clock;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available || !audio->playback.active)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  const size_t bytesPerSample =
    sampleSize(audio->playback.format.sampleFormat);
  const size_t stride =
    bytesPerSample * audio->playback.format.channelCount;
  if (!stride || !size || !data || size % stride)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    DEBUG_ERROR("Invalid SPICE playback packet size: %zu", size);
    return;
  }

  frames = size / stride;
  generation = audio->playback.generation;
  clock = playbackClockNL(audio, time);
  audio->playbackPosition += frames;
  dispatch = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->playbackData &&
      playbackEventCurrent(audio, &target, generation, true))
    target.events->playbackData(target.opaque, generation,
        data, frames, &clock);
  endEvent(audio);
}

void spiceAudio_recordStart(SpiceAudio * audio,
    int channels, int sampleRate, PSAudioFormat sourceFormat)
{
  LG_AudioFormat format;
  if (!makeFormat(channels, sampleRate, sourceFormat, &format))
  {
    DEBUG_ERROR("Invalid SPICE record format: %d channels, %d Hz, %d",
        channels, sampleRate, sourceFormat);
    return;
  }

  SpiceAudioEventTarget target;
  SpiceAudioStream record;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  audio->record.active     = true;
  audio->record.generation = nextGeneration(audio->record.generation);
  audio->record.format     = format;
  record   = audio->record;
  dispatch = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (recordEventCurrent(audio, &target, record.generation, true) &&
      target.events->recordStart)
  {
    target.events->recordStart(
        target.opaque, record.generation, &record.format);
    if (record.volumeValid &&
        recordEventCurrent(audio, &target, record.generation, true) &&
        target.events->recordVolume)
      target.events->recordVolume(target.opaque, record.generation,
          record.volumeChannels, record.volume);
    if (record.muteValid &&
        recordEventCurrent(audio, &target, record.generation, true) &&
        target.events->recordMute)
      target.events->recordMute(
          target.opaque, record.generation, record.mute);
  }
  endEvent(audio);
}

void spiceAudio_recordStop(SpiceAudio * audio)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->record.active)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  generation           = audio->record.generation;
  audio->record.active = false;
  dispatch = beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->recordStop &&
      recordEventCurrent(audio, &target, generation, false))
    target.events->recordStop(target.opaque, generation);
  endEvent(audio);
}

void spiceAudio_recordVolume(SpiceAudio * audio,
    int channels, const uint16_t volume[])
{
  if (channels < 1 || channels > LG_AUDIO_MAX_CHANNELS || !volume)
    return;

  SpiceAudioEventTarget target;
  uint32_t generation;
  uint16_t snapshot[LG_AUDIO_MAX_CHANNELS];
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  memcpy(audio->record.volume, volume,
      (size_t)channels * sizeof(*volume));
  audio->record.volumeChannels = channels;
  audio->record.volumeValid    = true;
  generation = audio->record.generation;
  memcpy(snapshot, volume, (size_t)channels * sizeof(*volume));
  dispatch   = audio->record.active && beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->recordVolume &&
      recordEventCurrent(audio, &target, generation, true))
    target.events->recordVolume(
        target.opaque, generation, channels, snapshot);
  endEvent(audio);
}

void spiceAudio_recordMute(SpiceAudio * audio, bool mute)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(audio->lock);
  if (!audio->available)
  {
    LG_UNLOCK_EXCLUSIVE(audio->lock);
    return;
  }

  audio->record.mute      = mute;
  audio->record.muteValid = true;
  generation = audio->record.generation;
  dispatch   = audio->record.active && beginEventNL(audio, &target);
  LG_UNLOCK_EXCLUSIVE(audio->lock);

  if (!dispatch)
    return;

  if (target.events->recordMute &&
      recordEventCurrent(audio, &target, generation, true))
    target.events->recordMute(target.opaque, generation, mute);
  endEvent(audio);
}
