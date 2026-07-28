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
#define PLAYBACK_TIMESTAMP_DISCONTINUITY_MS 2000
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
  STREAM_STATE_SETUP_SPICE,
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

  uint32_t mediaTime;
  int64_t  mediaTimeMs;
  int64_t  mediaLocalOrigin;
  int64_t  lastPacketTime;
  int64_t  lastArrivalTime;
  double   arrivalJitterSec;
  double   sourcePhaseBaselineSec;
  double   sourcePhaseReserveSec;
  double   sourcePacketDurationSec;
  bool     sourcePhaseBaselineValid;
  bool     mediaClockValid;

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
PlaybackSpiceData;

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
  struct LG_AudioDevOps * audioDev;

  struct
  {
    _Atomic(StreamState) state;
    atomic_uint          callbackState;
    int         volumeChannels;
    uint16_t    volume[8];
    bool        mute;
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
     * Spice data threads respectively. Keep them on separate cache lines to
     * avoid false sharing. */
    alignas(64) PlaybackDeviceData deviceData;
    alignas(64) PlaybackSpiceData  spiceData;
  }
  playback;

  struct
  {
    LG_Lock       lock;
    bool          shuttingDown;
    bool          requested;
    bool          started;
    int           volumeChannels;
    uint16_t      volume[8];
    bool          mute;
    atomic_int    stride;
    uint32_t      time;
    int           lastChannels;
    int           lastSampleRate;
    PSAudioFormat lastFormat;
    MsgBoxHandle  confirmHandle;
    uint64_t      confirmGeneration;
    bool          confirmPending;
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

  /* SPICE multimedia time periodically changes phase by several
   * milliseconds. Preserve it as a diagnostic and discontinuity signal, but
   * advance the source clock solely from decoded sample position. Short-term
   * timestamp corrections must not move the playback buffer. */
  clock->time = llrint(predicted);
  clock->position = position;
  clock->phaseResidualSec = residual;
  ++clock->updates;
  return true;
}

static void playbackDeviceClockAcquireReset(PlaybackSpiceData * spiceData)
{
  spiceData->deviceClockAcquireStart  = INT64_MIN;
  spiceData->deviceClockCheckTime     = INT64_MIN;
  spiceData->deviceClockStableSec     = 0.0;
  spiceData->devicePositionOffsetFrames = 0.0;
  spiceData->deviceClockStable        = false;
  spiceData->ratioIntegral            = 0.0;
  spiceData->lastClockRatio           = 1.0;
}

static bool playbackDeviceClockAcquire(
    PlaybackSpiceData * spiceData, int64_t time)
{
  if (spiceData->deviceClockStable)
    return false;

  if (spiceData->deviceClockAcquireStart == INT64_MIN)
  {
    spiceData->deviceClockAcquireStart = time;
    spiceData->deviceClockCheckTime = time;
    spiceData->deviceClockCheckFrameSec =
      spiceData->deviceClock.frameSec;
    return false;
  }

  const double checkSec =
    (time - spiceData->deviceClockCheckTime) * 1.0e-9;
  if (checkSec < PLAYBACK_DEVICE_RATE_CHECK_SEC)
    return false;

  const double rateDeltaPpm = fabs(
      spiceData->deviceClock.frameSec /
        spiceData->deviceClockCheckFrameSec - 1.0) * 1.0e6;
  if (rateDeltaPpm <= PLAYBACK_DEVICE_RATE_STABLE_DELTA_PPM)
    spiceData->deviceClockStableSec += checkSec;
  else
    spiceData->deviceClockStableSec = 0.0;

  spiceData->deviceClockCheckTime = time;
  spiceData->deviceClockCheckFrameSec =
    spiceData->deviceClock.frameSec;

  const double acquireSec =
    (time - spiceData->deviceClockAcquireStart) * 1.0e-9;
  if (spiceData->deviceClockStableSec <
        PLAYBACK_DEVICE_RATE_STABLE_SEC &&
      acquireSec < PLAYBACK_DEVICE_RATE_MAX_ACQUIRE_SEC)
    return false;

  spiceData->deviceClockStable = true;
  return true;
}

static double playbackClockPosition(const PlaybackClock * clock, int64_t time)
{
  return clock->position +
    (time - clock->time) * 1.0e-9 / clock->frameSec;
}

static void playbackSourceRateReset(PlaybackSpiceData * spiceData)
{
  spiceData->rateSampleStart      = 0;
  spiceData->rateSampleCount      = 0;
  spiceData->rateLastSampleTimeMs = INT64_MIN;
  spiceData->rateFilterTimeMs     = INT64_MIN;
  spiceData->sourceRateValid      = false;
}

static void playbackSourceRateAdd(
    PlaybackSpiceData * spiceData, double nominalFrameSec)
{
  const int64_t timeMs = spiceData->mediaTimeMs;
  if (spiceData->rateLastSampleTimeMs != INT64_MIN &&
      timeMs - spiceData->rateLastSampleTimeMs <
        PLAYBACK_RATE_SAMPLE_INTERVAL_MS)
    return;

  spiceData->rateLastSampleTimeMs = timeMs;

  while (spiceData->rateSampleCount > 0)
  {
    const PlaybackRateSample * oldest =
      &spiceData->rateSamples[spiceData->rateSampleStart];
    if (timeMs - oldest->timeMs <= PLAYBACK_RATE_WINDOW_MS)
      break;

    spiceData->rateSampleStart =
      (spiceData->rateSampleStart + 1) % PLAYBACK_RATE_MAX_SAMPLES;
    --spiceData->rateSampleCount;
  }

  if (spiceData->rateSampleCount == PLAYBACK_RATE_MAX_SAMPLES)
  {
    spiceData->rateSampleStart =
      (spiceData->rateSampleStart + 1) % PLAYBACK_RATE_MAX_SAMPLES;
    --spiceData->rateSampleCount;
  }

  const unsigned int index =
    (spiceData->rateSampleStart + spiceData->rateSampleCount) %
      PLAYBACK_RATE_MAX_SAMPLES;
  spiceData->rateSamples[index] = (PlaybackRateSample)
  {
    .timeMs   = timeMs,
    .position = spiceData->inputPosition
  };
  ++spiceData->rateSampleCount;

  const PlaybackRateSample * first =
    &spiceData->rateSamples[spiceData->rateSampleStart];
  if (spiceData->rateSampleCount < 2 ||
      timeMs - first->timeMs < PLAYBACK_RATE_MIN_SPAN_MS)
    return;

  double sumPosition = 0.0;
  double sumTime     = 0.0;
  double sumPosition2 = 0.0;
  double sumPositionTime = 0.0;
  for (unsigned int i = 0; i < spiceData->rateSampleCount; ++i)
  {
    const PlaybackRateSample * sample =
      &spiceData->rateSamples[
        (spiceData->rateSampleStart + i) % PLAYBACK_RATE_MAX_SAMPLES];
    const double position = sample->position - first->position;
    const double timeSec = (sample->timeMs - first->timeMs) / 1000.0;

    sumPosition     += position;
    sumTime         += timeSec;
    sumPosition2    += position * position;
    sumPositionTime += position * timeSec;
  }

  const double count = spiceData->rateSampleCount;
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

  if (!spiceData->sourceRateValid)
  {
    spiceData->sourceRateFrameSec = frameSec;
    spiceData->sourceRateValid = true;
  }
  else
  {
    const double elapsedSec =
      (timeMs - spiceData->rateFilterTimeMs) / 1000.0;
    const double alpha =
      -expm1(-elapsedSec / PLAYBACK_RATE_FILTER_TIME_SEC);
    spiceData->sourceRateFrameSec +=
      alpha * (frameSec - spiceData->sourceRateFrameSec);
  }
  spiceData->rateFilterTimeMs = timeMs;
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

static void playbackResetMediaClock(
    PlaybackSpiceData * spiceData, uint32_t time, int64_t now)
{
  spiceData->mediaTime        = time;
  spiceData->mediaTimeMs      = 0;
  spiceData->mediaLocalOrigin = now;
  spiceData->lastPacketTime   = INT64_MIN;
  spiceData->lastArrivalTime  = INT64_MIN;
  spiceData->mediaClockValid  = true;
  spiceData->inputPosition    = 0;
  spiceData->sourceClock.valid = false;
  playbackSourceRateReset(spiceData);
}

static void playbackPrepareMediaClock(
    PlaybackSpiceData * spiceData, uint32_t time)
{
  spiceData->mediaTime       = time;
  spiceData->mediaClockValid = false;
  spiceData->inputPosition   = 0;
  spiceData->sourceClock.valid = false;
  playbackSourceRateReset(spiceData);
}

static int64_t playbackMapMediaTime(PlaybackSpiceData * spiceData,
    uint32_t time, int64_t now, bool * discontinuity)
{
  if (!spiceData->mediaClockValid)
  {
    playbackResetMediaClock(spiceData, time, now);
    return now;
  }

  const int32_t deltaMs = (int32_t)(time - spiceData->mediaTime);
  if (deltaMs < 0 || deltaMs > PLAYBACK_TIMESTAMP_DISCONTINUITY_MS)
  {
    playbackResetMediaClock(spiceData, time, now);
    *discontinuity = true;
    return now;
  }

  spiceData->mediaTime = time;
  spiceData->mediaTimeMs += deltaMs;
  return spiceData->mediaLocalOrigin + spiceData->mediaTimeMs * 1000000;
}

static void playbackStop(void);
static MsgBoxHandle recordCancelConfirmLocked(void);
static void realRecordStartLocked(
    int channels, int sampleRate, PSAudioFormat format);
static void realRecordStopLocked(void);

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

void audio_init(void)
{
  LG_LOCK_INIT(audio.record.lock);
  audio.record.shuttingDown = false;
  atomic_store_explicit(
      &audio.playback.callbackState, PLAYBACK_CALLBACK_DISABLED,
      memory_order_release);

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
  // immediate stop of the stream, do not wait for drain
  if (audio.audioDev)
    playbackStop();

  LG_LOCK(audio.record.lock);
  audio.record.shuttingDown = true;
  audio.record.requested = false;
  MsgBoxHandle confirm = recordCancelConfirmLocked();

  if (audio.audioDev && audio.record.started)
    realRecordStopLocked();

  struct LG_AudioDevOps * audioDev = audio.audioDev;
  audio.audioDev = NULL;
  LG_UNLOCK(audio.record.lock);

  app_msgBoxClose(confirm);

  if (audioDev)
    audioDev->free();
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
  if (playbackGetState() == STREAM_STATE_STOP)
    return;

  playbackDisableCallbacks();
  audio.audioDev->playback.stop();
  playbackWaitForCallbacks();

  playbackSetState(STREAM_STATE_STOP);
  ringbuffer_free(&audio.playback.buffer);
  audio.playback.spiceData.src = src_delete(audio.playback.spiceData.src);

  if (audio.playback.spiceData.framesIn)
  {
    free(audio.playback.spiceData.framesIn);
    free(audio.playback.spiceData.framesOut);
    audio.playback.spiceData.framesIn = NULL;
    audio.playback.spiceData.framesOut = NULL;
    audio.playback.spiceData.framesInSize = 0;
    audio.playback.spiceData.framesOutSize = 0;
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

void audio_playbackStart(int channels, int sampleRate, PSAudioFormat format,
  uint32_t time)
{
  if (!audio.audioDev)
    return;

  if (channels < 1 || channels > 8 || sampleRate < 8000 ||
      sampleRate > 384000 || format != PS_AUDIO_FMT_S16)
  {
    DEBUG_ERROR("Invalid playback format: %d channels, %d Hz, format %d",
        channels, sampleRate, format);
    if (playbackGetState() != STREAM_STATE_STOP)
      playbackStop();
    return;
  }

  static int lastChannels   = 0;
  static int lastSampleRate = 0;
  static PSAudioFormat lastFormat = PS_AUDIO_FMT_INVALID;

  StreamState state = playbackGetState();
  if (state == STREAM_STATE_KEEP_ALIVE &&
      channels == lastChannels && sampleRate == lastSampleRate &&
      format == lastFormat)
  {
    StreamState expected = STREAM_STATE_KEEP_ALIVE;
    if (atomic_compare_exchange_strong_explicit(
          &audio.playback.state, &expected, STREAM_STATE_RESUMING,
          memory_order_acq_rel, memory_order_acquire))
    {
      playbackPrepareMediaClock(&audio.playback.spiceData, time);
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

  lastChannels   = channels;
  lastSampleRate = sampleRate;
  lastFormat     = format;

  audio.playback.channels   = channels;
  audio.playback.sampleRate = sampleRate;
  audio.playback.stride     = channels * sizeof(float);
  playbackSetState(STREAM_STATE_SETUP_SPICE);

  audio.playback.deviceData.nextPosition         = 0;
  audio.playback.deviceData.outputPosition       = 0.0;
  audio.playback.deviceData.appliedRatio         = 1.0;
  audio.playback.deviceData.startupSilenceFrames = 0;

  audio.playback.spiceData.inputPosition       = 0;
  audio.playback.spiceData.outputPosition      = 0;
  audio.playback.spiceData.devPeriodFrames     = 0;
  audio.playback.spiceData.devReadPosition     = 0;
  audio.playback.spiceData.deviceTimingSequence = 0;
  playbackDeviceClockAcquireReset(&audio.playback.spiceData);
  audio.playback.spiceData.offsetError         = 0.0;
  audio.playback.spiceData.offsetErrorIntegral = 0.0;
  audio.playback.spiceData.ratioIntegral       = 0.0;
  audio.playback.spiceData.lastRatio           = 1.0;
  audio.playback.spiceData.lastClockRatio      = 1.0;
  audio.playback.spiceData.bufferOverrunPending = false;
  audio.playback.spiceData.bufferOverruns      = 0;
  audio.playback.spiceData.nextLogTime         =
    nanotime() + INT64_C(5000000000);
  audio.playback.spiceData.arrivalJitterSec    = 0.0;
  audio.playback.spiceData.sourcePhaseBaselineSec = 0.0;
  audio.playback.spiceData.sourcePhaseReserveSec = 0.0;
  audio.playback.spiceData.sourcePacketDurationSec = 0.0;
  audio.playback.spiceData.sourcePhaseBaselineValid = false;
  audio.playback.spiceData.deviceClock.valid   = false;
  audio.playback.spiceData.outputClock.valid   = false;
  playbackPrepareMediaClock(&audio.playback.spiceData, time);

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
  if (!audio.audioDev->playback.setup(channels, sampleRate,
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
    audio.playback.spiceData.src =
      src_new(SRC_SINC_FASTEST, channels, &srcError);
    if (!audio.playback.spiceData.src)
    {
      DEBUG_ERROR("Failed to create resampler: %s", src_strerror(srcError));
      playbackStop();
      return;
    }
  }
  else
    audio.playback.spiceData.src = NULL;

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

void audio_playbackStop(void)
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
      if (audio.playback.spiceData.src)
      {
        int error = src_reset(audio.playback.spiceData.src);
        if (error)
        {
          DEBUG_ERROR("Failed to reset resampler: %s", src_strerror(error));
          playbackStop();
        }
      }

      break;
    }

    case STREAM_STATE_SETUP_SPICE:
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

void audio_playbackVolume(int channels, const uint16_t volume[])
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

void audio_playbackMute(bool mute)
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
  const PlaybackSpiceData * spiceData =
    &audio.playback.spiceData;
  return playbackClockPosition(
      &spiceData->deviceClock, curTime) +
    spiceData->devicePositionOffsetFrames;
}

static bool playbackEnsureConversionBuffers(
    PlaybackSpiceData * spiceData, int frames)
{
  if (frames > spiceData->framesInSize)
  {
    float * framesIn = realloc(spiceData->framesIn,
        (size_t)frames * audio.playback.stride);
    if (!framesIn)
    {
      DEBUG_ERROR("Failed to grow playback input buffer");
      return false;
    }

    spiceData->framesIn = framesIn;
    spiceData->framesInSize = frames;
  }

  if (!audio.playback.backendResampler)
  {
    const int framesOut =
      (int)ceil(frames * (1.0 + PLAYBACK_MAX_RATE_CORRECTION)) + 64;
    if (framesOut > spiceData->framesOutSize)
    {
      float * output = realloc(spiceData->framesOut,
          (size_t)framesOut * audio.playback.stride);
      if (!output)
      {
        DEBUG_ERROR("Failed to grow playback output buffer");
        return false;
      }

      spiceData->framesOut = output;
      spiceData->framesOutSize = framesOut;
    }
  }

  return true;
}

static int playbackAppendFrames(
    PlaybackSpiceData * spiceData, const void * frames, int count)
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
    spiceData->bufferOverrunPending = true;
    ++spiceData->bufferOverruns;
  }

  return advanced;
}

static int playbackSlewBuffer(
    PlaybackSpiceData * spiceData, int requested)
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
    spiceData->bufferOverrunPending = true;

  return advanced;
}

void audio_playbackData(uint8_t * data, size_t size, uint32_t time)
{
  StreamState state = playbackGetState();
  if (state == STREAM_STATE_STOP_PENDING)
  {
    playbackStop();
    return;
  }

  if (state == STREAM_STATE_STOP || !audio.audioDev || size == 0)
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

  PlaybackSpiceData * spiceData = &audio.playback.spiceData;
  /* Backend resampling changes how many source frames PipeWire requests per
   * device period. Use the command-normalized output clock for rate matching,
   * while deviceClock remains in the ring's source-frame domain for latency. */
  const PlaybackClock * rateClock = audio.playback.backendResampler ?
    &spiceData->outputClock : &spiceData->deviceClock;
  const int64_t now = nanotime();
  const double nominalFrameSec = 1.0 / audio.playback.sampleRate;

  const int spiceStride = audio.playback.channels * sizeof(int16_t);
  if (size % spiceStride != 0 || size / spiceStride > INT_MAX)
  {
    DEBUG_ERROR("Invalid playback packet size: %zu bytes for stride %d",
        size, spiceStride);
    playbackStop();
    return;
  }

  const int frames = size / spiceStride;
  if (frames == 0 || frames > audio.playback.sampleRate * 2)
  {
    DEBUG_ERROR("Invalid playback packet length: %d frames", frames);
    playbackStop();
    return;
  }

  if (!playbackEnsureConversionBuffers(spiceData, frames))
  {
    playbackStop();
    return;
  }
  src_short_to_float_array((int16_t *) data, spiceData->framesIn,
    frames * audio.playback.channels);

  bool discontinuity = false;
  const int64_t packetTime =
    playbackMapMediaTime(spiceData, time, now, &discontinuity);
  if (spiceData->bufferOverrunPending)
  {
    discontinuity = true;
    spiceData->bufferOverrunPending = false;
  }

  if (spiceData->lastPacketTime != INT64_MIN &&
      spiceData->lastArrivalTime != INT64_MIN)
  {
    const double mediaDelta =
      (packetTime - spiceData->lastPacketTime) * 1.0e-9;
    const double arrivalDelta =
      (now - spiceData->lastArrivalTime) * 1.0e-9;
    const double jitter = fabs(arrivalDelta - mediaDelta);

    /* Keep a slowly decaying peak rather than feeding arrival jitter into the
     * virtual clock. This lets the buffer absorb real delivery jitter while
     * the rate controller follows only the SPICE multimedia clock. */
    spiceData->arrivalJitterSec =
      min(PLAYBACK_MAX_JITTER_SEC,
          max(jitter, spiceData->arrivalJitterSec * 0.999));
  }
  spiceData->lastPacketTime  = packetTime;
  spiceData->lastArrivalTime = now;

  const bool sourceRateWasValid =
    spiceData->sourceRateValid;
  playbackSourceRateAdd(spiceData, nominalFrameSec);
  const bool sourceRateBecameValid =
    !sourceRateWasValid && spiceData->sourceRateValid;
  if (!playbackSourceClockUpdate(&spiceData->sourceClock,
        packetTime, spiceData->inputPosition, nominalFrameSec))
    discontinuity = true;
  if (spiceData->sourceRateValid)
    spiceData->sourceClock.frameSec = spiceData->sourceRateFrameSec;

  /* Track phase variation around its local baseline, not its absolute value.
   * The absolute phase depends on the arbitrary local origin assigned to the
   * SPICE multimedia clock and must not become buffer reserve. Positive
   * deviation means the latency model temporarily overstates how much audio
   * remains in the ring. */
  const double sourcePhaseSec =
    spiceData->sourceClock.phaseResidualSec;
  const double packetSec =
    frames * nominalFrameSec;
  spiceData->sourcePacketDurationSec =
    max(packetSec, spiceData->sourcePacketDurationSec *
        exp(-packetSec / PLAYBACK_PHASE_RESERVE_DECAY_SEC));

  if (!spiceData->sourcePhaseBaselineValid ||
      spiceData->sourceClock.updates == 1)
  {
    spiceData->sourcePhaseBaselineSec = sourcePhaseSec;
    spiceData->sourcePhaseBaselineValid = true;
  }
  else
  {
    const double alpha =
      -expm1(-packetSec / PLAYBACK_PHASE_BASELINE_TIME_SEC);
    spiceData->sourcePhaseBaselineSec +=
      alpha * (sourcePhaseSec -
        spiceData->sourcePhaseBaselineSec);
  }

  const double sourcePhaseDeviationSec =
    max(0.0, sourcePhaseSec -
      spiceData->sourcePhaseBaselineSec);
  spiceData->sourcePhaseReserveSec =
    min(PLAYBACK_MAX_JITTER_SEC,
        max(sourcePhaseDeviationSec,
          spiceData->sourcePhaseReserveSec *
            exp(-packetSec /
              PLAYBACK_PHASE_RESERVE_DECAY_SEC)));

  int64_t curTime = spiceData->sourceClock.time;
  int64_t curPosition = spiceData->outputPosition;
  const double sourceReserveFrames =
    max(spiceData->sourcePacketDurationSec * 0.5,
        spiceData->sourcePhaseReserveSec) *
      audio.playback.sampleRate;

  // Receive the newest timing information from the audio device thread.
  PlaybackDeviceTick deviceTick;
  unsigned int deviceSequence;
  bool deviceClockBecameStable = false;
  if (playbackReadDeviceTiming(spiceData->deviceTimingSequence,
        &deviceTick, &deviceSequence))
  {
    spiceData->deviceTimingSequence = deviceSequence;
    spiceData->devPeriodFrames = deviceTick.periodFrames;
    spiceData->devReadPosition =
      deviceTick.nextPosition + deviceTick.periodFrames;
    const bool deviceClockUpdated =
      playbackClockUpdate(&spiceData->deviceClock,
          deviceTick.nextTime, deviceTick.nextPosition, nominalFrameSec);
    const bool outputClockUpdated =
      !audio.playback.backendResampler ||
      playbackClockUpdate(&spiceData->outputClock,
          deviceTick.nextTime, deviceTick.outputPosition,
          nominalFrameSec);
    if (!deviceClockUpdated || !outputClockUpdated)
    {
      playbackDeviceClockAcquireReset(spiceData);
      discontinuity = true;
    }
    else
      deviceClockBecameStable =
        playbackDeviceClockAcquire(spiceData, deviceTick.nextTime);
  }

  if (deviceClockBecameStable)
  {
    /* Give the fitted device timeline the same latency reported by the
     * acquisition model. Their position origins are otherwise unrelated, so
     * switching models would create a false phase step and drive the resampler
     * despite an already-correct ring level. Keep the source clock untouched:
     * changing it would also disturb SPICE phase and jitter tracking. */
    const double rawDevicePosition =
      playbackClockPosition(&spiceData->deviceClock, curTime);
    spiceData->devicePositionOffsetFrames =
      spiceData->devReadPosition - sourceReserveFrames -
        rawDevicePosition;
  }

  const int maxPeriodFrames =
    max(audio.playback.deviceMaxPeriodFrames, spiceData->devPeriodFrames);
  /* The device period, delivery jitter, packet phase, and resampler delay
   * define the minimum viable latency. latencyOffset is strictly an additive
   * user offset over that same minimum for both startup and steady state. */
  const double latencyOffsetFrames =
    max(g_params.audioLatencyOffset, 0) *
      audio.playback.sampleRate / 1000.0;
  const double arrivalReserveFrames =
    (spiceData->arrivalJitterSec + 0.001) *
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
      spiceData->deviceClock.valid &&
      spiceData->deviceClockStable)
  {
    devPosition = computeDevicePosition(curTime);
    const double slew = devPosition + targetBufferFrames - curPosition;
    const int slewFrames = clamp(llrint(slew), (int64_t)INT_MIN,
        (int64_t)INT_MAX);
    const int actualSlew = playbackSlewBuffer(spiceData, slewFrames);
    spiceData->outputPosition += actualSlew;
    curPosition += actualSlew;

    spiceData->offsetError         = 0.0;
    spiceData->offsetErrorIntegral = 0.0;
    spiceData->ratioIntegral       = 0.0;
    playbackSetState(STREAM_STATE_RUN);
  }

  double actualLatencyFrames = 0.0;
  double actualOffsetError = 0.0;
  if (spiceData->deviceClock.valid)
  {
    if (spiceData->deviceClockStable)
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
        curPosition - spiceData->devReadPosition +
          sourceReserveFrames + resamplerDelayFrames;
      actualOffsetError =
        targetLatencyFrames - actualLatencyFrames;
    }

    const double error =
      actualOffsetError - spiceData->offsetError;
    const double periodSec = frames * nominalFrameSec;
    const double omega =
      2.0 * M_PI * PLAYBACK_OFFSET_FILTER_BANDWIDTH_HZ * periodSec;
    const double b = M_SQRT2 * omega;
    const double c = omega * omega;

    spiceData->offsetError += b * error +
      spiceData->offsetErrorIntegral;
    spiceData->offsetErrorIntegral += c * error;
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
    spiceData->ratioIntegral = 0.0;

  if (spiceData->deviceClockStable &&
      spiceData->sourceRateValid &&
      rateClock->updates >= 2)
  {
    const double clockRatio = clamp(
        spiceData->sourceRateFrameSec /
          rateClock->frameSec,
        1.0 - PLAYBACK_MAX_RATE_CORRECTION,
        1.0 + PLAYBACK_MAX_RATE_CORRECTION);
    spiceData->lastClockRatio = clockRatio;
  }

  const double periodSec = frames * nominalFrameSec;
  /* SPICE timestamps have millisecond resolution. Do not resample in
   * response to phase error that cannot be distinguished from quantization;
   * subtracting the deadband outside it keeps the response continuous. */
  const double phaseDeadbandFrames =
    PLAYBACK_PHASE_DEADBAND_SEC * audio.playback.sampleRate;
  const double rawPhaseError = spiceData->offsetError;
  double phaseError = rawPhaseError;
  if (fabs(phaseError) <= phaseDeadbandFrames)
    phaseError = 0.0;
  else
    phaseError -= copysign(phaseDeadbandFrames, phaseError);

  const bool acquiringDeviceClock =
    spiceData->deviceClock.valid && !spiceData->deviceClockStable;
  double controllerKp = kp;
  double controllerKi = ki;
  double controllerError = phaseError;
  double controllerBase = spiceData->lastClockRatio;

  if (acquiringDeviceClock)
  {
    const double acquireFrequency =
      2.0 * M_PI * PLAYBACK_ACQUIRE_PHASE_BANDWIDTH_HZ;
    controllerKp =
      2.0 * acquireFrequency / audio.playback.sampleRate;
    controllerBase = 1.0;
    spiceData->ratioIntegral = 0.0;

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
    spiceData->ratioIntegral = 0.0;
  }
  /* Use the unfiltered latency error here so filter lag cannot retain a phase
   * correction after the target has already been crossed. */
  else if (spiceData->ratioIntegral * actualOffsetError <= 0.0)
    spiceData->ratioIntegral = 0.0;

  const double candidateIntegral = acquiringDeviceClock ?
    0.0 :
    spiceData->ratioIntegral +
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

  if (!acquiringDeviceClock && spiceData->deviceClockStable &&
      (desiredRatio == boundedRatio ||
       (desiredRatio > boundedRatio && controllerError < 0.0) ||
       (desiredRatio < boundedRatio && controllerError > 0.0)))
    spiceData->ratioIntegral = candidateIntegral;

  const double maxRatioStep =
    PLAYBACK_MAX_RATE_SLEW_PER_SEC * periodSec;
  const double ratio = clamp(boundedRatio,
      spiceData->lastRatio - maxRatioStep,
      spiceData->lastRatio + maxRatioStep);
  spiceData->lastRatio = ratio;

  if (audio.playback.backendResampler)
  {
    atomic_store_explicit(
        &audio.playback.backendResampleRatio, ratio,
        memory_order_release);
    const int outputFrames =
      playbackAppendFrames(spiceData, spiceData->framesIn, frames);
    spiceData->outputPosition += outputFrames;
  }
  else
  {
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
          spiceData, spiceData->framesOut, srcData.output_frames_gen);

      consumed += srcData.input_frames_used;
      spiceData->outputPosition += outputFrames;
    }
  }
  spiceData->inputPosition += frames;

  if (playbackGetState() == STREAM_STATE_SETUP_SPICE)
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

  if (now >= spiceData->nextLogTime)
  {
    const double sourcePpm = spiceData->sourceRateValid ?
      (spiceData->sourceRateFrameSec / nominalFrameSec - 1.0) * 1.0e6 :
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
        spiceData->arrivalJitterSec * 1000.0,
        underruns, spiceData->bufferOverruns);

    spiceData->bufferOverruns = 0;
    spiceData->nextLogTime = now + INT64_C(5000000000);
  }
}

bool audio_supportsRecord(void)
{
  return audio.audioDev && audio.audioDev->record.start;
}

static void recordPushFrames(uint8_t * data, int frames)
{
  const int stride = atomic_load_explicit(
      &audio.record.stride, memory_order_acquire);
  purespice_writeAudio(data, frames * stride, 0);
}

static MsgBoxHandle recordCancelConfirmLocked(void)
{
  MsgBoxHandle handle = audio.record.confirmHandle;
  audio.record.confirmHandle = NULL;
  audio.record.confirmPending = false;
  ++audio.record.confirmGeneration;
  return handle;
}

static void realRecordStartLocked(
    int channels, int sampleRate, PSAudioFormat format)
{
  audio.record.started = true;
  atomic_store_explicit(&audio.record.stride,
      channels * sizeof(uint16_t), memory_order_release);

  audio.audioDev->record.start(channels, sampleRate, recordPushFrames);

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

struct AudioFormat
{
   int channels;
   int sampleRate;
   PSAudioFormat format;
};

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
    realRecordStartLocked(
        audio.record.confirmChannels,
        audio.record.confirmSampleRate,
        audio.record.confirmFormat);
  }
  else if (yes)
    DEBUG_INFO("Ignoring stale microphone access confirmation");
  else
    DEBUG_INFO("Microphone access denied");

  LG_UNLOCK(audio.record.lock);
}

void audio_recordStart(int channels, int sampleRate, PSAudioFormat format)
{
  LG_LOCK(audio.record.lock);
  if (!audio.audioDev || audio.record.shuttingDown)
  {
    LG_UNLOCK(audio.record.lock);
    return;
  }

  const bool restart = audio.record.started;
  if (audio.record.started)
  {
    if (channels   == audio.record.lastChannels &&
        sampleRate == audio.record.lastSampleRate &&
        format     == audio.record.lastFormat)
    {
      LG_UNLOCK(audio.record.lock);
      return;
    }

    realRecordStopLocked();
  }

  MsgBoxHandle oldConfirm = recordCancelConfirmLocked();
  audio.record.requested      = true;
  audio.record.lastChannels   = channels;
  audio.record.lastSampleRate = sampleRate;
  audio.record.lastFormat     = format;

  if (restart)
    realRecordStartLocked(channels, sampleRate, format);
  else if (g_state.micDefaultState == MIC_DEFAULT_DENY)
    DEBUG_INFO("Microphone access denied by default");
  else if (g_state.micDefaultState == MIC_DEFAULT_ALLOW)
  {
    DEBUG_INFO("Microphone access granted by default");
    realRecordStartLocked(channels, sampleRate, format);
  }
  else
  {
    audio.record.confirmChannels   = channels;
    audio.record.confirmSampleRate = sampleRate;
    audio.record.confirmFormat     = format;
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

void audio_recordStop(void)
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

void audio_recordToggleKeybind(int sc, void * opaque)
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
    realRecordStartLocked(
        audio.record.lastChannels,
        audio.record.lastSampleRate,
        audio.record.lastFormat);
    started = true;
  }
  LG_UNLOCK(audio.record.lock);

  app_msgBoxClose(confirm);
  app_alert(LG_ALERT_INFO,
      started ? "Microphone enabled" : "Microphone disabled");
}

void audio_recordVolume(int channels, const uint16_t volume[])
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

void audio_recordMute(bool mute)
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

#endif
