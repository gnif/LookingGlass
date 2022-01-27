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

#include <math.h>
#include <samplerate.h>
#include <stdalign.h>
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
  int     periodFrames;
  double  periodSec;
  int64_t nextTime;
  int64_t nextPosition;
  double  b;
  double  c;
}
PlaybackDeviceData;

typedef struct
{
  float * framesIn;
  float * framesOut;
  int     framesOutSize;

  int     periodFrames;
  double  periodSec;
  int64_t nextTime;
  int64_t nextPosition;
  double  b;
  double  c;

  int64_t devLastTime;
  int64_t devNextTime;

  int64_t devLastPosition;
  int64_t devNextPosition;

  double  offsetError;
  double  offsetErrorIntegral;

  double  ratioIntegral;

  SRC_STATE * src;
}
PlaybackSpiceData;

typedef struct
{
  struct LG_AudioDevOps * audioDev;

  struct
  {
    StreamState state;
    int         volumeChannels;
    uint16_t    volume[8];
    bool        mute;
    int         channels;
    int         sampleRate;
    int         stride;
    RingBuffer  buffer;
    RingBuffer  deviceTiming;

    LG_Lock     lock;
    RingBuffer  timings;
    GraphHandle graph;

    // These two structs contain data specifically for use in the device and
    // Spice data threads respectively. Keep them on separate cache lines to
    // avoid false sharing
    alignas(64) PlaybackDeviceData deviceData;
    alignas(64) PlaybackSpiceData  spiceData;
    int targetLatencyFrames;
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

typedef struct
{
  int64_t nextTime;
  int64_t nextPosition;
}
PlaybackDeviceTick;

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
  ringbuffer_free(&audio.playback.deviceTiming);
  audio.playback.spiceData.src = src_delete(audio.playback.spiceData.src);

  if (audio.playback.spiceData.framesIn)
  {
    free(audio.playback.spiceData.framesIn);
    free(audio.playback.spiceData.framesOut);
    audio.playback.spiceData.framesIn = NULL;
    audio.playback.spiceData.framesOut = NULL;
  }

  if (audio.playback.timings)
  {
    app_unregisterGraph(audio.playback.graph);
    ringbuffer_free(&audio.playback.timings);
  }
}

static int playbackPullFrames(uint8_t * dst, int frames)
{
  DEBUG_ASSERT(frames >= 0);
  if (frames == 0)
    return frames;

  PlaybackDeviceData * data = &audio.playback.deviceData;
  int64_t now = nanotime();

  if (audio.playback.buffer)
  {
    static bool first = true;
    // Measure the device clock and post to the Spice thread
    if (frames != data->periodFrames || first)
    {
      if (first)
      {
        data->nextTime = now;
        first = false;
      }

      data->nextTime     += llrint(data->periodSec * 1.0e9);
      data->nextPosition += frames;

      double bandwidth = 0.05;
      double omega = 2.0 * M_PI * bandwidth * data->periodSec;
      data->b = M_SQRT2 * omega;
      data->c = omega * omega;
    }
    else
    {
      double error = (now - data->nextTime) * 1.0e-9;
      if (fabs(error) >= 0.2)
      {
        // Clock error is too high; slew the read pointer and reset the timing
        // parameters to avoid getting too far out of sync
        int slewFrames = round(error * audio.playback.sampleRate);
        ringbuffer_consume(audio.playback.buffer, NULL, slewFrames);

        data->periodSec     = (double) frames / audio.playback.sampleRate;
        data->nextTime      = now + llrint(data->periodSec * 1.0e9);
        data->nextPosition += slewFrames + frames;
      }
      else
      {
        data->nextTime     +=
          llrint((data->b * error + data->periodSec) * 1.0e9);
        data->periodSec    += data->c * error;
        data->nextPosition += frames;
      }
    }

    PlaybackDeviceTick tick =
    {
      .nextTime     = data->nextTime,
      .nextPosition = data->nextPosition
    };
    ringbuffer_append(audio.playback.deviceTiming, &tick, 1);

    ringbuffer_consume(audio.playback.buffer, dst, frames);
  }
  else
    frames = 0;

  if (audio.playback.state == STREAM_STATE_DRAIN &&
      ringbuffer_getCount(audio.playback.buffer) <= 0)
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

  int srcError;
  audio.playback.spiceData.src =
    src_new(SRC_SINC_BEST_QUALITY, channels, &srcError);
  if (!audio.playback.spiceData.src)
  {
    DEBUG_ERROR("Failed to create resampler: %s", src_strerror(srcError));
    goto done;
  }

  const int bufferFrames = sampleRate;
  audio.playback.buffer = ringbuffer_newUnbounded(bufferFrames,
      channels * sizeof(float));

  audio.playback.deviceTiming = ringbuffer_new(16, sizeof(PlaybackDeviceTick));

  audio.playback.channels   = channels;
  audio.playback.sampleRate = sampleRate;
  audio.playback.stride     = channels * sizeof(float);
  audio.playback.state      = STREAM_STATE_SETUP;

  audio.playback.deviceData.nextPosition       = 0;

  audio.playback.spiceData.nextPosition        = 0;
  audio.playback.spiceData.devLastTime         = INT64_MIN;
  audio.playback.spiceData.devNextTime         = INT64_MIN;
  audio.playback.spiceData.offsetError         = 0.0;
  audio.playback.spiceData.offsetErrorIntegral = 0.0;
  audio.playback.spiceData.ratioIntegral       = 0.0;

  int frames;
  audio.audioDev->playback.setup(channels, sampleRate, playbackPullFrames,
      &frames);

  audio.playback.deviceData.periodFrames = frames;
  audio.playback.targetLatencyFrames     = frames;
  audio.playback.deviceData.periodSec    =
    (double)frames / audio.playback.sampleRate;

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

done:
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
  if (!audio.audioDev || size == 0)
    return;

  if (!STREAM_ACTIVE(audio.playback.state))
    return;

  PlaybackSpiceData  * spiceData = &audio.playback.spiceData;
  PlaybackDeviceData * devData   = &audio.playback.deviceData;
  int64_t now = nanotime();

  // Convert from s16 to f32 samples
  int spiceStride    = audio.playback.channels * sizeof(int16_t);
  int frames         = size / spiceStride;
  bool periodChanged = frames != spiceData->periodFrames;
  bool init          = spiceData->periodFrames == 0;

  if (periodChanged)
  {
    if (spiceData->framesIn)
    {
      free(spiceData->framesIn);
      free(spiceData->framesOut);
    }
    spiceData->periodFrames  = frames;
    spiceData->framesIn      = malloc(frames * audio.playback.stride);

    spiceData->framesOutSize = round(frames * 1.1);
    spiceData->framesOut     =
      malloc(spiceData->framesOutSize * audio.playback.stride);
  }

  src_short_to_float_array((int16_t *) data, spiceData->framesIn,
    frames * audio.playback.channels);

  // Receive timing information from the audio device thread
  PlaybackDeviceTick deviceTick;
  while (ringbuffer_consume(audio.playback.deviceTiming, &deviceTick, 1))
  {
    spiceData->devLastTime     = spiceData->devNextTime;
    spiceData->devLastPosition = spiceData->devNextPosition;
    spiceData->devNextTime     = deviceTick.nextTime;
    spiceData->devNextPosition = deviceTick.nextPosition;
  }

  // If the buffer is getting too empty increase the target latency
  static bool checkFill = false;
  if (checkFill && audio.playback.state == STREAM_STATE_RUN &&
      ringbuffer_getCount(audio.playback.buffer) < devData->periodFrames)
  {
    audio.playback.targetLatencyFrames += devData->periodFrames;
    checkFill = false;
  }

  // Measure the Spice audio clock
  int64_t curTime;
  int64_t curPosition;
  if (periodChanged)
  {
    if (init)
      spiceData->nextTime = now;

    curTime     = spiceData->nextTime;
    curPosition = spiceData->nextPosition;

    spiceData->periodSec = (double) frames / audio.playback.sampleRate;
    spiceData->nextTime += llrint(spiceData->periodSec * 1.0e9);

    double bandwidth = 0.05;
    double omega = 2.0 * M_PI * bandwidth * spiceData->periodSec;
    spiceData->b = M_SQRT2 * omega;
    spiceData->c = omega * omega;
  }
  else
  {
    double error = (now - spiceData->nextTime) * 1.0e-9;
    if (fabs(error) >= 0.2)
    {
      // Clock error is too high; slew the write pointer and reset the timing
      // parameters to avoid getting too far out of sync
      int slewFrames = round(error * audio.playback.sampleRate);
      ringbuffer_append(audio.playback.buffer, NULL, slewFrames);

      curTime     = now;
      curPosition = spiceData->nextPosition + slewFrames;

      spiceData->periodSec    = (double) frames / audio.playback.sampleRate;
      spiceData->nextTime     = now + llrint(spiceData->periodSec * 1.0e9);
      spiceData->nextPosition = curPosition;
    }
    else
    {
      curTime     = spiceData->nextTime;
      curPosition = spiceData->nextPosition;

      spiceData->nextTime  +=
        llrint((spiceData->b * error + spiceData->periodSec) * 1.0e9);
      spiceData->periodSec += spiceData->c * error;
    }
  }

  // Measure the offset between the Spice position and the device position,
  // and how far away this is from the target latency. We use this to adjust
  // the playback speed to bring them back in line. This value can change
  // quite rapidly, particularly at the start of playback, so filter it to
  // avoid sudden pitch shifts which will be noticeable to the user.
  double offsetError = spiceData->offsetError;
  if (spiceData->devLastTime != INT64_MIN)
  {
    // Interpolate to calculate the current device position
    double devPosition = spiceData->devLastPosition +
      (spiceData->devNextPosition - spiceData->devLastPosition) *
        ((double) (curTime - spiceData->devLastTime) /
          (spiceData->devNextTime - spiceData->devLastTime));

    double actualOffset = curPosition - devPosition;
    double actualOffsetError = -(actualOffset - audio.playback.targetLatencyFrames);

    double error = actualOffsetError - offsetError;
    spiceData->offsetError += spiceData->b * error +
      spiceData->offsetErrorIntegral;
    spiceData->offsetErrorIntegral += spiceData->c * error;
  }

  // Resample the audio to adjust the playback speed. Use a PI controller to
  // adjust the resampling ratio based upon the measured offset
  double kp = 0.5e-6;
  double ki = 1.0e-16;

  spiceData->ratioIntegral += offsetError * spiceData->periodSec;

  double piOutput = kp * offsetError + ki * spiceData->ratioIntegral;
  double ratio = 1.0 + piOutput;

  int consumed = 0;
  while (consumed < frames)
  {
    SRC_DATA srcData =
    {
      .data_in           = spiceData->framesIn +
        consumed * audio.playback.channels,
      .data_out          = spiceData->framesOut,
      .input_frames      = frames - consumed,
      .output_frames     = spiceData->framesOutSize,
      .input_frames_used = 0,
      .output_frames_gen = 0,
      .end_of_input      = 0,
      .src_ratio         = ratio
    };

    int error = src_process(spiceData->src, &srcData);
    if (error)
    {
      DEBUG_ERROR("Resampling failed: %s", src_strerror(error));
      return;
    }

    ringbuffer_append(audio.playback.buffer, spiceData->framesOut,
      srcData.output_frames_gen);

    consumed += srcData.input_frames_used;
    spiceData->nextPosition += srcData.output_frames_gen;
  }

  if (audio.playback.state == STREAM_STATE_SETUP)
  {
    frames = ringbuffer_getCount(audio.playback.buffer);
    if (frames >= max(devData->periodFrames,
          ringbuffer_getLength(audio.playback.buffer) / 20))
    {
      audio.playback.state = STREAM_STATE_RUN;
      audio.audioDev->playback.start();
    }
  }

  // re-arm the buffer fill check if we have buffered enough
  if (!checkFill && ringbuffer_getCount(audio.playback.buffer) >=
      audio.playback.targetLatencyFrames)
    checkFill = true;
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
