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

#include "audio_spice.h"

#include "common/debug.h"
#include "common/locking.h"

#include <stdatomic.h>
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

static struct
{
  LG_RWLock lock;

  bool             available;
  uint32_t         statusGeneration;
  LG_AudioStatusFn statusCallback;
  void           * statusOpaque;
  atomic_uint      statusInFlight;

  const LG_AudioEventOps * events;
  void                   * eventOpaque;
  uint32_t                 eventGeneration;
  atomic_uint              inFlight;

  SpiceAudioStream playback;
  SpiceAudioStream record;

  bool     playbackClockValid;
  uint32_t playbackMediaTime;
  int64_t  playbackTime;
  uint64_t playbackPosition;
  LG_AudioClock playbackClock;
}
l_spice =
{
  .lock =
  {
    .readers = ATOMIC_VAR_INIT(0),
    .writers = ATOMIC_VAR_INIT(0),
    .writer  = ATOMIC_FLAG_INIT,
  },
  .statusInFlight = ATOMIC_VAR_INIT(0),
  .inFlight       = ATOMIC_VAR_INIT(0),
};

static _Thread_local unsigned int l_eventDepth;
static _Thread_local unsigned int l_statusDepth;

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

/* l_spice.lock must be held exclusively while admitting a callback so detach
 * cannot invalidate the target between the snapshot and in-flight increment. */
static bool beginEventNL(SpiceAudioEventTarget * target)
{
  if (!l_spice.events)
    return false;

  target->events     = l_spice.events;
  target->opaque     = l_spice.eventOpaque;
  target->generation = l_spice.eventGeneration;
  atomic_fetch_add_explicit(&l_spice.inFlight, 1, memory_order_relaxed);
  ++l_eventDepth;
  return true;
}

static bool playbackEventCurrent(const SpiceAudioEventTarget * target,
    uint32_t generation, bool active)
{
  LG_LOCK_SHARED(l_spice.lock);
  const bool result =
    target->events              == l_spice.events &&
    target->opaque              == l_spice.eventOpaque &&
    target->generation          == l_spice.eventGeneration &&
    l_spice.playback.generation == generation &&
    l_spice.playback.active     == active;
  LG_UNLOCK_SHARED(l_spice.lock);
  return result;
}

static bool recordEventCurrent(const SpiceAudioEventTarget * target,
    uint32_t generation, bool active)
{
  LG_LOCK_SHARED(l_spice.lock);
  const bool result =
    target->events            == l_spice.events &&
    target->opaque            == l_spice.eventOpaque &&
    target->generation        == l_spice.eventGeneration &&
    l_spice.record.generation == generation &&
    l_spice.record.active     == active;
  LG_UNLOCK_SHARED(l_spice.lock);
  return result;
}

static void endEvent(void)
{
  --l_eventDepth;
  atomic_fetch_sub_explicit(&l_spice.inFlight, 1, memory_order_release);
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
    case LG_AUDIO_FMT_F64_LE: return 8;
  }

  return 0;
}

static LG_AudioClock playbackClockNL(uint32_t time)
{
  bool discontinuity = false;

  if (!l_spice.playbackClockValid)
  {
    l_spice.playbackClockValid = true;
    l_spice.playbackMediaTime  = time;
    l_spice.playbackTime       = 0;
  }
  else
  {
    const int32_t delta = (int32_t)(time - l_spice.playbackMediaTime);
    l_spice.playbackMediaTime = time;
    if (delta < 0 || delta > SPICE_AUDIO_TIMESTAMP_DISCONTINUITY_MS)
    {
      l_spice.playbackTime = 0;
      discontinuity = true;
    }
    else
      l_spice.playbackTime += (int64_t)delta * 1000000;
  }

  l_spice.playbackClock = (LG_AudioClock)
  {
    .position      = l_spice.playbackPosition,
    .time          = l_spice.playbackTime,
    .rate          = 0.0,
    .stable        = !discontinuity,
    .discontinuity = discontinuity,
  };
  return l_spice.playbackClock;
}

static void spiceSetStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  LG_AudioStatus status;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  l_spice.statusCallback = callback;
  l_spice.statusOpaque   = callbackOpaque;
  status = (LG_AudioStatus)
  {
    .available  = l_spice.available,
    .generation = l_spice.statusGeneration,
  };
  if (callback)
  {
    atomic_fetch_add_explicit(
        &l_spice.statusInFlight, 1, memory_order_relaxed);
    ++l_statusDepth;
  }
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (callback)
  {
    callback(callbackOpaque, &status);
    --l_statusDepth;
    atomic_fetch_sub_explicit(
        &l_spice.statusInFlight, 1, memory_order_release);
  }
  else
    while (atomic_load_explicit(
          &l_spice.statusInFlight, memory_order_acquire) > l_statusDepth)
      ;
}

static bool spiceAttach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  if (!events)
    return false;

  SpiceAudioEventTarget target;
  SpiceAudioStream playback;
  SpiceAudioStream record;
  LG_AudioClock playbackClock;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return false;
  }

  l_spice.events          = events;
  l_spice.eventOpaque     = eventOpaque;
  l_spice.eventGeneration =
    nextGeneration(l_spice.eventGeneration);
  playback      = l_spice.playback;
  record        = l_spice.record;
  playbackClock = l_spice.playbackClock;
  dispatch      = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return true;

  if (playback.active)
  {
    if (playbackEventCurrent(&target, playback.generation, true) &&
        target.events->playbackStart)
    {
      target.events->playbackStart(target.opaque,
          playback.generation, &playback.format, &playbackClock);
      if (playback.volumeValid &&
          playbackEventCurrent(&target, playback.generation, true) &&
          target.events->playbackVolume)
        target.events->playbackVolume(target.opaque,
            playback.generation,
            playback.volumeChannels, playback.volume);
      if (playback.muteValid &&
          playbackEventCurrent(&target, playback.generation, true) &&
          target.events->playbackMute)
        target.events->playbackMute(target.opaque,
            playback.generation, playback.mute);
    }
  }

  if (record.active)
  {
    if (recordEventCurrent(&target, record.generation, true) &&
        target.events->recordStart)
    {
      target.events->recordStart(target.opaque,
          record.generation, &record.format);
      if (record.volumeValid &&
          recordEventCurrent(&target, record.generation, true) &&
          target.events->recordVolume)
        target.events->recordVolume(target.opaque, record.generation,
            record.volumeChannels, record.volume);
      if (record.muteValid &&
          recordEventCurrent(&target, record.generation, true) &&
          target.events->recordMute)
        target.events->recordMute(target.opaque,
            record.generation, record.mute);
    }
  }

  endEvent();
  return true;
}

static void spiceDetach(void * opaque)
{
  LG_LOCK_EXCLUSIVE(l_spice.lock);
  l_spice.events          = NULL;
  l_spice.eventOpaque     = NULL;
  l_spice.eventGeneration =
    nextGeneration(l_spice.eventGeneration);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  while (atomic_load_explicit(
        &l_spice.inFlight, memory_order_acquire) > l_eventDepth)
    ;
}

static bool spiceRecordData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  size_t size  = 0;
  bool   valid = false;

  LG_LOCK_SHARED(l_spice.lock);
  if (l_spice.available && l_spice.events && l_spice.record.active &&
      generation == l_spice.record.generation)
  {
    const size_t bytesPerSample =
      sampleSize(l_spice.record.format.sampleFormat);
    const size_t channels = l_spice.record.format.channelCount;
    if (bytesPerSample && frames <= SIZE_MAX / bytesPerSample / channels &&
        (frames == 0 || data))
    {
      size = frames * bytesPerSample * channels;
      valid = true;
    }
  }
  LG_UNLOCK_SHARED(l_spice.lock);

  return valid && purespice_writeAudio((void *)data, size, 0);
}

const LG_AudioOps LGA_Spice =
{
  .name              = "SPICE",
  .setStatusListener = spiceSetStatusListener,
  .attach            = spiceAttach,
  .detach            = spiceDetach,
  .recordData        = spiceRecordData,
  .clockFeedback     = NULL,
};

void lgaSpice_setAvailable(bool available)
{
  LG_AudioStatusFn callback;
  void * callbackOpaque;
  LG_AudioStatus status;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (l_spice.available == available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  l_spice.available = available;
  l_spice.statusGeneration =
    nextGeneration(l_spice.statusGeneration);
  if (!available)
  {
    l_spice.playback.active      = false;
    l_spice.record.active        = false;
    l_spice.playbackClockValid = false;
    l_spice.playbackPosition   = 0;
    l_spice.playbackClock      = (LG_AudioClock) { 0 };
    l_spice.playback.volumeValid = false;
    l_spice.playback.muteValid   = false;
    l_spice.record.volumeValid   = false;
    l_spice.record.muteValid     = false;
  }

  callback       = l_spice.statusCallback;
  callbackOpaque = l_spice.statusOpaque;
  status = (LG_AudioStatus)
  {
    .available  = available,
    .generation = l_spice.statusGeneration,
  };
  if (callback)
  {
    atomic_fetch_add_explicit(
        &l_spice.statusInFlight, 1, memory_order_relaxed);
    ++l_statusDepth;
  }
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (callback)
  {
    callback(callbackOpaque, &status);
    --l_statusDepth;
    atomic_fetch_sub_explicit(
        &l_spice.statusInFlight, 1, memory_order_release);
  }
}

void lgaSpice_playbackStart(int channels, int sampleRate,
    PSAudioFormat sourceFormat, uint32_t time)
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

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  l_spice.playback.active = true;
  l_spice.playback.generation =
    nextGeneration(l_spice.playback.generation);
  l_spice.playback.format = format;
  l_spice.playbackClockValid = false;
  l_spice.playbackPosition   = 0;
  clock = playbackClockNL(time);
  clock.stable        = false;
  clock.discontinuity = true;
  l_spice.playbackClock = clock;
  playback = l_spice.playback;
  dispatch = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (playbackEventCurrent(&target, playback.generation, true) &&
      target.events->playbackStart)
  {
    target.events->playbackStart(target.opaque,
        playback.generation, &playback.format, &clock);
    if (playback.volumeValid &&
        playbackEventCurrent(&target, playback.generation, true) &&
        target.events->playbackVolume)
      target.events->playbackVolume(target.opaque,
          playback.generation, playback.volumeChannels, playback.volume);
    if (playback.muteValid &&
        playbackEventCurrent(&target, playback.generation, true) &&
        target.events->playbackMute)
      target.events->playbackMute(target.opaque,
          playback.generation, playback.mute);
  }
  endEvent();
}

void lgaSpice_playbackStop(void)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.playback.active)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  generation                 = l_spice.playback.generation;
  l_spice.playback.active    = false;
  l_spice.playbackClockValid = false;
  dispatch = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->playbackStop &&
      playbackEventCurrent(&target, generation, false))
    target.events->playbackStop(target.opaque, generation);
  endEvent();
}

void lgaSpice_playbackVolume(int channels, const uint16_t volume[])
{
  if (channels < 1 || channels > LG_AUDIO_MAX_CHANNELS || !volume)
    return;

  SpiceAudioEventTarget target;
  uint32_t generation;
  uint16_t snapshot[LG_AUDIO_MAX_CHANNELS];
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  memcpy(l_spice.playback.volume, volume,
      (size_t)channels * sizeof(*volume));
  l_spice.playback.volumeChannels = channels;
  l_spice.playback.volumeValid    = true;
  generation = l_spice.playback.generation;
  memcpy(snapshot, volume, (size_t)channels * sizeof(*volume));
  dispatch   = l_spice.playback.active && beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->playbackVolume &&
      playbackEventCurrent(&target, generation, true))
    target.events->playbackVolume(
        target.opaque, generation, channels, snapshot);
  endEvent();
}

void lgaSpice_playbackMute(bool mute)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  l_spice.playback.mute      = mute;
  l_spice.playback.muteValid = true;
  generation = l_spice.playback.generation;
  dispatch   = l_spice.playback.active && beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->playbackMute &&
      playbackEventCurrent(&target, generation, true))
    target.events->playbackMute(target.opaque, generation, mute);
  endEvent();
}

void lgaSpice_playbackData(uint8_t * data, size_t size, uint32_t time)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  size_t frames;
  LG_AudioClock clock;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available || !l_spice.playback.active)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  const size_t bytesPerSample =
    sampleSize(l_spice.playback.format.sampleFormat);
  const size_t stride =
    bytesPerSample * l_spice.playback.format.channelCount;
  if (!stride || !size || !data || size % stride)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    DEBUG_ERROR("Invalid SPICE playback packet size: %zu", size);
    return;
  }

  frames = size / stride;
  generation = l_spice.playback.generation;
  clock = playbackClockNL(time);
  l_spice.playbackPosition += frames;
  dispatch = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->playbackData &&
      playbackEventCurrent(&target, generation, true))
    target.events->playbackData(target.opaque, generation,
        data, frames, &clock);
  endEvent();
}

void lgaSpice_recordStart(int channels, int sampleRate,
    PSAudioFormat sourceFormat)
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

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  l_spice.record.active = true;
  l_spice.record.generation = nextGeneration(l_spice.record.generation);
  l_spice.record.format = format;
  record   = l_spice.record;
  dispatch = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (recordEventCurrent(&target, record.generation, true) &&
      target.events->recordStart)
  {
    target.events->recordStart(
        target.opaque, record.generation, &record.format);
    if (record.volumeValid &&
        recordEventCurrent(&target, record.generation, true) &&
        target.events->recordVolume)
      target.events->recordVolume(target.opaque, record.generation,
          record.volumeChannels, record.volume);
    if (record.muteValid &&
        recordEventCurrent(&target, record.generation, true) &&
        target.events->recordMute)
      target.events->recordMute(
          target.opaque, record.generation, record.mute);
  }
  endEvent();
}

void lgaSpice_recordStop(void)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.record.active)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  generation            = l_spice.record.generation;
  l_spice.record.active = false;
  dispatch = beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->recordStop &&
      recordEventCurrent(&target, generation, false))
    target.events->recordStop(target.opaque, generation);
  endEvent();
}

void lgaSpice_recordVolume(int channels, const uint16_t volume[])
{
  if (channels < 1 || channels > LG_AUDIO_MAX_CHANNELS || !volume)
    return;

  SpiceAudioEventTarget target;
  uint32_t generation;
  uint16_t snapshot[LG_AUDIO_MAX_CHANNELS];
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  memcpy(l_spice.record.volume, volume,
      (size_t)channels * sizeof(*volume));
  l_spice.record.volumeChannels = channels;
  l_spice.record.volumeValid    = true;
  generation = l_spice.record.generation;
  memcpy(snapshot, volume, (size_t)channels * sizeof(*volume));
  dispatch   = l_spice.record.active && beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->recordVolume &&
      recordEventCurrent(&target, generation, true))
    target.events->recordVolume(
        target.opaque, generation, channels, snapshot);
  endEvent();
}

void lgaSpice_recordMute(bool mute)
{
  SpiceAudioEventTarget target;
  uint32_t generation;
  bool dispatch;

  LG_LOCK_EXCLUSIVE(l_spice.lock);
  if (!l_spice.available)
  {
    LG_UNLOCK_EXCLUSIVE(l_spice.lock);
    return;
  }

  l_spice.record.mute      = mute;
  l_spice.record.muteValid = true;
  generation = l_spice.record.generation;
  dispatch   = l_spice.record.active && beginEventNL(&target);
  LG_UNLOCK_EXCLUSIVE(l_spice.lock);

  if (!dispatch)
    return;

  if (target.events->recordMute &&
      recordEventCurrent(&target, generation, true))
    target.events->recordMute(target.opaque, generation, mute);
  endEvent();
}
