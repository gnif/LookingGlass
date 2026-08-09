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

#if ENABLE_AUDIO

#include "audio.h"
#include "main.h"
#include "common/array.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/util.h"
#include "common/ringbuffer.h"

#include "dynamic/audiodev.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <samplerate.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <string.h>

#define PLAYBACK_CLOCK_BANDWIDTH_HZ 0.05
#define PLAYBACK_ACQUIRE_PHASE_BANDWIDTH_HZ 0.05
#define PLAYBACK_PHASE_BANDWIDTH_HZ 0.005
#define PLAYBACK_OFFSET_FILTER_BANDWIDTH_HZ \
  (20.0 * PLAYBACK_PHASE_BANDWIDTH_HZ)
#define PLAYBACK_PHASE_DEADBAND_SEC 0.0005
#define PLAYBACK_MAX_RATE_CORRECTION 0.005
#define PLAYBACK_MAX_RATE_SLEW_PER_SEC 0.005
#define PLAYBACK_MAX_JITTER_SEC 0.1
#define PLAYBACK_PHASE_BASELINE_TIME_SEC 5.0
#define PLAYBACK_PHASE_RESERVE_DECAY_SEC 60.0
/* libsamplerate does not expose its buffered-frame delay. SRC_SINC_FASTEST
 * retains 20 frames at unity; the bounded ratio range changes this by less
 * than one frame. Keep it in the latency model, but not in the safety buffer. */
#define PLAYBACK_RESAMPLER_DELAY_FRAMES 20
#define PLAYBACK_TIMESTAMP_DISCONTINUITY_NS INT64_C(2000000000)
#define PLAYBACK_RATE_WINDOW_MS 60000
#define PLAYBACK_RATE_MIN_SPAN_MS 45000
#define PLAYBACK_RATE_SAMPLE_INTERVAL_MS 100
#define PLAYBACK_RATE_FILTER_TIME_SEC 60.0
#define PLAYBACK_RATE_MAX_SAMPLES 1024
#define PLAYBACK_DEVICE_RATE_CHECK_SEC 1.0
#define PLAYBACK_DEVICE_RATE_STABLE_SEC 2.0
#define PLAYBACK_DEVICE_RATE_STABLE_DELTA_PPM 50.0
#define PLAYBACK_DEVICE_RATE_MAX_ACQUIRE_SEC 20.0

typedef enum
{
  STREAM_STATE_STOP,
  STREAM_STATE_SETUP_SOURCE,
  STREAM_STATE_SETUP_DEVICE,
  STREAM_STATE_RUN,
  STREAM_STATE_KEEP_ALIVE,
  STREAM_STATE_RESUMING,
  STREAM_STATE_STOP_PENDING
}
StreamState;

#define STREAM_ACTIVE(state) \
  (state == STREAM_STATE_RUN        || \
   state == STREAM_STATE_KEEP_ALIVE || \
   state == STREAM_STATE_RESUMING)

#define PLAYBACK_CALLBACK_DISABLED (UINT32_C(1) << 31)
#define PLAYBACK_CALLBACK_COUNT_MASK (PLAYBACK_CALLBACK_DISABLED - 1)

typedef struct
{
  int64_t nextPosition;
  double  outputPosition;
  double  appliedRatio;
  int     startupSilenceFrames;
}
PlaybackDeviceData;

typedef struct
{
  bool    valid;
  unsigned int updates;
  int64_t time;
  double  position;
  double  frameSec;
  double  phaseResidualSec;
}
PlaybackClock;

typedef struct
{
  int64_t timeMs;
  int64_t position;
}
PlaybackRateSample;

typedef struct
{
  float * framesIn;
  float * framesOut;
  int     framesInSize;
  int     framesOutSize;

  int64_t inputPosition;
  int64_t outputPosition;

  int64_t  mediaTime;
  int64_t  mediaElapsed;
  int64_t  mediaTimeMs;
  int64_t  mediaLocalOrigin;
  uint64_t mediaPosition;
  int64_t  lastPacketTime;
  int64_t  lastArrivalTime;
  double   arrivalJitterSec;
  double   sourcePhaseBaselineSec;
  double   sourcePhaseReserveSec;
  double   sourcePacketDurationSec;
  bool     sourcePhaseBaselineValid;
  bool     mediaClockValid;
  bool     mediaPositionValid;
  bool     mediaClockFromSource;

  PlaybackRateSample rateSamples[PLAYBACK_RATE_MAX_SAMPLES];
  unsigned int rateSampleStart;
  unsigned int rateSampleCount;
  int64_t      rateLastSampleTimeMs;
  int64_t      rateFilterTimeMs;
  double       sourceRateFrameSec;
  bool         sourceRateValid;
  bool         bufferOverrunPending;

  int     devPeriodFrames;
  int64_t devReadPosition;
  unsigned int deviceTimingSequence;
  int64_t deviceClockAcquireStart;
  int64_t deviceClockCheckTime;
  double  deviceClockCheckFrameSec;
  double  deviceClockStableSec;
  double  devicePositionOffsetFrames;
  bool    deviceClockStable;

  double  offsetError;
  double  offsetErrorIntegral;
  double  ratioIntegral;
  double  lastRatio;
  double  lastClockRatio;
  int64_t nextLogTime;
  unsigned int bufferOverruns;

  PlaybackClock sourceClock;
  PlaybackClock deviceClock;
  PlaybackClock outputClock;
  SRC_STATE * src;
}
PlaybackSourceData;

typedef struct
{
  atomic_uint       sequence;
  atomic_int        periodFrames;
  _Atomic(int64_t)  time;
  _Atomic(int64_t)  position;
  _Atomic(double)   outputPosition;
}
PlaybackDeviceTiming;

typedef struct
{
  const LG_AudioOps * ops;
  void              * opaque;
  bool                available;
  uint32_t            generation;
}
AudioBinding;

typedef struct
{
  struct LG_AudioDevOps * audioDev;

  LG_Lock   providerLock;
  LG_RWLock activeLock;
  AudioBinding fallback;
  AudioBinding transport;
  AudioBinding active;

  struct
  {
    LG_Lock              sourceLock;
    _Atomic(StreamState) state;
    atomic_uint          callbackState;
    atomic_uint          streamGeneration;
    int         volumeChannels;
    uint16_t    volume[LG_AUDIO_MAX_CHANNELS];
    bool        mute;
    LG_AudioFormat format;
    LG_AudioFormat lastFormat;
    bool        lastFormatValid;
    int         channels;
    int         sampleRate;
    int         stride;
    int         deviceMaxPeriodFrames;
    int         deviceStartFrames;
    int         targetStartFrames;
    int         startupLowWaterFrames;
    int64_t     startupPacketDeadline;
    int64_t     startupPacketPeriod;
    bool        backendResampler;
    _Atomic(double) backendResampleRatio;
    atomic_bool     backendResamplerFailed;
    RingBuffer  buffer;
    PlaybackDeviceTiming deviceTiming;
    atomic_uint underruns;

    RingBuffer  timings;
    GraphHandle graph;

    /* These two structs contain data specifically for use in the device and
     * source data threads respectively. Keep them on separate cache lines to
     * avoid false sharing. */
    alignas(64) PlaybackDeviceData deviceData;
    alignas(64) PlaybackSourceData  sourceData;
  }
  playback;

  struct
  {
    LG_Lock       lock;
    atomic_uint    streamGeneration;
    bool          shuttingDown;
    bool          requested;
    bool          started;
    int           volumeChannels;
    uint16_t      volume[LG_AUDIO_MAX_CHANNELS];
    bool          mute;
    LG_AudioFormat format;
    LG_AudioFormat lastFormat;
    MsgBoxHandle  confirmHandle;
    uint64_t      confirmGeneration;
    bool          confirmPending;
    LG_AudioFormat confirmFormat;
  }
  record;

  struct
  {
    LG_Lock    lock;
    LGEvent  * event;
    LGThread * thread;
    atomic_bool stop;
    bool        pending;

    const LG_AudioOps * ops;
    void              * opaque;
    uint32_t            bindingGeneration;
    uint32_t            generation;
    LG_AudioClock       clock;
  }
  feedback;
}
AudioState;

static AudioState audio = { 0 };

static size_t audioSampleSize(LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8:     return 1;
    case LG_AUDIO_FMT_S16_LE: return 2;
    case LG_AUDIO_FMT_S24_LE: return 3;
    case LG_AUDIO_FMT_S32_LE:
    case LG_AUDIO_FMT_F32_LE: return 4;
    case LG_AUDIO_FMT_F64_LE: return 8;
  }

  return 0;
}

static bool audioFormatValid(const LG_AudioFormat * format)
{
  if (!format || format->channelCount < 1 ||
      format->channelCount > LG_AUDIO_MAX_CHANNELS ||
      format->sampleRate < 8000 || format->sampleRate > 384000 ||
      audioSampleSize(format->sampleFormat) == 0)
    return false;

  for (unsigned int i = 0; i < format->channelCount; ++i)
    if (format->channels[i] > LG_AUDIO_CH_TOP_REAR_RIGHT)
      return false;

  return true;
}

static bool audioFormatEqual(const LG_AudioFormat * a,
    const LG_AudioFormat * b)
{
  return a->sampleFormat == b->sampleFormat &&
    a->sampleRate == b->sampleRate &&
    a->channelCount == b->channelCount &&
    memcmp(a->channels, b->channels,
      sizeof(*a->channels) * a->channelCount) == 0;
}

static bool audioConvertToFloat(float * dst, const void * src,
    size_t samples, LG_AudioSampleFormat format)
{
  if (!dst || !src)
    return false;

  switch (format)
  {
    case LG_AUDIO_FMT_U8:
    {
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i)
        dst[i] = ((int)in[i] - 128) / 128.0f;
      return true;
    }

    case LG_AUDIO_FMT_S16_LE:
    {
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i, in += 2)
      {
        const int16_t value = (int16_t)(
          (uint16_t)in[0] | (uint16_t)in[1] << 8);
        dst[i] = value / 32768.0f;
      }
      return true;
    }

    case LG_AUDIO_FMT_S24_LE:
    {
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i, in += 3)
      {
        int32_t value =
          (int32_t)((uint32_t)in[0] |
            (uint32_t)in[1] << 8 |
            (uint32_t)in[2] << 16);
        if (value & 0x800000)
          value |= (int32_t)0xff000000;
        dst[i] = value / 8388608.0f;
      }
      return true;
    }

    case LG_AUDIO_FMT_S32_LE:
    {
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i, in += 4)
      {
        const int32_t value = (int32_t)(
          (uint32_t)in[0] |
          (uint32_t)in[1] << 8 |
          (uint32_t)in[2] << 16 |
          (uint32_t)in[3] << 24);
        dst[i] = value / 2147483648.0f;
      }
      return true;
    }

    case LG_AUDIO_FMT_F32_LE:
    {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
      memcpy(dst, src, samples * sizeof(*dst));
#else
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i, in += 4)
      {
        const uint32_t bits =
          (uint32_t)in[0] |
          (uint32_t)in[1] << 8 |
          (uint32_t)in[2] << 16 |
          (uint32_t)in[3] << 24;
        memcpy(&dst[i], &bits, sizeof(bits));
      }
#endif
      return true;
    }

    case LG_AUDIO_FMT_F64_LE:
    {
      const uint8_t * in = src;
      for (size_t i = 0; i < samples; ++i, in += 8)
      {
        const uint64_t bits =
          (uint64_t)in[0] |
          (uint64_t)in[1] << 8 |
          (uint64_t)in[2] << 16 |
          (uint64_t)in[3] << 24 |
          (uint64_t)in[4] << 32 |
          (uint64_t)in[5] << 40 |
          (uint64_t)in[6] << 48 |
          (uint64_t)in[7] << 56;
        double value;
        memcpy(&value, &bits, sizeof(value));
        dst[i] = value;
      }
      return true;
    }
  }

  return false;
}

typedef struct
{
  int     periodFrames;
  int64_t nextTime;
  int64_t nextPosition;
  double  outputPosition;
}
PlaybackDeviceTick;

static void playbackClockReset(PlaybackClock * clock, int64_t time,
    double position, double frameSec)
{
  clock->valid    = true;
  clock->updates  = 1;
  clock->time     = time;
  clock->position = position;
  clock->frameSec = frameSec;
  clock->phaseResidualSec = 0.0;
}

static bool playbackClockUpdate(PlaybackClock * clock, int64_t time,
    double position, double nominalFrameSec)
{
  if (!clock->valid)
  {
    playbackClockReset(clock, time, position, nominalFrameSec);
    return true;
  }

  const double frames = position - clock->position;
  if (frames <= 0)
    return frames == 0;

  const double predicted =
    clock->time + frames * clock->frameSec * 1.0e9;
  const double error = (time - predicted) * 1.0e-9;
  if (fabs(error) >= 0.2)
  {
    playbackClockReset(clock, time, position, nominalFrameSec);
    return false;
  }
  clock->phaseResidualSec = error;

  const double periodSec = frames * clock->frameSec;
  const double omega =
    2.0 * M_PI * PLAYBACK_CLOCK_BANDWIDTH_HZ * periodSec;
  const double b = M_SQRT2 * omega;
  const double c = omega * omega;

  clock->time = llrint(predicted + b * error * 1.0e9);
  clock->position = position;
  clock->frameSec += c * error / frames;
  clock->frameSec = clamp(clock->frameSec,
      nominalFrameSec * (1.0 - PLAYBACK_MAX_RATE_CORRECTION),
      nominalFrameSec * (1.0 + PLAYBACK_MAX_RATE_CORRECTION));
  ++clock->updates;
  return true;
}

static bool playbackSourceClockUpdate(PlaybackClock * clock, int64_t time,
    double position, double nominalFrameSec)
{
  if (!clock->valid)
  {
    playbackClockReset(clock, time, position, nominalFrameSec);
    return true;
  }

  const double frames = position - clock->position;
  if (frames <= 0)
    return frames == 0;

  const double predicted =
    clock->time + frames * clock->frameSec * 1.0e9;
  const double residual = (time - predicted) * 1.0e-9;
  if (fabs(residual) >= 0.2)
  {
    playbackClockReset(clock, time, position, nominalFrameSec);
    return false;
  }

  /* source media time periodically changes phase by several
   * milliseconds. Preserve it as a diagnostic and discontinuity signal, but
   * advance the source clock solely from decoded sample position. Short-term
   * timestamp corrections must not move the playback buffer. */
  clock->time = llrint(predicted);
  clock->position = position;
  clock->phaseResidualSec = residual;
  ++clock->updates;
  return true;
}

static void playbackDeviceClockAcquireReset(PlaybackSourceData * sourceData)
{
  sourceData->deviceClockAcquireStart  = INT64_MIN;
  sourceData->deviceClockCheckTime     = INT64_MIN;
  sourceData->deviceClockStableSec     = 0.0;
  sourceData->devicePositionOffsetFrames = 0.0;
  sourceData->deviceClockStable        = false;
  sourceData->ratioIntegral            = 0.0;
  sourceData->lastClockRatio           = 1.0;
}

static bool playbackDeviceClockAcquire(
    PlaybackSourceData * sourceData, int64_t time)
{
  if (sourceData->deviceClockStable)
    return false;

  if (sourceData->deviceClockAcquireStart == INT64_MIN)
  {
    sourceData->deviceClockAcquireStart = time;
    sourceData->deviceClockCheckTime = time;
    sourceData->deviceClockCheckFrameSec =
      sourceData->deviceClock.frameSec;
    return false;
  }

  const double checkSec =
    (time - sourceData->deviceClockCheckTime) * 1.0e-9;
  if (checkSec < PLAYBACK_DEVICE_RATE_CHECK_SEC)
    return false;

  const double rateDeltaPpm = fabs(
      sourceData->deviceClock.frameSec /
        sourceData->deviceClockCheckFrameSec - 1.0) * 1.0e6;
  if (rateDeltaPpm <= PLAYBACK_DEVICE_RATE_STABLE_DELTA_PPM)
    sourceData->deviceClockStableSec += checkSec;
  else
    sourceData->deviceClockStableSec = 0.0;

  sourceData->deviceClockCheckTime = time;
  sourceData->deviceClockCheckFrameSec =
    sourceData->deviceClock.frameSec;

  const double acquireSec =
    (time - sourceData->deviceClockAcquireStart) * 1.0e-9;
  if (sourceData->deviceClockStableSec <
        PLAYBACK_DEVICE_RATE_STABLE_SEC &&
      acquireSec < PLAYBACK_DEVICE_RATE_MAX_ACQUIRE_SEC)
    return false;

  sourceData->deviceClockStable = true;
  return true;
}

static double playbackClockPosition(const PlaybackClock * clock, int64_t time)
{
  return clock->position +
    (time - clock->time) * 1.0e-9 / clock->frameSec;
}

static void playbackSourceRateReset(PlaybackSourceData * sourceData)
{
  sourceData->rateSampleStart      = 0;
  sourceData->rateSampleCount      = 0;
  sourceData->rateLastSampleTimeMs = INT64_MIN;
  sourceData->rateFilterTimeMs     = INT64_MIN;
  sourceData->sourceRateValid      = false;
}

static void playbackSourceRateAdd(
    PlaybackSourceData * sourceData, double nominalFrameSec)
{
  const int64_t timeMs = sourceData->mediaTimeMs;
  if (sourceData->rateLastSampleTimeMs != INT64_MIN &&
      timeMs - sourceData->rateLastSampleTimeMs <
        PLAYBACK_RATE_SAMPLE_INTERVAL_MS)
    return;

  sourceData->rateLastSampleTimeMs = timeMs;

  while (sourceData->rateSampleCount > 0)
  {
    const PlaybackRateSample * oldest =
      &sourceData->rateSamples[sourceData->rateSampleStart];
    if (timeMs - oldest->timeMs <= PLAYBACK_RATE_WINDOW_MS)
      break;

    sourceData->rateSampleStart =
      (sourceData->rateSampleStart + 1) % PLAYBACK_RATE_MAX_SAMPLES;
    --sourceData->rateSampleCount;
  }

  if (sourceData->rateSampleCount == PLAYBACK_RATE_MAX_SAMPLES)
  {
    sourceData->rateSampleStart =
      (sourceData->rateSampleStart + 1) % PLAYBACK_RATE_MAX_SAMPLES;
    --sourceData->rateSampleCount;
  }

  const unsigned int index =
    (sourceData->rateSampleStart + sourceData->rateSampleCount) %
      PLAYBACK_RATE_MAX_SAMPLES;
  sourceData->rateSamples[index] = (PlaybackRateSample)
  {
    .timeMs   = timeMs,
    .position = sourceData->inputPosition
  };
  ++sourceData->rateSampleCount;

  const PlaybackRateSample * first =
    &sourceData->rateSamples[sourceData->rateSampleStart];
  if (sourceData->rateSampleCount < 2 ||
      timeMs - first->timeMs < PLAYBACK_RATE_MIN_SPAN_MS)
    return;

  double sumPosition = 0.0;
  double sumTime     = 0.0;
  double sumPosition2 = 0.0;
  double sumPositionTime = 0.0;
  for (unsigned int i = 0; i < sourceData->rateSampleCount; ++i)
  {
    const PlaybackRateSample * sample =
      &sourceData->rateSamples[
        (sourceData->rateSampleStart + i) % PLAYBACK_RATE_MAX_SAMPLES];
    const double position = sample->position - first->position;
    const double timeSec = (sample->timeMs - first->timeMs) / 1000.0;

    sumPosition     += position;
    sumTime         += timeSec;
    sumPosition2    += position * position;
    sumPositionTime += position * timeSec;
  }

  const double count = sourceData->rateSampleCount;
  const double denominator =
    sumPosition2 - sumPosition * sumPosition / count;
  if (denominator <= 0.0)
    return;

  const double frameSec =
    (sumPositionTime - sumPosition * sumTime / count) / denominator;
  if (frameSec < nominalFrameSec *
        (1.0 - PLAYBACK_MAX_RATE_CORRECTION) ||
      frameSec > nominalFrameSec *
        (1.0 + PLAYBACK_MAX_RATE_CORRECTION))
    return;

  if (!sourceData->sourceRateValid)
  {
    sourceData->sourceRateFrameSec = frameSec;
    sourceData->sourceRateValid = true;
  }
  else
  {
    const double elapsedSec =
      (timeMs - sourceData->rateFilterTimeMs) / 1000.0;
    const double alpha =
      -expm1(-elapsedSec / PLAYBACK_RATE_FILTER_TIME_SEC);
    sourceData->sourceRateFrameSec +=
      alpha * (frameSec - sourceData->sourceRateFrameSec);
  }
  sourceData->rateFilterTimeMs = timeMs;
}

static void playbackPublishDeviceTiming(
    int periodFrames, int64_t time, int64_t position,
    double outputPosition)
{
  PlaybackDeviceTiming * timing = &audio.playback.deviceTiming;
  atomic_fetch_add_explicit(&timing->sequence, 1, memory_order_relaxed);
  atomic_store_explicit(
      &timing->periodFrames, periodFrames, memory_order_relaxed);
  atomic_store_explicit(&timing->time, time, memory_order_relaxed);
  atomic_store_explicit(&timing->position, position, memory_order_relaxed);
  atomic_store_explicit(
      &timing->outputPosition, outputPosition, memory_order_relaxed);
  atomic_fetch_add_explicit(&timing->sequence, 1, memory_order_release);
}

static bool playbackReadDeviceTiming(
    unsigned int previousSequence, PlaybackDeviceTick * tick,
    unsigned int * sequence)
{
  PlaybackDeviceTiming * timing = &audio.playback.deviceTiming;
  unsigned int before;
  unsigned int after;

  do
  {
    before = atomic_load_explicit(&timing->sequence, memory_order_acquire);
    if ((before & 1) || before == previousSequence)
      return false;

    tick->periodFrames =
      atomic_load_explicit(&timing->periodFrames, memory_order_relaxed);
    tick->nextTime =
      atomic_load_explicit(&timing->time, memory_order_relaxed);
    tick->nextPosition =
      atomic_load_explicit(&timing->position, memory_order_relaxed);
    tick->outputPosition =
      atomic_load_explicit(
          &timing->outputPosition, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    after = atomic_load_explicit(&timing->sequence, memory_order_relaxed);
  }
  while (before != after);

  *sequence = after;
  return true;
}

static void playbackResetMediaClock(PlaybackSourceData * sourceData,
    int64_t time, uint64_t position, bool fromSource, int64_t now)
{
  sourceData->mediaTime        = time;
  sourceData->mediaElapsed     = 0;
  sourceData->mediaTimeMs      = 0;
  sourceData->mediaLocalOrigin = now;
  sourceData->mediaPosition    = position;
  sourceData->lastPacketTime   = INT64_MIN;
  sourceData->lastArrivalTime  = INT64_MIN;
  sourceData->mediaClockValid  = true;
  sourceData->mediaPositionValid = true;
  sourceData->mediaClockFromSource = fromSource;
  sourceData->sourceClock.valid = false;
  playbackSourceRateReset(sourceData);
}

static void playbackPrepareMediaClock(
    PlaybackSourceData * sourceData, const LG_AudioClock * sourceClock)
{
  sourceData->mediaTime          = 0;
  sourceData->mediaClockValid    = false;
  sourceData->mediaPositionValid = sourceClock != NULL;
  sourceData->mediaClockFromSource = sourceClock != NULL;
  sourceData->mediaPosition = sourceClock ? sourceClock->position : 0;
  sourceData->inputPosition = 0;
  sourceData->sourceClock.valid = false;
  playbackSourceRateReset(sourceData);
}

static int64_t playbackMapMediaTime(PlaybackSourceData * sourceData,
    const LG_AudioClock * clock, int frames, int sampleRate,
    int64_t now, bool * discontinuity)
{
  const bool fromSource = clock != NULL;
  const uint64_t position = clock ? clock->position :
    (uint64_t)sourceData->inputPosition;
  const int64_t time = clock ? clock->time :
    llrint(sourceData->inputPosition * (1.0e9 / sampleRate));

  if (clock && clock->discontinuity)
    *discontinuity = true;

  if (!sourceData->mediaClockValid)
  {
    if (sourceData->mediaPositionValid &&
        (sourceData->mediaClockFromSource != fromSource ||
         sourceData->mediaPosition != position))
      *discontinuity = true;

    playbackResetMediaClock(
        sourceData, time, position, fromSource, now);
    sourceData->mediaPosition = position + frames;
    return now;
  }

  const int64_t delta = time - sourceData->mediaTime;
  if (*discontinuity ||
      sourceData->mediaClockFromSource != fromSource ||
      !sourceData->mediaPositionValid ||
      sourceData->mediaPosition != position || delta < 0 ||
      delta > PLAYBACK_TIMESTAMP_DISCONTINUITY_NS)
  {
    playbackResetMediaClock(
        sourceData, time, position, fromSource, now);
    sourceData->mediaPosition = position + frames;
    *discontinuity = true;
    return now;
  }

  sourceData->mediaTime = time;
  sourceData->mediaPosition = position + frames;
  sourceData->mediaElapsed += delta;
  sourceData->mediaTimeMs =
    sourceData->mediaElapsed / INT64_C(1000000);
  return sourceData->mediaLocalOrigin + sourceData->mediaElapsed;
}

static void playbackStop(void);
static MsgBoxHandle recordCancelConfirmLocked(void);
static void realRecordStartLocked(const LG_AudioFormat * format);
static void realRecordStopLocked(void);
static void recordStop(void);

static StreamState playbackGetState(void)
{
  return atomic_load_explicit(
      &audio.playback.state, memory_order_acquire);
}

static void playbackSetState(StreamState state)
{
  atomic_store_explicit(
      &audio.playback.state, state, memory_order_release);
}

static bool playbackCallbackEnter(void)
{
  unsigned int state = atomic_load_explicit(
      &audio.playback.callbackState, memory_order_relaxed);
  for (;;)
  {
    if (state & PLAYBACK_CALLBACK_DISABLED)
      return false;

    DEBUG_ASSERT((state & PLAYBACK_CALLBACK_COUNT_MASK) !=
        PLAYBACK_CALLBACK_COUNT_MASK);
    if (atomic_compare_exchange_weak_explicit(
          &audio.playback.callbackState, &state, state + 1,
          memory_order_acquire, memory_order_relaxed))
      return true;
  }
}

static void playbackCallbackExit(void)
{
  atomic_fetch_sub_explicit(
      &audio.playback.callbackState, 1, memory_order_release);
}

static void playbackDisableCallbacks(void)
{
  atomic_fetch_or_explicit(
      &audio.playback.callbackState, PLAYBACK_CALLBACK_DISABLED,
      memory_order_acq_rel);
}

static void playbackWaitForCallbacks(void)
{
  while((atomic_load_explicit(
          &audio.playback.callbackState, memory_order_acquire) &
        PLAYBACK_CALLBACK_COUNT_MASK) != 0)
    ;
}

bool lgAudio_supportsPlayback(void)
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
  if (playbackGetState() == STREAM_STATE_STOP)
    return;

  playbackDisableCallbacks();
  audio.audioDev->playback.stop();
  playbackWaitForCallbacks();

  playbackSetState(STREAM_STATE_STOP);
  ringbuffer_free(&audio.playback.buffer);
  audio.playback.sourceData.src = src_delete(audio.playback.sourceData.src);

  if (audio.playback.sourceData.framesIn)
  {
    free(audio.playback.sourceData.framesIn);
    free(audio.playback.sourceData.framesOut);
    audio.playback.sourceData.framesIn = NULL;
    audio.playback.sourceData.framesOut = NULL;
    audio.playback.sourceData.framesInSize = 0;
    audio.playback.sourceData.framesOutSize = 0;
  }

  if (audio.playback.timings)
  {
    if (audio.playback.graph)
      app_unregisterGraph(audio.playback.graph);
    audio.playback.graph = NULL;
    ringbuffer_free(&audio.playback.timings);
  }
}

static int playbackPullFrames(uint8_t * dst, int frames)
{
  DEBUG_ASSERT(frames >= 0);
  if (frames == 0)
    return frames;

  if (!playbackCallbackEnter())
    return 0;

  PlaybackDeviceData * data = &audio.playback.deviceData;
  double nextRatio = 1.0;
  if (audio.playback.backendResampler)
  {
    nextRatio = atomic_load_explicit(
        &audio.playback.backendResampleRatio, memory_order_acquire);
    if (!audio.audioDev->playback.setRate(nextRatio))
    {
      atomic_store_explicit(
          &audio.playback.backendResamplerFailed, true,
          memory_order_release);
      nextRatio = data->appliedRatio;
    }
  }

  const int64_t now = nanotime();

  if (audio.playback.buffer)
  {
    if (playbackGetState() == STREAM_STATE_SETUP_DEVICE)
    {
      /* The backend may begin pulling either immediately or long after it was
       * activated. Only retain enough of the source packet to reach its next
       * expected delivery time; retaining the complete packet after part of
       * its interval has already elapsed turns backend startup delay into
       * persistent playback latency. Still cover the backend's immediate
       * startup pull if it is larger than the remaining packet interval. */
      const int64_t packetPeriod =
        max(audio.playback.startupPacketPeriod, INT64_C(1));
      const int64_t remainingNs =
        audio.playback.startupPacketDeadline > now ?
          audio.playback.startupPacketDeadline - now :
          packetPeriod -
            (now - audio.playback.startupPacketDeadline) % packetPeriod;
      const int remainingFrames = clamp(
          (remainingNs * audio.playback.sampleRate + INT64_C(999999999)) /
            INT64_C(1000000000),
          INT64_C(0), (int64_t)INT_MAX);
      const int targetFrames = min(
          (int64_t)audio.playback.startupLowWaterFrames +
            max(audio.playback.deviceStartFrames, remainingFrames),
          (int64_t)ringbuffer_getLength(audio.playback.buffer));

      /* Align in both directions: insert silence if the backend started before
       * the target was available, or discard the oldest queued audio if it
       * started late. */
      const int offset = ringbuffer_getCount(audio.playback.buffer) -
        targetFrames;
      if (offset > 0)
      {
        data->nextPosition += offset;
        ringbuffer_consume(audio.playback.buffer, NULL, offset);
      }
      else if (offset < 0)
      {
        /* Seeking the reader backwards exposes storage from a previous ring
         * wrap. Preserve the logical position but generate the missing startup
         * reserve explicitly as silence. */
        data->nextPosition += offset;
        data->startupSilenceFrames = -offset;
      }

      playbackSetState(STREAM_STATE_RUN);
    }

    /* Timestamp the dequeue boundary before the current pull. The logical
     * position tracks source frames consumed from the ring for latency
     * measurement. With backend resampling, outputPosition separately tracks
     * the equivalent number of device-rate frames. PipeWire computes the
     * current request using the rate set by the previous callback, so apply
     * that same ratio to this period before adopting nextRatio. */
    playbackPublishDeviceTiming(
        frames, now, data->nextPosition, data->outputPosition);
    data->nextPosition += frames;
    data->outputPosition += frames * data->appliedRatio;
    data->appliedRatio = nextRatio;

    const int silenceFrames =
      min(frames, data->startupSilenceFrames);
    if (silenceFrames > 0)
    {
      memset(dst, 0, (size_t)silenceFrames * audio.playback.stride);
      data->startupSilenceFrames -= silenceFrames;
    }

    const int audioFrames = frames - silenceFrames;
    if (g_params.audioDebug &&
        playbackGetState() == STREAM_STATE_RUN &&
        ringbuffer_getCount(audio.playback.buffer) < audioFrames)
      atomic_fetch_add_explicit(
          &audio.playback.underruns, 1, memory_order_relaxed);
    ringbuffer_consume(audio.playback.buffer,
        dst + (size_t)silenceFrames * audio.playback.stride, audioFrames);
  }
  else
    frames = 0;

  // Close the stream if nothing has played for a while
  if (audio.playback.buffer &&
      playbackGetState() == STREAM_STATE_KEEP_ALIVE)
  {
    int stopTimeSec = 30;
    int stopTimeFrames = stopTimeSec * audio.playback.sampleRate;
    if (ringbuffer_getCount(audio.playback.buffer) <= -stopTimeFrames)
    {
      StreamState expected = STREAM_STATE_KEEP_ALIVE;
      if (atomic_compare_exchange_strong_explicit(
            &audio.playback.state, &expected, STREAM_STATE_STOP_PENDING,
            memory_order_acq_rel, memory_order_acquire))
      {
        playbackDisableCallbacks();
        audio.audioDev->playback.stop();
        frames = 0;
      }
    }
  }

  playbackCallbackExit();
  return frames;
}

static void playbackStart(const LG_AudioFormat * format,
    const LG_AudioClock * sourceClock)
{
  if (!audio.audioDev)
    return;

  if (!audioFormatValid(format))
  {
    DEBUG_ERROR("Invalid playback format");
    if (playbackGetState() != STREAM_STATE_STOP)
      playbackStop();
    return;
  }

  const int channels   = format->channelCount;
  const int sampleRate = format->sampleRate;

  StreamState state = playbackGetState();
  if (state == STREAM_STATE_KEEP_ALIVE &&
      audio.playback.lastFormatValid &&
      audioFormatEqual(format, &audio.playback.lastFormat))
  {
    StreamState expected = STREAM_STATE_KEEP_ALIVE;
    if (atomic_compare_exchange_strong_explicit(
          &audio.playback.state, &expected, STREAM_STATE_RESUMING,
          memory_order_acq_rel, memory_order_acquire))
    {
      playbackPrepareMediaClock(
          &audio.playback.sourceData, sourceClock);
      return;
    }

    state = expected;
  }

  if (state != STREAM_STATE_STOP)
    playbackStop();

  const int bufferFrames = sampleRate;
  audio.playback.buffer = ringbuffer_newUnbounded(bufferFrames,
      channels * sizeof(float));
  if (!audio.playback.buffer)
    return;

  audio.playback.format          = *format;
  audio.playback.lastFormat      = *format;
  audio.playback.lastFormatValid = true;

  audio.playback.channels   = channels;
  audio.playback.sampleRate = sampleRate;
  audio.playback.stride     = channels * sizeof(float);
  playbackSetState(STREAM_STATE_SETUP_SOURCE);

  audio.playback.deviceData.nextPosition         = 0;
  audio.playback.deviceData.outputPosition       = 0.0;
  audio.playback.deviceData.appliedRatio         = 1.0;
  audio.playback.deviceData.startupSilenceFrames = 0;

  audio.playback.sourceData.inputPosition       = 0;
  audio.playback.sourceData.outputPosition      = 0;
  audio.playback.sourceData.devPeriodFrames     = 0;
  audio.playback.sourceData.devReadPosition     = 0;
  audio.playback.sourceData.deviceTimingSequence = 0;
  playbackDeviceClockAcquireReset(&audio.playback.sourceData);
  audio.playback.sourceData.offsetError         = 0.0;
  audio.playback.sourceData.offsetErrorIntegral = 0.0;
  audio.playback.sourceData.ratioIntegral       = 0.0;
  audio.playback.sourceData.lastRatio           = 1.0;
  audio.playback.sourceData.lastClockRatio      = 1.0;
  audio.playback.sourceData.bufferOverrunPending = false;
  audio.playback.sourceData.bufferOverruns      = 0;
  audio.playback.sourceData.nextLogTime         =
    nanotime() + INT64_C(5000000000);
  audio.playback.sourceData.arrivalJitterSec    = 0.0;
  audio.playback.sourceData.sourcePhaseBaselineSec = 0.0;
  audio.playback.sourceData.sourcePhaseReserveSec = 0.0;
  audio.playback.sourceData.sourcePacketDurationSec = 0.0;
  audio.playback.sourceData.sourcePhaseBaselineValid = false;
  audio.playback.sourceData.deviceClock.valid   = false;
  audio.playback.sourceData.outputClock.valid   = false;
  playbackPrepareMediaClock(&audio.playback.sourceData, sourceClock);

  atomic_store_explicit(
      &audio.playback.deviceTiming.sequence, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.deviceTiming.periodFrames, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.deviceTiming.time, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.deviceTiming.position, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.deviceTiming.outputPosition, 0.0,
      memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.underruns, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.backendResampleRatio, 1.0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.backendResamplerFailed, false,
      memory_order_relaxed);

  const int requestedPeriodFrames = g_params.audioPeriodSize > 0 ?
    clamp(g_params.audioPeriodSize, 1, sampleRate) :
    max(sampleRate / 100, 1);
  audio.playback.deviceMaxPeriodFrames = 0;
  audio.playback.deviceStartFrames     = 0;
  audio.playback.targetStartFrames     = 0;
  audio.playback.startupLowWaterFrames = 0;
  audio.playback.startupPacketDeadline = 0;
  audio.playback.startupPacketPeriod   = 0;
  const bool requestBackendResampler =
    g_params.audioResampler != AUDIO_RESAMPLER_LIBSAMPLERATE;
  LG_AudioFormat deviceFormat = *format;
  deviceFormat.sampleFormat = LG_AUDIO_FMT_F32_LE;
  if (!audio.audioDev->playback.setup(&deviceFormat,
        requestedPeriodFrames, requestBackendResampler,
        &audio.playback.backendResampler,
        &audio.playback.deviceMaxPeriodFrames,
        &audio.playback.deviceStartFrames, playbackPullFrames) ||
      audio.playback.deviceMaxPeriodFrames <= 0 ||
      audio.playback.deviceStartFrames < 0)
  {
    DEBUG_ERROR("Failed to configure audio playback device");
    playbackStop();
    return;
  }

  if (g_params.audioResampler == AUDIO_RESAMPLER_BACKEND &&
      !audio.playback.backendResampler)
    DEBUG_WARN("%s could not activate backend resampling; "
        "using libsamplerate", audio.audioDev->name);

  if (!audio.playback.backendResampler)
  {
    int srcError;
    audio.playback.sourceData.src =
      src_new(SRC_SINC_FASTEST, channels, &srcError);
    if (!audio.playback.sourceData.src)
    {
      DEBUG_ERROR("Failed to create resampler: %s", src_strerror(srcError));
      playbackStop();
      return;
    }
  }
  else
    audio.playback.sourceData.src = NULL;

  DEBUG_INFO("Using audio resampler: %s",
      audio.playback.backendResampler ?
        audio.audioDev->name : "libsamplerate");

  // if a volume level was stored, set it before we return
  if (audio.playback.volumeChannels)
    audio.audioDev->playback.volume(
        audio.playback.volumeChannels,
        audio.playback.volume);

  // set the inital mute state
  if (audio.audioDev->playback.mute)
    audio.audioDev->playback.mute(audio.playback.mute);

  // Set up synchronization instrumentation only when explicitly requested.
  if (g_params.audioDebug)
    audio.playback.timings = ringbuffer_new(1200, sizeof(float));

  atomic_store_explicit(
      &audio.playback.callbackState, 0, memory_order_release);
}

static void playbackSourceStop(void)
{
  if (!audio.audioDev)
    return;

  switch (playbackGetState())
  {
    case STREAM_STATE_RUN:
    case STREAM_STATE_RESUMING:
    {
      // Keep the audio device open for a while to reduce startup latency if
      // playback starts again
      playbackSetState(STREAM_STATE_KEEP_ALIVE);

      // Reset the software resampler so it is safe for the next playback
      if (audio.playback.sourceData.src)
      {
        int error = src_reset(audio.playback.sourceData.src);
        if (error)
        {
          DEBUG_ERROR("Failed to reset resampler: %s", src_strerror(error));
          playbackStop();
        }
      }

      break;
    }

    case STREAM_STATE_SETUP_SOURCE:
    case STREAM_STATE_SETUP_DEVICE:
    case STREAM_STATE_STOP_PENDING:
      // Playback hasn't actually started yet so just clean up
      playbackStop();
      break;

    case STREAM_STATE_KEEP_ALIVE:
    case STREAM_STATE_STOP:
      // Nothing to do
      break;
  }
}

static void playbackVolume(int channels, const uint16_t volume[])
{
  if (!audio.audioDev || !audio.audioDev->playback.volume ||
      !g_params.audioSyncVolume)
    return;

  // store the values so we can restore the state if the stream is restarted
  channels = min(ARRAY_LENGTH(audio.playback.volume), channels);
  memcpy(audio.playback.volume, volume, sizeof(uint16_t) * channels);
  audio.playback.volumeChannels = channels;

  if (!STREAM_ACTIVE(playbackGetState()))
    return;

  audio.audioDev->playback.volume(channels, volume);
}

static void playbackMute(bool mute)
{
  if (!audio.audioDev || !audio.audioDev->playback.mute)
    return;

  // store the value so we can restore it if the stream is restarted
  audio.playback.mute = mute;
  if (!STREAM_ACTIVE(playbackGetState()))
    return;

  audio.audioDev->playback.mute(mute);
}

static double computeDevicePosition(int64_t curTime)
{
  const PlaybackSourceData * sourceData =
    &audio.playback.sourceData;
  return playbackClockPosition(
      &sourceData->deviceClock, curTime) +
    sourceData->devicePositionOffsetFrames;
}

static bool playbackEnsureConversionBuffers(
    PlaybackSourceData * sourceData, int frames)
{
  if (frames > sourceData->framesInSize)
  {
    float * framesIn = realloc(sourceData->framesIn,
        (size_t)frames * audio.playback.stride);
    if (!framesIn)
    {
      DEBUG_ERROR("Failed to grow playback input buffer");
      return false;
    }

    sourceData->framesIn = framesIn;
    sourceData->framesInSize = frames;
  }

  if (!audio.playback.backendResampler)
  {
    const int framesOut =
      (int)ceil(frames * (1.0 + PLAYBACK_MAX_RATE_CORRECTION)) + 64;
    if (framesOut > sourceData->framesOutSize)
    {
      float * output = realloc(sourceData->framesOut,
          (size_t)framesOut * audio.playback.stride);
      if (!output)
      {
        DEBUG_ERROR("Failed to grow playback output buffer");
        return false;
      }

      sourceData->framesOut = output;
      sourceData->framesOutSize = framesOut;
    }
  }

  return true;
}

static int playbackAppendFrames(
    PlaybackSourceData * sourceData, const void * frames, int count)
{
  const int occupancy = ringbuffer_getCount(audio.playback.buffer);
  const int length = ringbuffer_getLength(audio.playback.buffer);
  const int64_t available = (int64_t)length - occupancy;
  const int append = clamp(
      (int64_t)count, INT64_C(0), max(INT64_C(0), available));

  const int advanced =
    ringbuffer_append(audio.playback.buffer, frames, append);
  DEBUG_ASSERT(advanced == append);

  if (append != count)
  {
    /* Never allow the logical writer to get beyond the physical storage.
     * Doing so makes a positive buffer count refer to overwritten samples and
     * sounds like corrupted PCM rather than an underrun. Resynchronize on the
     * next packet after dropping the excess output. */
    sourceData->bufferOverrunPending = true;
    ++sourceData->bufferOverruns;
  }

  return advanced;
}

static int playbackSlewBuffer(
    PlaybackSourceData * sourceData, int requested)
{
  const int occupancy = ringbuffer_getCount(audio.playback.buffer);
  const int length = ringbuffer_getLength(audio.playback.buffer);
  const int64_t minimum = -max(occupancy, 0);
  const int64_t maximum = (int64_t)length - occupancy;
  const int slew = clamp((int64_t)requested, minimum, maximum);

  const int advanced =
    ringbuffer_append(audio.playback.buffer, NULL, slew);
  DEBUG_ASSERT(advanced == slew);

  if (slew != requested)
    sourceData->bufferOverrunPending = true;

  return advanced;
}

static void playbackData(const void * data, size_t frameCount,
    const LG_AudioClock * sourceClock)
{
  StreamState state = playbackGetState();
  if (state == STREAM_STATE_STOP_PENDING)
  {
    playbackStop();
    return;
  }

  if (state == STREAM_STATE_STOP || !audio.audioDev || frameCount == 0)
    return;

  if (audio.playback.backendResampler &&
      atomic_exchange_explicit(
        &audio.playback.backendResamplerFailed, false,
        memory_order_acq_rel))
  {
    DEBUG_ERROR("Audio backend resampler failed");
    playbackStop();
    return;
  }

  PlaybackSourceData * sourceData = &audio.playback.sourceData;
  /* Backend resampling changes how many source frames PipeWire requests per
   * device period. Use the command-normalized output clock for rate matching,
   * while deviceClock remains in the ring's source-frame domain for latency. */
  const PlaybackClock * rateClock = audio.playback.backendResampler ?
    &sourceData->outputClock : &sourceData->deviceClock;
  const int64_t now = nanotime();
  const double nominalFrameSec = 1.0 / audio.playback.sampleRate;

  if (!data || frameCount > INT_MAX ||
      frameCount > (size_t)audio.playback.sampleRate * 2)
  {
    DEBUG_ERROR("Invalid playback packet length: %zu frames", frameCount);
    playbackStop();
    return;
  }
  const int frames = frameCount;

  if (!playbackEnsureConversionBuffers(sourceData, frames))
  {
    playbackStop();
    return;
  }
  if (!audioConvertToFloat(sourceData->framesIn, data,
        (size_t)frames * audio.playback.channels,
        audio.playback.format.sampleFormat))
  {
    DEBUG_ERROR("Failed to convert playback samples");
    playbackStop();
    return;
  }

  bool discontinuity = sourceClock && sourceClock->discontinuity;
  const int64_t packetTime =
    playbackMapMediaTime(sourceData, sourceClock, frames,
        audio.playback.sampleRate, now, &discontinuity);
  if (sourceData->bufferOverrunPending)
  {
    discontinuity = true;
    sourceData->bufferOverrunPending = false;
  }

  if (sourceData->lastPacketTime != INT64_MIN &&
      sourceData->lastArrivalTime != INT64_MIN)
  {
    const double mediaDelta =
      (packetTime - sourceData->lastPacketTime) * 1.0e-9;
    const double arrivalDelta =
      (now - sourceData->lastArrivalTime) * 1.0e-9;
    const double jitter = fabs(arrivalDelta - mediaDelta);

    /* Keep a slowly decaying peak rather than feeding arrival jitter into the
     * virtual clock. This lets the buffer absorb real delivery jitter while
     * the rate controller follows only the source media clock. */
    sourceData->arrivalJitterSec =
      min(PLAYBACK_MAX_JITTER_SEC,
          max(jitter, sourceData->arrivalJitterSec * 0.999));
  }
  sourceData->lastPacketTime  = packetTime;
  sourceData->lastArrivalTime = now;

  const bool sourceRateWasValid =
    sourceData->sourceRateValid;
  playbackSourceRateAdd(sourceData, nominalFrameSec);
  if (sourceClock && sourceClock->stable && sourceClock->rate > 0.0)
  {
    const double frameSec = 1.0 / sourceClock->rate;
    if (frameSec >= nominalFrameSec *
          (1.0 - PLAYBACK_MAX_RATE_CORRECTION) &&
        frameSec <= nominalFrameSec *
          (1.0 + PLAYBACK_MAX_RATE_CORRECTION))
    {
      sourceData->sourceRateFrameSec = frameSec;
      sourceData->sourceRateValid = true;
    }
  }
  const bool sourceRateBecameValid =
    !sourceRateWasValid && sourceData->sourceRateValid;
  if (!playbackSourceClockUpdate(&sourceData->sourceClock,
        packetTime, sourceData->inputPosition, nominalFrameSec))
    discontinuity = true;
  if (sourceData->sourceRateValid)
    sourceData->sourceClock.frameSec = sourceData->sourceRateFrameSec;

  /* Track phase variation around its local baseline, not its absolute value.
   * The absolute phase depends on the arbitrary local origin assigned to the
   * source media clock and must not become buffer reserve. Positive
   * deviation means the latency model temporarily overstates how much audio
   * remains in the ring. */
  const double sourcePhaseSec =
    sourceData->sourceClock.phaseResidualSec;
  const double packetSec =
    frames * nominalFrameSec;
  sourceData->sourcePacketDurationSec =
    max(packetSec, sourceData->sourcePacketDurationSec *
        exp(-packetSec / PLAYBACK_PHASE_RESERVE_DECAY_SEC));

  if (!sourceData->sourcePhaseBaselineValid ||
      sourceData->sourceClock.updates == 1)
  {
    sourceData->sourcePhaseBaselineSec = sourcePhaseSec;
    sourceData->sourcePhaseBaselineValid = true;
  }
  else
  {
    const double alpha =
      -expm1(-packetSec / PLAYBACK_PHASE_BASELINE_TIME_SEC);
    sourceData->sourcePhaseBaselineSec +=
      alpha * (sourcePhaseSec -
        sourceData->sourcePhaseBaselineSec);
  }

  const double sourcePhaseDeviationSec =
    max(0.0, sourcePhaseSec -
      sourceData->sourcePhaseBaselineSec);
  sourceData->sourcePhaseReserveSec =
    min(PLAYBACK_MAX_JITTER_SEC,
        max(sourcePhaseDeviationSec,
          sourceData->sourcePhaseReserveSec *
            exp(-packetSec /
              PLAYBACK_PHASE_RESERVE_DECAY_SEC)));

  int64_t curTime = sourceData->sourceClock.time;
  int64_t curPosition = sourceData->outputPosition;
  const double sourceReserveFrames =
    max(sourceData->sourcePacketDurationSec * 0.5,
        sourceData->sourcePhaseReserveSec) *
      audio.playback.sampleRate;

  // Receive the newest timing information from the audio device thread.
  PlaybackDeviceTick deviceTick;
  unsigned int deviceSequence;
  bool deviceClockBecameStable = false;
  if (playbackReadDeviceTiming(sourceData->deviceTimingSequence,
        &deviceTick, &deviceSequence))
  {
    sourceData->deviceTimingSequence = deviceSequence;
    sourceData->devPeriodFrames = deviceTick.periodFrames;
    sourceData->devReadPosition =
      deviceTick.nextPosition + deviceTick.periodFrames;
    const bool deviceClockUpdated =
      playbackClockUpdate(&sourceData->deviceClock,
          deviceTick.nextTime, deviceTick.nextPosition, nominalFrameSec);
    const bool outputClockUpdated =
      !audio.playback.backendResampler ||
      playbackClockUpdate(&sourceData->outputClock,
          deviceTick.nextTime, deviceTick.outputPosition,
          nominalFrameSec);
    if (!deviceClockUpdated || !outputClockUpdated)
    {
      playbackDeviceClockAcquireReset(sourceData);
      discontinuity = true;
    }
    else
      deviceClockBecameStable =
        playbackDeviceClockAcquire(sourceData, deviceTick.nextTime);
  }

  if (deviceClockBecameStable)
  {
    /* Give the fitted device timeline the same latency reported by the
     * acquisition model. Their position origins are otherwise unrelated, so
     * switching models would create a false phase step and drive the resampler
     * despite an already-correct ring level. Keep the source clock untouched:
     * changing it would also disturb source phase and jitter tracking. */
    const double rawDevicePosition =
      playbackClockPosition(&sourceData->deviceClock, curTime);
    sourceData->devicePositionOffsetFrames =
      sourceData->devReadPosition - sourceReserveFrames -
        rawDevicePosition;
  }

  const int maxPeriodFrames =
    max(audio.playback.deviceMaxPeriodFrames, sourceData->devPeriodFrames);
  /* The device period, delivery jitter, packet phase, and resampler delay
   * define the minimum viable latency. latencyOffset is strictly an additive
   * user offset over that same minimum for both startup and steady state. */
  const double latencyOffsetFrames =
    max(g_params.audioLatencyOffset, 0) *
      audio.playback.sampleRate / 1000.0;
  const double arrivalReserveFrames =
    (sourceData->arrivalJitterSec + 0.001) *
      audio.playback.sampleRate;
  const double minimumLowWaterReserveFrames =
    maxPeriodFrames * 0.1 + arrivalReserveFrames;
  const double minimumLowWaterFrames =
    maxPeriodFrames + minimumLowWaterReserveFrames;
  const double targetLowWaterFrames =
    minimumLowWaterFrames + latencyOffsetFrames;
  const double minimumBufferFrames =
    minimumLowWaterFrames + sourceReserveFrames;
  const double targetBufferFrames =
    minimumBufferFrames + latencyOffsetFrames;
  const double resamplerDelayFrames =
    audio.playback.backendResampler ?
      0.0 : PLAYBACK_RESAMPLER_DELAY_FRAMES;
  const double minimumLatencyFrames =
    minimumBufferFrames + resamplerDelayFrames;
  const double targetLatencyFrames =
    minimumLatencyFrames + latencyOffsetFrames;

  double devPosition = DBL_MIN;
  state = playbackGetState();
  if ((discontinuity ||
       state == STREAM_STATE_KEEP_ALIVE ||
       state == STREAM_STATE_RESUMING) &&
      sourceData->deviceClock.valid &&
      sourceData->deviceClockStable)
  {
    devPosition = computeDevicePosition(curTime);
    const double slew = devPosition + targetBufferFrames - curPosition;
    const int slewFrames = clamp(llrint(slew), (int64_t)INT_MIN,
        (int64_t)INT_MAX);
    const int actualSlew = playbackSlewBuffer(sourceData, slewFrames);
    sourceData->outputPosition += actualSlew;
    curPosition += actualSlew;

    sourceData->offsetError         = 0.0;
    sourceData->offsetErrorIntegral = 0.0;
    sourceData->ratioIntegral       = 0.0;
    playbackSetState(STREAM_STATE_RUN);
  }

  double actualLatencyFrames = 0.0;
  double actualOffsetError = 0.0;
  if (sourceData->deviceClock.valid)
  {
    if (sourceData->deviceClockStable)
    {
      if (devPosition == DBL_MIN)
        devPosition = computeDevicePosition(curTime);

      actualLatencyFrames =
        curPosition - devPosition + resamplerDelayFrames;
      actualOffsetError =
        targetLatencyFrames - actualLatencyFrames;
    }
    else
    {
      actualLatencyFrames =
        curPosition - sourceData->devReadPosition +
          sourceReserveFrames + resamplerDelayFrames;
      actualOffsetError =
        targetLatencyFrames - actualLatencyFrames;
    }

    const double error =
      actualOffsetError - sourceData->offsetError;
    const double periodSec = frames * nominalFrameSec;
    const double omega =
      2.0 * M_PI * PLAYBACK_OFFSET_FILTER_BANDWIDTH_HZ * periodSec;
    const double b = M_SQRT2 * omega;
    const double c = omega * omega;

    sourceData->offsetError += b * error +
      sourceData->offsetErrorIntegral;
    sourceData->offsetErrorIntegral += c * error;
  }

  /* Feed forward the measured source/device rate ratio, then use a slow,
   * bounded phase controller to keep the ring at its target. While the device
   * clock is acquiring, its rate estimate is not trustworthy, but the logical
   * producer/consumer latency above is. Use that with a faster, one-sided
   * controller which can restore missing reserve without draining an initial
   * surplus. The stable controller is critically damped so latency approaches
   * the target without a designed-in overshoot.
   *
   * Before the long-term source estimate is available, the phase integral
   * necessarily contains the clock-rate error. Discard that provisional
   * integral when measured feed-forward first takes over, then allow later
   * filtered clock updates to change the requested ratio directly. Hiding
   * those updates in the integral preserves a stale correction and steadily
   * moves an already-correct buffer away from its target. */
  const double naturalFrequency =
    2.0 * M_PI * PLAYBACK_PHASE_BANDWIDTH_HZ;
  const double kp =
    2.0 * naturalFrequency / audio.playback.sampleRate;
  const double ki =
    naturalFrequency * naturalFrequency / audio.playback.sampleRate;
  if (sourceRateBecameValid)
    sourceData->ratioIntegral = 0.0;

  if (sourceData->deviceClockStable &&
      sourceData->sourceRateValid &&
      rateClock->updates >= 2)
  {
    const double clockRatio = clamp(
        sourceData->sourceRateFrameSec /
          rateClock->frameSec,
        1.0 - PLAYBACK_MAX_RATE_CORRECTION,
        1.0 + PLAYBACK_MAX_RATE_CORRECTION);
    sourceData->lastClockRatio = clockRatio;
  }

  const double periodSec = frames * nominalFrameSec;
  /* source timestamps have millisecond resolution. Do not resample in
   * response to phase error that cannot be distinguished from quantization;
   * subtracting the deadband outside it keeps the response continuous. */
  const double phaseDeadbandFrames =
    PLAYBACK_PHASE_DEADBAND_SEC * audio.playback.sampleRate;
  const double rawPhaseError = sourceData->offsetError;
  double phaseError = rawPhaseError;
  if (fabs(phaseError) <= phaseDeadbandFrames)
    phaseError = 0.0;
  else
    phaseError -= copysign(phaseDeadbandFrames, phaseError);

  const bool acquiringDeviceClock =
    sourceData->deviceClock.valid && !sourceData->deviceClockStable;
  double controllerKp = kp;
  double controllerKi = ki;
  double controllerError = phaseError;
  double controllerBase = sourceData->lastClockRatio;

  if (acquiringDeviceClock)
  {
    const double acquireFrequency =
      2.0 * M_PI * PLAYBACK_ACQUIRE_PHASE_BANDWIDTH_HZ;
    controllerKp =
      2.0 * acquireFrequency / audio.playback.sampleRate;
    controllerBase = 1.0;
    sourceData->ratioIntegral = 0.0;

    if (actualOffsetError <= 0.0)
      controllerError = 0.0;
    else
      controllerError = max(phaseError, 0.0);
  }
  else if (deviceClockBecameStable)
  {
    /* Acquisition correction is transient, not a clock-rate estimate. Start
     * the stable integral clean; the output-rate slew keeps the applied ratio
     * continuous across this transition. */
    sourceData->ratioIntegral = 0.0;
  }
  /* Use the unfiltered latency error here so filter lag cannot retain a phase
   * correction after the target has already been crossed. */
  else if (sourceData->ratioIntegral * actualOffsetError <= 0.0)
    sourceData->ratioIntegral = 0.0;

  const double candidateIntegral = acquiringDeviceClock ?
    0.0 :
    sourceData->ratioIntegral +
      (deviceClockBecameStable ? 0.0 : controllerError * periodSec);
  const double phaseCorrection =
    controllerKp * controllerError +
      (acquiringDeviceClock ? 0.0 :
        controllerKi * candidateIntegral);
  const double desiredRatio =
    controllerBase + phaseCorrection;
  const double boundedRatio = clamp(desiredRatio,
      acquiringDeviceClock ? 1.0 :
        1.0 - PLAYBACK_MAX_RATE_CORRECTION,
      1.0 + PLAYBACK_MAX_RATE_CORRECTION);

  if (!acquiringDeviceClock && sourceData->deviceClockStable &&
      (desiredRatio == boundedRatio ||
       (desiredRatio > boundedRatio && controllerError < 0.0) ||
       (desiredRatio < boundedRatio && controllerError > 0.0)))
    sourceData->ratioIntegral = candidateIntegral;

  const double maxRatioStep =
    PLAYBACK_MAX_RATE_SLEW_PER_SEC * periodSec;
  const double ratio = clamp(boundedRatio,
      sourceData->lastRatio - maxRatioStep,
      sourceData->lastRatio + maxRatioStep);
  sourceData->lastRatio = ratio;

  if (audio.playback.backendResampler)
  {
    atomic_store_explicit(
        &audio.playback.backendResampleRatio, ratio,
        memory_order_release);
    const int outputFrames =
      playbackAppendFrames(sourceData, sourceData->framesIn, frames);
    sourceData->outputPosition += outputFrames;
  }
  else
  {
    int consumed = 0;
    while (consumed < frames)
    {
      SRC_DATA srcData =
      {
        .data_in           = sourceData->framesIn +
          consumed * audio.playback.channels,
        .data_out          = sourceData->framesOut,
        .input_frames      = frames - consumed,
        .output_frames     = sourceData->framesOutSize,
        .input_frames_used = 0,
        .output_frames_gen = 0,
        .end_of_input      = 0,
        .src_ratio         = ratio
      };

      int error = src_process(sourceData->src, &srcData);
      if (error)
      {
        DEBUG_ERROR("Resampling failed: %s", src_strerror(error));
        playbackStop();
        return;
      }

      if (srcData.input_frames_used == 0 && srcData.output_frames_gen == 0)
      {
        DEBUG_ERROR("Resampler made no progress");
        playbackStop();
        return;
      }

      const int outputFrames = playbackAppendFrames(
          sourceData, sourceData->framesOut, srcData.output_frames_gen);

      consumed += srcData.input_frames_used;
      sourceData->outputPosition += outputFrames;
    }
  }
  sourceData->inputPosition += frames;

  if (playbackGetState() == STREAM_STATE_SETUP_SOURCE)
  {
    /* At a packet boundary, targetLowWaterFrames is the physical ring target;
     * sourceReserveFrames accounts for the packet's average delivery phase
     * and must not be prefetched a second time. Cover whichever is larger:
     * the backend's immediate startup pull or the interval until the next
     * source packet. This starts at the requested average latency without
     * risking an underrun before that packet arrives. */
    const int bufferLength =
      ringbuffer_getLength(audio.playback.buffer);
    const int startupLowWaterFrames = clamp(
        llrint(ceil(targetLowWaterFrames)),
        INT64_C(0), (int64_t)bufferLength);
    audio.playback.targetStartFrames = min(
      (int64_t)startupLowWaterFrames +
        max(audio.playback.deviceStartFrames, frames),
      (int64_t)bufferLength);
    if (ringbuffer_getCount(audio.playback.buffer) >=
        audio.playback.targetStartFrames)
    {
      if (audio.playback.timings && !audio.playback.graph)
      {
        const float graphMax =
          targetLatencyFrames * 1000.0 /
          audio.playback.sampleRate * 2;
        audio.playback.graph = app_registerGraph("PLAYBACK",
            audio.playback.timings, 0.0f, graphMax,
            audioGraphFormatFn);
        if (!audio.playback.graph)
          ringbuffer_free(&audio.playback.timings);
      }

      audio.playback.startupLowWaterFrames =
        startupLowWaterFrames;
      audio.playback.startupPacketPeriod =
        max(llrint(packetSec * 1.0e9), INT64_C(1));
      audio.playback.startupPacketDeadline =
        now + audio.playback.startupPacketPeriod;

      if (g_params.audioDebug)
        DEBUG_INFO(
            "Audio start: %.2f/%.2f ms target/queued",
            targetLatencyFrames * 1000.0 /
              audio.playback.sampleRate,
            audio.playback.targetStartFrames * 1000.0 /
              audio.playback.sampleRate);

      playbackSetState(STREAM_STATE_SETUP_DEVICE);
      audio.audioDev->playback.start();
    }
  }

  if (!g_params.audioDebug)
    return;

  const double softwareLatencyMs =
    actualLatencyFrames * 1000.0 / audio.playback.sampleRate;

  if (audio.playback.graph)
  {
    const float latency = softwareLatencyMs;
    ringbuffer_push(audio.playback.timings, &latency);
    app_invalidateGraph(audio.playback.graph);
  }

  if (now >= sourceData->nextLogTime)
  {
    const double sourcePpm = sourceData->sourceRateValid ?
      (sourceData->sourceRateFrameSec / nominalFrameSec - 1.0) * 1.0e6 :
      0.0;
    const double devicePpm = rateClock->valid ?
      (rateClock->frameSec / nominalFrameSec - 1.0) * 1.0e6 :
      0.0;
    const unsigned int underruns = atomic_exchange_explicit(
        &audio.playback.underruns, 0, memory_order_relaxed);

    DEBUG_INFO(
        "Audio sync: %.2f/%.2f ms, ratio %+.1f ppm, "
        "clocks %+.1f/%+.1f ppm, jitter %.2f ms, xruns %u/%u",
        softwareLatencyMs,
        targetLatencyFrames * 1000.0 / audio.playback.sampleRate,
        (ratio - 1.0) * 1.0e6, sourcePpm, devicePpm,
        sourceData->arrivalJitterSec * 1000.0,
        underruns, sourceData->bufferOverruns);

    sourceData->bufferOverruns = 0;
    sourceData->nextLogTime = now + INT64_C(5000000000);
  }
}

static bool playbackGetFeedback(LG_AudioClock * clock)
{
  const PlaybackSourceData * sourceData = &audio.playback.sourceData;
  if (!clock || !sourceData->deviceClock.valid ||
      sourceData->deviceClock.position < 0.0 ||
      sourceData->deviceClock.frameSec <= 0.0)
    return false;

  const uint64_t latency = audio.audioDev->playback.latency ?
    audio.audioDev->playback.latency() : 0;

  *clock = (LG_AudioClock)
  {
    .position = llrint(sourceData->deviceClock.position),
    .time     = sourceData->deviceClock.time + latency * 1000,
    .rate     = 1.0 / sourceData->deviceClock.frameSec,
    .stable   = sourceData->deviceClockStable,
  };
  return true;
}

bool lgAudio_supportsRecord(void)
{
  return audio.audioDev && audio.audioDev->record.start;
}

static void recordPushFrames(uint8_t * data, int frames)
{
  if (frames <= 0)
    return;

  const uint32_t generation = atomic_load_explicit(
      &audio.record.streamGeneration, memory_order_acquire);
  if (!generation)
    return;

  LG_LOCK_SHARED(audio.activeLock);
  if (generation == atomic_load_explicit(
        &audio.record.streamGeneration, memory_order_acquire) &&
      audio.active.ops && audio.active.ops->recordData)
    audio.active.ops->recordData(audio.active.opaque,
        generation, data, frames, NULL);
  LG_UNLOCK_SHARED(audio.activeLock);
}

static MsgBoxHandle recordCancelConfirmLocked(void)
{
  MsgBoxHandle handle = audio.record.confirmHandle;
  audio.record.confirmHandle = NULL;
  audio.record.confirmPending = false;
  ++audio.record.confirmGeneration;
  return handle;
}

static void realRecordStartLocked(const LG_AudioFormat * format)
{
  audio.record.started = true;
  audio.record.format  = *format;

  audio.audioDev->record.start(format, recordPushFrames);

  // if a volume level was stored, set it before we return
  if (audio.record.volumeChannels)
    audio.audioDev->record.volume(
        audio.record.volumeChannels,
        audio.record.volume);

  // set the inital mute state
  if (audio.audioDev->record.mute)
    audio.audioDev->record.mute(audio.record.mute);

  if (g_params.micShowIndicator)
    app_showRecord(true);
}

static void recordConfirm(bool yes, void * opaque)
{
  const uint64_t generation = (uint64_t)(uintptr_t)opaque;

  LG_LOCK(audio.record.lock);
  if (!audio.record.confirmPending ||
      generation != audio.record.confirmGeneration)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  audio.record.confirmPending = false;
  audio.record.confirmHandle  = NULL;

  if (yes && audio.record.requested &&
      !audio.record.shuttingDown && audio.audioDev)
  {
    DEBUG_INFO("Microphone access granted");
    realRecordStartLocked(&audio.record.confirmFormat);
  }
  else if (yes)
    DEBUG_INFO("Ignoring stale microphone access confirmation");
  else
    DEBUG_INFO("Microphone access denied");

  LG_UNLOCK(audio.record.lock);
}

static void recordStart(const LG_AudioFormat * format)
{
  LG_LOCK(audio.record.lock);
  if (!audio.audioDev || audio.record.shuttingDown ||
      !audioFormatValid(format))
  {
    if (format && !audioFormatValid(format))
      DEBUG_ERROR("Invalid recording format");
    LG_UNLOCK(audio.record.lock);
    return;
  }

  const bool restart = audio.record.started;
  if (audio.record.started)
  {
    if (audioFormatEqual(format, &audio.record.lastFormat))
    {
      LG_UNLOCK(audio.record.lock);
      return;
    }

    realRecordStopLocked();
  }

  MsgBoxHandle oldConfirm = recordCancelConfirmLocked();
  audio.record.requested  = true;
  audio.record.lastFormat = *format;

  if (restart)
    realRecordStartLocked(format);
  else if (g_state.micDefaultState == MIC_DEFAULT_DENY)
    DEBUG_INFO("Microphone access denied by default");
  else if (g_state.micDefaultState == MIC_DEFAULT_ALLOW)
  {
    DEBUG_INFO("Microphone access granted by default");
    realRecordStartLocked(format);
  }
  else
  {
    audio.record.confirmFormat     = *format;
    audio.record.confirmPending    = true;
    const uint64_t generation = ++audio.record.confirmGeneration;
    LG_UNLOCK(audio.record.lock);

    app_msgBoxClose(oldConfirm);

    LG_LOCK(audio.record.lock);
    const bool current =
      audio.record.confirmPending &&
      generation == audio.record.confirmGeneration &&
      audio.record.requested &&
      !audio.record.shuttingDown &&
      audio.audioDev;
    if (current)
    {
      audio.record.confirmHandle = app_confirmMsgBox(
        "Microphone", recordConfirm, (void *)(uintptr_t)generation,
        "An application just opened the microphone!\n"
        "Do you want it to access your microphone?");
      if (!audio.record.confirmHandle)
      {
        audio.record.confirmPending = false;
        ++audio.record.confirmGeneration;
      }
    }
    LG_UNLOCK(audio.record.lock);
    return;
  }

  LG_UNLOCK(audio.record.lock);
  app_msgBoxClose(oldConfirm);
}

static void realRecordStopLocked(void)
{
  audio.audioDev->record.stop();
  audio.record.started = false;

  if (g_params.micShowIndicator)
    app_showRecord(false);
}

static void recordStop(void)
{
  LG_LOCK(audio.record.lock);
  audio.record.requested = false;
  MsgBoxHandle confirm = recordCancelConfirmLocked();

  if (audio.audioDev && audio.record.started)
  {
    DEBUG_INFO("Microphone recording stopped");
    realRecordStopLocked();
  }
  LG_UNLOCK(audio.record.lock);

  app_msgBoxClose(confirm);
}

void lgAudio_recordToggleKeybind(int sc, void * opaque)
{
  LG_LOCK(audio.record.lock);
  if (!audio.audioDev || audio.record.shuttingDown)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  if (!audio.record.requested)
  {
    LG_UNLOCK(audio.record.lock);
    app_alert(LG_ALERT_WARNING,
      "No application is requesting microphone access.");
    return;
  }

  MsgBoxHandle confirm = recordCancelConfirmLocked();
  bool started;
  if (audio.record.started)
  {
    DEBUG_INFO("Microphone recording stopped by user");
    realRecordStopLocked();
    started = false;
  }
  else
  {
    DEBUG_INFO("Microphone recording started by user");
    realRecordStartLocked(&audio.record.lastFormat);
    started = true;
  }
  LG_UNLOCK(audio.record.lock);

  app_msgBoxClose(confirm);
  app_alert(LG_ALERT_INFO,
      started ? "Microphone enabled" : "Microphone disabled");
}

static void recordVolume(int channels, const uint16_t volume[])
{
  LG_LOCK(audio.record.lock);
  if (!audio.audioDev || !audio.audioDev->record.volume ||
      !g_params.audioSyncVolume || audio.record.shuttingDown)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  // store the values so we can restore the state if the stream is restarted
  channels = min(ARRAY_LENGTH(audio.record.volume), channels);
  memcpy(audio.record.volume, volume, sizeof(uint16_t) * channels);
  audio.record.volumeChannels = channels;

  if (!audio.record.started)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  audio.audioDev->record.volume(channels, volume);
  LG_UNLOCK(audio.record.lock);
}

static void recordMute(bool mute)
{
  LG_LOCK(audio.record.lock);
  if (!audio.audioDev || !audio.audioDev->record.mute ||
      audio.record.shuttingDown)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  // store the value so we can restore it if the stream is restarted
  audio.record.mute = mute;
  if (!audio.record.started)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  audio.audioDev->record.mute(mute);
  LG_UNLOCK(audio.record.lock);
}

static bool bindingActiveNL(const AudioBinding * binding)
{
  return binding->ops &&
    audio.active.ops    == binding->ops &&
    audio.active.opaque == binding->opaque &&
    audio.active.generation == binding->generation;
}

static void queueFeedback(const LG_AudioOps * ops, void * opaque,
    uint32_t bindingGeneration, uint32_t generation,
    const LG_AudioClock * clock)
{
  if (!audio.feedback.event || !audio.feedback.thread || !clock)
    return;

  LG_LOCK(audio.feedback.lock);
  audio.feedback.ops        = ops;
  audio.feedback.opaque     = opaque;
  audio.feedback.bindingGeneration = bindingGeneration;
  audio.feedback.generation = generation;
  audio.feedback.clock      = *clock;
  audio.feedback.pending    = true;
  LG_UNLOCK(audio.feedback.lock);
  lgSignalEvent(audio.feedback.event);
}

static void eventPlaybackStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format, const LG_AudioClock * sourceClock)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  if (bindingActiveNL(binding))
  {
    LG_LOCK(audio.playback.sourceLock);
    atomic_store_explicit(&audio.playback.streamGeneration,
        generation, memory_order_release);
    playbackStart(format, sourceClock);
    LG_UNLOCK(audio.playback.sourceLock);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventPlaybackStop(void * opaque, uint32_t generation)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  if (bindingActiveNL(binding))
  {
    LG_LOCK(audio.playback.sourceLock);
    if (atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
    {
      playbackSourceStop();
      atomic_store_explicit(
          &audio.playback.streamGeneration, 0, memory_order_release);
    }
    LG_UNLOCK(audio.playback.sourceLock);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventPlaybackVolume(void * opaque, uint32_t generation,
    uint8_t channels, const uint16_t volume[])
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  if (bindingActiveNL(binding))
  {
    LG_LOCK(audio.playback.sourceLock);
    if (volume &&
        atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
      playbackVolume(channels, volume);
    LG_UNLOCK(audio.playback.sourceLock);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventPlaybackMute(void * opaque, uint32_t generation, bool mute)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  if (bindingActiveNL(binding))
  {
    LG_LOCK(audio.playback.sourceLock);
    if (atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
      playbackMute(mute);
    LG_UNLOCK(audio.playback.sourceLock);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventPlaybackData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  AudioBinding * binding = opaque;
  const LG_AudioOps * ops;
  void * providerOpaque;

  LG_LOCK_SHARED(audio.activeLock);
  const bool active = bindingActiveNL(binding);
  ops               = active ? binding->ops    : NULL;
  providerOpaque    = active ? binding->opaque : NULL;
  if (active)
  {
    LG_LOCK(audio.playback.sourceLock);
    if (atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
    {
      playbackData(data, frames, sourceClock);

      LG_AudioClock feedback;
      if (ops->clockFeedback && playbackGetFeedback(&feedback))
        queueFeedback(ops, providerOpaque, binding->generation,
            generation, &feedback);
    }
    LG_UNLOCK(audio.playback.sourceLock);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventRecordStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  const bool active = bindingActiveNL(binding) &&
    binding->ops->recordData;
  if (active)
  {
    atomic_store_explicit(&audio.record.streamGeneration,
        generation, memory_order_release);
    recordStart(format);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventRecordStop(void * opaque, uint32_t generation)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  const bool active = bindingActiveNL(binding);
  if (active &&
      atomic_load_explicit(&audio.record.streamGeneration,
        memory_order_acquire) == generation)
  {
    atomic_store_explicit(
        &audio.record.streamGeneration, 0, memory_order_release);
    recordStop();
  }
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventRecordVolume(void * opaque, uint32_t generation,
    uint8_t channels, const uint16_t volume[])
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  const bool active = bindingActiveNL(binding);
  if (active && volume &&
      atomic_load_explicit(&audio.record.streamGeneration,
        memory_order_acquire) == generation)
    recordVolume(channels, volume);
  LG_UNLOCK_SHARED(audio.activeLock);
}

static void eventRecordMute(void * opaque, uint32_t generation, bool mute)
{
  AudioBinding * binding = opaque;

  LG_LOCK_SHARED(audio.activeLock);
  const bool active = bindingActiveNL(binding);
  if (active &&
      atomic_load_explicit(&audio.record.streamGeneration,
        memory_order_acquire) == generation)
    recordMute(mute);
  LG_UNLOCK_SHARED(audio.activeLock);
}

static const LG_AudioEventOps eventOps =
{
  .playbackStart  = eventPlaybackStart,
  .playbackStop   = eventPlaybackStop,
  .playbackVolume = eventPlaybackVolume,
  .playbackMute   = eventPlaybackMute,
  .playbackData   = eventPlaybackData,
  .recordStart    = eventRecordStart,
  .recordStop     = eventRecordStop,
  .recordVolume   = eventRecordVolume,
  .recordMute     = eventRecordMute,
};

static bool validOps(const LG_AudioOps * ops)
{
  return ops && ops->name && ops->attach && ops->detach;
}

static AudioBinding makeBinding(const LG_AudioOps * ops, void * opaque)
{
  return (AudioBinding)
  {
    .ops        = ops,
    .opaque     = opaque,
    .available  = ops && !ops->setStatusListener,
    .generation = 0,
  };
}

static AudioBinding * nextBindingSlotNL(void)
{
  if (audio.transport.available)
    return &audio.transport;
  if (audio.fallback.available)
    return &audio.fallback;
  return NULL;
}

static void stopStreams(void)
{
  LG_LOCK(audio.playback.sourceLock);
  if (audio.audioDev)
    playbackStop();
  atomic_store_explicit(
      &audio.playback.streamGeneration, 0, memory_order_release);
  LG_UNLOCK(audio.playback.sourceLock);

  atomic_store_explicit(
      &audio.record.streamGeneration, 0, memory_order_release);
  recordStop();
}

/* providerLock must be held. dropActive suppresses calls into an endpoint
 * which has already disappeared. */
static void updateActive(bool dropActive)
{
  for (;;)
  {
    LG_LOCK_EXCLUSIVE(audio.activeLock);
    AudioBinding * slot = nextBindingSlotNL();
    AudioBinding next = slot ? *slot : (AudioBinding) { 0 };
    const AudioBinding old = audio.active;
    if (old.ops == next.ops && old.opaque == next.opaque &&
        old.generation == next.generation)
    {
      audio.active = next;
      LG_UNLOCK_EXCLUSIVE(audio.activeLock);
      return;
    }
    audio.active = (AudioBinding) { 0 };
    LG_UNLOCK_EXCLUSIVE(audio.activeLock);

    if (old.ops && !dropActive)
      old.ops->detach(old.opaque);
    dropActive = false;
    stopStreams();

    LG_LOCK_EXCLUSIVE(audio.activeLock);
    slot = nextBindingSlotNL();
    if (!slot)
    {
      LG_UNLOCK_EXCLUSIVE(audio.activeLock);
      DEBUG_INFO("Audio is unavailable");
      return;
    }
    next = *slot;
    audio.active = next;
    LG_UNLOCK_EXCLUSIVE(audio.activeLock);

    if (next.ops->attach(next.opaque, &eventOps, slot))
    {
      DEBUG_INFO("Using Audio: %s", next.ops->name);
      return;
    }

    next.ops->detach(next.opaque);
    stopStreams();

    LG_LOCK_EXCLUSIVE(audio.activeLock);
    if (audio.active.ops == next.ops &&
        audio.active.opaque == next.opaque)
      audio.active = (AudioBinding) { 0 };
    if (slot->ops == next.ops && slot->opaque == next.opaque)
      slot->available = false;
    LG_UNLOCK_EXCLUSIVE(audio.activeLock);

    DEBUG_WARN("Failed to attach Audio provider: %s", next.ops->name);
  }
}

static void fallbackStatusChanged(void * opaque,
    const LG_AudioStatus * status)
{
  if (!status)
    return;

  LG_LOCK(audio.providerLock);
  LG_LOCK_EXCLUSIVE(audio.activeLock);
  const bool current = audio.fallback.ops &&
    audio.fallback.opaque == opaque;
  if (current)
  {
    audio.fallback.available  = status->available;
    audio.fallback.generation = status->generation;
  }
  LG_UNLOCK_EXCLUSIVE(audio.activeLock);
  if (current)
    updateActive(false);
  LG_UNLOCK(audio.providerLock);
}

static void transportStatusChanged(void * opaque,
    const LG_AudioStatus * status)
{
  if (!status)
    return;

  LG_LOCK(audio.providerLock);
  LG_LOCK_EXCLUSIVE(audio.activeLock);
  const bool current = audio.transport.ops &&
    audio.transport.opaque == opaque;
  if (current)
  {
    audio.transport.available  = status->available;
    audio.transport.generation = status->generation;
  }
  LG_UNLOCK_EXCLUSIVE(audio.activeLock);
  if (current)
    updateActive(false);
  LG_UNLOCK(audio.providerLock);
}

static void setBinding(AudioBinding * target, const LG_AudioOps * ops,
    void * opaque, LG_AudioStatusFn statusFn)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid audio operations");
    ops    = NULL;
    opaque = NULL;
  }

  LG_LOCK(audio.providerLock);
  const AudioBinding old = *target;
  LG_UNLOCK(audio.providerLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);

  const AudioBinding next = makeBinding(ops, opaque);
  LG_LOCK(audio.providerLock);
  LG_LOCK_EXCLUSIVE(audio.activeLock);
  *target = next;
  LG_UNLOCK_EXCLUSIVE(audio.activeLock);
  updateActive(false);
  LG_UNLOCK(audio.providerLock);

  if (next.ops && next.ops->setStatusListener)
    next.ops->setStatusListener(next.opaque, statusFn, next.opaque);
}

static int feedbackThread(void * opaque)
{
  while (lgWaitEvent(audio.feedback.event, TIMEOUT_INFINITE))
  {
    if (atomic_load_explicit(
          &audio.feedback.stop, memory_order_acquire))
      break;

    const LG_AudioOps * ops;
    void * providerOpaque;
    uint32_t bindingGeneration;
    uint32_t generation;
    LG_AudioClock clock;

    LG_LOCK(audio.feedback.lock);
    const bool pending = audio.feedback.pending;
    ops                = audio.feedback.ops;
    providerOpaque     = audio.feedback.opaque;
    bindingGeneration  = audio.feedback.bindingGeneration;
    generation         = audio.feedback.generation;
    clock              = audio.feedback.clock;
    audio.feedback.pending = false;
    LG_UNLOCK(audio.feedback.lock);

    if (!pending || !ops || !ops->clockFeedback)
      continue;

    LG_LOCK_SHARED(audio.activeLock);
    if (audio.active.ops == ops &&
        audio.active.opaque == providerOpaque &&
        audio.active.generation == bindingGeneration &&
        atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
      ops->clockFeedback(providerOpaque, generation, &clock);
    LG_UNLOCK_SHARED(audio.activeLock);
  }

  return 0;
}

void lgAudio_init(void)
{
  LG_LOCK_INIT(audio.providerLock);
  LG_RWLOCK_INIT(audio.activeLock);
  LG_LOCK_INIT(audio.playback.sourceLock);
  LG_LOCK_INIT(audio.record.lock);
  LG_LOCK_INIT(audio.feedback.lock);
  audio.record.shuttingDown = false;
  atomic_init(&audio.playback.streamGeneration, 0);
  atomic_init(&audio.record.streamGeneration, 0);
  atomic_init(&audio.feedback.stop, false);
  atomic_store_explicit(
      &audio.playback.callbackState, PLAYBACK_CALLBACK_DISABLED,
      memory_order_release);

  audio.feedback.event = lgCreateEvent(true, 0);
  if (audio.feedback.event &&
      !lgCreateThread("audioFeedback", feedbackThread,
        NULL, &audio.feedback.thread))
  {
    lgFreeEvent(audio.feedback.event);
    audio.feedback.event = NULL;
  }

  for (int i = 0; i < LG_AUDIODEV_COUNT; ++i)
    if (LG_AudioDevs[i]->init())
    {
      audio.audioDev = LG_AudioDevs[i];
      DEBUG_INFO("Using AudioDev: %s", audio.audioDev->name);
      return;
    }

  DEBUG_WARN("Failed to initialize an audio backend");
}

void lgAudio_free(void)
{
  lgAudio_setTransport(NULL, NULL);
  lgAudio_setFallback(NULL, NULL);
  stopStreams();

  if (audio.feedback.thread)
  {
    atomic_store_explicit(
        &audio.feedback.stop, true, memory_order_release);
    lgSignalEvent(audio.feedback.event);
    lgJoinThread(audio.feedback.thread, NULL);
    audio.feedback.thread = NULL;
  }
  if (audio.feedback.event)
  {
    lgFreeEvent(audio.feedback.event);
    audio.feedback.event = NULL;
  }

  LG_LOCK(audio.record.lock);
  audio.record.shuttingDown = true;
  audio.record.requested = false;
  MsgBoxHandle confirm = recordCancelConfirmLocked();
  struct LG_AudioDevOps * audioDev = audio.audioDev;
  audio.audioDev = NULL;
  LG_UNLOCK(audio.record.lock);

  app_msgBoxClose(confirm);
  if (audioDev)
    audioDev->free();

  LG_RWLOCK_FREE(audio.activeLock);
  LG_LOCK_FREE(audio.playback.sourceLock);
  LG_LOCK_FREE(audio.providerLock);
  LG_LOCK_FREE(audio.record.lock);
  LG_LOCK_FREE(audio.feedback.lock);
}

void lgAudio_setFallback(const LG_AudioOps * ops, void * opaque)
{
  setBinding(&audio.fallback, ops, opaque, fallbackStatusChanged);
}

void lgAudio_setTransport(const LG_AudioOps * ops, void * opaque)
{
  setBinding(&audio.transport, ops, opaque, transportStatusChanged);
}

void lgAudio_dropTransport(void)
{
  LG_LOCK(audio.providerLock);
  LG_LOCK_EXCLUSIVE(audio.activeLock);
  const AudioBinding old = audio.transport;
  const bool wasActive = bindingActiveNL(&audio.transport);
  audio.transport = (AudioBinding) { 0 };
  LG_UNLOCK_EXCLUSIVE(audio.activeLock);
  updateActive(wasActive);
  LG_UNLOCK(audio.providerLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);
}

#endif
