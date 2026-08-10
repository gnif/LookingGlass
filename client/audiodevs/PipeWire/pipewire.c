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

#include "interface/audiodev.h"

#include <spa/param/audio/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <pipewire/pipewire.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/stringutils.h"
#include "common/util.h"
#include "common/option.h"
#include "common/ringbuffer.h"
#include "common/thread.h"
#include "common/time.h"

typedef enum
{
  STREAM_STATE_INACTIVE,
  STREAM_STATE_ACTIVE
}
StreamState;

typedef enum
{
  RECORD_LATEST_FREE,
  RECORD_LATEST_WRITING,
  RECORD_LATEST_READY,
  RECORD_LATEST_READING
}
RecordLatestState;

typedef struct
{
  int           frames;
  bool          clockValid;
  LG_AudioClock clock;
}
RecordBlock;

#define RECORD_LATEST_SLOTS 2
#define RECORD_CLOCK_TOLERANCE_FRAMES 2.0
#define RECORD_ERROR_REPORT_INTERVAL_NS INT64_C(5000000000)
#define PIPEWIRE_CONNECT_TIMEOUT_NS INT64_C(5000000000)

struct PipeWire
{
  struct pw_loop        * loop;
  struct pw_context     * context;
  struct pw_core        * core;
  struct pw_thread_loop * thread;

  struct
  {
    struct pw_stream * stream;
    _Atomic(struct spa_io_rate_match *) rateMatch;
    _Atomic(int64_t) latencyNs;
    atomic_bool      latencyUpdateRequested;
    atomic_uint      bufferErrors;
    atomic_uint      pullErrors;
    atomic_uint      timingErrors;
    atomic_int       streamError;
#if PW_CHECK_VERSION(1, 4, 0)
    double           appliedResampleRatio;
    atomic_int       resampleError;
#endif
    enum pw_stream_state connectionState;
    bool                 resamplerEnabled;

    LG_AudioFormat format;
    int            stride;
    LG_AudioPullFn pullFn;
    LG_AudioFailureFn failureFn;
    uint32_t          failureCookie;
    int            maxPeriodFrames;
    int            startFrames;

    atomic_uint state;
  }
  playback;

  struct
  {
    struct pw_stream * stream;
    enum pw_stream_state connectionState;

    LG_AudioFormat format;
    int            stride;
    LG_AudioPushFn pushFn;
    LG_AudioFailureFn failureFn;
    uint32_t          failureCookie;

    RingBuffer  sendQueue;
    RingBuffer  sendBlocks;
    uint8_t *   sendBuffer;
    int         sendBufferFrames;
    int         maxQueuedFrames;
    sem_t       sendWake;
    bool        sendWakeInitialized;
    LGThread *  sendThread;
    atomic_bool sendStop;
    atomic_bool sendWakePending;
    atomic_bool latestMode;
    atomic_uint_fast64_t latestSerial;
    uint8_t *   latestBuffer;
    struct
    {
      atomic_uint          state;
      atomic_uint_fast64_t serial;
      int                  frames;
      bool                 clockValid;
      LG_AudioClock        clock;
    }
    latest[RECORD_LATEST_SLOTS];
    atomic_uint bufferErrors;
    atomic_uint droppedFrames;
    atomic_uint rejectedBatches;
    atomic_uint rejectedFrames;
    atomic_uint signalErrors;
    atomic_int  recycleError;

    uint64_t            clockPosition;
    uint64_t            clockLastPosition;
    uint64_t            clockLastTicks;
    int64_t             clockLastTime;
    struct spa_fraction clockLastRate;
    bool                clockLastValid;
    bool                clockLastTimingValid;
    bool                clockDiscontinuityPending;
    bool active;
  }
  record;
};

static struct PipeWire pw = {0};

static inline void pipewire_storeError(atomic_int * target, int result)
{
  if (result >= 0)
    return;

  int expected = 0;
  atomic_compare_exchange_strong_explicit(
      target, &expected, result,
      memory_order_relaxed, memory_order_relaxed);
}

static bool pipewire_audioFormatEqual(const LG_AudioFormat * a,
    const LG_AudioFormat * b)
{
  return
    a->sampleFormat == b->sampleFormat &&
    a->sampleRate   == b->sampleRate &&
    a->channelCount == b->channelCount &&
    memcmp(a->channels, b->channels,
        a->channelCount * sizeof(*a->channels)) == 0;
}

static enum spa_audio_channel pipewire_channel(
    LG_AudioChannel channel, uint8_t index)
{
  switch (channel)
  {
    case LG_AUDIO_CH_UNKNOWN:
      return (enum spa_audio_channel)(SPA_AUDIO_CHANNEL_AUX0 + index);
    case LG_AUDIO_CH_MONO               : return SPA_AUDIO_CHANNEL_MONO;
    case LG_AUDIO_CH_FRONT_LEFT         : return SPA_AUDIO_CHANNEL_FL;
    case LG_AUDIO_CH_FRONT_RIGHT        : return SPA_AUDIO_CHANNEL_FR;
    case LG_AUDIO_CH_FRONT_CENTER       : return SPA_AUDIO_CHANNEL_FC;
    case LG_AUDIO_CH_LFE                : return SPA_AUDIO_CHANNEL_LFE;
    case LG_AUDIO_CH_REAR_LEFT          : return SPA_AUDIO_CHANNEL_RL;
    case LG_AUDIO_CH_REAR_RIGHT         : return SPA_AUDIO_CHANNEL_RR;
    case LG_AUDIO_CH_FRONT_LEFT_CENTER  : return SPA_AUDIO_CHANNEL_FLC;
    case LG_AUDIO_CH_FRONT_RIGHT_CENTER : return SPA_AUDIO_CHANNEL_FRC;
    case LG_AUDIO_CH_REAR_CENTER        : return SPA_AUDIO_CHANNEL_RC;
    case LG_AUDIO_CH_SIDE_LEFT          : return SPA_AUDIO_CHANNEL_SL;
    case LG_AUDIO_CH_SIDE_RIGHT         : return SPA_AUDIO_CHANNEL_SR;
    case LG_AUDIO_CH_TOP_CENTER         : return SPA_AUDIO_CHANNEL_TC;
    case LG_AUDIO_CH_TOP_FRONT_LEFT     : return SPA_AUDIO_CHANNEL_TFL;
    case LG_AUDIO_CH_TOP_FRONT_CENTER   : return SPA_AUDIO_CHANNEL_TFC;
    case LG_AUDIO_CH_TOP_FRONT_RIGHT    : return SPA_AUDIO_CHANNEL_TFR;
    case LG_AUDIO_CH_TOP_REAR_LEFT      : return SPA_AUDIO_CHANNEL_TRL;
    case LG_AUDIO_CH_TOP_REAR_CENTER    : return SPA_AUDIO_CHANNEL_TRC;
    case LG_AUDIO_CH_TOP_REAR_RIGHT     : return SPA_AUDIO_CHANNEL_TRR;
  }

  return (enum spa_audio_channel)(SPA_AUDIO_CHANNEL_AUX0 + index);
}

static enum spa_audio_format pipewire_sampleFormat(
    LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8     : return SPA_AUDIO_FORMAT_U8;
    case LG_AUDIO_FMT_S16_LE : return SPA_AUDIO_FORMAT_S16_LE;
    case LG_AUDIO_FMT_S24_LE : return SPA_AUDIO_FORMAT_S24_LE;
    case LG_AUDIO_FMT_S32_LE : return SPA_AUDIO_FORMAT_S32_LE;
    case LG_AUDIO_FMT_F32_LE : return SPA_AUDIO_FORMAT_F32_LE;
    case LG_AUDIO_FMT_F32_NE : return SPA_AUDIO_FORMAT_F32;
    case LG_AUDIO_FMT_F64_LE : return SPA_AUDIO_FORMAT_F64_LE;
  }

  return SPA_AUDIO_FORMAT_UNKNOWN;
}

static int pipewire_sampleSize(LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8     : return 1;
    case LG_AUDIO_FMT_S16_LE : return 2;
    case LG_AUDIO_FMT_S24_LE : return 3;
    case LG_AUDIO_FMT_S32_LE :
    case LG_AUDIO_FMT_F32_LE :
    case LG_AUDIO_FMT_F32_NE : return 4;
    case LG_AUDIO_FMT_F64_LE : return 8;
  }

  return 0;
}

static struct spa_audio_info_raw pipewire_audioInfo(
    const LG_AudioFormat * format, enum spa_audio_format sampleFormat)
{
  struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
    .format   = sampleFormat,
    .channels = format->channelCount,
    .rate     = format->sampleRate
  );

  for (uint8_t i = 0; i < format->channelCount; ++i)
    info.position[i] = pipewire_channel(format->channels[i], i);

  return info;
}

static void pipewire_reportPlaybackErrors(void)
{
  const unsigned int bufferErrors = atomic_exchange_explicit(
      &pw.playback.bufferErrors, 0, memory_order_relaxed);
  const unsigned int pullErrors = atomic_exchange_explicit(
      &pw.playback.pullErrors, 0, memory_order_relaxed);
  const unsigned int timingErrors = atomic_exchange_explicit(
      &pw.playback.timingErrors, 0, memory_order_relaxed);
  const int streamError = atomic_exchange_explicit(
      &pw.playback.streamError, 0, memory_order_relaxed);
#if PW_CHECK_VERSION(1, 4, 0)
  const int resampleError = atomic_exchange_explicit(
      &pw.playback.resampleError, 0, memory_order_relaxed);
#endif

  if (bufferErrors)
    DEBUG_WARN("PipeWire playback ran out of buffers %u time(s)",
        bufferErrors);
  if (pullErrors)
    DEBUG_WARN("PipeWire playback returned an invalid frame count %u time(s)",
        pullErrors);
  if (timingErrors)
    DEBUG_WARN("PipeWire playback timing query failed %u time(s)",
        timingErrors);
  if (streamError)
    DEBUG_WARN("PipeWire playback stream operation failed: %s",
        spa_strerror(streamError));
#if PW_CHECK_VERSION(1, 4, 0)
  if (resampleError)
    DEBUG_WARN("PipeWire resampler rate update failed: %s",
        spa_strerror(resampleError));
#endif
}

static void pipewire_reportRecordErrors(void)
{
  const unsigned int bufferErrors = atomic_exchange_explicit(
      &pw.record.bufferErrors, 0, memory_order_relaxed);
  const unsigned int droppedFrames = atomic_exchange_explicit(
      &pw.record.droppedFrames, 0, memory_order_relaxed);
  const unsigned int rejectedBatches = atomic_exchange_explicit(
      &pw.record.rejectedBatches, 0, memory_order_relaxed);
  const unsigned int rejectedFrames = atomic_exchange_explicit(
      &pw.record.rejectedFrames, 0, memory_order_relaxed);
  const unsigned int signalErrors = atomic_exchange_explicit(
      &pw.record.signalErrors, 0, memory_order_relaxed);
  const int recycleError = atomic_exchange_explicit(
      &pw.record.recycleError, 0, memory_order_relaxed);

  if (bufferErrors)
    DEBUG_WARN("PipeWire recording encountered %u buffer error(s)",
        bufferErrors);
  if (droppedFrames)
    DEBUG_WARN("PipeWire recording dropped %u frame(s)", droppedFrames);
  if (rejectedFrames)
    DEBUG_WARN("PipeWire recording provider rejected %u frame(s) "
        "in %u batch(es)", rejectedFrames, rejectedBatches);
  if (signalErrors)
    DEBUG_WARN("PipeWire recording worker notification failed %u time(s)",
        signalErrors);
  if (recycleError)
    DEBUG_WARN("PipeWire recording buffer recycle failed: %s",
        spa_strerror(recycleError));
}

static void pipewire_reportRecordErrorsDue(uint64_t * nextReport)
{
  const uint64_t now = nanotime();
  if (now < *nextReport)
    return;

  pipewire_reportRecordErrors();
  *nextReport = now + RECORD_ERROR_REPORT_INTERVAL_NS;
}

#if !PW_CHECK_VERSION(1, 1, 0)
static int64_t pipewire_monotonicTime(void)
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return SPA_TIMESPEC_TO_NSEC(&time);
}
#endif

static inline void pipewire_updatePlaybackLatency(void)
{
  if (!atomic_exchange_explicit(
        &pw.playback.latencyUpdateRequested, false,
        memory_order_acquire))
    return;

  struct pw_time time;
#if PW_CHECK_VERSION(0, 3, 50)
  if (pw_stream_get_time_n(pw.playback.stream, &time, sizeof(time)) < 0)
#else
  if (pw_stream_get_time(pw.playback.stream, &time) < 0)
#endif
  {
    atomic_fetch_add_explicit(
        &pw.playback.timingErrors, 1, memory_order_relaxed);
    return;
  }

  if (time.rate.num == 0 || time.rate.denom == 0)
    return;

#if PW_CHECK_VERSION(1, 1, 0)
  const int64_t now = (int64_t)pw_stream_get_nsec(pw.playback.stream);
#else
  const int64_t now = pipewire_monotonicTime();
#endif
  const int64_t elapsedNs = max(INT64_C(0), now - time.now);
  const double graphLatencyNs =
    (double)time.delay * time.rate.num *
      SPA_NSEC_PER_SEC / time.rate.denom - elapsedNs;
#if PW_CHECK_VERSION(0, 3, 50)
  const double streamFrames = time.queued + time.buffered;
#else
  const double streamFrames =
    (double)time.queued / pw.playback.stride;
#endif
  const double streamLatencyNs = streamFrames * SPA_NSEC_PER_SEC /
    pw.playback.format.sampleRate;
  const int64_t latencyNs =
    llrint(max(0.0, graphLatencyNs + streamLatencyNs));
  atomic_store_explicit(
      &pw.playback.latencyNs, latencyNs, memory_order_release);
}

#if PW_CHECK_VERSION(1, 4, 0)
static bool pipewire_playbackSetRate(double * ratio)
{
  if (!ratio || *ratio <= 0.0 ||
      !pw.playback.resamplerEnabled)
    return false;
  if (*ratio == pw.playback.appliedResampleRatio)
    return true;

  const int result = pw_stream_set_rate(pw.playback.stream, *ratio);
  if (result < 0)
  {
    int expected = 0;
    atomic_compare_exchange_strong_explicit(
        &pw.playback.resampleError, &expected, result,
        memory_order_relaxed, memory_order_relaxed);
    return false;
  }

  pw.playback.appliedResampleRatio = *ratio;
  return true;
}

static bool pipewire_configurePlaybackResampler(
    bool enable, bool * reusable)
{
  const bool wasEnabled = pw.playback.resamplerEnabled;
  const int result = pw_stream_set_rate(
      pw.playback.stream, enable ? 1.0 : 0.0);
  if (reusable)
    *reusable = result >= 0 || !wasEnabled;
  if (result < 0)
    return false;

  pw.playback.resamplerEnabled     = enable;
  pw.playback.appliedResampleRatio = enable ? 1.0 : 0.0;
  return enable;
}
#endif

static void pipewire_onPlaybackIoChanged(void * userdata, uint32_t id,
  void * data, uint32_t size)
{
  switch (id)
  {
    case SPA_IO_RateMatch:
      atomic_store_explicit(&pw.playback.rateMatch,
          data && size >= offsetof(struct spa_io_rate_match, size) +
            sizeof(((struct spa_io_rate_match *)0)->size) ? data : NULL,
          memory_order_release);
      break;
  }
}

static void pipewire_onPlaybackStateChanged(void * userdata,
    enum pw_stream_state old, enum pw_stream_state state,
    const char * error)
{
  (void)error;
  pw.playback.connectionState = state;
  if ((state == PW_STREAM_STATE_ERROR ||
       state == PW_STREAM_STATE_UNCONNECTED) &&
      pw.playback.failureFn)
  {
    LG_AudioFailureFn failureFn = pw.playback.failureFn;
    const uint32_t failureCookie = pw.playback.failureCookie;
    pw.playback.failureFn     = NULL;
    pw.playback.failureCookie = 0;
    failureFn(failureCookie);
  }
  pw_thread_loop_signal(pw.thread, false);
}

static void pipewire_onPlaybackProcess(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.playback.stream)))
  {
    atomic_fetch_add_explicit(
        &pw.playback.bufferErrors, 1, memory_order_relaxed);
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  uint8_t * dst;

  if (!sbuf || sbuf->n_datas == 0 || !sbuf->datas ||
      !sbuf->datas[0].chunk ||
      !(dst = sbuf->datas[0].data))
  {
#if PW_CHECK_VERSION(1, 4, 0)
    const int recycleResult =
      pw_stream_return_buffer(pw.playback.stream, pbuf);
#else
    const int recycleResult =
      pw_stream_queue_buffer(pw.playback.stream, pbuf);
#endif
    pipewire_storeError(&pw.playback.streamError, recycleResult);
    return;
  }

  int frames = sbuf->datas[0].maxsize / pw.playback.stride;
  struct spa_io_rate_match * rateMatch = atomic_load_explicit(
      &pw.playback.rateMatch, memory_order_acquire);
  if (rateMatch && rateMatch->size > 0)
    frames = min(frames, rateMatch->size);
#if PW_CHECK_VERSION(0, 3, 50)
  else if (pbuf->requested > 0)
    frames = min(frames, pbuf->requested);
#endif

  const int requestedFrames = frames;
  frames = pw.playback.pullFn(dst, requestedFrames);
  if (frames < 0 || frames > requestedFrames)
  {
    atomic_fetch_add_explicit(
        &pw.playback.pullErrors, 1, memory_order_relaxed);
    frames = clamp(frames, 0, requestedFrames);
  }
  pipewire_updatePlaybackLatency();
  if (!frames)
  {
    pbuf->size = 0;
    sbuf->datas[0].chunk->offset = 0;
    sbuf->datas[0].chunk->stride = pw.playback.stride;
    sbuf->datas[0].chunk->size = 0;
    pipewire_storeError(&pw.playback.streamError,
        pw_stream_queue_buffer(pw.playback.stream, pbuf));
    return;
  }

  pbuf->size = frames;
  sbuf->datas[0].chunk->offset = 0;
  sbuf->datas[0].chunk->stride = pw.playback.stride;
  sbuf->datas[0].chunk->size   = frames * pw.playback.stride;

  pipewire_storeError(&pw.playback.streamError,
      pw_stream_queue_buffer(pw.playback.stream, pbuf));
}

static struct Option pipewire_options[] =
{
  {
    .module       = "pipewire",
    .name         = "outDevice",
    .description  = "The default playback device to use",
    .type         = OPTION_TYPE_STRING
  },
  {
    .module       = "pipewire",
    .name         = "recDevice",
    .description  = "The default record device to use",
    .type         = OPTION_TYPE_STRING
  },

  {0}
};

static void pipewire_earlyInit(void)
{
  option_register(pipewire_options);
}

static bool pipewire_waitForStream(enum pw_stream_state * state)
{
  int result = 0;
#if PW_CHECK_VERSION(0, 3, 7)
  struct timespec deadline;
  result = pw_thread_loop_get_time(
      pw.thread, &deadline, PIPEWIRE_CONNECT_TIMEOUT_NS);
  if (result < 0)
    return false;

  while (*state == PW_STREAM_STATE_CONNECTING && result >= 0)
    result = pw_thread_loop_timed_wait_full(pw.thread, &deadline);
#else
  while (*state == PW_STREAM_STATE_CONNECTING && result >= 0)
    result = pw_thread_loop_timed_wait(pw.thread, 5);
#endif
  return result >= 0;
}

static bool pipewire_init(void)
{
  pw_init(NULL, NULL);

  pw.loop = pw_loop_new(NULL);
  if (!pw.loop)
  {
    DEBUG_ERROR("Failed to create a PipeWire loop");
    goto err;
  }
#if PW_CHECK_VERSION(1, 3, 81)
  pw.context = pw_context_new(
    pw.loop,
    NULL,
    0);
#else
  pw.context = pw_context_new(
    pw.loop,
    pw_properties_new(
      // Request real-time priority on the PipeWire threads
      PW_KEY_CONFIG_NAME, "client-rt.conf",
      NULL
    ),
    0);
#endif
  if (!pw.context)
  {
    DEBUG_ERROR("Failed to create a context");
    goto err;
  }

  /* this is just to test for PipeWire availabillity */
  pw.core = pw_context_connect(pw.context, NULL, 0);
  if (!pw.core)
    goto err_context;

  /* PipeWire is available so create the loop thread and start it */
  pw.thread = pw_thread_loop_new_full(pw.loop, "PipeWire", NULL);
  if (!pw.thread)
  {
    DEBUG_ERROR("Failed to create the thread loop");
    goto err_core;
  }

  const int result = pw_thread_loop_start(pw.thread);
  if (result < 0)
  {
    DEBUG_ERROR("Failed to start the PipeWire thread loop: %s",
        spa_strerror(result));
    pw_thread_loop_destroy(pw.thread);
    pw.thread = NULL;
    goto err_core;
  }
  return true;

err_core:
  pw_core_disconnect(pw.core);
  pw.core = NULL;

err_context:
  pw_context_destroy(pw.context);
  pw.context = NULL;

err:
  if (pw.loop)
    pw_loop_destroy(pw.loop);
  pw.loop = NULL;
  pw_deinit();
  return false;
}

static void pipewire_playbackStopStream(void)
{
  if (!pw.playback.stream)
  {
    pipewire_reportPlaybackErrors();
    return;
  }

  pw_thread_loop_lock(pw.thread);
  atomic_store_explicit(
      &pw.playback.rateMatch, NULL, memory_order_release);
  pw.playback.failureFn     = NULL;
  pw.playback.failureCookie = 0;
  pw_stream_destroy(pw.playback.stream);
  pw.playback.stream = NULL;
  pw.playback.resamplerEnabled = false;
  atomic_store_explicit(&pw.playback.state,
      STREAM_STATE_INACTIVE, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, false, memory_order_relaxed);
  pw_thread_loop_unlock(pw.thread);
  pipewire_reportPlaybackErrors();
}

static bool pipewire_playbackSetup(const LG_AudioFormat * format,
    int requestedPeriodFrames, bool requestResampler,
    bool * resamplerEnabled, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  *resamplerEnabled = false;

  const int channels   = format->channelCount;
  const int sampleRate = format->sampleRate;
  const int sampleSize =
    pipewire_sampleSize(format->sampleFormat);
  const enum spa_audio_format sampleFormat =
    pipewire_sampleFormat(format->sampleFormat);
  if (!sampleSize || sampleFormat == SPA_AUDIO_FORMAT_UNKNOWN)
    return false;

  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = pipewire_onPlaybackStateChanged,
    .io_changed    = pipewire_onPlaybackIoChanged,
    .process       = pipewire_onPlaybackProcess
  };

  bool reuse = false;
  pw_thread_loop_lock(pw.thread);
  if (pw.playback.stream &&
      pw.playback.connectionState == PW_STREAM_STATE_PAUSED &&
      atomic_load_explicit(
        &pw.playback.state, memory_order_acquire) == STREAM_STATE_INACTIVE &&
      pipewire_audioFormatEqual(&pw.playback.format, format))
  {
    pw.playback.pullFn = pullFn;
    bool resamplerReusable = true;
#if PW_CHECK_VERSION(1, 4, 0)
    atomic_store_explicit(
        &pw.playback.resampleError, 0, memory_order_relaxed);
    *resamplerEnabled =
      pipewire_configurePlaybackResampler(
          requestResampler, &resamplerReusable);
#endif
    if (resamplerReusable)
    {
      *maxPeriodFrames = pw.playback.maxPeriodFrames;
      *startFrames     = pw.playback.startFrames;
      reuse = true;
    }
  }
  pw_thread_loop_unlock(pw.thread);
  if (reuse)
    return true;

  pipewire_playbackStopStream();

  char requestedNodeLatency[32];
  snprintf(requestedNodeLatency, sizeof(requestedNodeLatency), "%d/%d",
    requestedPeriodFrames, sampleRate);

  pw.playback.format = *format;
  pw.playback.stride = sampleSize * channels;
  pw.playback.pullFn = pullFn;

  pw_thread_loop_lock(pw.thread);
  atomic_store_explicit(
      &pw.playback.rateMatch, NULL, memory_order_release);

  struct pw_properties * props =
    pw_properties_new(
      PW_KEY_APP_NAME      , "Looking Glass",
      PW_KEY_NODE_NAME     , "Looking Glass",
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE    , "Music",
      PW_KEY_NODE_LATENCY  , requestedNodeLatency,
      NULL
    );
  if (!props)
  {
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create playback stream properties");
    return false;
  }

  const char * device = option_get_string("pipewire", "outDevice");
  if (device)
  {
#ifdef PW_KEY_TARGET_OBJECT
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, device);
#else
    pw_properties_set(props, PW_KEY_NODE_TARGET, device);
#endif
  }

  pw.playback.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    props,
    &events,
    NULL
  );

  if (!pw.playback.stream)
  {
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create the stream");
    return false;
  }

  // The user can override the default node latency with the PIPEWIRE_LATENCY
  // environment variable, so get the actual node latency value from the stream.
  // The actual quantum size may be lower than this value depending on what else
  // is using the audio device, but we can treat this value as a maximum
  const struct pw_properties * properties =
    pw_stream_get_properties(pw.playback.stream);
  const char * actualNodeLatency = properties ?
    pw_properties_get(properties, PW_KEY_NODE_LATENCY) : NULL;

  unsigned num, denom;
  uint64_t actualPeriodFrames = 0;
  if (!actualNodeLatency ||
      sscanf(actualNodeLatency, "%u/%u", &num, &denom) != 2 ||
      num == 0 || denom == 0 ||
      (actualPeriodFrames =
        ((uint64_t)num * sampleRate + denom - 1) / denom) > INT_MAX)
  {
    DEBUG_WARN(
      "PIPEWIRE_LATENCY value '%s' is invalid; using %d/%d",
      actualNodeLatency ? actualNodeLatency : "(unset)", requestedPeriodFrames,
      sampleRate);

    struct spa_dict_item items[] = {
      { PW_KEY_NODE_LATENCY, requestedNodeLatency }
    };
    pw_stream_update_properties(pw.playback.stream,
      &SPA_DICT_INIT_ARRAY(items));

    pw.playback.maxPeriodFrames = requestedPeriodFrames;
  }
  else
    pw.playback.maxPeriodFrames = (int)actualPeriodFrames;

  // If the previous quantum size was very small, PipeWire can request two full
  // periods almost immediately at the start of playback
  pw.playback.startFrames = (int)min(
      (int64_t)pw.playback.maxPeriodFrames * 2, (int64_t)INT_MAX);

  *maxPeriodFrames = pw.playback.maxPeriodFrames;
  *startFrames     = pw.playback.startFrames;

  struct spa_audio_info_raw info =
    pipewire_audioInfo(format, sampleFormat);
  params[0] = spa_format_audio_raw_build(
      &b, SPA_PARAM_EnumFormat, &info);

  pw.playback.connectionState = PW_STREAM_STATE_CONNECTING;
  const int result = pw_stream_connect(
      pw.playback.stream,
      PW_DIRECTION_OUTPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS  |
      PW_STREAM_FLAG_INACTIVE,
      params, 1);

  if (result < 0)
  {
    DEBUG_ERROR("Failed to connect playback stream: %s", spa_strerror(result));
    atomic_store_explicit(
        &pw.playback.rateMatch, NULL, memory_order_release);
    pw_stream_destroy(pw.playback.stream);
    pw.playback.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
    return false;
  }

  if (!pipewire_waitForStream(&pw.playback.connectionState) ||
      pw.playback.connectionState != PW_STREAM_STATE_PAUSED)
  {
    DEBUG_ERROR("PipeWire playback stream did not become ready");
    atomic_store_explicit(
        &pw.playback.rateMatch, NULL, memory_order_release);
    pw_stream_destroy(pw.playback.stream);
    pw.playback.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
    return false;
  }

  atomic_store_explicit(&pw.playback.state,
      STREAM_STATE_INACTIVE, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, false, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.bufferErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.pullErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.timingErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.streamError, 0, memory_order_relaxed);
#if PW_CHECK_VERSION(1, 4, 0)
  atomic_store_explicit(
      &pw.playback.resampleError, 0, memory_order_relaxed);
  *resamplerEnabled =
    pipewire_configurePlaybackResampler(
        requestResampler, NULL);
#else
  (void)requestResampler;
#endif
  pw_thread_loop_unlock(pw.thread);
  return true;
}

static int pipewire_playbackControl(bool active,
    LG_AudioFailureFn failureFn, uint32_t failureCookie)
{
  int error = 0;
  pw_thread_loop_lock(pw.thread);
  if (!active)
  {
    pw.playback.failureFn     = NULL;
    pw.playback.failureCookie = 0;
  }

  if (!pw.playback.stream)
    error = active ? -ENODEV : 0;
  else if (active)
  {
    if (pw.playback.connectionState == PW_STREAM_STATE_ERROR ||
        pw.playback.connectionState == PW_STREAM_STATE_UNCONNECTED)
      error = -EPIPE;
    else
    {
      pw.playback.failureFn     = failureFn;
      pw.playback.failureCookie = failureCookie;
      if (atomic_load_explicit(
            &pw.playback.state, memory_order_acquire) != STREAM_STATE_ACTIVE)
      {
        /* A stopped stream must not replay data queued before reactivation. */
        error = pw_stream_flush(pw.playback.stream, false);
        if (error >= 0)
          error = pw_stream_set_active(pw.playback.stream, true);
      }
      if (error >= 0)
      {
        atomic_store_explicit(&pw.playback.state,
            STREAM_STATE_ACTIVE, memory_order_release);
      }
    }

    if (error < 0 ||
        pw.playback.connectionState == PW_STREAM_STATE_ERROR ||
        pw.playback.connectionState == PW_STREAM_STATE_UNCONNECTED)
    {
      pw.playback.failureFn     = NULL;
      pw.playback.failureCookie = 0;
      if (error >= 0)
        error = -EPIPE;
    }
  }
  else if (atomic_load_explicit(
        &pw.playback.state, memory_order_acquire) == STREAM_STATE_ACTIVE)
  {
    const int activeError =
      pw_stream_set_active(pw.playback.stream, false);
    const int flushError =
      pw_stream_flush(pw.playback.stream, false);

    error = activeError < 0 ? activeError : flushError;
    if (activeError >= 0)
      atomic_store_explicit(&pw.playback.state,
          STREAM_STATE_INACTIVE, memory_order_release);
  }

  pw_thread_loop_unlock(pw.thread);
  if (error < 0)
  {
    int expected = 0;
    atomic_compare_exchange_strong_explicit(
        &pw.playback.streamError, &expected, error,
        memory_order_relaxed, memory_order_relaxed);
  }
  return error;
}

static bool pipewire_playbackStart(
    LG_AudioFailureFn failureFn, uint32_t failureCookie)
{
  return pipewire_playbackControl(
      true, failureFn, failureCookie) >= 0;
}

static void pipewire_playbackStop(void)
{
  if (pipewire_playbackControl(false, NULL, 0) < 0)
  {
    pipewire_playbackStopStream();
    return;
  }
  atomic_store_explicit(
      &pw.playback.latencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, false, memory_order_relaxed);
}

static void pipewire_playbackVolume(int channels, const uint16_t volume[])
{
  if (channels <= 0 || channels > LG_AUDIO_MAX_CHANNELS)
    return;

  float param[channels];
  for(int i = 0; i < channels; ++i)
    param[i] = max(0.0,
        9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787);

  int result = 0;
  pw_thread_loop_lock(pw.thread);
  if (pw.playback.stream &&
      channels == pw.playback.format.channelCount)
    result = pw_stream_set_control(pw.playback.stream,
        SPA_PROP_channelVolumes, channels, param, 0);
  pw_thread_loop_unlock(pw.thread);

  if (result < 0)
    DEBUG_WARN("Failed to set PipeWire playback volume: %s",
        spa_strerror(result));
}

static void pipewire_playbackMute(bool mute)
{
  int result = 0;
  pw_thread_loop_lock(pw.thread);
  if (pw.playback.stream)
  {
    float val = mute ? 1.0f : 0.0f;
    result = pw_stream_set_control(
        pw.playback.stream, SPA_PROP_mute, 1, &val, 0);
  }
  pw_thread_loop_unlock(pw.thread);

  if (result < 0)
    DEBUG_WARN("Failed to set PipeWire playback mute: %s",
        spa_strerror(result));
}

static uint64_t pipewire_playbackLatency(void)
{
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, true, memory_order_release);

  const int64_t latencyNs = atomic_load_explicit(
      &pw.playback.latencyNs, memory_order_acquire);
  if (latencyNs <= 0)
    return 0;

  return latencyNs / 1000;
}

static void pipewire_recordSignalSender(void)
{
  if (atomic_exchange_explicit(
        &pw.record.sendWakePending, true, memory_order_acq_rel))
    return;

  if (sem_post(&pw.record.sendWake) < 0)
  {
    atomic_fetch_add_explicit(
        &pw.record.signalErrors, 1, memory_order_relaxed);
    if (errno != EOVERFLOW)
      atomic_store_explicit(
          &pw.record.sendWakePending, false, memory_order_release);
  }
}

static void pipewire_recordRejected(int frames)
{
  atomic_fetch_add_explicit(
      &pw.record.rejectedBatches, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(
      &pw.record.rejectedFrames, frames, memory_order_relaxed);
}

static void pipewire_recordAdvanceClock(
    LG_AudioClock * clock, int frames)
{
  const double rate = clock->rate > 0.0 ?
    clock->rate : pw.record.format.sampleRate;
  clock->position += frames;
  if (rate > 0.0)
    clock->time += (int64_t)llrint(
        (double)frames * SPA_NSEC_PER_SEC / rate);
  clock->discontinuity = false;
}

static bool pipewire_recordClockJump(
    double elapsedNs, double expectedNs)
{
  const double tolerance =
    RECORD_CLOCK_TOLERANCE_FRAMES * SPA_NSEC_PER_SEC /
    pw.record.format.sampleRate;
  return elapsedNs <= 0 ||
    fabs(elapsedNs - expectedNs) > tolerance;
}

static RecordBlock pipewire_recordMakeBlock(
    struct pw_buffer * pbuf, const struct spa_buffer * sbuf, int frames)
{
  RecordBlock block =
  {
    .frames = frames
  };
  struct pw_time timing = {0};
#if PW_CHECK_VERSION(0, 3, 50)
  const int timingResult = pw_stream_get_time_n(
      pw.record.stream, &timing, sizeof(timing));
#else
  const int timingResult = pw_stream_get_time(
      pw.record.stream, &timing);
#endif
  const bool timingValid = timingResult >= 0 &&
    timing.rate.num != 0 && timing.rate.denom != 0;

  int64_t clockTime = 0;
#if PW_CHECK_VERSION(1, 0, 5)
  if (pbuf->time)
    clockTime = (int64_t)pbuf->time;
#endif
  if (!clockTime && timingResult >= 0)
    clockTime = timing.now;

  const uint64_t position = pw.record.clockPosition;
  pw.record.clockPosition += frames;

  bool discontinuity = pw.record.clockDiscontinuityPending;
  const struct spa_meta_header * header = spa_buffer_find_meta_data(
      sbuf, SPA_META_Header, sizeof(*header));
  if (header &&
      (header->flags & (SPA_META_HEADER_FLAG_DISCONT |
          SPA_META_HEADER_FLAG_CORRUPTED)))
    discontinuity = true;
  if (sbuf->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
    discontinuity = true;

  block.clockValid = clockTime > 0;
  if (block.clockValid)
  {
    const bool clockStable = pw.record.clockLastValid;
    if (pw.record.clockLastValid)
    {
      const uint64_t elapsedFrames =
        position - pw.record.clockLastPosition;
      const double expectedNs =
        (double)elapsedFrames * SPA_NSEC_PER_SEC /
        pw.record.format.sampleRate;
      if (pipewire_recordClockJump(
            clockTime - pw.record.clockLastTime, expectedNs))
        discontinuity = true;
    }

    if (timingValid && pw.record.clockLastTimingValid)
    {
      if (timing.rate.num != pw.record.clockLastRate.num ||
          timing.rate.denom != pw.record.clockLastRate.denom ||
          timing.ticks <= pw.record.clockLastTicks)
        discontinuity = true;
      else
      {
        const double elapsedTickNs =
          (double)(timing.ticks - pw.record.clockLastTicks) *
          timing.rate.num * SPA_NSEC_PER_SEC / timing.rate.denom;
        const double expectedNs =
          (double)(position - pw.record.clockLastPosition) *
          SPA_NSEC_PER_SEC / pw.record.format.sampleRate;
        if (pipewire_recordClockJump(elapsedTickNs, expectedNs))
          discontinuity = true;
      }
    }

    block.clock = (LG_AudioClock)
    {
      .position      = position,
      .time          = clockTime,
      .rate          = 0.0,
      .stable        = clockStable && !discontinuity,
      .discontinuity = discontinuity
    };
    pw.record.clockDiscontinuityPending = false;
    pw.record.clockLastPosition = position;
    pw.record.clockLastTime     = clockTime;
    pw.record.clockLastValid    = true;
  }
  else
  {
    pw.record.clockDiscontinuityPending |= discontinuity;
    pw.record.clockLastValid = false;
  }

  if (timingValid)
  {
    pw.record.clockLastTicks       = timing.ticks;
    pw.record.clockLastRate        = timing.rate;
    pw.record.clockLastTimingValid = block.clockValid;
  }
  else
    pw.record.clockLastTimingValid = false;

  return block;
}

static bool pipewire_recordLatestPending(void)
{
  for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
    if (atomic_load_explicit(
          &pw.record.latest[i].state, memory_order_acquire) !=
        RECORD_LATEST_FREE)
      return true;

  return false;
}

static bool pipewire_recordSendLatest(void)
{
  int chosen = -1;
  uint64_t chosenSerial = 0;
  for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
  {
    if (atomic_load_explicit(
          &pw.record.latest[i].state, memory_order_acquire) !=
        RECORD_LATEST_READY)
      continue;

    const uint64_t serial = atomic_load_explicit(
        &pw.record.latest[i].serial, memory_order_relaxed);
    if (chosen < 0 || serial > chosenSerial)
    {
      chosen       = i;
      chosenSerial = serial;
    }
  }

  if (chosen < 0)
    return false;

  unsigned int expected = RECORD_LATEST_READY;
  if (!atomic_compare_exchange_strong_explicit(
        &pw.record.latest[chosen].state, &expected,
        RECORD_LATEST_READING, memory_order_acq_rel, memory_order_acquire))
    return true;

  /* Only the newest overload block is useful. Drop older blocks before
   * delivering it so recovery cannot build another backlog. */
  for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
  {
    if (i == chosen ||
        atomic_load_explicit(
          &pw.record.latest[i].serial, memory_order_relaxed) > chosenSerial)
      continue;

    expected = RECORD_LATEST_READY;
    if (atomic_compare_exchange_strong_explicit(
          &pw.record.latest[i].state, &expected,
          RECORD_LATEST_READING,
          memory_order_acq_rel, memory_order_acquire))
    {
      const int frames = pw.record.latest[i].frames;
      atomic_store_explicit(&pw.record.latest[i].state,
          RECORD_LATEST_FREE, memory_order_release);
      atomic_fetch_add_explicit(&pw.record.droppedFrames,
          frames, memory_order_relaxed);
    }
  }

  const int frames = pw.record.latest[chosen].frames;
  const LG_AudioClock * clock =
    pw.record.latest[chosen].clockValid ?
      &pw.record.latest[chosen].clock : NULL;
  if (!pw.record.pushFn(
        pw.record.latestBuffer +
          (size_t)chosen * pw.record.sendBufferFrames * pw.record.stride,
        frames, clock))
    pipewire_recordRejected(frames);
  atomic_store_explicit(&pw.record.latest[chosen].state,
      RECORD_LATEST_FREE, memory_order_release);
  return true;
}

static int pipewire_recordSendThread(void * opaque)
{
  uint64_t nextErrorReport = nanotime() +
    RECORD_ERROR_REPORT_INTERVAL_NS;

  for (;;)
  {
    int result;
    do
      result = sem_wait(&pw.record.sendWake);
    while (result < 0 && errno == EINTR);

    if (result < 0)
    {
      DEBUG_ERROR("Failed to wait for the PipeWire recording worker");
      break;
    }

    /* Clear before draining. A producer racing before this point is covered by
     * this wake, while a producer racing after it publishes the sole next
     * wake. */
    atomic_store_explicit(
        &pw.record.sendWakePending, false, memory_order_release);
    if (atomic_load_explicit(&pw.record.sendStop, memory_order_acquire))
      break;

    for (;;)
    {
      pipewire_reportRecordErrorsDue(&nextErrorReport);

      if (atomic_load_explicit(
            &pw.record.sendStop, memory_order_acquire))
        break;

      if (atomic_load_explicit(
            &pw.record.latestMode, memory_order_acquire))
      {
        const int queued = ringbuffer_getCount(pw.record.sendQueue);
        if (queued > 0)
        {
          ringbuffer_consume(pw.record.sendQueue, NULL, queued);
          atomic_fetch_add_explicit(
              &pw.record.droppedFrames, queued, memory_order_relaxed);
        }
        const int queuedBlocks = ringbuffer_getCount(pw.record.sendBlocks);
        if (queuedBlocks > 0)
          ringbuffer_consume(
              pw.record.sendBlocks, NULL, queuedBlocks);

        while (!atomic_load_explicit(
                 &pw.record.sendStop, memory_order_acquire) &&
               pipewire_recordSendLatest())
          ;

        atomic_store_explicit(
            &pw.record.latestMode, false, memory_order_release);
        if (pipewire_recordLatestPending())
        {
          atomic_store_explicit(
              &pw.record.latestMode, true, memory_order_release);
          continue;
        }
      }

      RecordBlock block;
      if (ringbuffer_consume(
            pw.record.sendBlocks, &block, 1) != 1)
        break;

      LG_AudioClock clock = block.clock;
      int remaining = block.frames;
      while (remaining > 0 && !atomic_load_explicit(
               &pw.record.sendStop, memory_order_acquire))
      {
        const int frames = min(
            remaining, pw.record.sendBufferFrames);
        const int consumed = ringbuffer_consume(
            pw.record.sendQueue, pw.record.sendBuffer, frames);
        DEBUG_ASSERT(consumed == frames);
        if (consumed != frames)
        {
          atomic_fetch_add_explicit(
              &pw.record.bufferErrors, 1, memory_order_relaxed);
          break;
        }

        if (!pw.record.pushFn(pw.record.sendBuffer, frames,
              block.clockValid ? &clock : NULL))
          pipewire_recordRejected(frames);
        remaining -= frames;
        if (block.clockValid)
          pipewire_recordAdvanceClock(&clock, frames);
      }
    }

    if (atomic_load_explicit(
          &pw.record.sendStop, memory_order_acquire))
      break;
  }

  pipewire_reportRecordErrors();
  return 0;
}

static void pipewire_recordStopSender(void)
{
  const bool hadThread = pw.record.sendThread != NULL;
  if (pw.record.sendThread)
  {
    atomic_store_explicit(
        &pw.record.sendStop, true, memory_order_release);
    pipewire_recordSignalSender();
    lgJoinThread(pw.record.sendThread, NULL);
    pw.record.sendThread = NULL;
  }

  if (pw.record.sendWakeInitialized)
  {
    sem_destroy(&pw.record.sendWake);
    pw.record.sendWakeInitialized = false;
  }

  ringbuffer_free(&pw.record.sendQueue);
  ringbuffer_free(&pw.record.sendBlocks);
  free(pw.record.sendBuffer);
  free(pw.record.latestBuffer);
  pw.record.sendBuffer       = NULL;
  pw.record.latestBuffer     = NULL;
  pw.record.sendBufferFrames = 0;
  pw.record.maxQueuedFrames  = 0;
  if (!hadThread)
    pipewire_reportRecordErrors();
}

static bool pipewire_recordStartSender(int sampleRate)
{
  /* This is overload capacity, not a prebuffer. The sender drains whatever is
   * available as soon as the RT callback transitions the queue from empty. */
  const int queueFrames = max(sampleRate / 10, 1);
  const int sendFrames = max(sampleRate / 100, 1);

  pw.record.sendQueue = ringbuffer_new(
      queueFrames, pw.record.stride);
  pw.record.sendBlocks = ringbuffer_new(
      queueFrames, sizeof(RecordBlock));
  pw.record.sendBuffer = malloc(
      (size_t)sendFrames * pw.record.stride);
  pw.record.latestBuffer = malloc(
      (size_t)RECORD_LATEST_SLOTS * sendFrames * pw.record.stride);
  pw.record.sendBufferFrames = sendFrames;
  pw.record.maxQueuedFrames  = max(sampleRate / 50, 1);
  if (!pw.record.sendQueue || !pw.record.sendBlocks ||
      !pw.record.sendBuffer || !pw.record.latestBuffer)
  {
    DEBUG_ERROR("Failed to allocate the PipeWire recording queue");
    pipewire_recordStopSender();
    return false;
  }

  if (sem_init(&pw.record.sendWake, 0, 0) < 0)
  {
    DEBUG_ERROR("Failed to create the PipeWire recording worker semaphore");
    pipewire_recordStopSender();
    return false;
  }
  pw.record.sendWakeInitialized = true;

  atomic_store_explicit(
      &pw.record.sendStop, false, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.sendWakePending, false, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.latestMode, false, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.latestSerial, 0, memory_order_relaxed);
  for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
  {
    atomic_store_explicit(&pw.record.latest[i].state,
        RECORD_LATEST_FREE, memory_order_relaxed);
    atomic_store_explicit(
        &pw.record.latest[i].serial, 0, memory_order_relaxed);
    pw.record.latest[i].frames     = 0;
    pw.record.latest[i].clockValid = false;
    pw.record.latest[i].clock      = (LG_AudioClock) {0};
  }
  pw.record.clockPosition             = 0;
  pw.record.clockLastPosition         = 0;
  pw.record.clockLastTicks            = 0;
  pw.record.clockLastTime             = 0;
  pw.record.clockLastRate             = (struct spa_fraction) {0};
  pw.record.clockLastValid            = false;
  pw.record.clockLastTimingValid      = false;
  pw.record.clockDiscontinuityPending = false;
  atomic_store_explicit(
      &pw.record.bufferErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.droppedFrames, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.rejectedBatches, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.rejectedFrames, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.signalErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.recycleError, 0, memory_order_relaxed);
  if (!lgCreateThread("pwRecordSend", pipewire_recordSendThread,
        NULL, &pw.record.sendThread))
  {
    pipewire_recordStopSender();
    return false;
  }

  return true;
}

static void pipewire_recordStopStream(void)
{
  pw_thread_loop_lock(pw.thread);
  pw.record.failureFn     = NULL;
  pw.record.failureCookie = 0;
  if (pw.record.stream)
  {
    pw_stream_destroy(pw.record.stream);
    pw.record.stream = NULL;
  }
  pw.record.active = false;
  pw_thread_loop_unlock(pw.thread);

  pipewire_recordStopSender();
  pw.record.pushFn = NULL;
}

static void pipewire_recordQueueFrames(
    const void * data, const RecordBlock * block)
{
  const int frames = block->frames;
  int       chosen = -1;
  if (atomic_load_explicit(
        &pw.record.latestMode, memory_order_acquire))
    goto latest;

  const int occupancy = ringbuffer_getCount(pw.record.sendQueue);
  const int available =
    max(0, ringbuffer_getLength(pw.record.sendQueue) - occupancy);
  const bool exceedsBacklog = occupancy > 0 &&
    (occupancy >= pw.record.maxQueuedFrames ||
     frames > pw.record.maxQueuedFrames - occupancy);
  if (frames > available || exceedsBacklog)
    goto latest;

  const int advanced =
    ringbuffer_append(pw.record.sendQueue, data, frames);
  DEBUG_ASSERT(advanced == frames);
  if (advanced == frames)
  {
    const int blocks = ringbuffer_append(
        pw.record.sendBlocks, block, 1);
    DEBUG_ASSERT(blocks == 1);
    if (blocks == 1)
    {
      pipewire_recordSignalSender();
      return;
    }
  }

latest:
  for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
  {
    unsigned int expected = RECORD_LATEST_FREE;
    if (atomic_compare_exchange_strong_explicit(
          &pw.record.latest[i].state, &expected,
          RECORD_LATEST_WRITING, memory_order_acq_rel, memory_order_acquire))
    {
      chosen = i;
      break;
    }
  }

  if (chosen < 0)
  {
    int oldest = -1;
    uint64_t oldestSerial = UINT64_MAX;
    for (int i = 0; i < RECORD_LATEST_SLOTS; ++i)
    {
      if (atomic_load_explicit(
            &pw.record.latest[i].state, memory_order_acquire) !=
          RECORD_LATEST_READY)
        continue;

      const uint64_t serial = atomic_load_explicit(
          &pw.record.latest[i].serial, memory_order_relaxed);
      if (serial < oldestSerial)
      {
        oldest       = i;
        oldestSerial = serial;
      }
    }

    if (oldest >= 0)
    {
      unsigned int expected = RECORD_LATEST_READY;
      if (atomic_compare_exchange_strong_explicit(
            &pw.record.latest[oldest].state, &expected,
            RECORD_LATEST_WRITING,
            memory_order_acq_rel, memory_order_acquire))
      {
        chosen = oldest;
        const int dropped = pw.record.latest[oldest].frames;
        atomic_fetch_add_explicit(&pw.record.droppedFrames,
            dropped, memory_order_relaxed);
      }
    }
  }

  if (chosen < 0)
  {
    atomic_fetch_add_explicit(
        &pw.record.droppedFrames, frames, memory_order_relaxed);
    pw.record.clockDiscontinuityPending = true;
    atomic_store_explicit(
        &pw.record.latestMode, true, memory_order_release);
    pipewire_recordSignalSender();
    return;
  }

  const int keep = min(frames, pw.record.sendBufferFrames);
  memcpy(
      pw.record.latestBuffer +
        (size_t)chosen * pw.record.sendBufferFrames * pw.record.stride,
      (const uint8_t *)data + (size_t)(frames - keep) * pw.record.stride,
      (size_t)keep * pw.record.stride);
  pw.record.latest[chosen].frames     = keep;
  pw.record.latest[chosen].clockValid = block->clockValid;
  if (block->clockValid)
  {
    pw.record.latest[chosen].clock = block->clock;
    pipewire_recordAdvanceClock(
        &pw.record.latest[chosen].clock, frames - keep);
    pw.record.latest[chosen].clock.discontinuity = true;
    pw.record.latest[chosen].clock.stable        = false;
  }
  atomic_store_explicit(&pw.record.latest[chosen].serial,
      atomic_fetch_add_explicit(
        &pw.record.latestSerial, 1, memory_order_relaxed) + 1,
      memory_order_relaxed);
  atomic_store_explicit(&pw.record.latest[chosen].state,
      RECORD_LATEST_READY, memory_order_release);
  atomic_store_explicit(
      &pw.record.latestMode, true, memory_order_release);

  if (keep != frames)
    atomic_fetch_add_explicit(
        &pw.record.droppedFrames, frames - keep, memory_order_relaxed);
  pipewire_recordSignalSender();
}

static void pipewire_onRecordProcess(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.record.stream)))
  {
    atomic_fetch_add_explicit(
        &pw.record.bufferErrors, 1, memory_order_relaxed);
    pw.record.clockDiscontinuityPending = true;
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  if (!sbuf || sbuf->n_datas == 0 || !sbuf->datas[0].data ||
      !sbuf->datas[0].chunk ||
      sbuf->datas[0].chunk->offset > sbuf->datas[0].maxsize)
  {
    atomic_fetch_add_explicit(
        &pw.record.bufferErrors, 1, memory_order_relaxed);
    pw.record.clockDiscontinuityPending = true;
#if PW_CHECK_VERSION(1, 4, 0)
    const int recycleResult =
      pw_stream_return_buffer(pw.record.stream, pbuf);
#else
    const int recycleResult =
      pw_stream_queue_buffer(pw.record.stream, pbuf);
#endif
    pipewire_storeError(&pw.record.recycleError, recycleResult);
    return;
  }

  const uint32_t offset = sbuf->datas[0].chunk->offset;
  const uint32_t flags  = sbuf->datas[0].chunk->flags;
  const bool     empty  = flags & SPA_CHUNK_FLAG_EMPTY;
  if (flags & (SPA_CHUNK_FLAG_EMPTY | SPA_CHUNK_FLAG_CORRUPTED))
    pw.record.clockDiscontinuityPending = true;

  const uint32_t bytes = empty ? 0 : min(
      sbuf->datas[0].chunk->size,
      sbuf->datas[0].maxsize - offset);
  const int frames = bytes / pw.record.stride;
  if (bytes % pw.record.stride)
    pw.record.clockDiscontinuityPending = true;
  if (frames > 0)
  {
    const RecordBlock block =
      pipewire_recordMakeBlock(pbuf, sbuf, frames);
    pipewire_recordQueueFrames(
        (uint8_t *)sbuf->datas[0].data + offset, &block);
  }

  pipewire_storeError(&pw.record.recycleError,
      pw_stream_queue_buffer(pw.record.stream, pbuf));
}

static void pipewire_onRecordStateChanged(void * userdata,
    enum pw_stream_state old, enum pw_stream_state state,
    const char * error)
{
  (void)error;
  pw.record.connectionState = state;
  if (state == PW_STREAM_STATE_ERROR ||
      state == PW_STREAM_STATE_UNCONNECTED)
  {
    LG_AudioFailureFn failureFn = pw.record.failureFn;
    const uint32_t failureCookie = pw.record.failureCookie;
    pw.record.failureFn     = NULL;
    pw.record.failureCookie = 0;

    if (failureFn && failureCookie)
      failureFn(failureCookie);
  }
  pw_thread_loop_signal(pw.thread, false);
}

static int pipewire_recordControl(bool active)
{
  int error = 0;
  pw_thread_loop_lock(pw.thread);
  if (!pw.record.stream)
    error = active ? -ENODEV : 0;
  else if (pw.record.active != active)
  {
    error = pw_stream_set_active(pw.record.stream, active);
    if (error >= 0)
      pw.record.active = active;
  }
  pw_thread_loop_unlock(pw.thread);
  return error;
}

static bool pipewire_recordStart(const LG_AudioFormat * format,
    LG_AudioPushFn pushFn, LG_AudioFailureFn failureFn,
    uint32_t failureCookie)
{
  pipewire_recordStopStream();

  const int channels     = format->channelCount;
  const int sampleRate   = format->sampleRate;
  const int sampleSize   = pipewire_sampleSize(format->sampleFormat);
  const enum spa_audio_format sampleFormat =
    pipewire_sampleFormat(format->sampleFormat);
  if (!sampleSize || sampleFormat == SPA_AUDIO_FORMAT_UNKNOWN)
    return false;

  const int periodFrames = max(sampleRate / 1000, 1);
  char requestedNodeLatency[32];
  snprintf(requestedNodeLatency, sizeof(requestedNodeLatency), "%d/%d",
      periodFrames, sampleRate);
  const struct spa_pod * params[2];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = pipewire_onRecordStateChanged,
    .process       = pipewire_onRecordProcess
  };

  pw.record.format = *format;
  pw.record.stride = sampleSize * channels;
  pw.record.pushFn = pushFn;
  if (!pipewire_recordStartSender(sampleRate))
    return false;

  struct pw_properties * props =
    pw_properties_new(
      PW_KEY_NODE_NAME     , "Looking Glass",
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE    , "Music",
      PW_KEY_NODE_LATENCY  , requestedNodeLatency,
      NULL
    );
  if (!props)
  {
    DEBUG_ERROR("Failed to create recording stream properties");
    pipewire_recordStopSender();
    return false;
  }

  const char * device = option_get_string("pipewire", "recDevice");
  if (device)
  {
#ifdef PW_KEY_TARGET_OBJECT
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, device);
#else
    pw_properties_set(props, PW_KEY_NODE_TARGET, device);
#endif
  }

  pw_thread_loop_lock(pw.thread);
  pw.record.failureFn     = failureFn;
  pw.record.failureCookie = failureCookie;
  pw.record.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    props,
    &events,
    NULL
  );

  if (!pw.record.stream)
  {
    pw.record.failureFn     = NULL;
    pw.record.failureCookie = 0;
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create the stream");
    pipewire_recordStopSender();
    return false;
  }

  struct spa_audio_info_raw info =
    pipewire_audioInfo(format, sampleFormat);
  params[0] = spa_format_audio_raw_build(
      &b, SPA_PARAM_EnumFormat, &info);
  params[1] = spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));

  pw.record.connectionState = PW_STREAM_STATE_CONNECTING;
  int result = pw_stream_connect(
      pw.record.stream,
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS  |
      PW_STREAM_FLAG_INACTIVE,
      params, 2);

  if (result < 0)
  {
    DEBUG_ERROR("Failed to connect recording stream: %s",
        spa_strerror(result));
    pw.record.failureFn     = NULL;
    pw.record.failureCookie = 0;
    pw_stream_destroy(pw.record.stream);
    pw.record.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
    pipewire_recordStopSender();
    return false;
  }

  if (!pipewire_waitForStream(&pw.record.connectionState) ||
      pw.record.connectionState != PW_STREAM_STATE_PAUSED)
  {
    DEBUG_ERROR("PipeWire recording stream did not become ready");
    pw.record.failureFn     = NULL;
    pw.record.failureCookie = 0;
    pw_stream_destroy(pw.record.stream);
    pw.record.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
    pipewire_recordStopSender();
    return false;
  }

  pw_thread_loop_unlock(pw.thread);
  result = pipewire_recordControl(true);
  if (result < 0)
  {
    DEBUG_ERROR("Failed to activate recording stream: %s",
        spa_strerror(result));
    pipewire_recordStopStream();
    return false;
  }

  return true;
}

static void pipewire_recordStop(void)
{
  pipewire_recordStopStream();
}

static void pipewire_recordVolume(int channels, const uint16_t volume[])
{
  if (channels <= 0 || channels > LG_AUDIO_MAX_CHANNELS)
    return;

  float param[channels];
  for(int i = 0; i < channels; ++i)
    param[i] = max(0.0,
        9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787);

  int result = 0;
  pw_thread_loop_lock(pw.thread);
  if (pw.record.stream &&
      channels == pw.record.format.channelCount)
    result = pw_stream_set_control(pw.record.stream,
        SPA_PROP_channelVolumes, channels, param, 0);
  pw_thread_loop_unlock(pw.thread);

  if (result < 0)
    DEBUG_WARN("Failed to set PipeWire recording volume: %s",
        spa_strerror(result));
}

static void pipewire_recordMute(bool mute)
{
  int result = 0;
  pw_thread_loop_lock(pw.thread);
  if (pw.record.stream)
  {
    float val = mute ? 1.0f : 0.0f;
    result = pw_stream_set_control(
        pw.record.stream, SPA_PROP_mute, 1, &val, 0);
  }
  pw_thread_loop_unlock(pw.thread);

  if (result < 0)
    DEBUG_WARN("Failed to set PipeWire recording mute: %s",
        spa_strerror(result));
}

static void pipewire_free(void)
{
  pipewire_playbackStopStream();
  pipewire_recordStopStream();

  pw_thread_loop_lock(pw.thread);
  if (pw.core)
  {
    pw_core_disconnect(pw.core);
    pw.core = NULL;
  }
  pw_thread_loop_unlock(pw.thread);
  pw_thread_loop_stop(pw.thread);
  pw_thread_loop_destroy(pw.thread);
  pw_context_destroy(pw.context);
  pw_loop_destroy(pw.loop);

  pw.loop    = NULL;
  pw.context = NULL;
  pw.thread  = NULL;

  pw_deinit();
}

struct LG_AudioDevOps LGAD_PipeWire =
{
  .name      = "PipeWire",
  .earlyInit = pipewire_earlyInit,
  .init      = pipewire_init,
  .free      = pipewire_free,
  .playback =
  {
    .setup   = pipewire_playbackSetup,
    .start   = pipewire_playbackStart,
    .stop    = pipewire_playbackStop,
    .volume  = pipewire_playbackVolume,
    .mute    = pipewire_playbackMute,
#if PW_CHECK_VERSION(1, 4, 0)
    .setRate = pipewire_playbackSetRate,
#endif
    .latency = pipewire_playbackLatency
  },
  .record =
  {
    .start  = pipewire_recordStart,
    .stop   = pipewire_recordStop,
    .volume = pipewire_recordVolume,
    .mute   = pipewire_recordMute
  }
};
