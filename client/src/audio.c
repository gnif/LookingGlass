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

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <samplerate.h>
#include <semaphore.h>
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
#define PLAYBACK_JITTER_DECAY_SEC 10.0
#define AUDIO_START_RETRY_MIN_NS INT64_C(5000000)
#define AUDIO_START_RETRY_MAX_NS INT64_C(250000000)
#define AUDIO_RETRY_RESET_NS INT64_C(1000000000)
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
#define PLAYBACK_MAX_SOURCE_PACKET_MS 100
#define PLAYBACK_FEEDBACK_INTERVAL_NS INT64_C(1000000)
#define PLAYBACK_FEEDBACK_FAILURE_NS INT64_C(100000000)
#define PLAYBACK_GRAPH_INTERVAL_NS INT64_C(25000000)

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

typedef enum
{
  PLAYBACK_DATA_DROP,
  PLAYBACK_DATA_PROCESSED,
  PLAYBACK_DATA_RETRY,
  PLAYBACK_DATA_RETRY_NOW
}
PlaybackDataResult;

typedef enum
{
  PLAYBACK_RATE_SOFTWARE,
  PLAYBACK_RATE_BACKEND,
  PLAYBACK_RATE_PROVIDER,
}
PlaybackRateControl;

typedef enum
{
  PLAYBACK_DIAGNOSTIC_REGISTER_GRAPH = 1U << 0,
  PLAYBACK_DIAGNOSTIC_INVALIDATE     = 1U << 1,
  PLAYBACK_DIAGNOSTIC_START_LOG      = 1U << 2,
  PLAYBACK_DIAGNOSTIC_SYNC_LOG       = 1U << 3,
}
PlaybackDiagnosticFlags;

#define STREAM_ACTIVE(state) \
  (state == STREAM_STATE_RUN        || \
   state == STREAM_STATE_KEEP_ALIVE || \
   state == STREAM_STATE_RESUMING)

#define PLAYBACK_CALLBACK_DISABLED (UINT32_C(1) << 31)
#define PLAYBACK_CALLBACK_COUNT_MASK (PLAYBACK_CALLBACK_DISABLED - 1)
#define ACTIVE_CALLBACK_WAITING (UINT32_C(1) << 31)
#define ACTIVE_CALLBACK_COUNT_MASK (ACTIVE_CALLBACK_WAITING - 1)

typedef struct
{
  int64_t      nextPosition;
  double       outputPosition;
  double       appliedRatio;
  int          startupSilenceFrames;
  unsigned int discontinuity;
  bool         underrunning;
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
  int     maxSourcePacketFrames;

  int64_t inputPosition;
  int64_t outputPosition;

  int64_t  mediaTime;
  int64_t  mediaElapsed;
  int64_t  mediaTimeMs;
  int64_t  mediaLocalOrigin;
  uint64_t mediaPosition;
  int64_t  lastArrivalTime;
  double   arrivalJitterSec;
  double       sourcePhaseBaselineSec;
  double       sourcePhaseReserveSec;
  double       sourcePacketDurationSec;
  unsigned int sourcePacketPacedCount;
  bool         sourcePhaseBaselineValid;
  bool         sourcePacketRecovering;
  bool         mediaClockValid;
  bool         mediaPositionValid;
  bool         mediaClockFromSource;

  PlaybackRateSample rateSamples[PLAYBACK_RATE_MAX_SAMPLES];
  unsigned int rateSampleStart;
  unsigned int rateSampleCount;
  int64_t      rateLastSampleTimeMs;
  int64_t      rateFilterTimeMs;
  double       sourceRateFrameSec;
  bool         sourceRateValid;
  bool         bufferOverrunPending;
  bool         backlogTrimArmed;

  int          devPeriodFrames;
  int64_t      devReadPosition;
  unsigned int deviceTimingSequence;
  unsigned int deviceDiscontinuity;
  int64_t      deviceClockAcquireStart;
  int64_t      deviceClockCheckTime;
  double       deviceClockCheckFrameSec;
  double       deviceClockStableSec;
  double       devicePositionOffsetFrames;
  bool         deviceClockStable;

  double  offsetError;
  double  offsetErrorIntegral;
  double  ratioIntegral;
  double  lastRatio;
  double  lastClockRatio;
  int64_t nextFeedbackTime;
  int64_t nextGraphTime;
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
  atomic_uint       discontinuity;
  atomic_int        periodFrames;
  _Atomic(int64_t)  time;
  _Atomic(int64_t)  position;
  _Atomic(double)   outputPosition;
}
PlaybackDeviceTiming;

typedef struct
{
  double targetLatencyMs;
  double queuedLatencyMs;
}
PlaybackStartDiagnostics;

typedef struct
{
  unsigned int        epoch;
  double              softwareLatencyMs;
  double              targetLatencyMs;
  double              controlPpm;
  double              jitterMs;
  double              backlogTrimmedMs;
  unsigned int        underruns;
  unsigned int        overruns;
  PlaybackRateControl rateControl;
}
PlaybackSyncDiagnostics;

typedef struct
{
  unsigned int             epoch;
  unsigned int             pending;
  float                    graphMax;
  PlaybackStartDiagnostics start;
  PlaybackSyncDiagnostics  sync;
}
PlaybackDiagnostics;

typedef struct
{
  const LG_AudioOps * ops;
  void              * opaque;
  bool                available;
  uint32_t            epoch;
  uint32_t            generation;
}
AudioBinding;

typedef struct
{
  struct LG_AudioDevOps * audioDev;
  atomic_bool             ready;

  LG_Lock      bindingLock;
  LG_Lock      providerLock;
  LG_RWLock    activeLock;
  uint32_t     nextBindingEpoch;
  atomic_uint  activeCallbacks;
  LGEvent    * activeIdle;
  AudioBinding fallback;
  AudioBinding transport;
  AudioBinding active;

  struct
  {
    LG_Lock              sourceLock;
    LG_Lock              deviceLock;
    struct
    {
      sem_t       wake;
      bool        wakeInitialized;
      atomic_bool wakePending;
      LGThread  * thread;
      atomic_bool stop;
    }
    worker;
    _Atomic(StreamState) state;
    atomic_uint          callbackState;
    sem_t                callbackIdle;
    bool                 callbackIdleInitialized;
    atomic_uint          streamGeneration;
    uint64_t             requestSerial;
    uint32_t             requestedGeneration;
    LG_AudioFormat       requestedFormat;
    bool                 requestedFormatValid;
    bool                 requestedProviderRateControl;
    bool                 forceSoftwareResampler;
    bool                 startPending;
    bool                 startInProgress;
    unsigned int         startFailures;
    int64_t              nextStartRetry;
    atomic_uint          activeAttemptSerial;
    atomic_uint          failedAttemptSerial;
    uint32_t             nextAttemptSerial;
    uint64_t             backendRequestSerial;
    int64_t              backendStartTime;
    uint64_t             controlSerial;
    bool                 controlsPending;
    int                  volumeChannels;
    uint16_t             volume[LG_AUDIO_MAX_CHANNELS];
    bool                 mute;
    LG_AudioFormat       format;
    LG_AudioFormat       lastFormat;
    bool                 lastFormatValid;
    int                  channels;
    int                  sampleRate;
    int                  stride;
    bool                 convertToFloat;
    int                  deviceMaxPeriodFrames;
    int                  deviceStartFrames;
    int                  targetStartFrames;
    int                  startupLowWaterFrames;
    int64_t              startupPacketDeadline;
    int64_t              startupPacketPeriod;
    PlaybackRateControl rateControl;
    bool                lastProviderRateControl;
    _Atomic(double) backendResampleRatio;
    atomic_bool     backendResamplerFailed;
    RingBuffer            buffer;
    PlaybackDeviceTiming  deviceTiming;
    atomic_int            backlogTrimTarget;
    atomic_flag           deviceStateGate;
    atomic_uint           underruns;
    atomic_uint_fast64_t  backlogTrimmedFrames;

    RingBuffer          timings;
    GraphHandle         graph;
    atomic_uint         diagnosticsEpoch;
    atomic_uint         syncDiagnosticsEpoch;
    atomic_bool         graphReady;
    bool                graphRegistrationAttempted;
    PlaybackDiagnostics diagnostics;

    /* These two structs contain data specifically for use in the device and
     * source data threads respectively. Keep them on separate cache lines to
     * avoid false sharing. */
    alignas(64) PlaybackDeviceData deviceData;
    alignas(64) PlaybackSourceData  sourceData;
  }
  playback;

  struct
  {
    LG_RWLock              lock;
    LGEvent              * wake;
    LGThread             * thread;
    atomic_bool            workerAlive;
    atomic_uint            deliverySerial;
    atomic_uint            failedAttemptSerial;
    uint32_t               nextAttemptSerial;
    AudioBinding           deliveryBinding;
    uint32_t               deliveryGeneration;
    bool                   shuttingDown;
    bool                   requested;
    bool                   enabled;
    unsigned int           startFailures;
    int64_t                nextStartRetry;
    uint64_t               requestSerial;
    AudioBinding           requestedBinding;
    uint32_t               requestedGeneration;
    LG_AudioFormat         requestedFormat;
    int                    volumeChannels;
    uint16_t               volume[LG_AUDIO_MAX_CHANNELS];
    bool                   mute;
    uint64_t               controlSerial;
    MsgBoxHandle           confirmHandle;
    uint64_t               confirmGeneration;
    bool                   confirmPending;
  }
  record;

  struct
  {
    LG_Lock     lock;
    sem_t       wake;
    bool        wakeInitialized;
    atomic_bool wakePending;
    LGThread  * thread;
    atomic_bool stop;
    bool        pending;

    const LG_AudioOps * ops;
    void              * opaque;
    uint32_t            bindingEpoch;
    uint32_t            bindingGeneration;
    uint32_t            generation;
    LG_AudioClock       clock;
    double              targetRate;
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
    case LG_AUDIO_FMT_F32_LE:
    case LG_AUDIO_FMT_F32_NE: return 4;
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

    case LG_AUDIO_FMT_F32_NE:
      memcpy(dst, src, samples * sizeof(*dst));
      return true;

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
  int          periodFrames;
  int64_t      nextTime;
  int64_t      nextPosition;
  double       outputPosition;
  unsigned int discontinuity;
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

static void playbackClockRebase(
    PlaybackClock * clock, int64_t time, double position)
{
  DEBUG_ASSERT(clock->valid);
  clock->time             = time;
  clock->position         = position;
  clock->phaseResidualSec = 0.0;
  ++clock->updates;
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

static void playbackSourceRateAdd(PlaybackSourceData * sourceData,
    int64_t timeMs, double nominalFrameSec)
{
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

static void playbackPublishDeviceTiming(int periodFrames, int64_t time,
    int64_t position, double outputPosition,
    unsigned int discontinuity)
{
  PlaybackDeviceTiming * timing = &audio.playback.deviceTiming;
  /* Publish the odd writer marker before any snapshot field. */
  atomic_fetch_add_explicit(&timing->sequence, 1, memory_order_seq_cst);
  atomic_store_explicit(
      &timing->periodFrames, periodFrames, memory_order_relaxed);
  atomic_store_explicit(
      &timing->discontinuity, discontinuity, memory_order_relaxed);
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
    tick->discontinuity = atomic_load_explicit(
        &timing->discontinuity, memory_order_relaxed);
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
  if (!*discontinuity && (!clock || !clock->discontinuity) &&
      sourceData->mediaClockFromSource == fromSource &&
      sourceData->mediaPositionValid &&
      sourceData->mediaPosition == position && delta >= 0 &&
      delta <= PLAYBACK_TIMESTAMP_DISCONTINUITY_NS &&
      sourceData->lastArrivalTime != INT64_MIN &&
      now >= sourceData->lastArrivalTime)
  {
    const double mediaDelta = delta * 1.0e-9;
    const double arrivalDelta =
      (now - sourceData->lastArrivalTime) * 1.0e-9;
    const double lateness = max(arrivalDelta - mediaDelta, 0.0);

    /* Learn harmful late delivery only while playback remains continuous.
     * An underrun rebases the media clock and must not become future
     * latency. Early catch-up packets do not require additional reserve. */
    sourceData->arrivalJitterSec =
      min(PLAYBACK_MAX_JITTER_SEC,
          max(lateness, sourceData->arrivalJitterSec *
            exp(-arrivalDelta / PLAYBACK_JITTER_DECAY_SEC)));
  }

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
static bool playbackEnsureConversionBuffers(
    PlaybackSourceData * sourceData, int frames);
static MsgBoxHandle recordCancelConfirmLocked(void);
static void recordStop(
    const AudioBinding * binding, uint32_t generation);
static bool eventBegin(
    const AudioBinding * binding, AudioBinding * active);
static void eventEnd(void);

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
  const unsigned int previous = atomic_fetch_sub_explicit(
      &audio.playback.callbackState, 1, memory_order_release);
  if ((previous & PLAYBACK_CALLBACK_DISABLED) &&
      (previous & PLAYBACK_CALLBACK_COUNT_MASK) == 1 &&
      audio.playback.callbackIdleInitialized)
    sem_post(&audio.playback.callbackIdle);
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
  {
    int result;
    do
      result = sem_wait(&audio.playback.callbackIdle);
    while (result < 0 && errno == EINTR);

    if (result < 0)
      break;
  }
}

static void playbackWorkerWake(void)
{
  if (!audio.playback.worker.wakeInitialized ||
      !audio.playback.worker.thread ||
      atomic_exchange_explicit(
        &audio.playback.worker.wakePending, true, memory_order_acq_rel))
    return;

  if (sem_post(&audio.playback.worker.wake) < 0 && errno != EOVERFLOW)
    atomic_store_explicit(
        &audio.playback.worker.wakePending, false, memory_order_release);
}

static void feedbackWorkerWake(void)
{
  if (!audio.feedback.wakeInitialized || !audio.feedback.thread ||
      atomic_exchange_explicit(
        &audio.feedback.wakePending, true, memory_order_acq_rel))
    return;

  if (sem_post(&audio.feedback.wake) < 0 && errno != EOVERFLOW)
    atomic_store_explicit(
        &audio.feedback.wakePending, false, memory_order_release);
}

static void playbackQueueStop(void)
{
  atomic_store_explicit(
      &audio.playback.streamGeneration, 0, memory_order_release);
  playbackDisableCallbacks();
  playbackSetState(STREAM_STATE_STOP_PENDING);
  playbackWorkerWake();
}

static void playbackQueueSourceStop(void)
{
  atomic_store_explicit(
      &audio.playback.activeAttemptSerial, 0, memory_order_release);
  playbackQueueStop();
}

static void playbackBackendFailed(uint32_t attemptSerial)
{
  unsigned int expected = attemptSerial;
  if (!attemptSerial || !atomic_compare_exchange_strong_explicit(
        &audio.playback.activeAttemptSerial, &expected, 0,
        memory_order_acq_rel, memory_order_acquire))
    return;

  atomic_store_explicit(&audio.playback.failedAttemptSerial,
      attemptSerial, memory_order_release);
  playbackQueueStop();
}

/* sourceLock must be held. */
static uint32_t playbackArmBackend(void)
{
  if (!++audio.playback.nextAttemptSerial)
    ++audio.playback.nextAttemptSerial;

  const uint32_t attemptSerial = audio.playback.nextAttemptSerial;
  atomic_store_explicit(
      &audio.playback.failedAttemptSerial, 0, memory_order_release);
  audio.playback.backendRequestSerial = audio.playback.requestSerial;
  audio.playback.backendStartTime     = 0;
  atomic_store_explicit(&audio.playback.activeAttemptSerial,
      attemptSerial, memory_order_release);
  return attemptSerial;
}

bool lgAudio_supportsPlayback(void)
{
  return atomic_load_explicit(&audio.ready, memory_order_acquire) &&
    audio.audioDev && audio.audioDev->playback.start;
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

/* sourceLock must be held. */
static PlaybackDiagnostics * playbackDiagnosticsLocked(void)
{
  PlaybackDiagnostics * diagnostics = &audio.playback.diagnostics;
  const unsigned int epoch = atomic_load_explicit(
      &audio.playback.diagnosticsEpoch, memory_order_acquire);
  if (diagnostics->epoch != epoch)
    *diagnostics = (PlaybackDiagnostics) { .epoch = epoch };

  return diagnostics;
}

/* sourceLock must be held. End the old source's diagnostic interval without
 * invalidating graph work, which belongs to the lifetime of the backend. */
static void playbackResetSyncDiagnosticsLocked(void)
{
  atomic_fetch_add_explicit(
      &audio.playback.syncDiagnosticsEpoch, 1, memory_order_release);
  while (atomic_flag_test_and_set_explicit(
      &audio.playback.deviceStateGate, memory_order_acquire))
    ;
  atomic_store_explicit(
      &audio.playback.backlogTrimTarget, -1, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.underruns, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.backlogTrimmedFrames, 0, memory_order_relaxed);
  audio.playback.deviceData.underrunning = false;
  atomic_flag_clear_explicit(
      &audio.playback.deviceStateGate, memory_order_release);
  audio.playback.sourceData.bufferOverruns = 0;

  PlaybackDiagnostics * diagnostics = playbackDiagnosticsLocked();
  diagnostics->pending &= ~PLAYBACK_DIAGNOSTIC_SYNC_LOG;
  diagnostics->sync     = (PlaybackSyncDiagnostics) { 0 };
}

static void playbackStop(void)
{
  const bool alreadyStopped = playbackGetState() == STREAM_STATE_STOP;
  playbackDisableCallbacks();
  atomic_store_explicit(
      &audio.playback.activeAttemptSerial, 0, memory_order_release);
  if (!alreadyStopped)
    audio.audioDev->playback.stop();
  playbackWaitForCallbacks();
  atomic_store_explicit(
      &audio.playback.failedAttemptSerial, 0, memory_order_release);
  audio.playback.backendRequestSerial = 0;
  audio.playback.backendStartTime     = 0;

  if (alreadyStopped)
    return;

  atomic_store_explicit(
      &audio.playback.graphReady, false, memory_order_release);
  atomic_fetch_add_explicit(
      &audio.playback.diagnosticsEpoch, 1, memory_order_release);
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
  audio.playback.sourceData.maxSourcePacketFrames = 0;

  if (audio.playback.timings)
  {
    if (audio.playback.graph)
      app_unregisterGraph(audio.playback.graph);
    ringbuffer_free(&audio.playback.timings);
  }
  audio.playback.graph = NULL;
  audio.playback.graphRegistrationAttempted = false;
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
  if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND)
  {
    nextRatio = atomic_load_explicit(
        &audio.playback.backendResampleRatio, memory_order_acquire);
    if (!audio.audioDev->playback.setRate(&nextRatio))
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

      StreamState expected = STREAM_STATE_SETUP_DEVICE;
      atomic_compare_exchange_strong_explicit(
          &audio.playback.state, &expected, STREAM_STATE_RUN,
          memory_order_acq_rel, memory_order_acquire);
    }

    if (playbackGetState() == STREAM_STATE_RUN &&
        atomic_load_explicit(
          &audio.playback.backlogTrimTarget, memory_order_acquire) >= 0 &&
        !atomic_flag_test_and_set_explicit(
          &audio.playback.deviceStateGate, memory_order_acquire))
    {
      if (playbackGetState() == STREAM_STATE_RUN)
      {
        const int trimTarget = atomic_exchange_explicit(
            &audio.playback.backlogTrimTarget, -1, memory_order_acq_rel);
        if (trimTarget >= 0)
        {
          const int queued    = ringbuffer_getCount(audio.playback.buffer);
          const int requested = max(queued - trimTarget, 0);
          const int dropped   = ringbuffer_consume(
              audio.playback.buffer, NULL, requested);
          if (dropped > 0)
          {
            data->nextPosition += dropped;
            ++data->discontinuity;
            atomic_fetch_add_explicit(
                &audio.playback.backlogTrimmedFrames, dropped,
                memory_order_relaxed);
          }
        }
      }
      atomic_flag_clear_explicit(
          &audio.playback.deviceStateGate, memory_order_release);
    }

    /* Timestamp the dequeue boundary before the current pull. The logical
     * position tracks source frames consumed from the ring for latency
     * measurement. With backend resampling, outputPosition separately tracks
     * the equivalent number of device-rate frames. PipeWire computes the
     * current request using the rate set by the previous callback, so apply
     * that same ratio to this period before adopting nextRatio. */
    playbackPublishDeviceTiming(
        frames, now, data->nextPosition, data->outputPosition,
        data->discontinuity);
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
        !atomic_flag_test_and_set_explicit(
          &audio.playback.deviceStateGate, memory_order_acquire))
    {
      const bool underrunning    = audioFrames > 0 &&
        playbackGetState() == STREAM_STATE_RUN &&
        ringbuffer_getCount(audio.playback.buffer) < audioFrames;
      const bool wasUnderrunning = data->underrunning;
      data->underrunning         = underrunning;
      if (underrunning && !wasUnderrunning)
        atomic_fetch_add_explicit(
            &audio.playback.underruns, 1, memory_order_relaxed);
      atomic_flag_clear_explicit(
          &audio.playback.deviceStateGate, memory_order_release);
    }
    ringbuffer_consume(audio.playback.buffer,
        dst + (size_t)silenceFrames * audio.playback.stride, audioFrames);
  }
  else
    frames = 0;

  // Close the stream if nothing has played for a while
  if (audio.playback.buffer &&
      audio.playback.worker.wakeInitialized &&
      audio.playback.worker.thread &&
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
        playbackWorkerWake();
        frames = 0;
      }
    }
  }

  playbackCallbackExit();
  return frames;
}

static bool playbackSetupDevice(const LG_AudioFormat * format,
    int requestedPeriodFrames, bool requestResampler,
    bool * backendResampler)
{
  audio.playback.deviceMaxPeriodFrames = 0;
  audio.playback.deviceStartFrames     = 0;
  *backendResampler                    = false;

  return audio.audioDev->playback.setup(format,
      requestedPeriodFrames, requestResampler,
      backendResampler,
      &audio.playback.deviceMaxPeriodFrames,
      &audio.playback.deviceStartFrames, playbackPullFrames) &&
    audio.playback.deviceMaxPeriodFrames > 0 &&
    audio.playback.deviceStartFrames >= 0;
}

static bool playbackResume(const LG_AudioFormat * format,
    const LG_AudioClock * sourceClock, bool providerRateControl,
    bool forceSoftwareResampler)
{
  if (forceSoftwareResampler ||
      !audio.playback.lastFormatValid ||
      audio.playback.lastProviderRateControl != providerRateControl ||
      !audioFormatEqual(format, &audio.playback.lastFormat))
    return false;

  StreamState expected = STREAM_STATE_KEEP_ALIVE;
  if (!atomic_compare_exchange_strong_explicit(
        &audio.playback.state, &expected, STREAM_STATE_RESUMING,
        memory_order_acq_rel, memory_order_acquire))
    return false;

  playbackPrepareMediaClock(&audio.playback.sourceData, sourceClock);
  audio.playback.sourceData.nextLogTime =
    nanotime() + INT64_C(5000000000);
  return true;
}

static bool playbackStart(const LG_AudioFormat * format,
    const LG_AudioClock * sourceClock, bool providerRateControl,
    bool forceSoftwareResampler)
{
  if (!audio.audioDev)
    return false;

  if (!audioFormatValid(format))
  {
    DEBUG_ERROR("Invalid playback format");
    if (playbackGetState() != STREAM_STATE_STOP)
      playbackStop();
    return false;
  }

  const int channels   = format->channelCount;
  const int sampleRate = format->sampleRate;

  if (playbackResume(format, sourceClock, providerRateControl,
        forceSoftwareResampler))
    return true;

  const StreamState state = playbackGetState();

  if (state != STREAM_STATE_STOP)
    playbackStop();

  audio.playback.format                  = *format;
  audio.playback.lastFormat              = *format;
  audio.playback.lastFormatValid         = true;
  audio.playback.lastProviderRateControl = providerRateControl;

  audio.playback.channels   = channels;
  audio.playback.sampleRate = sampleRate;
  audio.playback.sourceData.maxSourcePacketFrames = max(
      (sampleRate * PLAYBACK_MAX_SOURCE_PACKET_MS + 999) / 1000, 1);
  playbackSetState(STREAM_STATE_SETUP_SOURCE);

  audio.playback.deviceData.nextPosition         = 0;
  audio.playback.deviceData.outputPosition       = 0.0;
  audio.playback.deviceData.appliedRatio         = 1.0;
  audio.playback.deviceData.startupSilenceFrames = 0;
  audio.playback.deviceData.discontinuity        = 0;
  audio.playback.deviceData.underrunning         = false;

  audio.playback.sourceData.inputPosition        = 0;
  audio.playback.sourceData.outputPosition       = 0;
  audio.playback.sourceData.devPeriodFrames      = 0;
  audio.playback.sourceData.devReadPosition      = 0;
  audio.playback.sourceData.deviceTimingSequence = 0;
  audio.playback.sourceData.deviceDiscontinuity  = 0;
  playbackDeviceClockAcquireReset(&audio.playback.sourceData);
  audio.playback.sourceData.offsetError              = 0.0;
  audio.playback.sourceData.offsetErrorIntegral      = 0.0;
  audio.playback.sourceData.ratioIntegral            = 0.0;
  audio.playback.sourceData.lastRatio                = 1.0;
  audio.playback.sourceData.lastClockRatio           = 1.0;
  audio.playback.sourceData.nextFeedbackTime         = 0;
  audio.playback.sourceData.nextGraphTime            = 0;
  audio.playback.sourceData.bufferOverrunPending     = false;
  audio.playback.sourceData.backlogTrimArmed         = true;
  audio.playback.sourceData.bufferOverruns           = 0;
  audio.playback.sourceData.nextLogTime              =
    nanotime() + INT64_C(5000000000);
  audio.playback.sourceData.arrivalJitterSec         = 0.0;
  audio.playback.sourceData.sourcePhaseBaselineSec   = 0.0;
  audio.playback.sourceData.sourcePhaseReserveSec    = 0.0;
  audio.playback.sourceData.sourcePacketDurationSec  = 0.0;
  audio.playback.sourceData.sourcePacketPacedCount   = 0;
  audio.playback.sourceData.sourcePhaseBaselineValid = false;
  audio.playback.sourceData.sourcePacketRecovering   = false;
  audio.playback.sourceData.deviceClock.valid        = false;
  audio.playback.sourceData.outputClock.valid        = false;
  playbackPrepareMediaClock(&audio.playback.sourceData, sourceClock);

  atomic_store_explicit(
      &audio.playback.backlogTrimTarget, -1, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.backlogTrimmedFrames, 0, memory_order_relaxed);

  atomic_store_explicit(
      &audio.playback.deviceTiming.sequence, 0, memory_order_relaxed);
  atomic_store_explicit(
      &audio.playback.deviceTiming.discontinuity, 0,
      memory_order_relaxed);
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
  audio.playback.targetStartFrames     = 0;
  audio.playback.startupLowWaterFrames = 0;
  audio.playback.startupPacketDeadline = 0;
  audio.playback.startupPacketPeriod   = 0;
  const bool requestBackendResampler = !providerRateControl &&
    !forceSoftwareResampler &&
    g_params.audioResampler != AUDIO_RESAMPLER_LIBSAMPLERATE;
  LG_AudioFormat deviceFormat = *format;

  /* The ring generates zero-filled silence. Keep unsigned PCM on the float
   * path because its silence level is biased rather than zero. */
  if ((!requestBackendResampler && !providerRateControl) ||
      deviceFormat.sampleFormat == LG_AUDIO_FMT_U8)
    deviceFormat.sampleFormat = LG_AUDIO_FMT_F32_NE;

  bool backendResampler;
  bool deviceConfigured = playbackSetupDevice(
      &deviceFormat, requestedPeriodFrames, requestBackendResampler,
      &backendResampler);

  /* Native samples require either provider feedback or backend rate control.
   * Otherwise reconnect using float samples for the libsamplerate path. This
   * also provides a float fallback for formats unsupported by the backend. */
  if ((!deviceConfigured ||
       (!providerRateControl && !backendResampler)) &&
      deviceFormat.sampleFormat != LG_AUDIO_FMT_F32_NE)
  {
    deviceFormat.sampleFormat = LG_AUDIO_FMT_F32_NE;
    deviceConfigured = playbackSetupDevice(
        &deviceFormat, requestedPeriodFrames, requestBackendResampler,
        &backendResampler);
  }

  if (!deviceConfigured)
  {
    DEBUG_ERROR("Failed to configure audio playback device");
    playbackStop();
    return false;
  }

  audio.playback.stride = channels *
    audioSampleSize(deviceFormat.sampleFormat);
  audio.playback.convertToFloat =
    (!providerRateControl && !backendResampler) ||
    deviceFormat.sampleFormat != format->sampleFormat;
  audio.playback.rateControl = providerRateControl ?
    PLAYBACK_RATE_PROVIDER : backendResampler ?
      PLAYBACK_RATE_BACKEND : PLAYBACK_RATE_SOFTWARE;

  const int conversionBufferFrames = max(requestedPeriodFrames,
      audio.playback.sourceData.maxSourcePacketFrames);
  if (!playbackEnsureConversionBuffers(
        &audio.playback.sourceData, conversionBufferFrames))
  {
    playbackStop();
    return false;
  }

  audio.playback.buffer = ringbuffer_newUnbounded(
      sampleRate, audio.playback.stride);
  if (!audio.playback.buffer)
  {
    playbackStop();
    return false;
  }

  if (g_params.audioResampler == AUDIO_RESAMPLER_BACKEND &&
      !providerRateControl && !backendResampler)
    DEBUG_WARN("%s could not activate backend resampling; "
        "using libsamplerate", audio.audioDev->name);

  if (audio.playback.rateControl == PLAYBACK_RATE_SOFTWARE)
  {
    int srcError;
    audio.playback.sourceData.src =
      src_new(SRC_SINC_FASTEST, channels, &srcError);
    if (!audio.playback.sourceData.src)
    {
      DEBUG_ERROR("Failed to create resampler: %s", src_strerror(srcError));
      playbackStop();
      return false;
    }
  }
  else
    audio.playback.sourceData.src = NULL;

  switch (audio.playback.rateControl)
  {
    case PLAYBACK_RATE_PROVIDER:
      DEBUG_INFO("Using audio rate control: provider feedback");
      break;

    case PLAYBACK_RATE_BACKEND:
      DEBUG_INFO("Using audio resampler: %s", audio.audioDev->name);
      break;

    case PLAYBACK_RATE_SOFTWARE:
      DEBUG_INFO("Using audio resampler: libsamplerate");
      break;
  }

  // Set up synchronization instrumentation only when explicitly requested.
  if (g_params.audioDebug)
    audio.playback.timings = ringbuffer_new(1200, sizeof(float));

  atomic_store_explicit(
      &audio.playback.callbackState, 0, memory_order_release);
  return true;
}

static int64_t audioStartRetryDelay(unsigned int failures)
{
  const unsigned int shift = min(failures, 6U);
  return min(AUDIO_START_RETRY_MIN_NS << shift,
      AUDIO_START_RETRY_MAX_NS);
}

/* sourceLock must be held. */
static bool playbackDelayStart(void)
{
  const unsigned int failures = audio.playback.startFailures;
  audio.playback.nextStartRetry = nanotime() +
    audioStartRetryDelay(failures);
  if (audio.playback.startFailures < 7U)
    ++audio.playback.startFailures;

  return failures == 0U || failures == 6U;
}

/* sourceLock must be held. */
static void playbackScheduleStart(void)
{
  if (!audio.playback.requestedGeneration ||
      !audio.playback.requestedFormatValid ||
      audio.playback.startPending ||
      audio.playback.startInProgress ||
      (atomic_load_explicit(&audio.playback.streamGeneration,
           memory_order_acquire) ==
         audio.playback.requestedGeneration &&
       playbackGetState() != STREAM_STATE_STOP) ||
      nanotime() < audio.playback.nextStartRetry)
    return;

  audio.playback.startPending = true;
  playbackWorkerWake();
}

static void playbackProcessControls(void)
{
  uint64_t controlSerial;
  uint32_t generation;
  int      volumeChannels;
  uint16_t volume[LG_AUDIO_MAX_CHANNELS];
  bool     mute;

  LG_LOCK(audio.playback.sourceLock);
  generation = atomic_load_explicit(
      &audio.playback.streamGeneration, memory_order_acquire);
  if (!audio.playback.controlsPending || !generation ||
      generation != audio.playback.requestedGeneration)
  {
    LG_UNLOCK(audio.playback.sourceLock);
    return;
  }

  controlSerial = audio.playback.controlSerial;
  volumeChannels = audio.playback.volumeChannels;
  if (volumeChannels)
    memcpy(volume, audio.playback.volume,
        sizeof(*volume) * volumeChannels);
  mute = audio.playback.mute;
  LG_UNLOCK(audio.playback.sourceLock);

  bool applied = false;
  LG_LOCK(audio.playback.deviceLock);
  const StreamState state = playbackGetState();
  if (audio.audioDev && state != STREAM_STATE_STOP &&
      state != STREAM_STATE_STOP_PENDING &&
      atomic_load_explicit(&audio.playback.streamGeneration,
        memory_order_acquire) == generation)
  {
    if (volumeChannels && audio.audioDev->playback.volume)
      audio.audioDev->playback.volume(volumeChannels, volume);

    if (audio.audioDev->playback.mute)
      audio.audioDev->playback.mute(mute);
    applied = true;
  }
  LG_UNLOCK(audio.playback.deviceLock);

  if (!applied)
    return;

  LG_LOCK(audio.playback.sourceLock);
  if (audio.playback.requestedGeneration == generation &&
      atomic_load_explicit(&audio.playback.streamGeneration,
        memory_order_acquire) == generation &&
      audio.playback.controlSerial == controlSerial)
    audio.playback.controlsPending = false;
  LG_UNLOCK(audio.playback.sourceLock);
}

static void playbackProcessStopPending(void)
{
  bool     pending              = false;
  bool     reportFailure        = false;
  uint32_t failedAttempt        = 0;
  uint64_t backendRequestSerial = 0;
  int64_t  backendStartTime     = 0;

  LG_LOCK(audio.playback.sourceLock);
  if (audio.audioDev &&
      playbackGetState() == STREAM_STATE_STOP_PENDING)
  {
    failedAttempt = atomic_load_explicit(
        &audio.playback.failedAttemptSerial, memory_order_acquire);
    backendRequestSerial = audio.playback.backendRequestSerial;
    backendStartTime     = audio.playback.backendStartTime;
    atomic_store_explicit(
        &audio.playback.streamGeneration, 0, memory_order_release);
    pending = true;
  }
  LG_UNLOCK(audio.playback.sourceLock);

  if (!pending)
    return;

  LG_LOCK(audio.playback.deviceLock);
  playbackStop();
  LG_UNLOCK(audio.playback.deviceLock);

  LG_LOCK(audio.playback.sourceLock);
  if (failedAttempt && audio.playback.requestedGeneration &&
      audio.playback.requestedFormatValid &&
      backendRequestSerial == audio.playback.requestSerial)
  {
    if (backendStartTime &&
        nanotime() - backendStartTime >= AUDIO_RETRY_RESET_NS)
    {
      audio.playback.startFailures  = 0;
      audio.playback.nextStartRetry = 0;
    }

    audio.playback.startPending = false;
    reportFailure = playbackDelayStart();
  }
  LG_UNLOCK(audio.playback.sourceLock);

  if (reportFailure)
    DEBUG_ERROR("Audio playback device failed; retrying");
}

static void playbackProcessStart(void)
{
  LG_AudioFormat format;
  uint64_t       requestSerial;
  uint32_t       generation;
  bool           providerRateControl;
  bool           forceSoftwareResampler;

  LG_LOCK(audio.playback.sourceLock);
  if (!audio.playback.startPending ||
      audio.playback.startInProgress ||
      !audio.playback.requestedGeneration ||
      !audio.playback.requestedFormatValid ||
      nanotime() < audio.playback.nextStartRetry)
  {
    LG_UNLOCK(audio.playback.sourceLock);
    return;
  }

  audio.playback.startPending      = false;
  audio.playback.startInProgress   = true;
  format                           = audio.playback.requestedFormat;
  requestSerial                    = audio.playback.requestSerial;
  generation                       = audio.playback.requestedGeneration;
  providerRateControl              =
    audio.playback.requestedProviderRateControl;
  forceSoftwareResampler           =
    audio.playback.forceSoftwareResampler;
  LG_UNLOCK(audio.playback.sourceLock);

  LG_LOCK(audio.playback.deviceLock);
  const bool started = playbackStart(&format, NULL,
      providerRateControl, forceSoftwareResampler);
  LG_UNLOCK(audio.playback.deviceLock);

  bool stopStale = false;
  bool wake      = false;
  LG_LOCK(audio.playback.sourceLock);
  const bool current =
    requestSerial == audio.playback.requestSerial &&
    generation == audio.playback.requestedGeneration &&
    audio.playback.requestedFormatValid &&
    audioFormatEqual(&format, &audio.playback.requestedFormat) &&
    providerRateControl ==
      audio.playback.requestedProviderRateControl &&
    forceSoftwareResampler ==
      audio.playback.forceSoftwareResampler;

  audio.playback.startInProgress = false;
  if (started && current)
  {
    audio.playback.startPending = false;
    LG_LOCK(audio.playback.deviceLock);
    if (playbackGetState() != STREAM_STATE_STOP_PENDING)
    {
      if (atomic_load_explicit(&audio.playback.activeAttemptSerial,
            memory_order_acquire))
        audio.playback.backendRequestSerial = requestSerial;
      atomic_store_explicit(&audio.playback.streamGeneration,
          generation, memory_order_release);
      audio.playback.controlsPending = true;
      audio.playback.nextStartRetry = 0;
    }
    LG_UNLOCK(audio.playback.deviceLock);
  }
  else
  {
    atomic_store_explicit(
        &audio.playback.streamGeneration, 0, memory_order_release);
    if (started)
      stopStale = true;
    else if (current)
      playbackDelayStart();
  }

  wake = audio.playback.startPending;
  LG_UNLOCK(audio.playback.sourceLock);

  if (stopStale)
  {
    LG_LOCK(audio.playback.deviceLock);
    playbackStop();
    LG_UNLOCK(audio.playback.deviceLock);
  }

  if (wake)
    playbackWorkerWake();
}

static void playbackProcessDeviceStart(void)
{
  uint64_t requestSerial = 0;
  uint32_t generation    = 0;
  uint32_t attemptSerial = 0;

  LG_LOCK(audio.playback.sourceLock);
  const StreamState state = playbackGetState();
  attemptSerial = atomic_load_explicit(
      &audio.playback.activeAttemptSerial, memory_order_acquire);
  generation = atomic_load_explicit(
      &audio.playback.streamGeneration, memory_order_acquire);
  if (audio.audioDev && state == STREAM_STATE_SETUP_DEVICE &&
      attemptSerial && !audio.playback.backendStartTime && generation &&
      generation == audio.playback.requestedGeneration)
    requestSerial = audio.playback.requestSerial;
  else
    attemptSerial = 0;
  LG_UNLOCK(audio.playback.sourceLock);

  if (!attemptSerial)
    return;

  bool started = false;
  LG_LOCK(audio.playback.deviceLock);
  if (audio.audioDev &&
      playbackGetState() == STREAM_STATE_SETUP_DEVICE &&
      atomic_load_explicit(&audio.playback.activeAttemptSerial,
        memory_order_acquire) == attemptSerial &&
      atomic_load_explicit(&audio.playback.streamGeneration,
        memory_order_acquire) == generation)
    started = audio.audioDev->playback.start(
        playbackBackendFailed, attemptSerial);
  LG_UNLOCK(audio.playback.deviceLock);

  LG_LOCK(audio.playback.sourceLock);
  const StreamState completedState = playbackGetState();
  const bool attemptCurrent = attemptSerial == atomic_load_explicit(
      &audio.playback.activeAttemptSerial, memory_order_acquire);
  const bool current = started &&
    attemptCurrent &&
    requestSerial == audio.playback.requestSerial &&
    requestSerial == audio.playback.backendRequestSerial &&
    generation == audio.playback.requestedGeneration &&
    generation == atomic_load_explicit(
      &audio.playback.streamGeneration, memory_order_acquire) &&
    (completedState == STREAM_STATE_SETUP_DEVICE ||
     completedState == STREAM_STATE_RUN);
  const bool keepAlive = started && attemptCurrent &&
    completedState == STREAM_STATE_KEEP_ALIVE;
  if (current || keepAlive)
    audio.playback.backendStartTime = nanotime();
  LG_UNLOCK(audio.playback.sourceLock);

  if (!current && !keepAlive)
    playbackBackendFailed(attemptSerial);
}

static void playbackProcessDiagnostics(void)
{
  PlaybackDiagnostics diagnostics;
  RingBuffer          timings;

  LG_LOCK(audio.playback.sourceLock);
  if (!audio.playback.diagnostics.pending)
  {
    LG_UNLOCK(audio.playback.sourceLock);
    return;
  }

  diagnostics                        = audio.playback.diagnostics;
  timings                            = audio.playback.timings;
  audio.playback.diagnostics.pending = 0;
  LG_UNLOCK(audio.playback.sourceLock);

  bool   current          = false;
  bool   syncCurrent      = false;
  double backendLatencyMs = 0.0;
  LG_LOCK(audio.playback.deviceLock);
  const StreamState state = playbackGetState();
  current = diagnostics.epoch == atomic_load_explicit(
        &audio.playback.diagnosticsEpoch, memory_order_acquire) &&
    state != STREAM_STATE_STOP && state != STREAM_STATE_STOP_PENDING;
  if (current)
  {
    syncCurrent = diagnostics.sync.epoch == atomic_load_explicit(
        &audio.playback.syncDiagnosticsEpoch, memory_order_acquire);
    const bool timingsCurrent = timings &&
      timings == audio.playback.timings;
    if ((diagnostics.pending & PLAYBACK_DIAGNOSTIC_REGISTER_GRAPH) &&
        timingsCurrent && !audio.playback.graph &&
        !audio.playback.graphRegistrationAttempted)
    {
      audio.playback.graphRegistrationAttempted = true;
      audio.playback.graph = app_registerGraph("PLAYBACK RING",
          timings, 0.0f, diagnostics.graphMax, audioGraphFormatFn);
      atomic_store_explicit(&audio.playback.graphReady,
          audio.playback.graph != NULL, memory_order_release);
    }

    if ((diagnostics.pending & PLAYBACK_DIAGNOSTIC_INVALIDATE) &&
        timingsCurrent && audio.playback.graph)
      app_invalidateGraph(audio.playback.graph);

    if ((diagnostics.pending & PLAYBACK_DIAGNOSTIC_SYNC_LOG) &&
        syncCurrent &&
        audio.audioDev && audio.audioDev->playback.latency)
      backendLatencyMs =
        audio.audioDev->playback.latency() / 1000.0;
  }
  LG_UNLOCK(audio.playback.deviceLock);

  if (!current)
    return;

  if (diagnostics.pending & PLAYBACK_DIAGNOSTIC_START_LOG)
    DEBUG_INFO(
        "Audio start: %.2f/%.2f ms target/queued",
        diagnostics.start.targetLatencyMs,
        diagnostics.start.queuedLatencyMs);

  if ((diagnostics.pending & PLAYBACK_DIAGNOSTIC_SYNC_LOG) &&
      syncCurrent &&
      diagnostics.sync.epoch == atomic_load_explicit(
        &audio.playback.syncDiagnosticsEpoch, memory_order_acquire))
  {
    const char * controlName =
      diagnostics.sync.rateControl == PLAYBACK_RATE_PROVIDER ? "feedback" :
      diagnostics.sync.rateControl == PLAYBACK_RATE_BACKEND ?
        "backend" : "software";

    DEBUG_INFO(
        "Audio sync: ring %.2f/%.2f ms, backend %.2f ms, "
        "%s %+.1f ppm, jitter %.2f ms, xruns %u/%u, drop %.2f ms",
        diagnostics.sync.softwareLatencyMs,
        diagnostics.sync.targetLatencyMs,
        backendLatencyMs, controlName, diagnostics.sync.controlPpm,
        diagnostics.sync.jitterMs,
        diagnostics.sync.underruns, diagnostics.sync.overruns,
        diagnostics.sync.backlogTrimmedMs);
  }
}

static void playbackSourceStop(void)
{
  if (!audio.audioDev)
    return;

  StreamState state = playbackGetState();
  switch (state)
  {
    case STREAM_STATE_RUN:
    case STREAM_STATE_RESUMING:
    {
      // Keep the audio device open for a while to reduce startup latency if
      // playback starts again
      if (!atomic_compare_exchange_strong_explicit(
            &audio.playback.state, &state, STREAM_STATE_KEEP_ALIVE,
            memory_order_acq_rel, memory_order_acquire))
        break;

      audio.playback.sourceData.backlogTrimArmed = true;
      playbackResetSyncDiagnosticsLocked();

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

static int playbackStoreVolume(int channels, const uint16_t volume[])
{
  if (!audio.audioDev || !audio.audioDev->playback.volume ||
      !g_params.audioSyncVolume)
    return 0;

  // store the values so we can restore the state if the stream is restarted
  channels = min(ARRAY_LENGTH(audio.playback.volume), channels);
  memcpy(audio.playback.volume, volume, sizeof(uint16_t) * channels);
  audio.playback.volumeChannels = channels;
  return channels;
}

static bool playbackStoreMute(bool mute)
{
  if (!audio.audioDev || !audio.audioDev->playback.mute)
    return false;

  // store the value so we can restore it if the stream is restarted
  audio.playback.mute = mute;
  return true;
}

static double computeDevicePosition(int64_t curTime)
{
  const PlaybackSourceData * sourceData =
    &audio.playback.sourceData;
  return playbackClockPosition(
      &sourceData->deviceClock, curTime) +
    sourceData->devicePositionOffsetFrames;
}

static double playbackProviderRate(const PlaybackSourceData * sourceData)
{
  const double nominalRate = audio.playback.sampleRate;
  if (!sourceData->deviceClock.valid ||
      sourceData->deviceClock.frameSec <= 0.0)
    return nominalRate;

  return clamp(
      sourceData->lastRatio / sourceData->deviceClock.frameSec,
      nominalRate * (1.0 - PLAYBACK_MAX_RATE_CORRECTION),
      nominalRate * (1.0 + PLAYBACK_MAX_RATE_CORRECTION));
}

static bool playbackEnsureConversionBuffers(
    PlaybackSourceData * sourceData, int frames)
{
  if (audio.playback.convertToFloat &&
      frames > sourceData->framesInSize)
  {
    const int capacity = max(frames,
        max(sourceData->framesInSize * 2, 64));
    float * framesIn = realloc(sourceData->framesIn,
        (size_t)capacity * audio.playback.channels * sizeof(float));
    if (!framesIn)
    {
      DEBUG_ERROR("Failed to grow playback input buffer");
      return false;
    }

    sourceData->framesIn = framesIn;
    sourceData->framesInSize = capacity;
  }

  if (audio.playback.rateControl == PLAYBACK_RATE_SOFTWARE)
  {
    const int framesOut =
      (int)ceil(frames * (1.0 + PLAYBACK_MAX_RATE_CORRECTION)) + 64;
    if (framesOut > sourceData->framesOutSize)
    {
      const int capacity = max(framesOut,
          max(sourceData->framesOutSize * 2, 64));
      float * output = realloc(sourceData->framesOut,
          (size_t)capacity * audio.playback.channels * sizeof(float));
      if (!output)
      {
        DEBUG_ERROR("Failed to grow playback output buffer");
        return false;
      }

      sourceData->framesOut = output;
      sourceData->framesOutSize = capacity;
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

static bool playbackUseLowWaterRecovery(bool providerRateControl,
    bool bufferUnderrun, StreamState state,
    const PlaybackSourceData * sourceData)
{
  return providerRateControl ||
    ((!sourceData->deviceClock.valid || !sourceData->deviceClockStable) &&
      (bufferUnderrun || state == STREAM_STATE_KEEP_ALIVE ||
        state == STREAM_STATE_RESUMING));
}

static void playbackResetRateControl(PlaybackSourceData * sourceData,
    bool providerRateControl)
{
  sourceData->offsetError         = 0.0;
  sourceData->offsetErrorIntegral = 0.0;
  sourceData->ratioIntegral       = 0.0;
  sourceData->lastRatio           = providerRateControl ?
    1.0 : sourceData->lastClockRatio;
  if (providerRateControl)
    sourceData->nextFeedbackTime = 0;
}

static PlaybackDataResult playbackData(const void * data, size_t frameCount,
    const LG_AudioClock * sourceClock, int64_t arrivalTime)
{
  StreamState state = playbackGetState();
  if (state == STREAM_STATE_STOP_PENDING)
    return PLAYBACK_DATA_DROP;

  if (frameCount == 0)
    return PLAYBACK_DATA_DROP;
  if (state == STREAM_STATE_STOP || !audio.audioDev)
    return PLAYBACK_DATA_RETRY_NOW;

  PlaybackSourceData * sourceData = &audio.playback.sourceData;
  if (!data || frameCount > (size_t)sourceData->maxSourcePacketFrames)
  {
    DEBUG_ERROR("Invalid playback packet length: %zu frames", frameCount);
    return PLAYBACK_DATA_DROP;
  }
  const int frames = frameCount;

  if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND &&
      atomic_exchange_explicit(
        &audio.playback.backendResamplerFailed, false,
        memory_order_acq_rel))
  {
    DEBUG_WARN("Audio backend resampler failed; using libsamplerate");
    audio.playback.forceSoftwareResampler = true;
    playbackQueueSourceStop();
    return PLAYBACK_DATA_RETRY_NOW;
  }

  /* Backend resampling changes how many source frames PipeWire requests per
   * device period. Use the command-normalized output clock for rate matching,
   * while deviceClock remains in the ring's source-frame domain for latency. */
  const PlaybackClock * rateClock =
    audio.playback.rateControl == PLAYBACK_RATE_BACKEND ?
    &sourceData->outputClock : &sourceData->deviceClock;
  const int64_t now = nanotime();
  if (audio.playback.startFailures &&
      audio.playback.backendStartTime &&
      now - audio.playback.backendStartTime >= AUDIO_RETRY_RESET_NS)
  {
    audio.playback.startFailures  = 0;
    audio.playback.nextStartRetry = 0;
  }
  const double nominalFrameSec = 1.0 / audio.playback.sampleRate;

  const void * inputFrames = data;
  if (audio.playback.convertToFloat)
  {
    if (!audioConvertToFloat(sourceData->framesIn, data,
          (size_t)frames * audio.playback.channels,
          audio.playback.format.sampleFormat))
    {
      DEBUG_ERROR("Failed to convert playback samples");
      playbackQueueSourceStop();
      return PLAYBACK_DATA_RETRY;
    }
    inputFrames = sourceData->framesIn;
  }

  const bool providerRateControl    =
    audio.playback.rateControl == PLAYBACK_RATE_PROVIDER;
  /* An unbounded ring represents an underrun by advancing the reader beyond
   * the writer. Do not carry that logical debt into resumed playback: bounded
   * rate correction would otherwise discard fresh audio for many seconds. */
  const bool bufferUnderrun         =
    STREAM_ACTIVE(playbackGetState()) &&
    ringbuffer_getCount(audio.playback.buffer) < 0;
  bool discontinuity                =
    bufferUnderrun || (sourceClock && sourceClock->discontinuity);
  const int64_t previousArrivalTime = sourceData->lastArrivalTime;
  const int64_t packetTime          =
    playbackMapMediaTime(sourceData, sourceClock, frames,
        audio.playback.sampleRate, arrivalTime, &discontinuity);
  if (sourceData->bufferOverrunPending)
  {
    discontinuity = true;
    sourceData->bufferOverrunPending = false;
  }

  sourceData->lastArrivalTime = arrivalTime;

  const bool sourceRateWasValid  =
    sourceData->sourceRateValid;
  const int64_t sourceRateTimeMs = providerRateControl ?
    (arrivalTime - sourceData->mediaLocalOrigin) / INT64_C(1000000) :
    sourceData->mediaTimeMs;
  playbackSourceRateAdd(
      sourceData, sourceRateTimeMs, nominalFrameSec);
  if (sourceClock && sourceClock->stable && sourceClock->rate > 0.0)
  {
    const double frameSec = 1.0 / sourceClock->rate;
    if (frameSec >= nominalFrameSec *
          (1.0 - PLAYBACK_MAX_RATE_CORRECTION) &&
        frameSec <= nominalFrameSec *
          (1.0 + PLAYBACK_MAX_RATE_CORRECTION))
    {
      if (!sourceData->sourceRateValid ||
          sourceData->rateFilterTimeMs == INT64_MIN)
        sourceData->rateFilterTimeMs = sourceRateTimeMs;
      sourceData->sourceRateFrameSec = frameSec;
      sourceData->sourceRateValid = true;
    }
  }
  const bool sourceRateBecameValid =
    !sourceRateWasValid && sourceData->sourceRateValid;
  if (!playbackSourceClockUpdate(&sourceData->sourceClock,
        packetTime, sourceData->inputPosition, nominalFrameSec))
    discontinuity = true;
  if (sourceData->sourceRateValid && !providerRateControl)
    sourceData->sourceClock.frameSec = sourceData->sourceRateFrameSec;

  /* Track phase variation around its local baseline, not its absolute value.
   * The absolute phase depends on the arbitrary local origin assigned to the
   * source media clock and must not become buffer reserve. Positive
   * deviation means the latency model temporarily overstates how much audio
   * remains in the ring. */
  const double sourcePhaseSec           =
    sourceData->sourceClock.phaseResidualSec;
  const double packetSec                =
    frames * nominalFrameSec;
  const double decayedPacketDurationSec =
    sourceData->sourcePacketDurationSec *
      exp(-packetSec / PLAYBACK_PHASE_RESERVE_DECAY_SEC);
  const bool cadenceReset               = discontinuity ||
    state == STREAM_STATE_KEEP_ALIVE ||
    state == STREAM_STATE_RESUMING;
  if (cadenceReset && sourceData->sourcePacketDurationSec > 0.0)
  {
    sourceData->sourcePacketRecovering = true;
    sourceData->sourcePacketPacedCount = 0;
  }
  else if (sourceData->sourcePacketRecovering)
  {
    if (packetSec <= decayedPacketDurationSec + nominalFrameSec)
    {
      sourceData->sourcePacketRecovering = false;
      sourceData->sourcePacketPacedCount = 0;
    }
    else
    {
      const bool paced = previousArrivalTime != INT64_MIN &&
        arrivalTime > previousArrivalTime &&
        (arrivalTime - previousArrivalTime) * 2.0e-9 >= packetSec;
      sourceData->sourcePacketPacedCount = paced ?
        sourceData->sourcePacketPacedCount + 1 : 0;
      if (sourceData->sourcePacketPacedCount >= 2)
      {
        sourceData->sourcePacketRecovering = false;
        sourceData->sourcePacketPacedCount = 0;
      }
    }
  }

  /* Catch-up packets describe audio accumulated during a gap, not the future
   * delivery cadence. Resume when the prior cadence returns or a larger
   * cadence is confirmed by consecutive plausibly paced packets. */
  sourceData->sourcePacketDurationSec =
    !sourceData->sourcePacketRecovering ?
      max(packetSec, decayedPacketDurationSec) :
      decayedPacketDurationSec;

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
    if (deviceTick.discontinuity != sourceData->deviceDiscontinuity)
    {
      sourceData->deviceDiscontinuity = deviceTick.discontinuity;
      playbackClockReset(&sourceData->deviceClock,
          deviceTick.nextTime, deviceTick.nextPosition, nominalFrameSec);
      if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND)
        playbackClockReset(&sourceData->outputClock,
            deviceTick.nextTime, deviceTick.outputPosition,
            nominalFrameSec);
      playbackDeviceClockAcquireReset(sourceData);
      sourceData->offsetError         = 0.0;
      sourceData->offsetErrorIntegral = 0.0;
      sourceData->ratioIntegral       = 0.0;
      sourceData->lastRatio           = 1.0;
      sourceData->nextFeedbackTime    = 0;
    }
    else if (state == STREAM_STATE_RESUMING &&
        sourceData->deviceClock.valid)
    {
      /* The callback clock continued throughout KEEP_ALIVE. Rebase its phase
       * to the newest published tick while preserving the disciplined rate;
       * feeding the entire idle gap through the short-step filter produces
       * unstable gains. An incomplete acquisition starts over from this tick. */
      const bool monotonic =
        deviceTick.nextTime >= sourceData->deviceClock.time &&
        deviceTick.nextPosition >= sourceData->deviceClock.position &&
        (audio.playback.rateControl != PLAYBACK_RATE_BACKEND ||
          !sourceData->outputClock.valid ||
          (deviceTick.nextTime >= sourceData->outputClock.time &&
            deviceTick.outputPosition >= sourceData->outputClock.position));
      if (!monotonic)
      {
        playbackClockReset(&sourceData->deviceClock,
            deviceTick.nextTime, deviceTick.nextPosition, nominalFrameSec);
        if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND)
          playbackClockReset(&sourceData->outputClock,
              deviceTick.nextTime, deviceTick.outputPosition,
              nominalFrameSec);
        playbackDeviceClockAcquireReset(sourceData);
        discontinuity = true;
      }
      else
      {
        playbackClockRebase(&sourceData->deviceClock,
            deviceTick.nextTime, deviceTick.nextPosition);
        if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND)
        {
          if (sourceData->outputClock.valid)
            playbackClockRebase(&sourceData->outputClock,
                deviceTick.nextTime, deviceTick.outputPosition);
          else
            playbackClockReset(&sourceData->outputClock,
                deviceTick.nextTime, deviceTick.outputPosition,
                nominalFrameSec);
        }
        if (!sourceData->deviceClockStable)
          playbackDeviceClockAcquireReset(sourceData);
      }
    }
    else
    {
      const bool deviceClockUpdated =
        playbackClockUpdate(&sourceData->deviceClock,
            deviceTick.nextTime, deviceTick.nextPosition, nominalFrameSec);
      const bool outputClockUpdated =
        audio.playback.rateControl != PLAYBACK_RATE_BACKEND ||
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
  }

  if (deviceClockBecameStable)
  {
    /* Give the fitted device timeline the same latency reported by the
     * acquisition model. Their position origins are otherwise unrelated, so
     * switching models would create a false phase step and drive the resampler
     * despite an already-correct ring level. Keep the source clock untouched:
     * changing it would also disturb source phase and jitter tracking. */
    const int64_t referenceTime = providerRateControl ? now : curTime;
    const double rawDevicePosition =
      playbackClockPosition(&sourceData->deviceClock, referenceTime);
    sourceData->devicePositionOffsetFrames =
      sourceData->devReadPosition - rawDevicePosition -
        (providerRateControl ? 0.0 : sourceReserveFrames);
  }

  if (discontinuity && sourceData->src)
  {
    const int error = src_reset(sourceData->src);
    if (error)
    {
      DEBUG_ERROR("Failed to reset resampler: %s", src_strerror(error));
      playbackQueueSourceStop();
      return PLAYBACK_DATA_RETRY;
    }
  }

  const int maxPeriodFrames       =
    max(audio.playback.deviceMaxPeriodFrames, sourceData->devPeriodFrames);
  const double backlogGuardFrames =
    max((double)maxPeriodFrames,
        sourceData->sourcePacketDurationSec * audio.playback.sampleRate);
  /* The device period, delivery jitter, packet phase, and resampler delay
   * define the minimum viable latency. Provider feedback directly controls
   * the source rate, so latencyOffset only applies to local rate control. */
  const double latencyOffsetFrames          = providerRateControl ? 0.0 :
    max(g_params.audioLatencyOffset, 0) *
      audio.playback.sampleRate / 1000.0;
  const double arrivalJitterFrames          =
    sourceData->arrivalJitterSec * audio.playback.sampleRate;
  const double arrivalReserveFrames         =
    arrivalJitterFrames + 0.001 * audio.playback.sampleRate;
  const double minimumLowWaterReserveFrames =
    maxPeriodFrames * 0.1 + arrivalReserveFrames;
  const double minimumLowWaterFrames        =
    maxPeriodFrames + minimumLowWaterReserveFrames;
  const double targetLowWaterFrames         =
    minimumLowWaterFrames + latencyOffsetFrames;
  const double minimumBufferFrames          =
    minimumLowWaterFrames + sourceReserveFrames;
  const double targetBufferFrames           =
    minimumBufferFrames + latencyOffsetFrames;
  const double resamplerDelayFrames         =
    audio.playback.rateControl == PLAYBACK_RATE_SOFTWARE ?
      PLAYBACK_RESAMPLER_DELAY_FRAMES : 0.0;
  const double minimumLatencyFrames         =
    minimumBufferFrames + resamplerDelayFrames;
  const double targetLatencyFrames          =
    minimumLatencyFrames + latencyOffsetFrames;

  double devPosition = DBL_MIN;
  state = playbackGetState();
  if (playbackUseLowWaterRecovery(providerRateControl,
        bufferUnderrun, state, sourceData) &&
      (discontinuity ||
       state == STREAM_STATE_KEEP_ALIVE ||
       state == STREAM_STATE_RESUMING))
  {
    const int occupancy = ringbuffer_getCount(audio.playback.buffer);
    const int slewFrames = clamp(
        llrint(targetLowWaterFrames - occupancy),
        (int64_t)INT_MIN, (int64_t)INT_MAX);
    const int actualSlew = playbackSlewBuffer(sourceData, slewFrames);
    sourceData->outputPosition += actualSlew;
    curPosition += actualSlew;

    playbackResetRateControl(sourceData, providerRateControl);
    if (state == STREAM_STATE_KEEP_ALIVE ||
        state == STREAM_STATE_RESUMING)
      atomic_compare_exchange_strong_explicit(
          &audio.playback.state, &state, STREAM_STATE_RUN,
          memory_order_acq_rel, memory_order_acquire);
  }
  else if ((discontinuity ||
            state == STREAM_STATE_KEEP_ALIVE ||
            state == STREAM_STATE_RESUMING) &&
      sourceData->deviceClock.valid &&
      sourceData->deviceClockStable)
  {
    devPosition = computeDevicePosition(curTime);
    const bool   activeUnderrun =
      bufferUnderrun && state == STREAM_STATE_RUN;
    const double clockSlew      =
      devPosition + targetBufferFrames - curPosition;
    double       slew           = clockSlew;
    if (activeUnderrun)
    {
      const int    occupancy    = ringbuffer_getCount(audio.playback.buffer);
      const double physicalSlew =
        ceil(targetLowWaterFrames - occupancy);
      if (physicalSlew > slew)
        slew = physicalSlew;
    }

    const int slewFrames = clamp(llrint(slew), (int64_t)INT_MIN,
        (int64_t)INT_MAX);
    const int actualSlew = playbackSlewBuffer(sourceData, slewFrames);

    /* A negative unbounded ring must be restored in the physical domain even
     * when the fitted clock asks for a smaller correction. Move the clock's
     * phase translation by the excess so the forced refill neither appears as
     * additional latency nor restarts the underrun/slew cycle. */
    const double correction = actualSlew - clockSlew;
    if (activeUnderrun && correction > 0.0)
    {
      sourceData->devicePositionOffsetFrames += correction;
      devPosition                            += correction;
    }

    sourceData->outputPosition += actualSlew;
    curPosition                += actualSlew;

    playbackResetRateControl(sourceData, providerRateControl);
    if (state == STREAM_STATE_KEEP_ALIVE ||
        state == STREAM_STATE_RESUMING)
      atomic_compare_exchange_strong_explicit(
          &audio.playback.state, &state, STREAM_STATE_RUN,
          memory_order_acq_rel, memory_order_acquire);
  }

  double actualLatencyFrames = 0.0;
  double actualOffsetError = 0.0;
  if (providerRateControl)
  {
    const bool trimPending = !sourceData->backlogTrimArmed;
    const int occupancy = ringbuffer_getCount(audio.playback.buffer);
    actualLatencyFrames = occupancy + sourceReserveFrames;
    actualOffsetError   = targetLowWaterFrames - occupancy;

    if (trimPending)
    {
      actualOffsetError                 = 0.0;
      sourceData->offsetError           = 0.0;
      sourceData->offsetErrorIntegral   = 0.0;
    }
    else
    {
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
  }
  else if (sourceData->deviceClock.valid)
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
  if (sourceRateBecameValid && !providerRateControl)
    sourceData->ratioIntegral = 0.0;

  if (!providerRateControl &&
      sourceData->deviceClockStable &&
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
  double controllerBase = providerRateControl ?
    1.0 : sourceData->lastClockRatio;

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

  if (audio.playback.rateControl == PLAYBACK_RATE_BACKEND)
  {
    atomic_store_explicit(
        &audio.playback.backendResampleRatio, ratio,
        memory_order_release);
    const int outputFrames =
      playbackAppendFrames(sourceData, inputFrames, frames);
    sourceData->outputPosition += outputFrames;
  }
  else if (audio.playback.rateControl == PLAYBACK_RATE_PROVIDER)
  {
    const int outputFrames =
      playbackAppendFrames(sourceData, inputFrames, frames);
    sourceData->outputPosition += outputFrames;

    const int occupancy = ringbuffer_getCount(audio.playback.buffer);
    if (!sourceData->backlogTrimArmed &&
        occupancy <= targetLowWaterFrames + backlogGuardFrames)
      sourceData->backlogTrimArmed = true;

    /* Feedback corrects clock drift, but cannot remove audio which is
     * already queued. Ask the device thread to discard the oldest part of a
     * delayed burst instead of replaying stale audio for several seconds. */
    if (playbackGetState() == STREAM_STATE_RUN && !discontinuity &&
        sourceData->backlogTrimArmed &&
        occupancy > targetLowWaterFrames + 2.0 * backlogGuardFrames)
    {
      const int trimTarget = clamp(
          llrint(ceil(targetLowWaterFrames)),
          INT64_C(0), (int64_t)INT_MAX);
      atomic_store_explicit(
          &audio.playback.backlogTrimTarget, trimTarget,
          memory_order_release);
      sourceData->backlogTrimArmed     = false;
      sourceData->offsetError          = 0.0;
      sourceData->offsetErrorIntegral  = 0.0;
      sourceData->ratioIntegral        = 0.0;
      sourceData->lastRatio            = 1.0;
      sourceData->nextFeedbackTime     = 0;
    }
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
        playbackQueueSourceStop();
        return PLAYBACK_DATA_RETRY;
      }

      if (srcData.input_frames_used == 0 && srcData.output_frames_gen == 0)
      {
        DEBUG_ERROR("Resampler made no progress");
        playbackQueueSourceStop();
        return PLAYBACK_DATA_RETRY;
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
      if (g_params.audioDebug)
      {
        PlaybackDiagnostics * diagnostics = playbackDiagnosticsLocked();
        if (audio.playback.timings)
        {
          diagnostics->graphMax =
            targetLatencyFrames * 1000.0 /
            audio.playback.sampleRate * 2;
          diagnostics->pending |=
            PLAYBACK_DIAGNOSTIC_REGISTER_GRAPH;
        }

        diagnostics->start.targetLatencyMs =
          targetLatencyFrames * 1000.0 /
          audio.playback.sampleRate;
        diagnostics->start.queuedLatencyMs =
          audio.playback.targetStartFrames * 1000.0 /
          audio.playback.sampleRate;
        diagnostics->pending |= PLAYBACK_DIAGNOSTIC_START_LOG;
      }

      audio.playback.startupLowWaterFrames =
        startupLowWaterFrames;
      audio.playback.startupPacketPeriod =
        max(llrint(packetSec * 1.0e9), INT64_C(1));
      audio.playback.startupPacketDeadline =
        arrivalTime + audio.playback.startupPacketPeriod;

      playbackSetState(STREAM_STATE_SETUP_DEVICE);
      playbackArmBackend();
      playbackWorkerWake();
    }
  }

  if (!g_params.audioDebug)
    return PLAYBACK_DATA_PROCESSED;

  const double softwareLatencyMs =
    actualLatencyFrames * 1000.0 / audio.playback.sampleRate;
  bool wakeDiagnostics = false;

  if (audio.playback.timings &&
      atomic_load_explicit(
        &audio.playback.graphReady, memory_order_acquire) &&
      now >= sourceData->nextGraphTime)
  {
    sourceData->nextGraphTime = now + PLAYBACK_GRAPH_INTERVAL_NS;
    const float latency = softwareLatencyMs;
    ringbuffer_push(audio.playback.timings, &latency);
    playbackDiagnosticsLocked()->pending |=
      PLAYBACK_DIAGNOSTIC_INVALIDATE;
    wakeDiagnostics = true;
  }

  if (now >= sourceData->nextLogTime)
  {
    const bool providerControl           =
      audio.playback.rateControl == PLAYBACK_RATE_PROVIDER;
    const double controlPpm              = providerControl ?
      (playbackProviderRate(sourceData) /
        audio.playback.sampleRate - 1.0) * 1.0e6 :
      (ratio - 1.0) * 1.0e6;
    const unsigned int underruns         = atomic_exchange_explicit(
        &audio.playback.underruns, 0, memory_order_relaxed);
    PlaybackDiagnostics * diagnostics    = playbackDiagnosticsLocked();
    const unsigned int syncEpoch         = atomic_load_explicit(
        &audio.playback.syncDiagnosticsEpoch, memory_order_acquire);
    const bool pendingSync               =
      (diagnostics->pending & PLAYBACK_DIAGNOSTIC_SYNC_LOG) &&
      diagnostics->sync.epoch == syncEpoch;
    const unsigned int pendingUnderruns  = pendingSync ?
        diagnostics->sync.underruns : 0;
    const unsigned int pendingOverruns   = pendingSync ?
        diagnostics->sync.overruns : 0;
    const double pendingBacklogTrimmedMs = pendingSync ?
        diagnostics->sync.backlogTrimmedMs : 0.0;
    diagnostics->sync = (PlaybackSyncDiagnostics)
    {
      .epoch             = syncEpoch,
      .softwareLatencyMs = softwareLatencyMs,
      .targetLatencyMs   =
        targetLatencyFrames * 1000.0 / audio.playback.sampleRate,
      .controlPpm        = controlPpm,
      .jitterMs          = sourceData->arrivalJitterSec * 1000.0,
      .underruns         = pendingUnderruns + underruns,
      .overruns          = pendingOverruns + sourceData->bufferOverruns,
      .backlogTrimmedMs  = pendingBacklogTrimmedMs +
        atomic_exchange_explicit(
          &audio.playback.backlogTrimmedFrames, 0,
          memory_order_relaxed) * 1000.0 /
          audio.playback.sampleRate,
      .rateControl       = audio.playback.rateControl,
    };
    diagnostics->pending |= PLAYBACK_DIAGNOSTIC_SYNC_LOG;

    sourceData->bufferOverruns = 0;
    sourceData->nextLogTime    = now + INT64_C(5000000000);
    wakeDiagnostics = true;
  }

  if (wakeDiagnostics)
    playbackWorkerWake();

  return PLAYBACK_DATA_PROCESSED;
}

static bool playbackGetFeedback(
    LG_AudioClock * clock, double * targetRate)
{
  PlaybackSourceData * sourceData = &audio.playback.sourceData;
  if (!clock || !targetRate ||
      audio.playback.rateControl != PLAYBACK_RATE_PROVIDER ||
      !sourceData->deviceClock.valid ||
      sourceData->deviceClock.frameSec <= 0.0)
    return false;

  const int64_t now = nanotime();
  if (now < sourceData->nextFeedbackTime)
    return false;

  sourceData->nextFeedbackTime = now + PLAYBACK_FEEDBACK_INTERVAL_NS;
  const double position =
    playbackClockPosition(&sourceData->deviceClock, now);
  if (position < 0.0)
    return false;

  const uint64_t latency = audio.audioDev->playback.latency ?
    audio.audioDev->playback.latency() : 0;

  *clock = (LG_AudioClock)
  {
    .position = llrint(position),
    .time     = now + latency * 1000,
    .rate     = 1.0 / sourceData->deviceClock.frameSec,
    .stable   = sourceData->deviceClockStable,
  };
  *targetRate = playbackProviderRate(sourceData);
  return true;
}

static bool audioBindingEqual(
    const AudioBinding * a, const AudioBinding * b)
{
  return
    a->ops        == b->ops        &&
    a->opaque     == b->opaque     &&
    a->epoch      == b->epoch      &&
    a->generation == b->generation;
}

static void recordWorkerWake(void)
{
  LG_LOCK_SHARED(audio.record.lock);
  if (audio.record.wake && audio.record.thread)
    lgSignalEvent(audio.record.wake);
  LG_UNLOCK_SHARED(audio.record.lock);
}

static void recordDisarmLocked(void)
{
  atomic_store_explicit(
      &audio.record.deliverySerial, 0, memory_order_release);
}

static void recordBackendFailed(uint32_t attemptSerial)
{
  unsigned int expected = attemptSerial;
  if (!attemptSerial || !atomic_compare_exchange_strong_explicit(
        &audio.record.deliverySerial, &expected, 0,
        memory_order_acq_rel, memory_order_acquire))
    return;

  atomic_store_explicit(&audio.record.failedAttemptSerial,
      attemptSerial, memory_order_release);
  lgSignalEvent(audio.record.wake);
}

static void recordAdvanceRequestLocked(void)
{
  if (!++audio.record.requestSerial)
    ++audio.record.requestSerial;
}

static void recordResetStartRetryLocked(void)
{
  audio.record.startFailures  = 0;
  audio.record.nextStartRetry = 0;
}

/* record.lock must be held exclusively. */
static bool recordDelayStartLocked(void)
{
  const unsigned int failures = audio.record.startFailures;
  audio.record.nextStartRetry = nanotime() +
    audioStartRetryDelay(failures);
  if (audio.record.startFailures < 7U)
    ++audio.record.startFailures;

  return failures == 0U || failures == 6U;
}

/* record.lock must be held exclusively. */
static bool recordDelayRequestLocked(
    uint64_t requestSerial, bool resetRetry)
{
  if (audio.record.shuttingDown || !audio.record.requested ||
      !audio.record.enabled || !audio.record.requestedGeneration ||
      requestSerial != audio.record.requestSerial)
    return false;

  if (resetRetry)
    recordResetStartRetryLocked();
  return recordDelayStartLocked();
}

static unsigned int recordWorkerWaitTimeout(void)
{
  unsigned int timeout = TIMEOUT_INFINITE;

  LG_LOCK_SHARED(audio.record.lock);
  if (!audio.record.shuttingDown && audio.record.requested &&
      audio.record.enabled && audio.record.requestedGeneration &&
      audio.record.nextStartRetry)
  {
    const int64_t remaining =
      audio.record.nextStartRetry - nanotime();
    timeout = remaining <= 0 ? 0 :
      (unsigned int)((remaining + INT64_C(999999)) /
          INT64_C(1000000));
  }
  LG_UNLOCK_SHARED(audio.record.lock);

  return timeout;
}

bool lgAudio_supportsRecord(void)
{
  return atomic_load_explicit(&audio.ready, memory_order_acquire) &&
    audio.audioDev && audio.audioDev->record.start &&
    audio.audioDev->record.stop &&
    audio.record.wake && audio.record.thread &&
    atomic_load_explicit(
        &audio.record.workerAlive, memory_order_acquire);
}

static bool recordPushFrames(uint8_t * data, int frames,
    const LG_AudioClock * sourceClock)
{
  if (frames <= 0)
    return true;

  const uint32_t serial = atomic_load_explicit(
      &audio.record.deliverySerial, memory_order_acquire);
  if (!serial)
    return true;

  AudioBinding binding;
  uint32_t generation;

  LG_LOCK_SHARED(audio.record.lock);
  if (serial != atomic_load_explicit(
        &audio.record.deliverySerial, memory_order_acquire))
  {
    LG_UNLOCK_SHARED(audio.record.lock);
    return true;
  }
  binding    = audio.record.deliveryBinding;
  generation = audio.record.deliveryGeneration;
  LG_UNLOCK_SHARED(audio.record.lock);

  AudioBinding active;
  if (!eventBegin(&binding, &active))
    return true;

  bool accepted = true;
  if (serial == atomic_load_explicit(
        &audio.record.deliverySerial, memory_order_acquire) &&
      active.ops->recordData)
    accepted = active.ops->recordData(active.opaque,
        generation, data, frames, sourceClock);
  eventEnd();
  return accepted;
}

static MsgBoxHandle recordCancelConfirmLocked(void)
{
  MsgBoxHandle handle = audio.record.confirmHandle;
  audio.record.confirmHandle = NULL;
  audio.record.confirmPending = false;
  ++audio.record.confirmGeneration;
  return handle;
}

static void recordConfirm(bool yes, void * opaque)
{
  const uint64_t generation = (uint64_t)(uintptr_t)opaque;
  bool wake = false;

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (!audio.record.confirmPending ||
      generation != audio.record.confirmGeneration)
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  audio.record.confirmPending = false;
  audio.record.confirmHandle  = NULL;

  if (yes && audio.record.requested &&
      audio.record.requestedGeneration &&
      !audio.record.shuttingDown && audio.audioDev &&
      audio.record.thread && atomic_load_explicit(
        &audio.record.workerAlive, memory_order_acquire))
  {
    DEBUG_INFO("Microphone access granted");
    audio.record.enabled = true;
    recordResetStartRetryLocked();
    recordAdvanceRequestLocked();
    recordDisarmLocked();
    wake = true;
  }
  else if (yes)
    DEBUG_INFO("Ignoring stale microphone access confirmation");
  else
    DEBUG_INFO("Microphone access denied");

  LG_UNLOCK_EXCLUSIVE(audio.record.lock);
  if (wake)
    recordWorkerWake();
}

static void recordCreateConfirm(
    MsgBoxHandle oldConfirm, uint64_t generation)
{
  app_msgBoxClose(oldConfirm);

  MsgBoxHandle handle = app_confirmMsgBox(
      "Microphone", recordConfirm, (void *)(uintptr_t)generation,
      "An application just opened the microphone!\n"
      "Do you want it to access your microphone?");
  MsgBoxHandle stale = NULL;

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (handle && audio.record.confirmPending &&
      generation == audio.record.confirmGeneration)
    audio.record.confirmHandle = handle;
  else
  {
    stale = handle;
    if (!handle && audio.record.confirmPending &&
        generation == audio.record.confirmGeneration)
    {
      audio.record.confirmPending = false;
      ++audio.record.confirmGeneration;
    }
  }
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  app_msgBoxClose(stale);
}

static void recordStart(const AudioBinding * binding,
    uint32_t generation, const LG_AudioFormat * format)
{
  MsgBoxHandle oldConfirm = NULL;
  uint64_t confirmGeneration = 0;
  bool wake = false;

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (!audio.audioDev || !audio.audioDev->record.start ||
      !audio.audioDev->record.stop ||
      !audio.record.thread || audio.record.shuttingDown ||
      !atomic_load_explicit(
        &audio.record.workerAlive, memory_order_acquire) ||
      !binding || !generation)
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  if (!audioFormatValid(format))
  {
    oldConfirm = recordCancelConfirmLocked();
    audio.record.requested           = false;
    audio.record.enabled             = false;
    audio.record.requestedBinding    = (AudioBinding) { 0 };
    audio.record.requestedGeneration = 0;
    recordResetStartRetryLocked();
    recordAdvanceRequestLocked();
    recordDisarmLocked();
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    app_msgBoxClose(oldConfirm);
    recordWorkerWake();
    DEBUG_ERROR("Invalid recording format");
    return;
  }

  const bool sameRequest = audio.record.requested &&
    generation == audio.record.requestedGeneration &&
    audioBindingEqual(binding, &audio.record.requestedBinding);
  if (sameRequest)
  {
    const bool formatChanged = !audioFormatEqual(
        format, &audio.record.requestedFormat);
    if (formatChanged)
    {
      audio.record.requestedFormat = *format;
      recordResetStartRetryLocked();
      recordAdvanceRequestLocked();
      recordDisarmLocked();
      wake = true;
    }

    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    if (wake)
      recordWorkerWake();
    return;
  }

  const bool keepEnabled = audio.record.requested &&
    audio.record.enabled &&
    audioBindingEqual(binding, &audio.record.requestedBinding);
  wake = true;
  oldConfirm = recordCancelConfirmLocked();
  audio.record.requested           = true;
  audio.record.enabled             = false;
  audio.record.requestedBinding    = *binding;
  audio.record.requestedGeneration = generation;
  audio.record.requestedFormat     = *format;
  recordResetStartRetryLocked();
  recordAdvanceRequestLocked();
  recordDisarmLocked();

  if (keepEnabled)
  {
    audio.record.enabled = true;
    wake = true;
  }
  else if (g_state.micDefaultState == MIC_DEFAULT_DENY)
    DEBUG_INFO("Microphone access denied by default");
  else if (g_state.micDefaultState == MIC_DEFAULT_ALLOW)
  {
    DEBUG_INFO("Microphone access granted by default");
    audio.record.enabled = true;
    wake = true;
  }
  else
  {
    audio.record.confirmPending = true;
    confirmGeneration = ++audio.record.confirmGeneration;
  }

  LG_UNLOCK_EXCLUSIVE(audio.record.lock);
  if (wake)
    recordWorkerWake();
  if (confirmGeneration)
    recordCreateConfirm(oldConfirm, confirmGeneration);
  else
    app_msgBoxClose(oldConfirm);
}

static void recordStop(
    const AudioBinding * binding, uint32_t generation)
{
  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (binding &&
      (!generation || !audio.record.requested ||
       generation != audio.record.requestedGeneration ||
       !audioBindingEqual(binding, &audio.record.requestedBinding)))
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  const bool wasRequested = audio.record.requested;
  audio.record.requested           = false;
  audio.record.enabled             = false;
  audio.record.requestedBinding    = (AudioBinding) { 0 };
  audio.record.requestedGeneration = 0;
  recordResetStartRetryLocked();
  recordAdvanceRequestLocked();
  recordDisarmLocked();
  MsgBoxHandle confirm = recordCancelConfirmLocked();
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  if (wasRequested)
    DEBUG_INFO("Microphone recording stopped");
  app_msgBoxClose(confirm);
  recordWorkerWake();
}

void lgAudio_recordToggleKeybind(int sc, void * opaque)
{
  if (!atomic_load_explicit(&audio.ready, memory_order_acquire))
    return;

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (!audio.audioDev || !audio.record.thread ||
      !atomic_load_explicit(
        &audio.record.workerAlive, memory_order_acquire) ||
      audio.record.shuttingDown)
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  if (!audio.record.requested)
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    app_alert(LG_ALERT_WARNING,
      "No application is requesting microphone access.");
    return;
  }

  MsgBoxHandle confirm = recordCancelConfirmLocked();
  audio.record.enabled = !audio.record.enabled;
  const bool enabled = audio.record.enabled;
  recordResetStartRetryLocked();
  recordAdvanceRequestLocked();
  recordDisarmLocked();
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  DEBUG_INFO("Microphone recording %s by user",
      enabled ? "started" : "stopped");
  app_msgBoxClose(confirm);
  recordWorkerWake();
  app_alert(LG_ALERT_INFO,
      enabled ? "Microphone enabled" : "Microphone disabled");
}

static void recordVolume(const AudioBinding * binding,
    uint32_t generation, int channels, const uint16_t volume[])
{
  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (!audio.audioDev || !audio.audioDev->record.volume ||
      !g_params.audioSyncVolume || audio.record.shuttingDown ||
      !audio.record.requested ||
      generation != audio.record.requestedGeneration ||
      !audioBindingEqual(binding, &audio.record.requestedBinding))
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  channels = min(ARRAY_LENGTH(audio.record.volume), channels);
  memcpy(audio.record.volume, volume, sizeof(uint16_t) * channels);
  audio.record.volumeChannels = channels;
  if (!++audio.record.controlSerial)
    ++audio.record.controlSerial;
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  recordWorkerWake();
}

static void recordMute(
    const AudioBinding * binding, uint32_t generation, bool mute)
{
  LG_LOCK_EXCLUSIVE(audio.record.lock);
  if (!audio.audioDev || !audio.audioDev->record.mute ||
      audio.record.shuttingDown || !audio.record.requested ||
      generation != audio.record.requestedGeneration ||
      !audioBindingEqual(binding, &audio.record.requestedBinding))
  {
    LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    return;
  }

  audio.record.mute = mute;
  if (!++audio.record.controlSerial)
    ++audio.record.controlSerial;
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  recordWorkerWake();
}

static bool recordRequestCurrentLocked(uint64_t requestSerial,
    const AudioBinding * binding, uint32_t generation,
    const LG_AudioFormat * format)
{
  return !audio.record.shuttingDown && audio.record.requested &&
    audio.record.enabled &&
    requestSerial == audio.record.requestSerial &&
    generation == audio.record.requestedGeneration &&
    audioBindingEqual(binding, &audio.record.requestedBinding) &&
    audioFormatEqual(format, &audio.record.requestedFormat);
}

static uint32_t recordArmLocked(
    const AudioBinding * binding, uint32_t generation)
{
  if (!++audio.record.nextAttemptSerial)
    ++audio.record.nextAttemptSerial;

  atomic_store_explicit(
      &audio.record.failedAttemptSerial, 0, memory_order_release);
  audio.record.deliveryBinding    = *binding;
  audio.record.deliveryGeneration = generation;
  atomic_store_explicit(&audio.record.deliverySerial,
      audio.record.nextAttemptSerial, memory_order_release);
  return audio.record.nextAttemptSerial;
}

static int recordThread(void * opaque)
{
  bool     backendStarted        = false;
  bool     controlsApplied       = false;
  uint64_t backendRequestSerial  = 0;
  uint32_t backendAttemptSerial  = 0;
  int64_t  backendStartTime      = 0;
  uint64_t appliedControlSerial  = 0;

  for (;;)
  {
    const unsigned int timeout = recordWorkerWaitTimeout();
    if (!lgWaitEvent(audio.record.wake, timeout) &&
        timeout == TIMEOUT_INFINITE)
    {
      DEBUG_ERROR("Failed to wait for audio recording work");
      break;
    }

    for (;;)
    {
      enum
      {
        RECORD_ACTION_WAIT,
        RECORD_ACTION_START,
        RECORD_ACTION_STOP,
        RECORD_ACTION_CONTROLS,
        RECORD_ACTION_EXIT,
      }
      action = RECORD_ACTION_WAIT;

      uint64_t       requestSerial      = 0;
      uint32_t       attemptSerial      = 0;
      uint64_t       controlSerial      = 0;
      AudioBinding   binding            = { 0 };
      uint32_t       generation         = 0;
      LG_AudioFormat format             = { 0 };
      int            volumeChannels     = 0;
      uint16_t       volume[LG_AUDIO_MAX_CHANNELS];
      bool           mute               = false;
      bool           reportStartFailure = false;
      bool           runtimeFailure     = false;
      bool           resetStartRetry    = false;

      LG_LOCK_EXCLUSIVE(audio.record.lock);
      const bool desired = !audio.record.shuttingDown &&
        audio.record.requested && audio.record.enabled &&
        audio.record.requestedGeneration && audio.audioDev &&
        audio.audioDev->record.start && audio.audioDev->record.stop;

      const bool backendFailed = backendStarted &&
        backendAttemptSerial == atomic_load_explicit(
            &audio.record.failedAttemptSerial, memory_order_acquire);
      if (backendStarted &&
          (backendFailed || !desired ||
           backendRequestSerial != audio.record.requestSerial))
      {
        recordDisarmLocked();
        if (backendFailed && desired &&
            backendRequestSerial == audio.record.requestSerial)
        {
          requestSerial   = backendRequestSerial;
          runtimeFailure  = true;
          resetStartRetry =
            nanotime() - backendStartTime >= AUDIO_RETRY_RESET_NS;
        }
        action = RECORD_ACTION_STOP;
      }
      else if (!backendStarted && desired &&
          nanotime() >= audio.record.nextStartRetry)
      {
        requestSerial = audio.record.requestSerial;
        binding       = audio.record.requestedBinding;
        generation    = audio.record.requestedGeneration;
        format        = audio.record.requestedFormat;
        attemptSerial = recordArmLocked(&binding, generation);
        action = RECORD_ACTION_START;
      }
      else if (backendStarted &&
          (!controlsApplied ||
           appliedControlSerial != audio.record.controlSerial))
      {
        requestSerial  = backendRequestSerial;
        controlSerial  = audio.record.controlSerial;
        volumeChannels = audio.record.volumeChannels;
        if (volumeChannels)
          memcpy(volume, audio.record.volume,
              sizeof(*volume) * volumeChannels);
        mute = audio.record.mute;
        action = RECORD_ACTION_CONTROLS;
      }
      else if (audio.record.shuttingDown)
        action = RECORD_ACTION_EXIT;

      LG_UNLOCK_EXCLUSIVE(audio.record.lock);

      if (action == RECORD_ACTION_WAIT)
        break;

      if (action == RECORD_ACTION_EXIT)
        goto exit;

      if (action == RECORD_ACTION_STOP)
      {
        if (audio.audioDev && audio.audioDev->record.stop)
          audio.audioDev->record.stop();

        if (runtimeFailure)
        {
          LG_LOCK_EXCLUSIVE(audio.record.lock);
          reportStartFailure = recordDelayRequestLocked(
              requestSerial, resetStartRetry);
          LG_UNLOCK_EXCLUSIVE(audio.record.lock);
        }

        backendStarted       = false;
        controlsApplied      = false;
        backendRequestSerial = 0;
        backendAttemptSerial = 0;
        backendStartTime     = 0;

        if (g_params.micShowIndicator)
          app_showRecord(false);
        if (runtimeFailure && reportStartFailure)
          DEBUG_ERROR("Audio recording device failed; retrying");
        continue;
      }

      if (action == RECORD_ACTION_START)
      {
        const bool started = audio.audioDev->record.start(
            &format, recordPushFrames,
            recordBackendFailed, attemptSerial);

        LG_LOCK_EXCLUSIVE(audio.record.lock);
        const bool current = recordRequestCurrentLocked(
            requestSerial, &binding, generation, &format);
        const bool failed = attemptSerial == atomic_load_explicit(
            &audio.record.failedAttemptSerial, memory_order_acquire);
        const bool delayAfterStop = started && current && failed;
        if (started && current && !failed)
        {
          backendStarted              = true;
          backendRequestSerial        = requestSerial;
          backendAttemptSerial        = attemptSerial;
          backendStartTime            = nanotime();
          controlsApplied             = false;
          audio.record.nextStartRetry = 0;
        }
        else
        {
          if (attemptSerial == atomic_load_explicit(
                &audio.record.deliverySerial, memory_order_relaxed))
            recordDisarmLocked();
          if (current && !started)
            reportStartFailure = recordDelayStartLocked();
        }
        LG_UNLOCK_EXCLUSIVE(audio.record.lock);

        if (started && (!current || failed))
          audio.audioDev->record.stop();

        if (delayAfterStop)
        {
          LG_LOCK_EXCLUSIVE(audio.record.lock);
          if (recordRequestCurrentLocked(
                requestSerial, &binding, generation, &format))
            reportStartFailure = recordDelayStartLocked();
          LG_UNLOCK_EXCLUSIVE(audio.record.lock);
        }
        else if (started && current && g_params.micShowIndicator)
          app_showRecord(true);
        if (current && (!started || failed) && reportStartFailure)
          DEBUG_ERROR("Failed to start audio recording device; retrying");
        continue;
      }

      if (volumeChannels && audio.audioDev->record.volume)
        audio.audioDev->record.volume(volumeChannels, volume);
      if (audio.audioDev->record.mute)
        audio.audioDev->record.mute(mute);

      LG_LOCK_EXCLUSIVE(audio.record.lock);
      if (backendStarted &&
          requestSerial == backendRequestSerial &&
          requestSerial == audio.record.requestSerial)
      {
        controlsApplied      = true;
        appliedControlSerial = controlSerial;
      }
      LG_UNLOCK_EXCLUSIVE(audio.record.lock);
    }
  }

exit:
  if (backendStarted && audio.audioDev && audio.audioDev->record.stop)
    audio.audioDev->record.stop();

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  recordDisarmLocked();
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);
  if (backendStarted && g_params.micShowIndicator)
    app_showRecord(false);
  atomic_store_explicit(
      &audio.record.workerAlive, false, memory_order_release);
  return 0;
}

static bool bindingActiveNL(const AudioBinding * binding)
{
  return binding->ops && audioBindingEqual(&audio.active, binding);
}

static bool eventBegin(
    const AudioBinding * binding, AudioBinding * active)
{
  LG_LOCK_SHARED(audio.activeLock);
  const bool current = bindingActiveNL(binding);
  if (current)
  {
    *active = audio.active;
    atomic_fetch_add_explicit(
        &audio.activeCallbacks, 1, memory_order_acq_rel);
  }
  LG_UNLOCK_SHARED(audio.activeLock);
  return current;
}

static void eventEnd(void)
{
  const unsigned int previous = atomic_fetch_sub_explicit(
      &audio.activeCallbacks, 1, memory_order_release);
  if ((previous & ACTIVE_CALLBACK_WAITING) &&
      (previous & ACTIVE_CALLBACK_COUNT_MASK) == 1)
    lgSignalEvent(audio.activeIdle);
}

static void waitForActiveCallbacks(void)
{
  atomic_fetch_or_explicit(
      &audio.activeCallbacks, ACTIVE_CALLBACK_WAITING,
      memory_order_acq_rel);
  while ((atomic_load_explicit(
           &audio.activeCallbacks, memory_order_acquire) &
         ACTIVE_CALLBACK_COUNT_MASK) != 0)
    lgWaitEvent(audio.activeIdle, TIMEOUT_INFINITE);
  atomic_fetch_and_explicit(
      &audio.activeCallbacks, ~ACTIVE_CALLBACK_WAITING,
      memory_order_release);
}

static void queueFeedback(const LG_AudioOps * ops, void * opaque,
    uint32_t bindingEpoch, uint32_t bindingGeneration,
    uint32_t generation,
    const LG_AudioClock * clock, double targetRate)
{
  if (!audio.feedback.wakeInitialized || !audio.feedback.thread || !clock)
    return;

  LG_LOCK(audio.feedback.lock);
  audio.feedback.ops               = ops;
  audio.feedback.opaque            = opaque;
  audio.feedback.bindingEpoch      = bindingEpoch;
  audio.feedback.bindingGeneration = bindingGeneration;
  audio.feedback.generation        = generation;
  audio.feedback.clock             = *clock;
  audio.feedback.targetRate        = targetRate;
  audio.feedback.pending           = true;
  LG_UNLOCK(audio.feedback.lock);
  feedbackWorkerWake();
}

static void eventPlaybackStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format, const LG_AudioClock * sourceClock)
{
  AudioBinding * binding = opaque;
  AudioBinding active;
  (void)sourceClock;

  if (!eventBegin(binding, &active))
    return;

  const bool providerRateControl =
    active.ops->clockFeedback &&
    audio.feedback.wakeInitialized && audio.feedback.thread;
  bool       wake                = false;

  LG_LOCK(audio.playback.sourceLock);
  ++audio.playback.requestSerial;
  audio.playback.requestedGeneration          = generation;
  audio.playback.requestedFormatValid         =
    generation && audioFormatValid(format);
  audio.playback.requestedProviderRateControl = providerRateControl;
  audio.playback.forceSoftwareResampler       = false;
  audio.playback.startPending                 = false;
  audio.playback.startFailures                = 0;
  audio.playback.nextStartRetry               = 0;
  atomic_store_explicit(
      &audio.playback.streamGeneration, 0, memory_order_release);

  if (audio.playback.requestedFormatValid)
  {
    audio.playback.requestedFormat = *format;
    bool resumed = false;
    if (!audio.playback.startInProgress &&
        playbackGetState() == STREAM_STATE_KEEP_ALIVE &&
        audio.playback.backendStartTime != 0)
    {
      LG_LOCK(audio.playback.deviceLock);
      resumed = playbackResume(format, NULL, providerRateControl,
          false);
      if (resumed)
      {
        if (atomic_load_explicit(&audio.playback.activeAttemptSerial,
              memory_order_acquire))
          audio.playback.backendRequestSerial =
            audio.playback.requestSerial;
        atomic_store_explicit(&audio.playback.streamGeneration,
            generation, memory_order_release);
        audio.playback.controlsPending = true;
        audio.playback.nextStartRetry  = 0;
        wake                           = true;
      }
      LG_UNLOCK(audio.playback.deviceLock);
    }

    if (resumed)
      audio.playback.startPending = false;
    else if (audio.playback.startInProgress)
    {
      audio.playback.startPending = true;
      playbackWorkerWake();
    }
    else
      playbackScheduleStart();
  }
  else
  {
    if (generation)
      DEBUG_ERROR("Invalid playback format");
    if (playbackGetState() != STREAM_STATE_STOP)
    {
      LG_LOCK(audio.playback.deviceLock);
      playbackStop();
      LG_UNLOCK(audio.playback.deviceLock);
    }
    audio.playback.requestedGeneration = 0;
  }
  LG_UNLOCK(audio.playback.sourceLock);
  if (wake)
    playbackWorkerWake();
  eventEnd();
}

static void eventPlaybackStop(void * opaque, uint32_t generation)
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  LG_LOCK(audio.playback.sourceLock);
  if (audio.playback.requestedGeneration == generation)
  {
    ++audio.playback.requestSerial;
    if (atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
    {
      LG_LOCK(audio.playback.deviceLock);
      playbackSourceStop();
      LG_UNLOCK(audio.playback.deviceLock);
    }
    audio.playback.requestedGeneration          = 0;
    audio.playback.requestedFormatValid         = false;
    audio.playback.requestedProviderRateControl = false;
    audio.playback.forceSoftwareResampler       = false;
    audio.playback.startPending                 = false;
    audio.playback.startFailures                = 0;
    audio.playback.nextStartRetry               = 0;
    atomic_store_explicit(
        &audio.playback.streamGeneration, 0, memory_order_release);
  }
  LG_UNLOCK(audio.playback.sourceLock);
  eventEnd();
}

static void eventPlaybackVolume(void * opaque, uint32_t generation,
    uint8_t channels, const uint16_t volume[])
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  bool wake = false;
  LG_LOCK(audio.playback.sourceLock);
  if (volume &&
      audio.playback.requestedGeneration == generation)
  {
    const int storedChannels = playbackStoreVolume(channels, volume);
    if (storedChannels)
    {
      if (!++audio.playback.controlSerial)
        ++audio.playback.controlSerial;
      audio.playback.controlsPending = true;
      wake = atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation;
    }
  }
  LG_UNLOCK(audio.playback.sourceLock);
  if (wake)
    playbackWorkerWake();
  eventEnd();
}

static void eventPlaybackMute(void * opaque, uint32_t generation, bool mute)
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  bool wake = false;
  LG_LOCK(audio.playback.sourceLock);
  if (audio.playback.requestedGeneration == generation)
  {
    const bool stored = playbackStoreMute(mute);
    if (stored)
    {
      if (!++audio.playback.controlSerial)
        ++audio.playback.controlSerial;
      audio.playback.controlsPending = true;
      wake = atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation;
    }
  }
  LG_UNLOCK(audio.playback.sourceLock);
  if (wake)
    playbackWorkerWake();
  eventEnd();
}

static void eventPlaybackData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  const int64_t arrivalTime = nanotime();
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  LG_LOCK(audio.playback.sourceLock);
  if (audio.playback.requestedGeneration == generation)
  {
    const uint32_t liveGeneration = atomic_load_explicit(
        &audio.playback.streamGeneration, memory_order_acquire);
    PlaybackDataResult result = PLAYBACK_DATA_DROP;
    if (liveGeneration == generation)
    {
      result = playbackData(data, frames, sourceClock, arrivalTime);

      if (result == PLAYBACK_DATA_PROCESSED &&
          atomic_load_explicit(&audio.playback.streamGeneration,
            memory_order_acquire) != generation)
        result = PLAYBACK_DATA_DROP;

      if (result == PLAYBACK_DATA_RETRY ||
          result == PLAYBACK_DATA_RETRY_NOW)
      {
        atomic_store_explicit(
            &audio.playback.streamGeneration, 0, memory_order_release);
        if (result == PLAYBACK_DATA_RETRY_NOW)
          audio.playback.nextStartRetry = 0;
        else
          playbackDelayStart();
      }
    }

    if (result != PLAYBACK_DATA_PROCESSED)
      playbackScheduleStart();

    LG_AudioClock feedback;
    double targetRate;
    if (result == PLAYBACK_DATA_PROCESSED &&
        active.ops->clockFeedback &&
        playbackGetFeedback(&feedback, &targetRate))
      queueFeedback(active.ops, active.opaque, active.epoch,
          active.generation, generation, &feedback, targetRate);
  }
  LG_UNLOCK(audio.playback.sourceLock);
  eventEnd();
}

static void eventRecordStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format)
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  if (active.ops->recordData)
    recordStart(&active, generation, format);
  eventEnd();
}

static void eventRecordStop(void * opaque, uint32_t generation)
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  recordStop(&active, generation);
  eventEnd();
}

static void eventRecordVolume(void * opaque, uint32_t generation,
    uint8_t channels, const uint16_t volume[])
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  if (volume)
    recordVolume(&active, generation, channels, volume);
  eventEnd();
}

static void eventRecordMute(void * opaque, uint32_t generation, bool mute)
{
  AudioBinding * binding = opaque;
  AudioBinding active;

  if (!eventBegin(binding, &active))
    return;

  recordMute(&active, generation, mute);
  eventEnd();
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
  uint32_t epoch = 0;
  if (ops)
  {
    epoch = ++audio.nextBindingEpoch;
    if (!epoch)
      epoch = ++audio.nextBindingEpoch;
  }

  return (AudioBinding)
  {
    .ops        = ops,
    .opaque     = opaque,
    .available  = ops && !ops->setStatusListener,
    .epoch      = epoch,
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
  {
    LG_LOCK(audio.playback.deviceLock);
    playbackStop();
    LG_UNLOCK(audio.playback.deviceLock);
  }
  ++audio.playback.requestSerial;
  audio.playback.requestedGeneration          = 0;
  audio.playback.requestedFormatValid         = false;
  audio.playback.requestedProviderRateControl = false;
  audio.playback.forceSoftwareResampler       = false;
  audio.playback.startPending                 = false;
  audio.playback.startFailures                = 0;
  audio.playback.nextStartRetry               = 0;
  atomic_store_explicit(
      &audio.playback.streamGeneration, 0, memory_order_release);
  LG_UNLOCK(audio.playback.sourceLock);

  recordStop(NULL, 0);
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
        old.epoch == next.epoch &&
        old.generation == next.generation)
    {
      audio.active = next;
      LG_UNLOCK_EXCLUSIVE(audio.activeLock);
      return;
    }
    audio.active = (AudioBinding) { 0 };
    LG_UNLOCK_EXCLUSIVE(audio.activeLock);

    waitForActiveCallbacks();

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

    LG_LOCK_EXCLUSIVE(audio.activeLock);
    if (audio.active.ops == next.ops &&
        audio.active.opaque == next.opaque &&
        audio.active.epoch == next.epoch &&
        audio.active.generation == next.generation)
      audio.active = (AudioBinding) { 0 };
    if (slot->ops == next.ops && slot->opaque == next.opaque)
      slot->available = false;
    LG_UNLOCK_EXCLUSIVE(audio.activeLock);

    waitForActiveCallbacks();
    next.ops->detach(next.opaque);
    stopStreams();

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

/* bindingLock must be held. */
static void setBindingLocked(AudioBinding * target, const LG_AudioOps * ops,
    void * opaque, LG_AudioStatusFn statusFn)
{
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

static void setBinding(AudioBinding * target, const LG_AudioOps * ops,
    void * opaque, LG_AudioStatusFn statusFn)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid audio operations");
    ops    = NULL;
    opaque = NULL;
  }

  LG_LOCK(audio.bindingLock);
  if (atomic_load_explicit(&audio.ready, memory_order_acquire))
    setBindingLocked(target, ops, opaque, statusFn);
  LG_UNLOCK(audio.bindingLock);
}

static int playbackThread(void * opaque)
{
  for (;;)
  {
    int result;
    do
      result = sem_wait(&audio.playback.worker.wake);
    while (result < 0 && errno == EINTR);
    if (result < 0)
      break;

    atomic_store_explicit(
        &audio.playback.worker.wakePending, false, memory_order_release);
    if (atomic_load_explicit(
          &audio.playback.worker.stop, memory_order_acquire))
      break;

    playbackProcessStopPending();
    playbackProcessStart();
    playbackProcessControls();
    playbackProcessDeviceStart();
    playbackProcessDiagnostics();
  }

  return 0;
}

static void playbackFeedbackFailed(uint32_t generation)
{
  bool restart = false;

  LG_LOCK(audio.playback.sourceLock);
  if (generation &&
      audio.playback.requestedGeneration == generation &&
      audio.playback.requestedProviderRateControl &&
      audio.playback.rateControl == PLAYBACK_RATE_PROVIDER &&
      atomic_load_explicit(&audio.playback.streamGeneration,
        memory_order_acquire) == generation)
  {
    ++audio.playback.requestSerial;
    audio.playback.requestedProviderRateControl = false;
    audio.playback.forceSoftwareResampler       = false;
    audio.playback.startPending                 = true;
    audio.playback.startFailures                = 0;
    audio.playback.nextStartRetry               = 0;
    playbackQueueSourceStop();
    restart = true;
  }
  LG_UNLOCK(audio.playback.sourceLock);

  if (restart)
    DEBUG_WARN("Audio feedback stopped; using local rate control");
}

static int feedbackThread(void * opaque)
{
  AudioBinding rejectedBinding    = { 0 };
  uint32_t     rejectedGeneration = 0;
  int64_t      rejectedSince      = 0;

  for (;;)
  {
    int result;
    do
      result = sem_wait(&audio.feedback.wake);
    while (result < 0 && errno == EINTR);
    if (result < 0)
      break;

    atomic_store_explicit(
        &audio.feedback.wakePending, false, memory_order_release);
    if (atomic_load_explicit(
          &audio.feedback.stop, memory_order_acquire))
      break;

    const LG_AudioOps * ops;
    void              * providerOpaque;
    uint32_t            bindingEpoch;
    uint32_t            bindingGeneration;
    uint32_t            generation;
    LG_AudioClock       clock;
    double              targetRate;

    LG_LOCK(audio.feedback.lock);
    const bool pending     = audio.feedback.pending;
    ops                    = audio.feedback.ops;
    providerOpaque         = audio.feedback.opaque;
    bindingEpoch           = audio.feedback.bindingEpoch;
    bindingGeneration      = audio.feedback.bindingGeneration;
    generation             = audio.feedback.generation;
    clock                  = audio.feedback.clock;
    targetRate             = audio.feedback.targetRate;
    audio.feedback.pending = false;
    LG_UNLOCK(audio.feedback.lock);

    if (!pending)
      continue;

    const AudioBinding binding =
    {
      .ops        = ops,
      .opaque     = providerOpaque,
      .epoch      = bindingEpoch,
      .generation = bindingGeneration,
    };
    AudioBinding active;
    if (!eventBegin(&binding, &active))
    {
      rejectedSince = 0;
      continue;
    }

    bool attempted = false;
    bool accepted  = false;
    if (active.ops->clockFeedback &&
        atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == generation)
    {
      attempted = true;
      accepted = active.ops->clockFeedback(
          active.opaque, generation, &clock, targetRate);
    }

    if (!attempted || accepted)
    {
      rejectedSince = 0;
      eventEnd();
      continue;
    }

    const int64_t now = nanotime();
    if (!rejectedSince ||
        !audioBindingEqual(&rejectedBinding, &binding) ||
        rejectedGeneration != generation)
    {
      rejectedBinding    = binding;
      rejectedGeneration = generation;
      rejectedSince      = now;
      eventEnd();
      continue;
    }

    if (now - rejectedSince >= PLAYBACK_FEEDBACK_FAILURE_NS)
    {
      rejectedSince = 0;
      playbackFeedbackFailed(generation);
    }
    eventEnd();
  }

  return 0;
}

void lgAudio_init(void)
{
  atomic_init(&audio.ready, false);
  LG_LOCK_INIT(audio.bindingLock);
  LG_LOCK_INIT(audio.providerLock);
  LG_RWLOCK_INIT(audio.activeLock);
  atomic_init(&audio.activeCallbacks, 0);
  LG_LOCK_INIT(audio.playback.sourceLock);
  LG_LOCK_INIT(audio.playback.deviceLock);
  LG_RWLOCK_INIT(audio.record.lock);
  LG_LOCK_INIT(audio.feedback.lock);
  audio.record.shuttingDown = false;
  atomic_init(&audio.playback.streamGeneration, 0);
  atomic_init(&audio.playback.activeAttemptSerial, 0);
  atomic_init(&audio.playback.failedAttemptSerial, 0);
  atomic_init(&audio.playback.diagnosticsEpoch, 1);
  atomic_init(&audio.playback.syncDiagnosticsEpoch, 1);
  atomic_init(&audio.playback.graphReady, false);
  atomic_flag_clear(&audio.playback.deviceStateGate);
  atomic_init(&audio.playback.backlogTrimTarget, -1);
  atomic_init(&audio.playback.backlogTrimmedFrames, 0);
  atomic_init(&audio.playback.deviceTiming.discontinuity, 0);
  atomic_init(&audio.playback.worker.stop, false);
  atomic_init(&audio.playback.worker.wakePending, false);
  atomic_init(&audio.record.deliverySerial, 0);
  atomic_init(&audio.record.failedAttemptSerial, 0);
  atomic_init(&audio.record.workerAlive, false);
  atomic_init(&audio.feedback.stop, false);
  atomic_init(&audio.feedback.wakePending, false);
  atomic_store_explicit(
      &audio.playback.callbackState, PLAYBACK_CALLBACK_DISABLED,
      memory_order_release);

  if (sem_init(&audio.playback.callbackIdle, 0, 0) != 0)
  {
    DEBUG_ERROR("Failed to create the audio callback semaphore");
    return;
  }
  audio.playback.callbackIdleInitialized = true;

  audio.activeIdle = lgCreateEvent(true, 0);
  if (!audio.activeIdle)
  {
    DEBUG_ERROR("Failed to create the audio provider event");
    goto err_callback;
  }

  if (sem_init(&audio.playback.worker.wake, 0, 0) != 0)
  {
    DEBUG_ERROR("Failed to create the audio playback semaphore");
    goto err_active;
  }
  audio.playback.worker.wakeInitialized = true;
  if (!lgCreateThread("audioPlayback", playbackThread,
        NULL, &audio.playback.worker.thread))
  {
    DEBUG_ERROR("Failed to create the audio playback thread");
    goto err_playbackWake;
  }

  if (sem_init(&audio.feedback.wake, 0, 0) != 0)
  {
    DEBUG_ERROR("Failed to create the audio feedback semaphore");
    goto err_playbackThread;
  }
  audio.feedback.wakeInitialized = true;
  if (!lgCreateThread("audioFeedback", feedbackThread,
        NULL, &audio.feedback.thread))
  {
    DEBUG_ERROR("Failed to create the audio feedback thread");
    goto err_feedbackWake;
  }

  audio.record.wake = lgCreateEvent(true, 0);
  if (!audio.record.wake)
    DEBUG_ERROR("Failed to create the audio recording event");
  else
  {
    atomic_store_explicit(
        &audio.record.workerAlive, true, memory_order_release);
    if (!lgCreateThread("audioRecord", recordThread,
          NULL, &audio.record.thread))
    {
      atomic_store_explicit(
          &audio.record.workerAlive, false, memory_order_release);
      DEBUG_ERROR("Failed to create the audio recording thread");
      lgFreeEvent(audio.record.wake);
      audio.record.wake = NULL;
    }
  }

  for (int i = 0; i < LG_AUDIODEV_COUNT; ++i)
    if (LG_AudioDevs[i]->init())
    {
      audio.audioDev = LG_AudioDevs[i];
      atomic_store_explicit(&audio.ready, true, memory_order_release);
      DEBUG_INFO("Using AudioDev: %s", audio.audioDev->name);
      return;
    }

  DEBUG_WARN("Failed to initialize an audio backend");
  return;

err_feedbackWake:
  sem_destroy(&audio.feedback.wake);
  audio.feedback.wakeInitialized = false;

err_playbackThread:
  atomic_store_explicit(
      &audio.playback.worker.stop, true, memory_order_release);
  playbackWorkerWake();
  lgJoinThread(audio.playback.worker.thread, NULL);
  audio.playback.worker.thread = NULL;

err_playbackWake:
  sem_destroy(&audio.playback.worker.wake);
  audio.playback.worker.wakeInitialized = false;

err_active:
  lgFreeEvent(audio.activeIdle);
  audio.activeIdle = NULL;

err_callback:
  sem_destroy(&audio.playback.callbackIdle);
  audio.playback.callbackIdleInitialized = false;
}

void lgAudio_free(void)
{
  LG_LOCK(audio.bindingLock);
  const bool ready = atomic_exchange_explicit(
      &audio.ready, false, memory_order_acq_rel);
  if (ready)
  {
    setBindingLocked(
        &audio.fallback, NULL, NULL, fallbackStatusChanged);
    setBindingLocked(
        &audio.transport, NULL, NULL, transportStatusChanged);
  }
  LG_UNLOCK(audio.bindingLock);
  stopStreams();

  if (audio.playback.worker.thread)
  {
    atomic_store_explicit(
        &audio.playback.worker.stop, true, memory_order_release);
    playbackWorkerWake();
    lgJoinThread(audio.playback.worker.thread, NULL);
    audio.playback.worker.thread = NULL;
  }
  if (audio.playback.worker.wakeInitialized)
  {
    sem_destroy(&audio.playback.worker.wake);
    audio.playback.worker.wakeInitialized = false;
  }

  if (audio.feedback.thread)
  {
    atomic_store_explicit(
        &audio.feedback.stop, true, memory_order_release);
    feedbackWorkerWake();
    lgJoinThread(audio.feedback.thread, NULL);
    audio.feedback.thread = NULL;
  }
  if (audio.feedback.wakeInitialized)
  {
    sem_destroy(&audio.feedback.wake);
    audio.feedback.wakeInitialized = false;
  }

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  audio.record.shuttingDown = true;
  audio.record.requested    = false;
  audio.record.enabled      = false;
  recordAdvanceRequestLocked();
  recordDisarmLocked();
  MsgBoxHandle confirm = recordCancelConfirmLocked();
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);

  app_msgBoxClose(confirm);
  if (audio.record.thread)
  {
    recordWorkerWake();
    lgJoinThread(audio.record.thread, NULL);
  }

  LG_LOCK_EXCLUSIVE(audio.record.lock);
  LGEvent * recordWake = audio.record.wake;
  audio.record.thread = NULL;
  audio.record.wake   = NULL;
  LG_UNLOCK_EXCLUSIVE(audio.record.lock);
  if (recordWake)
    lgFreeEvent(recordWake);

  if (audio.playback.callbackIdleInitialized)
  {
    sem_destroy(&audio.playback.callbackIdle);
    audio.playback.callbackIdleInitialized = false;
  }
  if (audio.activeIdle)
  {
    lgFreeEvent(audio.activeIdle);
    audio.activeIdle = NULL;
  }

  struct LG_AudioDevOps * audioDev = audio.audioDev;
  audio.audioDev = NULL;
  if (audioDev)
    audioDev->free();

  LG_RWLOCK_FREE(audio.activeLock);
  LG_LOCK_FREE(audio.playback.sourceLock);
  LG_LOCK_FREE(audio.playback.deviceLock);
  LG_LOCK_FREE(audio.bindingLock);
  LG_LOCK_FREE(audio.providerLock);
  LG_RWLOCK_FREE(audio.record.lock);
  LG_LOCK_FREE(audio.feedback.lock);
}

void lgAudio_setFallback(const LG_AudioOps * ops, void * opaque)
{
  setBinding(&audio.fallback, ops, opaque, fallbackStatusChanged);
}

static void dropBinding(AudioBinding * target)
{
  LG_LOCK(audio.bindingLock);
  if (!atomic_load_explicit(&audio.ready, memory_order_acquire))
  {
    LG_UNLOCK(audio.bindingLock);
    return;
  }

  LG_LOCK(audio.providerLock);
  LG_LOCK_EXCLUSIVE(audio.activeLock);
  const bool wasActive = bindingActiveNL(target);
  *target = (AudioBinding) { 0 };
  LG_UNLOCK_EXCLUSIVE(audio.activeLock);
  updateActive(wasActive);
  LG_UNLOCK(audio.providerLock);
  LG_UNLOCK(audio.bindingLock);
}

void lgAudio_dropFallback(void)
{
  dropBinding(&audio.fallback);
}

void lgAudio_setTransport(const LG_AudioOps * ops, void * opaque)
{
  setBinding(&audio.transport, ops, opaque, transportStatusChanged);
}

void lgAudio_dropTransport(void)
{
  dropBinding(&audio.transport);
}

#endif
