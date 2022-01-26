/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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
#include "main.h"
#include "common/array.h"
#include "common/util.h"
#include "common/ringbuffer.h"

#include "dynamic/audiodev.h"

#include <string.h>

typedef enum
{
  STREAM_STATE_STOP,
  STREAM_STATE_SETUP,
  STREAM_STATE_RUN,
  STREAM_STATE_DRAIN
}
StreamState;

#define STREAM_ACTIVE(state) \
  (state == STREAM_STATE_SETUP || state == STREAM_STATE_RUN)

typedef struct
{
  struct LG_AudioDevOps * audioDev;

  struct
  {
    StreamState state;
    int         volumeChannels;
    uint16_t    volume[8];
    bool        mute;
    int         sampleRate;
    int         stride;
    RingBuffer  buffer;

    LG_Lock     lock;
    RingBuffer  timings;
    GraphHandle graph;
  }
  playback;

  struct
  {
    bool     started;
    int      volumeChannels;
    uint16_t volume[8];
    bool     mute;
    int      stride;
    uint32_t time;
  }
  record;
}
AudioState;

static AudioState audio = { 0 };

static void playbackStopNL(void);

void audio_init(void)
{
  // search for the best audiodev to use
  for(int i = 0; i < LG_AUDIODEV_COUNT; ++i)
    if (LG_AudioDevs[i]->init())
    {
      audio.audioDev = LG_AudioDevs[i];
      LG_LOCK_INIT(audio.playback.lock);
      DEBUG_INFO("Using AudioDev: %s", audio.audioDev->name);
      return;
    }

  DEBUG_WARN("Failed to initialize an audio backend");
}

void audio_free(void)
{
  if (!audio.audioDev)
    return;

  // immediate stop of the stream, do not wait for drain
  LG_LOCK(audio.playback.lock);
  playbackStopNL();
  LG_UNLOCK(audio.playback.lock);

  audio_recordStop();

  audio.audioDev->free();
  audio.audioDev = NULL;
  LG_LOCK_FREE(audio.playback.lock);
}

bool audio_supportsPlayback(void)
{
  return audio.audioDev && audio.audioDev->playback.start;
}

static const char * audioGraphFormatFn(const char * name,
    float min, float max, float avg, float freq, float last)
{
  static char title[64];
  snprintf(title, sizeof(title),
      "%s: min:%4.2f max:%4.2f avg:%4.2f now:%4.2f",
      name, min, max, avg, last);
  return title;
}

static void playbackStopNL(void)
{
  if (audio.playback.state == STREAM_STATE_STOP)
    return;

  audio.playback.state = STREAM_STATE_STOP;
  audio.audioDev->playback.stop();
  ringbuffer_free(&audio.playback.buffer);

  if (audio.playback.timings)
  {
    app_unregisterGraph(audio.playback.graph);
    ringbuffer_free(&audio.playback.timings);
  }
}

static int playbackPullFrames(uint8_t * dst, int frames)
{
  if (audio.playback.buffer)
    frames = ringbuffer_consume(audio.playback.buffer, dst, frames);
  else
    frames = 0;

  if (audio.playback.state == STREAM_STATE_DRAIN &&
      ringbuffer_getCount(audio.playback.buffer) == 0)
  {
    LG_LOCK(audio.playback.lock);
    playbackStopNL();
    LG_UNLOCK(audio.playback.lock);
  }
  return frames;
}

void audio_playbackStart(int channels, int sampleRate, PSAudioFormat format,
  uint32_t time)
{
  if (!audio.audioDev)
    return;

  LG_LOCK(audio.playback.lock);

  if (audio.playback.state != STREAM_STATE_STOP)
  {
    // Stop the current playback immediately. Even if the format is compatible,
    // we may not have enough data left in the buffers to avoid underrunning
    playbackStopNL();
  }

  // Using a bounded ring buffer for now. We are not currently doing anything to
  // keep the input and output in sync, so if we were using an unbounded buffer,
  // it would eventually end up in a state of permanent underrun or overrun and
  // the user would only hear silence
  const int bufferFrames = sampleRate;
  audio.playback.buffer = ringbuffer_new(bufferFrames,
      channels * sizeof(uint16_t));

  audio.playback.sampleRate = sampleRate;
  audio.playback.stride     = channels * sizeof(uint16_t);
  audio.playback.state      = STREAM_STATE_SETUP;

  audio.audioDev->playback.setup(channels, sampleRate, playbackPullFrames);

  // if a volume level was stored, set it before we return
  if (audio.playback.volumeChannels)
    audio.audioDev->playback.volume(
        audio.playback.volumeChannels,
        audio.playback.volume);

  // set the inital mute state
  if (audio.audioDev->playback.mute)
    audio.audioDev->playback.mute(audio.playback.mute);

  // if the audio dev can report it's latency setup a timing graph
  audio.playback.timings = ringbuffer_new(1200, sizeof(float));
  audio.playback.graph   = app_registerGraph("PLAYBACK",
      audio.playback.timings, 0.0f, 100.0f, audioGraphFormatFn);

  audio.playback.state = STREAM_STATE_SETUP;

  LG_UNLOCK(audio.playback.lock);
}

void audio_playbackStop(void)
{
  if (!audio.audioDev || audio.playback.state == STREAM_STATE_STOP)
    return;

  audio.playback.state = STREAM_STATE_DRAIN;
  return;
}

void audio_playbackVolume(int channels, const uint16_t volume[])
{
  if (!audio.audioDev || !audio.audioDev->playback.volume)
    return;

  // store the values so we can restore the state if the stream is restarted
  channels = min(ARRAY_LENGTH(audio.playback.volume), channels);
  memcpy(audio.playback.volume, volume, sizeof(uint16_t) * channels);
  audio.playback.volumeChannels = channels;

  if (!STREAM_ACTIVE(audio.playback.state))
    return;

  audio.audioDev->playback.volume(channels, volume);
}

void audio_playbackMute(bool mute)
{
  if (!audio.audioDev || !audio.audioDev->playback.mute)
    return;

  // store the value so we can restore it if the stream is restarted
  audio.playback.mute = mute;
  if (!STREAM_ACTIVE(audio.playback.state))
    return;

  audio.audioDev->playback.mute(mute);
}

void audio_playbackData(uint8_t * data, size_t size)
{
  if (!audio.audioDev)
    return;

  if (!STREAM_ACTIVE(audio.playback.state))
    return;

  int frames = size / audio.playback.stride;
  ringbuffer_append(audio.playback.buffer, data, frames);

  if (audio.playback.state == STREAM_STATE_SETUP)
  {
    frames = ringbuffer_getCount(audio.playback.buffer);
    if (audio.audioDev->playback.start(frames))
      audio.playback.state = STREAM_STATE_RUN;
  }
}

bool audio_supportsRecord(void)
{
  return audio.audioDev && audio.audioDev->record.start;
}

static void recordPushFrames(uint8_t * data, int frames)
{
  purespice_writeAudio(data, frames * audio.record.stride, 0);
}

void audio_recordStart(int channels, int sampleRate, PSAudioFormat format)
{
  if (!audio.audioDev)
    return;

  static int lastChannels   = 0;
  static int lastSampleRate = 0;

  if (audio.record.started)
  {
    if (channels != lastChannels || sampleRate != lastSampleRate)
      audio.audioDev->record.stop();
    else
      return;
  }

  lastChannels   = channels;
  lastSampleRate = sampleRate;
  audio.record.started = true;
  audio.record.stride  = channels * sizeof(uint16_t);

  audio.audioDev->record.start(channels, sampleRate, recordPushFrames);

  // if a volume level was stored, set it before we return
  if (audio.record.volumeChannels)
    audio.audioDev->record.volume(
        audio.playback.volumeChannels,
        audio.playback.volume);

  // set the inital mute state
  if (audio.audioDev->record.mute)
    audio.audioDev->record.mute(audio.playback.mute);
}

void audio_recordStop(void)
{
  if (!audio.audioDev || !audio.record.started)
    return;

  audio.audioDev->record.stop();
  audio.record.started = false;
}

void audio_recordVolume(int channels, const uint16_t volume[])
{
  if (!audio.audioDev || !audio.audioDev->record.volume)
    return;

  // store the values so we can restore the state if the stream is restarted
  channels = min(ARRAY_LENGTH(audio.record.volume), channels);
  memcpy(audio.record.volume, volume, sizeof(uint16_t) * channels);
  audio.record.volumeChannels = channels;

  if (!audio.record.started)
    return;

  audio.audioDev->record.volume(channels, volume);
}

void audio_recordMute(bool mute)
{
  if (!audio.audioDev || !audio.audioDev->record.mute)
    return;

  // store the value so we can restore it if the stream is restarted
  audio.record.mute = mute;
  if (!audio.record.started)
    return;

  audio.audioDev->record.mute(mute);
}

void audio_tick(unsigned long long tickCount)
{
  LG_LOCK(audio.playback.lock);
  if (!audio.playback.buffer)
  {
    LG_UNLOCK(audio.playback.lock);
    return;
  }

  int frames = ringbuffer_getCount(audio.playback.buffer);
  if (audio.audioDev->playback.latency)
    frames += audio.audioDev->playback.latency();

  const float latency = frames / (float)(audio.playback.sampleRate / 1000);

  ringbuffer_push(audio.playback.timings, &latency);

  LG_UNLOCK(audio.playback.lock);

  app_invalidateGraphs();
}
