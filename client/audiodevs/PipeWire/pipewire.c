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
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <pipewire/pipewire.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "common/debug.h"
#include "common/stringutils.h"
#include "common/util.h"
#include "common/option.h"
#include "common/ringbuffer.h"
#include "common/thread.h"

typedef enum
{
  STREAM_STATE_INACTIVE,
  STREAM_STATE_ACTIVE,
  STREAM_STATE_DRAINING
}
StreamState;

struct PipeWire
{
  struct pw_loop        * loop;
  struct pw_context     * context;
  struct pw_thread_loop * thread;

  struct
  {
    struct pw_stream * stream;
    struct spa_io_rate_match * rateMatch;
    _Atomic(int64_t) presentationDeadline;
    atomic_bool      latencyUpdateRequested;
    atomic_uint      bufferErrors;
    atomic_uint      timingErrors;
#if PW_CHECK_VERSION(1, 4, 0)
    double           appliedResampleRatio;
    atomic_int       resampleError;
#endif
    enum pw_stream_state connectionState;
    bool                 resamplerEnabled;

    int            channels;
    int            sampleRate;
    int            stride;
    LG_AudioPullFn pullFn;
    int            maxPeriodFrames;
    int            startFrames;

    StreamState state;
  }
  playback;

  struct
  {
    struct pw_stream * stream;

    int            channels;
    int            sampleRate;
    int            stride;
    LG_AudioPushFn pushFn;

    RingBuffer  sendQueue;
    uint8_t *   sendBuffer;
    int         sendBufferFrames;
    sem_t       sendWake;
    bool        sendWakeInitialized;
    LGThread *  sendThread;
    atomic_bool sendStop;
    atomic_uint bufferErrors;
    atomic_uint droppedFrames;
    atomic_uint signalErrors;

    bool active;
  }
  record;
};

static struct PipeWire pw = {0};

static void pipewire_reportPlaybackErrors(void)
{
  const unsigned int bufferErrors = atomic_exchange_explicit(
      &pw.playback.bufferErrors, 0, memory_order_relaxed);
  const unsigned int timingErrors = atomic_exchange_explicit(
      &pw.playback.timingErrors, 0, memory_order_relaxed);
#if PW_CHECK_VERSION(1, 4, 0)
  const int resampleError = atomic_exchange_explicit(
      &pw.playback.resampleError, 0, memory_order_relaxed);
#endif

  if (bufferErrors)
    DEBUG_WARN("PipeWire playback ran out of buffers %u time(s)",
        bufferErrors);
  if (timingErrors)
    DEBUG_WARN("PipeWire playback timing query failed %u time(s)",
        timingErrors);
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
  const unsigned int signalErrors = atomic_exchange_explicit(
      &pw.record.signalErrors, 0, memory_order_relaxed);

  if (bufferErrors)
    DEBUG_WARN("PipeWire recording encountered %u buffer error(s)",
        bufferErrors);
  if (droppedFrames)
    DEBUG_WARN("PipeWire recording dropped %u frame(s)", droppedFrames);
  if (signalErrors)
    DEBUG_WARN("PipeWire recording worker notification failed %u time(s)",
        signalErrors);
}

static int64_t pipewire_monotonicTime(void)
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return SPA_TIMESPEC_TO_NSEC(&time);
}

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

#if PW_CHECK_VERSION(0, 3, 50)
  const double latencyTicks =
    time.delay + (double)time.queued + time.buffered;
#else
  const double latencyTicks =
    time.delay + (double)time.queued / pw.playback.stride;
#endif
  const int64_t latencyNs = llrint(max(0.0, latencyTicks) *
      time.rate.num * SPA_NSEC_PER_SEC / time.rate.denom);
  atomic_store_explicit(&pw.playback.presentationDeadline,
      time.now + latencyNs, memory_order_release);
}

#if PW_CHECK_VERSION(1, 4, 0)
static bool pipewire_playbackSetRate(double ratio)
{
  if (ratio <= 0.0 ||
      !pw.playback.resamplerEnabled)
    return false;
  if (ratio == pw.playback.appliedResampleRatio)
    return true;

  const int result = pw_stream_set_rate(pw.playback.stream, ratio);
  if (result < 0)
  {
    int expected = 0;
    atomic_compare_exchange_strong_explicit(
        &pw.playback.resampleError, &expected, result,
        memory_order_relaxed, memory_order_relaxed);
    return false;
  }

  pw.playback.appliedResampleRatio = ratio;
  return true;
}

static bool pipewire_configurePlaybackResampler(bool enable)
{
  pw.playback.resamplerEnabled = false;
  pw.playback.appliedResampleRatio = 0.0;

  if (!enable)
  {
    pw_stream_set_rate(pw.playback.stream, 0.0);
    return false;
  }

  const int result = pw_stream_set_rate(pw.playback.stream, 1.0);
  if (result < 0)
    return false;

  pw.playback.resamplerEnabled = true;
  pw.playback.appliedResampleRatio = 1.0;
  return true;
}
#endif

static void pipewire_onPlaybackIoChanged(void * userdata, uint32_t id,
  void * data, uint32_t size)
{
  switch (id)
  {
    case SPA_IO_RateMatch:
      pw.playback.rateMatch = data;
      break;
  }
}

static void pipewire_onPlaybackStateChanged(void * userdata,
    enum pw_stream_state old, enum pw_stream_state state,
    const char * error)
{
  pw.playback.connectionState = state;
  if (state == PW_STREAM_STATE_ERROR)
    DEBUG_ERROR("PipeWire playback stream failed: %s",
        error ? error : "unknown error");
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

  if (sbuf->n_datas == 0 || !sbuf->datas[0].chunk ||
      !(dst = sbuf->datas[0].data))
  {
#if PW_CHECK_VERSION(1, 4, 0)
    pw_stream_return_buffer(pw.playback.stream, pbuf);
#else
    pw_stream_queue_buffer(pw.playback.stream, pbuf);
#endif
    return;
  }

  int frames = sbuf->datas[0].maxsize / pw.playback.stride;
  if (pw.playback.rateMatch && pw.playback.rateMatch->size > 0)
    frames = min(frames, pw.playback.rateMatch->size);
#if PW_CHECK_VERSION(0, 3, 50)
  else if (pbuf->requested > 0)
    frames = min(frames, pbuf->requested);
#endif

  frames = pw.playback.pullFn(dst, frames);
  if (!frames)
  {
    pbuf->size = 0;
    sbuf->datas[0].chunk->offset = 0;
    sbuf->datas[0].chunk->stride = pw.playback.stride;
    sbuf->datas[0].chunk->size = 0;
    pw_stream_queue_buffer(pw.playback.stream, pbuf);
    pipewire_updatePlaybackLatency();
    return;
  }

  pbuf->size = frames;
  sbuf->datas[0].chunk->offset = 0;
  sbuf->datas[0].chunk->stride = pw.playback.stride;
  sbuf->datas[0].chunk->size   = frames * pw.playback.stride;

  pw_stream_queue_buffer(pw.playback.stream, pbuf);
  pipewire_updatePlaybackLatency();
}

static void pipewire_onPlaybackDrained(void * userdata)
{
  pw_stream_set_active(pw.playback.stream, false);
  pw.playback.state = STREAM_STATE_INACTIVE;
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

static bool pipewire_init(void)
{
  pw_init(NULL, NULL);

  pw.loop = pw_loop_new(NULL);
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
  struct pw_core * core = pw_context_connect(pw.context, NULL, 0);
  if (!core)
    goto err_context;

  /* PipeWire is available so create the loop thread and start it */
  pw.thread = pw_thread_loop_new_full(pw.loop, "PipeWire", NULL);
  if (!pw.thread)
  {
    DEBUG_ERROR("Failed to create the thread loop");
    goto err_context;
  }

  pw_thread_loop_start(pw.thread);
  return true;

err_context:
  pw_context_destroy(pw.context);

err:
  pw_loop_destroy(pw.loop);
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
  pw_stream_destroy(pw.playback.stream);
  pw.playback.stream    = NULL;
  pw.playback.rateMatch = NULL;
  pw.playback.resamplerEnabled = false;
  atomic_store_explicit(
      &pw.playback.presentationDeadline, 0, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, false, memory_order_relaxed);
  pw_thread_loop_unlock(pw.thread);
  pipewire_reportPlaybackErrors();
}

static bool pipewire_playbackSetup(int channels, int sampleRate,
    int requestedPeriodFrames, bool requestResampler,
    bool * resamplerEnabled, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  *resamplerEnabled = false;

  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = pipewire_onPlaybackStateChanged,
    .io_changed    = pipewire_onPlaybackIoChanged,
    .process       = pipewire_onPlaybackProcess,
    .drained       = pipewire_onPlaybackDrained
  };

  if (pw.playback.stream &&
      pw.playback.channels == channels &&
      pw.playback.sampleRate == sampleRate)
  {
#if PW_CHECK_VERSION(1, 4, 0)
    atomic_store_explicit(
        &pw.playback.resampleError, 0, memory_order_relaxed);
    *resamplerEnabled =
      pipewire_configurePlaybackResampler(requestResampler);
#endif
    *maxPeriodFrames = pw.playback.maxPeriodFrames;
    *startFrames     = pw.playback.startFrames;
    return true;
  }

  pipewire_playbackStopStream();

  char requestedNodeLatency[32];
  snprintf(requestedNodeLatency, sizeof(requestedNodeLatency), "%d/%d",
    requestedPeriodFrames, sampleRate);

  pw.playback.channels    = channels;
  pw.playback.sampleRate  = sampleRate;
  pw.playback.stride      = sizeof(float) * channels;
  pw.playback.pullFn      = pullFn;

  pw_thread_loop_lock(pw.thread);

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
  const char *actualNodeLatency = properties ?
    pw_properties_get(properties, PW_KEY_NODE_LATENCY) : NULL;

  unsigned num, denom;
  if (!actualNodeLatency ||
    sscanf(actualNodeLatency, "%u/%u", &num, &denom) != 2 ||
    num == 0 || num > INT_MAX || denom != sampleRate)
  {
    DEBUG_WARN(
      "PIPEWIRE_LATENCY value '%s' is invalid or does not match stream sample "
      "rate; using %d/%d",
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
    pw.playback.maxPeriodFrames = num;

  // If the previous quantum size was very small, PipeWire can request two full
  // periods almost immediately at the start of playback
  pw.playback.startFrames = pw.playback.maxPeriodFrames * 2;

  *maxPeriodFrames = pw.playback.maxPeriodFrames;
  *startFrames     = pw.playback.startFrames;

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
        .format   = SPA_AUDIO_FORMAT_F32,
        .channels = channels,
        .rate     = sampleRate
        ));

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
    pw_stream_destroy(pw.playback.stream);
    pw.playback.stream = NULL;
    pw.playback.rateMatch = NULL;
    pw_thread_loop_unlock(pw.thread);
    return false;
  }

  while (pw.playback.connectionState == PW_STREAM_STATE_CONNECTING)
    pw_thread_loop_wait(pw.thread);

  if (pw.playback.connectionState != PW_STREAM_STATE_PAUSED)
  {
    DEBUG_ERROR("PipeWire playback stream did not become ready");
    pw_stream_destroy(pw.playback.stream);
    pw.playback.stream = NULL;
    pw.playback.rateMatch = NULL;
    pw_thread_loop_unlock(pw.thread);
    return false;
  }

  pw.playback.state = STREAM_STATE_INACTIVE;
  atomic_store_explicit(
      &pw.playback.presentationDeadline, 0, memory_order_release);
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, false, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.bufferErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.playback.timingErrors, 0, memory_order_relaxed);
#if PW_CHECK_VERSION(1, 4, 0)
  atomic_store_explicit(
      &pw.playback.resampleError, 0, memory_order_relaxed);
  *resamplerEnabled =
    pipewire_configurePlaybackResampler(requestResampler);
#else
  (void)requestResampler;
#endif
  pw_thread_loop_unlock(pw.thread);
  return true;
}

static void pipewire_playbackStart(void)
{
  if (!pw.playback.stream)
    return;

  pw_thread_loop_lock(pw.thread);
  if (pw.playback.state != STREAM_STATE_ACTIVE)
  {
    switch (pw.playback.state)
    {
      case STREAM_STATE_INACTIVE:
        pw_stream_set_active(pw.playback.stream, true);
        pw.playback.state = STREAM_STATE_ACTIVE;
        break;

      case STREAM_STATE_DRAINING:
        // We are in the middle of draining the PipeWire buffers; we need to
        // wait for this to complete before allowing the new playback to start
        break;

      default:
        DEBUG_UNREACHABLE();
    }
  }
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_playbackStop(void)
{
  const bool inThread = pw_thread_loop_in_thread(pw.thread);
  if (!inThread)
    pw_thread_loop_lock(pw.thread);

  if (pw.playback.state != STREAM_STATE_ACTIVE)
    goto done;

  if (inThread)
  {
    pw_stream_set_active(pw.playback.stream, false);
    pw.playback.state = STREAM_STATE_INACTIVE;
  }
  else
  {
    pw_stream_flush(pw.playback.stream, true);
    pw.playback.state = STREAM_STATE_DRAINING;
  }

done:
  if (!inThread)
    pw_thread_loop_unlock(pw.thread);
}

static void pipewire_playbackVolume(int channels, const uint16_t volume[])
{
  if (channels != pw.playback.channels)
    return;

  float param[channels];
  for(int i = 0; i < channels; ++i)
    param[i] = 9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787;

  pw_thread_loop_lock(pw.thread);
  pw_stream_set_control(pw.playback.stream, SPA_PROP_channelVolumes,
      channels, param, 0);
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_playbackMute(bool mute)
{
  pw_thread_loop_lock(pw.thread);
  float val = mute ? 1.0f : 0.0f;
  pw_stream_set_control(pw.playback.stream, SPA_PROP_mute, 1, &val, 0);
  pw_thread_loop_unlock(pw.thread);
}

static uint64_t pipewire_playbackLatency(void)
{
  pipewire_reportPlaybackErrors();
  atomic_store_explicit(
      &pw.playback.latencyUpdateRequested, true, memory_order_release);

  const int64_t deadline = atomic_load_explicit(
      &pw.playback.presentationDeadline, memory_order_acquire);
  if (deadline <= 0)
    return 0;

  return max(INT64_C(0), deadline - pipewire_monotonicTime()) / 1000;
}

static int pipewire_recordSendThread(void * opaque)
{
  for (;;)
  {
    int available;
    while ((available = ringbuffer_getCount(pw.record.sendQueue)) > 0)
    {
      const int frames = min(available, pw.record.sendBufferFrames);
      const int consumed = ringbuffer_consume(
          pw.record.sendQueue, pw.record.sendBuffer, frames);
      DEBUG_ASSERT(consumed == frames);
      pw.record.pushFn(pw.record.sendBuffer, frames);
    }

    pipewire_reportRecordErrors();
    if (atomic_load_explicit(&pw.record.sendStop, memory_order_acquire))
      break;

    int result;
    do
      result = sem_wait(&pw.record.sendWake);
    while (result < 0 && errno == EINTR);

    if (result < 0)
    {
      DEBUG_ERROR("Failed to wait for the PipeWire recording worker");
      break;
    }
  }

  pipewire_reportRecordErrors();
  return 0;
}

static void pipewire_recordStopSender(void)
{
  if (pw.record.sendThread)
  {
    atomic_store_explicit(
        &pw.record.sendStop, true, memory_order_release);
    if (sem_post(&pw.record.sendWake) < 0)
      DEBUG_ERROR("Failed to stop the PipeWire recording worker");
    lgJoinThread(pw.record.sendThread, NULL);
    pw.record.sendThread = NULL;
  }

  if (pw.record.sendWakeInitialized)
  {
    sem_destroy(&pw.record.sendWake);
    pw.record.sendWakeInitialized = false;
  }

  ringbuffer_free(&pw.record.sendQueue);
  free(pw.record.sendBuffer);
  pw.record.sendBuffer = NULL;
  pw.record.sendBufferFrames = 0;
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
  pw.record.sendBuffer = malloc(
      (size_t)sendFrames * pw.record.stride);
  pw.record.sendBufferFrames = sendFrames;
  if (!pw.record.sendQueue || !pw.record.sendBuffer)
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
      &pw.record.bufferErrors, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.droppedFrames, 0, memory_order_relaxed);
  atomic_store_explicit(
      &pw.record.signalErrors, 0, memory_order_relaxed);
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
  if (pw.record.stream)
  {
    pw_thread_loop_lock(pw.thread);
    pw_stream_destroy(pw.record.stream);
    pw.record.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
  }

  pw.record.active = false;
  pipewire_recordStopSender();
}

static void pipewire_recordQueueFrames(const void * data, int frames)
{
  const int occupancy = ringbuffer_getCount(pw.record.sendQueue);
  const int available =
    max(0, ringbuffer_getLength(pw.record.sendQueue) - occupancy);
  const int append = min(frames, available);
  const int advanced =
    ringbuffer_append(pw.record.sendQueue, data, append);
  DEBUG_ASSERT(advanced == append);

  if (append != frames)
    atomic_fetch_add_explicit(
        &pw.record.droppedFrames, frames - append, memory_order_relaxed);

  if (occupancy == 0 && append > 0 && sem_post(&pw.record.sendWake) < 0)
    atomic_fetch_add_explicit(
        &pw.record.signalErrors, 1, memory_order_relaxed);
}

static void pipewire_onRecordProcess(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.record.stream)))
  {
    atomic_fetch_add_explicit(
        &pw.record.bufferErrors, 1, memory_order_relaxed);
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  if (!sbuf || sbuf->n_datas == 0 || !sbuf->datas[0].data ||
      !sbuf->datas[0].chunk ||
      sbuf->datas[0].chunk->offset > sbuf->datas[0].maxsize)
  {
    atomic_fetch_add_explicit(
        &pw.record.bufferErrors, 1, memory_order_relaxed);
#if PW_CHECK_VERSION(1, 4, 0)
    pw_stream_return_buffer(pw.record.stream, pbuf);
#else
    pw_stream_queue_buffer(pw.record.stream, pbuf);
#endif
    return;
  }

  const uint32_t offset = sbuf->datas[0].chunk->offset;
  const uint32_t bytes = min(
      sbuf->datas[0].chunk->size,
      sbuf->datas[0].maxsize - offset);
  const int frames = bytes / pw.record.stride;
  if (frames > 0)
    pipewire_recordQueueFrames(
        (uint8_t *)sbuf->datas[0].data + offset, frames);

  pw_stream_queue_buffer(pw.record.stream, pbuf);
}

static void pipewire_recordStart(int channels, int sampleRate,
    LG_AudioPushFn pushFn)
{
  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = pipewire_onRecordProcess
  };

  if (pw.record.stream &&
      pw.record.channels == channels &&
      pw.record.sampleRate == sampleRate)
  {
    if (!pw.record.active)
    {
      pw_thread_loop_lock(pw.thread);
      pw_stream_set_active(pw.record.stream, true);
      pw.record.active = true;
      pw_thread_loop_unlock(pw.thread);
    }
    return;
  }

  pipewire_recordStopStream();

  pw.record.channels   = channels;
  pw.record.sampleRate = sampleRate;
  pw.record.stride     = sizeof(uint16_t) * channels;
  pw.record.pushFn     = pushFn;
  if (!pipewire_recordStartSender(sampleRate))
    return;

  struct pw_properties * props =
    pw_properties_new(
      PW_KEY_NODE_NAME     , "Looking Glass",
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE    , "Music",
      NULL
    );
  if (!props)
  {
    DEBUG_ERROR("Failed to create recording stream properties");
    pipewire_recordStopSender();
    return;
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
  pw.record.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    props,
    &events,
    NULL
  );

  if (!pw.record.stream)
  {
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create the stream");
    pipewire_recordStopSender();
    return;
  }

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
        .format   = SPA_AUDIO_FORMAT_S16,
        .channels = channels,
        .rate     = sampleRate
        ));

  const int result = pw_stream_connect(
      pw.record.stream,
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS,
      params, 1);

  if (result < 0)
  {
    DEBUG_ERROR("Failed to connect recording stream: %s",
        spa_strerror(result));
    pw_stream_destroy(pw.record.stream);
    pw.record.stream = NULL;
    pw_thread_loop_unlock(pw.thread);
    pipewire_recordStopSender();
    return;
  }

  pw_thread_loop_unlock(pw.thread);
  pw.record.active = true;
}

static void pipewire_recordStop(void)
{
  if (!pw.record.active)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_set_active(pw.record.stream, false);
  pw.record.active = false;
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_recordVolume(int channels, const uint16_t volume[])
{
  if (channels != pw.record.channels)
    return;

  float param[channels];
  for(int i = 0; i < channels; ++i)
    param[i] = 9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787;

  pw_thread_loop_lock(pw.thread);
  pw_stream_set_control(pw.record.stream, SPA_PROP_channelVolumes,
      channels, param, 0);
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_recordMute(bool mute)
{
  pw_thread_loop_lock(pw.thread);
  float val = mute ? 1.0f : 0.0f;
  pw_stream_set_control(pw.record.stream, SPA_PROP_mute, 1, &val, 0);
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_free(void)
{
  pipewire_playbackStopStream();
  pipewire_recordStopStream();
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
