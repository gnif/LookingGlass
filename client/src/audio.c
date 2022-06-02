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

#if ENABLE_AUDIO

#include "audio.h"
#include "main.h"
#include "common/array.h"
#include "common/util.h"
#include "common/ringbuffer.h"

#include "dynamic/audiodev.h"

#include <float.h>
#include <math.h>
#include <samplerate.h>
#include <stdalign.h>
#include <string.h>

typedef enum
{
  STREAM_STATE_STOP,
  STREAM_STATE_SETUP_SPICE,
  STREAM_STATE_SETUP_DEVICE,
  STREAM_STATE_RUN,
  STREAM_STATE_KEEP_ALIVE
}
StreamState;

#define STREAM_ACTIVE(state) \
  (state == STREAM_STATE_RUN || state == STREAM_STATE_KEEP_ALIVE)

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

  int     devPeriodFrames;
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
    int         deviceMaxPeriodFrames;
    int         deviceStartFrames;
    int         targetStartFrames;
    RingBuffer  buffer;
    RingBuffer  deviceTiming;

    RingBuffer  timings;
    GraphHandle graph;

    /* These two structs contain data specifically for use in the device and
     * Spice data threads respectively. Keep them on separate cache lines to
     * avoid false sharing. */
    alignas(64) PlaybackDeviceData deviceData;
    alignas(64) PlaybackSpiceData  spiceData;
  }
  playback;

  struct
  {
    bool          requested;
    bool          started;
    int           volumeChannels;
    uint16_t      volume[8];
    bool          mute;
    int           stride;
    uint32_t      time;
    int           lastChannels;
    int           lastSampleRate;
    PSAudioFormat lastFormat;
    MsgBoxHandle  confirmHandle;
    int           confirmChannels;
    int           confirmSampleRate;
    PSAudioFormat confirmFormat;
  }
  record;
}
AudioState;

static AudioState audio = { 0 };

typedef struct
{
  int     periodFrames;
  int64_t nextTime;
  int64_t nextPosition;
}
PlaybackDeviceTick;

static void playbackStop(void);

void audio_init(void)
{
  // search for the best audiodev to use
  for(int i = 0; i < LG_AUDIODEV_COUNT; ++i)
    if (LG_AudioDevs[i]->init())
    {
      audio.audioDev = LG_AudioDevs[i];
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
  playbackStop();
  audio_recordStop();

  audio.audioDev->free();
  audio.audioDev = NULL;
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

static void playbackStop(void)
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
    if (audio.playback.state == STREAM_STATE_SETUP_DEVICE)
    {
      /* If necessary, slew backwards to play silence until we reach the target
       * startup latency. This avoids underrunning the buffer if the audio
       * device starts earlier than required. */
      int offset = ringbuffer_getCount(audio.playback.buffer) -
        audio.playback.targetStartFrames;
      if (offset < 0)
      {
        data->nextPosition += offset;
        ringbuffer_consume(audio.playback.buffer, NULL, offset);
      }

      audio.playback.state = STREAM_STATE_RUN;
    }

    // Measure the device clock and post to the Spice thread
    if (frames != data->periodFrames)
    {
      double newPeriodSec = (double) frames / audio.playback.sampleRate;

      bool init = data->periodFrames == 0;
      if (init)
        data->nextTime = now + llrint(newPeriodSec * 1.0e9);
      else
        /* Due to the double-buffered nature of audio playback, we are filling
         * in the next buffer while the device is playing the previous buffer.
         * This results in slightly unintuitive behaviour when the period size
         * changes. The device will request enough samples for the new period
         * size, but won't call us again until the previous buffer at the old
         * size has finished playing. So, to avoid a blip in the timing
         * calculations, we must set the estimated next wakeup time based upon
         * the previous period size, not the new one. */
        data->nextTime += llrint(data->periodSec * 1.0e9);

      data->periodFrames  = frames;
      data->periodSec     = newPeriodSec;
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
      .periodFrames = data->periodFrames,
      .nextTime     = data->nextTime,
      .nextPosition = data->nextPosition
    };
    ringbuffer_push(audio.playback.deviceTiming, &tick);

    ringbuffer_consume(audio.playback.buffer, dst, frames);
  }
  else
    frames = 0;

  // Close the stream if nothing has played for a while
  if (audio.playback.state == STREAM_STATE_KEEP_ALIVE)
  {
    int stopTimeSec = 30;
    int stopTimeFrames = stopTimeSec * audio.playback.sampleRate;
    if (ringbuffer_getCount(audio.playback.buffer) <= -stopTimeFrames)
      playbackStop();
  }

  return frames;
}

void audio_playbackStart(int channels, int sampleRate, PSAudioFormat format,
  uint32_t time)
{
  if (!audio.audioDev)
    return;

  static int lastChannels   = 0;
  static int lastSampleRate = 0;

  if (audio.playback.state == STREAM_STATE_KEEP_ALIVE &&
    channels == lastChannels && sampleRate == lastSampleRate)
    return;
  if (audio.playback.state != STREAM_STATE_STOP)
    playbackStop();

  int srcError;
  audio.playback.spiceData.src = src_new(SRC_SINC_FASTEST, channels, &srcError);
  if (!audio.playback.spiceData.src)
  {
    DEBUG_ERROR("Failed to create resampler: %s", src_strerror(srcError));
    return;
  }

  const int bufferFrames = sampleRate;
  audio.playback.buffer = ringbuffer_newUnbounded(bufferFrames,
      channels * sizeof(float));

  audio.playback.deviceTiming = ringbuffer_new(16, sizeof(PlaybackDeviceTick));

  lastChannels   = channels;
  lastSampleRate = sampleRate;

  audio.playback.channels   = channels;
  audio.playback.sampleRate = sampleRate;
  audio.playback.stride     = channels * sizeof(float);
  audio.playback.state      = STREAM_STATE_SETUP_SPICE;

  audio.playback.deviceData.periodFrames       = 0;
  audio.playback.deviceData.nextPosition       = 0;

  audio.playback.spiceData.periodFrames        = 0;
  audio.playback.spiceData.nextPosition        = 0;
  audio.playback.spiceData.devPeriodFrames     = 0;
  audio.playback.spiceData.devLastTime         = INT64_MIN;
  audio.playback.spiceData.devNextTime         = INT64_MIN;
  audio.playback.spiceData.offsetError         = 0.0;
  audio.playback.spiceData.offsetErrorIntegral = 0.0;
  audio.playback.spiceData.ratioIntegral       = 0.0;

  int requestedPeriodFrames = max(g_params.audioPeriodSize, 1);
  audio.playback.deviceMaxPeriodFrames = 0;
  audio.playback.deviceStartFrames     = 0;
  audio.audioDev->playback.setup(channels, sampleRate, requestedPeriodFrames,
    &audio.playback.deviceMaxPeriodFrames, &audio.playback.deviceStartFrames,
    playbackPullFrames);
  DEBUG_ASSERT(audio.playback.deviceMaxPeriodFrames > 0);

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
      audio.playback.timings, 0.0f, 200.0f, audioGraphFormatFn);
}

void audio_playbackStop(void)
{
  if (!audio.audioDev)
    return;

  switch (audio.playback.state)
  {
    case STREAM_STATE_RUN:
    {
      // Keep the audio device open for a while to reduce startup latency if
      // playback starts again
      audio.playback.state = STREAM_STATE_KEEP_ALIVE;

      // Reset the resampler so it is safe to use for the next playback
      int error = src_reset(audio.playback.spiceData.src);
      if (error)
      {
        DEBUG_ERROR("Failed to reset resampler: %s", src_strerror(error));
        playbackStop();
      }

      break;
    }

    case STREAM_STATE_SETUP_SPICE:
    case STREAM_STATE_SETUP_DEVICE:
      // Playback hasn't actually started yet so just clean up
      playbackStop();
      break;

    case STREAM_STATE_KEEP_ALIVE:
    case STREAM_STATE_STOP:
      // Nothing to do
      break;
  }
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

static double computeDevicePosition(int64_t curTime)
{
  // Interpolate to calculate the current device position
  PlaybackSpiceData * spiceData = &audio.playback.spiceData;
  return spiceData->devLastPosition +
    (spiceData->devNextPosition - spiceData->devLastPosition) *
      ((double) (curTime - spiceData->devLastTime) /
        (spiceData->devNextTime - spiceData->devLastTime));
}

void audio_playbackData(uint8_t * data, size_t size)
{
  if (audio.playback.state == STREAM_STATE_STOP || !audio.audioDev || size == 0)
    return;

  PlaybackSpiceData * spiceData = &audio.playback.spiceData;
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
    if (!spiceData->framesIn)
    {
      DEBUG_ERROR("Failed to malloc framesIn");
      playbackStop();
      return;
    }

    spiceData->framesOutSize = round(frames * 1.1);
    spiceData->framesOut     =
      malloc(spiceData->framesOutSize * audio.playback.stride);
    if (!spiceData->framesOut)
    {
      DEBUG_ERROR("Failed to malloc framesOut");
      playbackStop();
      return;
    }
  }

  src_short_to_float_array((int16_t *) data, spiceData->framesIn,
    frames * audio.playback.channels);

  // Receive timing information from the audio device thread
  PlaybackDeviceTick deviceTick;
  while (ringbuffer_consume(audio.playback.deviceTiming, &deviceTick, 1))
  {
    spiceData->devPeriodFrames = deviceTick.periodFrames;
    spiceData->devLastTime     = spiceData->devNextTime;
    spiceData->devLastPosition = spiceData->devNextPosition;
    spiceData->devNextTime     = deviceTick.nextTime;
    spiceData->devNextPosition = deviceTick.nextPosition;
  }

  /* Determine the target latency. This is made up of the maximum audio device
   * period (plus a little extra to absorb timing jitter) and a configurable
   * additional buffer period. The default is set high enough to absorb typical
   * timing jitter from qemu. */
  int configLatencyMs = max(g_params.audioBufferLatency, 0);
  double targetLatencyFrames =
    audio.playback.deviceMaxPeriodFrames * 1.1 +
    configLatencyMs * audio.playback.sampleRate / 1000.0;

  /* If the device is currently at a lower period size than its maximum (which
   * can happen, for example, if another application has requested a lower
   * latency) then we need to take that into account in our target latency.
   *
   * The reason to do this is not necessarily obvious, since we already set the
   * target latency based upon the maximum period size. The problem stems from
   * the way the device changes the period size. When the period size is
   * reduced, there will be a transitional period where `playbackPullFrames` is
   * invoked with the new smaller period size, but the time until the next
   * invocation is based upon the previous size. This happens because the device
   * is preparing the next small buffer while still playing back the previous
   * large buffer. The result of this is that we end up with a surplus of data
   * in the ring buffer. The overall latency is unchanged, but the balance has
   * shifted: there is more data in our ring buffer and less in the device
   * buffer.
   *
   * Unaccounted for, this would be detected as an offset error and playback
   * would be sped up to bring things back in line. In isolation, this is not
   * inherently problematic, and may even be desirable because it would reduce
   * the overall latency. The real problem occurs when the period size goes back
   * up.
   *
   * When the period size increases, the exact opposite happens. The device will
   * suddenly request data at the new period size, but the timing interval will
   * be based upon the previous period size during the transition. If there is
   * not enough data to satisfy this then playback will start severely
   * underrunning until the timing loop can correct for the error.
   *
   * To counteract this issue, if the current period size is smaller than the
   * maximum period size then we increase the target latency by the difference.
   * This keeps the offset error stable and ensures we have enough data in the
   * buffer to absorb rate increases. */
  if (spiceData->devPeriodFrames != 0 &&
    spiceData->devPeriodFrames < audio.playback.deviceMaxPeriodFrames)
    targetLatencyFrames +=
      audio.playback.deviceMaxPeriodFrames - spiceData->devPeriodFrames;

  // Measure the Spice audio clock
  int64_t curTime;
  int64_t curPosition;
  double devPosition = DBL_MIN;
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
    if (fabs(error) >= 0.2 || audio.playback.state == STREAM_STATE_KEEP_ALIVE)
    {
      /* Clock error is too high or we are starting a new playback; slew the
       * write pointer and reset the timing parameters to get back in sync. If
       * we know the device playback position then we can slew directly to the
       * target latency, otherwise just slew based upon the error amount */
      int slewFrames;
      if (spiceData->devLastTime != INT64_MIN)
      {
        devPosition = computeDevicePosition(now);
        double targetPosition = devPosition + targetLatencyFrames;

        // If starting a new playback we need to allow a little extra time for
        // the resampler startup latency
        if (audio.playback.state == STREAM_STATE_KEEP_ALIVE)
        {
          int resamplerLatencyFrames = 20;
          targetPosition += resamplerLatencyFrames;
        }

        slewFrames = round(targetPosition - spiceData->nextPosition);
      }
      else
        slewFrames = round(error * audio.playback.sampleRate);

      ringbuffer_append(audio.playback.buffer, NULL, slewFrames);

      curTime     = now;
      curPosition = spiceData->nextPosition + slewFrames;

      spiceData->periodSec    = (double) frames / audio.playback.sampleRate;
      spiceData->nextTime     = now + llrint(spiceData->periodSec * 1.0e9);
      spiceData->nextPosition = curPosition;

      spiceData->offsetError         = 0.0;
      spiceData->offsetErrorIntegral = 0.0;
      spiceData->ratioIntegral       = 0.0;

      audio.playback.state = STREAM_STATE_RUN;
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

  /* Measure the offset between the Spice position and the device position,
   * and how far away this is from the target latency. We use this to adjust
   * the playback speed to bring them back in line. This value can change
   * quite rapidly, particularly at the start of playback, so filter it to
   * avoid sudden pitch shifts which will be noticeable to the user. */
  double actualOffset = 0.0;
  double offsetError = spiceData->offsetError;
  if (spiceData->devLastTime != INT64_MIN)
  {
    if (devPosition == DBL_MIN)
      devPosition = computeDevicePosition(curTime);

    actualOffset = curPosition - devPosition;
    double actualOffsetError = -(actualOffset - targetLatencyFrames);

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

  if (audio.playback.state == STREAM_STATE_SETUP_SPICE)
  {
    /* Latency corrections at startup can be quite significant due to poor
     * packet pacing from Spice, so require at least two full Spice periods'
     * worth of data in addition to the startup delay requested by the device
     * before starting playback to minimise the chances of underrunning. */
    int startFrames =
      spiceData->periodFrames * 2 + audio.playback.deviceStartFrames;
    audio.playback.targetStartFrames = startFrames;

    /* The actual time between opening the device and the device starting to
     * pull data can range anywhere between nearly instant and hundreds of
     * milliseconds. To minimise startup latency, we open the device
     * immediately. If the device starts earlier than required (as per the
     * `startFrames` value we just calculated), then a period of silence will be
     * inserted at the beginning of playback to avoid underrunning. If it starts
     * later, then we just accept the higher latency and let the adaptive
     * resampling deal with it. */
    audio.playback.state = STREAM_STATE_SETUP_DEVICE;
    audio.audioDev->playback.start();
  }

  double latencyFrames = actualOffset;
  if (audio.audioDev->playback.latency)
    latencyFrames += audio.audioDev->playback.latency();

  const float latency = latencyFrames * 1000.0 / audio.playback.sampleRate;
  ringbuffer_push(audio.playback.timings, &latency);
  app_invalidateGraph(audio.playback.graph);
}

bool audio_supportsRecord(void)
{
  return audio.audioDev && audio.audioDev->record.start;
}

static void recordPushFrames(uint8_t * data, int frames)
{
  purespice_writeAudio(data, frames * audio.record.stride, 0);
}

static void realRecordStart(int channels, int sampleRate, PSAudioFormat format)
{
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

  if (g_params.micShowIndicator)
    app_showRecord(true);
}

struct AudioFormat
{
   int channels;
   int sampleRate;
   PSAudioFormat format;
};

static void recordConfirm(bool yes, void * opaque)
{
  if (yes)
  {
    DEBUG_INFO("Microphone access granted");
    realRecordStart(
      audio.record.confirmChannels,
      audio.record.confirmSampleRate,
      audio.record.confirmFormat
    );
  }
  else
    DEBUG_INFO("Microphone access denied");

  audio.record.confirmHandle = NULL;
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

  audio.record.requested      = true;
  audio.record.lastChannels   = channels;
  audio.record.lastSampleRate = sampleRate;
  audio.record.lastFormat     = format;

  if (audio.record.started)
    realRecordStart(channels, sampleRate, format);
  else if (g_state.micDefaultState == MIC_DEFAULT_DENY)
    DEBUG_INFO("Microphone access denied by default");
  else if (g_state.micDefaultState == MIC_DEFAULT_ALLOW)
  {
    DEBUG_INFO("Microphone access granted by default");
    realRecordStart(channels, sampleRate, format);
  }
  else
  {
    if (audio.record.confirmHandle)
      app_msgBoxClose(audio.record.confirmHandle);

    audio.record.confirmChannels   = channels;
    audio.record.confirmSampleRate = sampleRate;
    audio.record.confirmFormat     = format;
    audio.record.confirmHandle     = app_confirmMsgBox(
      "Microphone", recordConfirm, NULL,
      "An application just opened the microphone!\n"
      "Do you want it to access your microphone?");
  }
}

static void realRecordStop(void)
{
  audio.audioDev->record.stop();
  audio.record.started = false;

  if (g_params.micShowIndicator)
    app_showRecord(false);
}

void audio_recordStop(void)
{
  audio.record.requested = false;
  if (!audio.audioDev || !audio.record.started)
    return;

  DEBUG_INFO("Microphone recording stopped");
  realRecordStop();
}

void audio_recordToggleKeybind(int sc, void * opaque)
{
  if (!audio.audioDev)
    return;

  if (!audio.record.requested)
  {
    app_alert(LG_ALERT_WARNING,
      "No application is requesting microphone access.");
    return;
  }

  if (audio.record.started)
  {
    app_alert(LG_ALERT_INFO, "Microphone disabled");
    DEBUG_INFO("Microphone recording stopped by user");
    realRecordStop();
  }
  else
  {
    app_alert(LG_ALERT_INFO, "Microphone enabled");
    DEBUG_INFO("Microphone recording started by user");
    realRecordStart(audio.record.lastChannels, audio.record.lastSampleRate,
      audio.record.lastFormat);
  }
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

#endif
