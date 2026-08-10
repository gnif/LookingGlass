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

#include <pulse/pulseaudio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#include "common/debug.h"
#include "common/time.h"

#define PULSEAUDIO_ERROR_REPORT_INTERVAL_NS INT64_C(1000000000)
#define PULSEAUDIO_OPERATION_TIMEOUT_US      UINT64_C(250000)
#define PULSEAUDIO_READY_TIMEOUT_US          UINT64_C(5000000)
#define PULSEAUDIO_RATE_UPDATE_INTERVAL_NS   INT64_C(50000000)
#define PULSEAUDIO_RATE_UPDATE_MAX_HOLD_NS   INT64_C(250000000)
#define PULSEAUDIO_RATE_UPDATE_DEADBAND_HZ   UINT32_C(1)

struct PulseAudio
{
  pa_threaded_mainloop * loop;
  pa_mainloop_api      * api;
  pa_context           * context;
  bool                   loopStarted;

  pa_stream            * sink;
  uint32_t               sinkIndex;
  bool                   sinkCorked;
  bool                   sinkResamplerEnabled;
  bool                   sinkRateFailed;
  int                    sinkMaxPeriodFrames;
  int                    sinkStartFrames;
  LG_AudioFormat         sinkFormat;
  int                    sinkStride;
  uint32_t               sinkNominalRate;
  uint32_t               sinkAppliedRate;
  uint32_t               sinkPendingRate;
  uint32_t               sinkRequestedRate;
  uint32_t               sinkDeferredRate;
  int64_t                sinkNextRateUpdate;
  int64_t                sinkRateDeferredSince;
  bool                   sinkRateUpdateArmed;
  pa_operation         * sinkRateOperation;
  pa_operation         * sinkStartOperation;
  pa_time_event        * sinkStartTimer;
  uint32_t               sinkStartSerial;
  LG_AudioPullFn         sinkPullFn;
  LG_AudioFailureFn      sinkFailureFn;
  uint32_t               sinkFailureCookie;
  _Atomic(int64_t)       sinkLatencyNs;
  atomic_bool            sinkLatencyUpdateRequested;
  atomic_bool            sinkErrorsPending;
  _Atomic(int64_t)       sinkNextErrorReport;
  atomic_uint            sinkUnderflows;
  atomic_uint            sinkOverflows;
  atomic_uint            sinkWriteErrors;
  atomic_uint            sinkRateErrors;
  atomic_uint            sinkControlErrors;
};

static struct PulseAudio pa = {0};

static bool pulseaudio_audioFormatEqual(const LG_AudioFormat * a,
    const LG_AudioFormat * b)
{
  return
    a->sampleFormat == b->sampleFormat &&
    a->sampleRate   == b->sampleRate &&
    a->channelCount == b->channelCount &&
    memcmp(a->channels, b->channels,
        a->channelCount * sizeof(*a->channels)) == 0;
}

static pa_sample_format_t pulseaudio_sampleFormat(
    LG_AudioSampleFormat format)
{
  switch (format)
  {
    case LG_AUDIO_FMT_U8     : return PA_SAMPLE_U8;
    case LG_AUDIO_FMT_S16_LE : return PA_SAMPLE_S16LE;
    case LG_AUDIO_FMT_S24_LE : return PA_SAMPLE_S24LE;
    case LG_AUDIO_FMT_S32_LE : return PA_SAMPLE_S32LE;
    case LG_AUDIO_FMT_F32_LE : return PA_SAMPLE_FLOAT32LE;
    case LG_AUDIO_FMT_F32_NE : return PA_SAMPLE_FLOAT32;
    case LG_AUDIO_FMT_F64_LE : return PA_SAMPLE_INVALID;
  }

  return PA_SAMPLE_INVALID;
}

static pa_channel_position_t pulseaudio_channel(
    LG_AudioChannel channel, uint8_t index)
{
  switch (channel)
  {
    case LG_AUDIO_CH_UNKNOWN:
      return (pa_channel_position_t)(PA_CHANNEL_POSITION_AUX0 + index);

    case LG_AUDIO_CH_MONO               :
      return PA_CHANNEL_POSITION_MONO;
    case LG_AUDIO_CH_FRONT_LEFT         :
      return PA_CHANNEL_POSITION_FRONT_LEFT;
    case LG_AUDIO_CH_FRONT_RIGHT        :
      return PA_CHANNEL_POSITION_FRONT_RIGHT;
    case LG_AUDIO_CH_FRONT_CENTER       :
      return PA_CHANNEL_POSITION_FRONT_CENTER;
    case LG_AUDIO_CH_LFE                :
      return PA_CHANNEL_POSITION_LFE;
    case LG_AUDIO_CH_REAR_LEFT          :
      return PA_CHANNEL_POSITION_REAR_LEFT;
    case LG_AUDIO_CH_REAR_RIGHT         :
      return PA_CHANNEL_POSITION_REAR_RIGHT;
    case LG_AUDIO_CH_FRONT_LEFT_CENTER  :
      return PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER;
    case LG_AUDIO_CH_FRONT_RIGHT_CENTER :
      return PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER;
    case LG_AUDIO_CH_REAR_CENTER        :
      return PA_CHANNEL_POSITION_REAR_CENTER;
    case LG_AUDIO_CH_SIDE_LEFT          :
      return PA_CHANNEL_POSITION_SIDE_LEFT;
    case LG_AUDIO_CH_SIDE_RIGHT         :
      return PA_CHANNEL_POSITION_SIDE_RIGHT;
    case LG_AUDIO_CH_TOP_CENTER         :
      return PA_CHANNEL_POSITION_TOP_CENTER;
    case LG_AUDIO_CH_TOP_FRONT_LEFT     :
      return PA_CHANNEL_POSITION_TOP_FRONT_LEFT;
    case LG_AUDIO_CH_TOP_FRONT_CENTER   :
      return PA_CHANNEL_POSITION_TOP_FRONT_CENTER;
    case LG_AUDIO_CH_TOP_FRONT_RIGHT    :
      return PA_CHANNEL_POSITION_TOP_FRONT_RIGHT;
    case LG_AUDIO_CH_TOP_REAR_LEFT      :
      return PA_CHANNEL_POSITION_TOP_REAR_LEFT;
    case LG_AUDIO_CH_TOP_REAR_CENTER    :
      return PA_CHANNEL_POSITION_TOP_REAR_CENTER;
    case LG_AUDIO_CH_TOP_REAR_RIGHT     :
      return PA_CHANNEL_POSITION_TOP_REAR_RIGHT;
  }

  return (pa_channel_position_t)(PA_CHANNEL_POSITION_AUX0 + index);
}

static void pulseaudio_noteError(atomic_uint * counter)
{
  atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
  atomic_store_explicit(
      &pa.sinkErrorsPending, true, memory_order_release);
}

static void pulseaudio_trackControlOperation(pa_operation * operation)
{
  if (!operation)
    pulseaudio_noteError(&pa.sinkControlErrors);
  else
    pa_operation_unref(operation);
}

static void pulseaudio_streamControl_cb(
    pa_stream * stream, int success, void * userdata)
{
  if (!success)
    pulseaudio_noteError(&pa.sinkControlErrors);
}

static void pulseaudio_contextControl_cb(
    pa_context * context, int success, void * userdata)
{
  if (!success)
    pulseaudio_noteError(&pa.sinkControlErrors);
}

static void pulseaudio_reportErrors(bool force)
{
  if (!atomic_load_explicit(
        &pa.sinkErrorsPending, memory_order_acquire))
    return;

  const int64_t now = (int64_t)nanotime();
  if (!force)
  {
    int64_t next = atomic_load_explicit(
        &pa.sinkNextErrorReport, memory_order_relaxed);
    if (now < next || !atomic_compare_exchange_strong_explicit(
          &pa.sinkNextErrorReport, &next,
          now + PULSEAUDIO_ERROR_REPORT_INTERVAL_NS,
          memory_order_relaxed, memory_order_relaxed))
      return;
  }

  if (!atomic_exchange_explicit(
        &pa.sinkErrorsPending, false, memory_order_acq_rel))
    return;

  const unsigned int underflows = atomic_exchange_explicit(
      &pa.sinkUnderflows, 0, memory_order_relaxed);
  const unsigned int overflows = atomic_exchange_explicit(
      &pa.sinkOverflows, 0, memory_order_relaxed);
  const unsigned int writeErrors = atomic_exchange_explicit(
      &pa.sinkWriteErrors, 0, memory_order_relaxed);
  const unsigned int rateErrors = atomic_exchange_explicit(
      &pa.sinkRateErrors, 0, memory_order_relaxed);
  const unsigned int controlErrors = atomic_exchange_explicit(
      &pa.sinkControlErrors, 0, memory_order_relaxed);

  if (underflows)
    DEBUG_WARN("PulseAudio playback underflowed %u time(s)", underflows);
  if (overflows)
    DEBUG_WARN("PulseAudio playback overflowed %u time(s)", overflows);
  if (writeErrors)
    DEBUG_WARN("PulseAudio playback write failed %u time(s)", writeErrors);
  if (rateErrors)
    DEBUG_WARN("PulseAudio sample rate update failed %u time(s)",
        rateErrors);
  if (controlErrors)
    DEBUG_WARN("PulseAudio control operation failed %u time(s)",
        controlErrors);
}

static bool pulseaudio_submitRateUpdate(bool force);

static void pulseaudio_rateUpdate_cb(pa_stream * stream, int success,
    void * userdata)
{
  if (stream != pa.sink || !pa.sinkRateOperation)
    return;

  pa_operation * operation = pa.sinkRateOperation;
  pa.sinkRateOperation = NULL;
  pa_operation_unref(operation);

  if (!success)
  {
    pulseaudio_noteError(&pa.sinkRateErrors);
    pa.sinkRateFailed = true;
    return;
  }

  pa.sinkAppliedRate = pa.sinkPendingRate;
  if (pa.sinkCorked)
    pulseaudio_submitRateUpdate(true);
}

static bool pulseaudio_submitRateUpdate(bool force)
{
  if (pa.sinkRateFailed)
    return false;

  if (pa.sinkRateOperation)
    return true;

  if (pa.sinkRequestedRate == pa.sinkAppliedRate)
  {
    pa.sinkDeferredRate      = pa.sinkRequestedRate;
    pa.sinkRateUpdateArmed   = false;
    pa.sinkRateDeferredSince = 0;
    return true;
  }

  if (!force && !pa.sinkRateUpdateArmed)
    return true;

  pa.sinkPendingRate       = pa.sinkRequestedRate;
  pa.sinkDeferredRate      = pa.sinkPendingRate;
  pa.sinkRateUpdateArmed   = false;
  pa.sinkRateDeferredSince = 0;
  pa.sinkNextRateUpdate =
    (int64_t)nanotime() + PULSEAUDIO_RATE_UPDATE_INTERVAL_NS;
  pa.sinkRateOperation = pa_stream_update_sample_rate(pa.sink,
      pa.sinkPendingRate, pulseaudio_rateUpdate_cb, NULL);
  if (!pa.sinkRateOperation)
  {
    pulseaudio_noteError(&pa.sinkRateErrors);
    pa.sinkRateFailed = true;
    return false;
  }

  return true;
}

static void pulseaudio_cancelStartTimer_nl(void)
{
  if (!pa.sinkStartTimer)
    return;

  pa_time_event * timer = pa.sinkStartTimer;
  pa.sinkStartTimer = NULL;
  pa.api->time_free(timer);
}

static void pulseaudio_cancelStartOperation_nl(void)
{
  if (!pa.sinkStartOperation)
    return;

  pa_operation * operation = pa.sinkStartOperation;
  pa.sinkStartOperation    = NULL;
  pa_operation_cancel(operation);
  pa_operation_unref(operation);
}

static void pulseaudio_sinkDisarm_nl(void)
{
  ++pa.sinkStartSerial;
  pulseaudio_cancelStartTimer_nl();
  pulseaudio_cancelStartOperation_nl();
  pa.sinkFailureFn     = NULL;
  pa.sinkFailureCookie = 0;
}

static void pulseaudio_sinkFailed_nl(void)
{
  LG_AudioFailureFn failureFn = pa.sinkFailureFn;
  const uint32_t failureCookie = pa.sinkFailureCookie;
  pulseaudio_sinkDisarm_nl();
  if (failureFn)
    failureFn(failureCookie);
}

static void pulseaudio_ctx_state_change_cb(pa_context * c, void * userdata)
{
  switch (pa_context_get_state(c))
  {
    case PA_CONTEXT_CONNECTING:
    case PA_CONTEXT_AUTHORIZING:
    case PA_CONTEXT_SETTING_NAME:
      break;

    case PA_CONTEXT_READY:
      DEBUG_INFO("Connected to PulseAudio server");
      pa_threaded_mainloop_signal(pa.loop, 0);
      break;

    case PA_CONTEXT_TERMINATED:
      if (c == pa.context)
        pulseaudio_sinkFailed_nl();
      pa_threaded_mainloop_signal(pa.loop, 0);
      break;

    case PA_CONTEXT_FAILED:
    default:
      if (c == pa.context)
        pulseaudio_sinkFailed_nl();
      DEBUG_ERROR("context error: %s", pa_strerror(pa_context_errno(c)));
      pa_threaded_mainloop_signal(pa.loop, 0);
      break;
  }
}

static void pulseaudio_context_close_nl(void)
{
  if (!pa.context)
    return;

  pa_context_set_state_callback(pa.context, NULL, NULL);
  pa_context_disconnect(pa.context);
  pa_context_unref(pa.context);
  pa.context = NULL;
}

struct PulseWait
{
  bool timedOut;
};

static void pulseaudio_timeout_cb(pa_mainloop_api * api,
    pa_time_event * event, const struct timeval * tv, void * userdata)
{
  (void)api;
  (void)event;
  (void)tv;
  struct PulseWait * wait = userdata;
  wait->timedOut = true;
  pa_threaded_mainloop_signal(pa.loop, 0);
}

/* The PulseAudio threaded mainloop must be locked. */
static bool pulseaudio_contextConnect_nl(void)
{
  pa_proplist * propList = pa_proplist_new();
  if (!propList)
  {
    DEBUG_ERROR("Failed to create the PulseAudio property list");
    return false;
  }
  pa_proplist_sets(propList, PA_PROP_MEDIA_ROLE, "video");

  pa.context = pa_context_new_with_proplist(
      pa.api, "Looking Glass", propList);
  pa_proplist_free(propList);
  if (!pa.context)
  {
    DEBUG_ERROR("Failed to create the PulseAudio context");
    return false;
  }

  pa_context_set_state_callback(pa.context,
      pulseaudio_ctx_state_change_cb, NULL);
  if (pa_context_connect(
        pa.context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
  {
    DEBUG_ERROR("Failed to connect to the PulseAudio server: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pulseaudio_context_close_nl();
    return false;
  }

  struct PulseWait wait = {0};
  pa_time_event * timer = pa_context_rttime_new(pa.context,
      pa_rtclock_now() + PULSEAUDIO_READY_TIMEOUT_US,
      pulseaudio_timeout_cb, &wait);
  if (!timer)
  {
    DEBUG_ERROR("Failed to create the PulseAudio connection timer");
    pulseaudio_context_close_nl();
    return false;
  }

  pa_context_state_t state;
  while (!wait.timedOut)
  {
    state = pa_context_get_state(pa.context);
    if (state == PA_CONTEXT_READY || !PA_CONTEXT_IS_GOOD(state))
      break;
    pa_threaded_mainloop_wait(pa.loop);
  }
  state = pa_context_get_state(pa.context);
  pa.api->time_free(timer);

  if (state == PA_CONTEXT_READY)
    return true;

  if (wait.timedOut)
    DEBUG_ERROR("Timed out connecting to the PulseAudio server");
  else
    DEBUG_ERROR("PulseAudio context did not become ready: %s",
        pa_strerror(pa_context_errno(pa.context)));
  pulseaudio_context_close_nl();
  return false;
}

static bool pulseaudio_init(void)
{
  pa.sinkIndex = PA_INVALID_INDEX;
  pa.loop = pa_threaded_mainloop_new();
  if (!pa.loop)
  {
    DEBUG_ERROR("Failed to create the main loop");
    goto err;
  }

  pa.api = pa_threaded_mainloop_get_api(pa.loop);
  if (pa_threaded_mainloop_start(pa.loop) < 0)
  {
    DEBUG_ERROR("Failed to start the main loop");
    goto err_loop;
  }
  pa.loopStarted = true;

  pa_threaded_mainloop_lock(pa.loop);
  if (!pulseaudio_contextConnect_nl())
  {
    pa_threaded_mainloop_unlock(pa.loop);
    goto err_thread;
  }
  pa_threaded_mainloop_unlock(pa.loop);
  return true;

err_thread:
  if (pa.loopStarted)
  {
    pa_threaded_mainloop_stop(pa.loop);
    pa.loopStarted = false;
  }

err_loop:
  pa_threaded_mainloop_free(pa.loop);
  pa.loop = NULL;
  pa.api  = NULL;

err:
  return false;
}

static void pulseaudio_sink_close_nl(void)
{
  pulseaudio_sinkDisarm_nl();
  if (!pa.sink)
    return;

  pa_stream_set_state_callback(pa.sink, NULL, NULL);
  pa_stream_set_write_callback(pa.sink, NULL, NULL);
  pa_stream_set_underflow_callback(pa.sink, NULL, NULL);
  pa_stream_set_overflow_callback(pa.sink, NULL, NULL);
  if (pa.sinkRateOperation)
  {
    pa_operation * operation = pa.sinkRateOperation;
    pa.sinkRateOperation = NULL;
    pa_operation_cancel(operation);
    pa_operation_unref(operation);
  }
  pa_stream_disconnect(pa.sink);
  pa_stream_unref(pa.sink);
  pa.sink                  = NULL;
  pa.sinkIndex             = PA_INVALID_INDEX;
  pa.sinkResamplerEnabled  = false;
  pa.sinkRateFailed        = false;
  pa.sinkNominalRate       = 0;
  pa.sinkAppliedRate       = 0;
  pa.sinkPendingRate       = 0;
  pa.sinkRequestedRate     = 0;
  pa.sinkDeferredRate      = 0;
  pa.sinkNextRateUpdate    = 0;
  pa.sinkRateDeferredSince = 0;
  pa.sinkRateUpdateArmed   = false;
  atomic_store_explicit(
      &pa.sinkLatencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pa.sinkLatencyUpdateRequested, false, memory_order_relaxed);
}

static void pulseaudio_free(void)
{
  if (!pa.loop)
    return;

  pa_threaded_mainloop_lock(pa.loop);

  pulseaudio_sink_close_nl();
  pulseaudio_context_close_nl();
  pa_threaded_mainloop_unlock(pa.loop);
  pulseaudio_reportErrors(true);

  if (pa.loopStarted)
  {
    pa_threaded_mainloop_stop(pa.loop);
    pa.loopStarted = false;
  }
  pa_threaded_mainloop_free(pa.loop);
  pa.loop = NULL;
  pa.api  = NULL;
}

static void pulseaudio_state_cb(pa_stream * p, void * userdata)
{
  const pa_stream_state_t state = pa_stream_get_state(p);
  if (p == pa.sink &&
      (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED))
    pulseaudio_sinkFailed_nl();
  pa_threaded_mainloop_signal(pa.loop, 0);
}

static void pulseaudio_start_cb(
    pa_stream * stream, int success, void * userdata)
{
  const uint32_t serial = (uint32_t)(uintptr_t)userdata;
  if (stream != pa.sink || serial != pa.sinkStartSerial)
    return;

  pa_operation * operation = pa.sinkStartOperation;
  pa.sinkStartOperation    = NULL;
  if (operation)
    pa_operation_unref(operation);
  pulseaudio_cancelStartTimer_nl();
  if (!success || pa_stream_get_state(stream) != PA_STREAM_READY ||
      !pa.context ||
      pa_context_get_state(pa.context) != PA_CONTEXT_READY)
  {
    pa.sinkCorked = true;
    pulseaudio_sinkFailed_nl();
  }
}

static void pulseaudio_startTimeout_cb(pa_mainloop_api * api,
    pa_time_event * event, const struct timeval * tv, void * userdata)
{
  (void)tv;
  const uint32_t serial = (uint32_t)(uintptr_t)userdata;
  if (event != pa.sinkStartTimer || serial != pa.sinkStartSerial)
    return;

  pa.sinkStartTimer = NULL;
  api->time_free(event);
  pa.sinkCorked = true;
  pulseaudio_sinkFailed_nl();
}

static void pulseaudio_write_cb(pa_stream * p, size_t nbytes, void * userdata)
{
  // PulseAudio tries to pull data from the stream as soon as it is created for
  // some reason, even though it is corked
  if (pa.sinkCorked)
    return;

  uint8_t * dst;

  if (pa_stream_begin_write(p, (void **)&dst, &nbytes) < 0)
  {
    pulseaudio_noteError(&pa.sinkWriteErrors);
    return;
  }

  pa_usec_t latency = 0;
  int negative = 0;
  bool latencyValid = false;
  if (atomic_exchange_explicit(
        &pa.sinkLatencyUpdateRequested, false, memory_order_acquire))
    latencyValid = pa_stream_get_latency(p, &latency, &negative) == 0;

  int frames = nbytes / pa.sinkStride;
  frames = pa.sinkPullFn(dst, frames);
  if (frames <= 0)
  {
    pa_stream_cancel_write(p);
    return;
  }

  if (pa_stream_write(
        p, dst, frames * pa.sinkStride, NULL, 0, PA_SEEK_RELATIVE) < 0)
  {
    pulseaudio_noteError(&pa.sinkWriteErrors);
    pa_stream_cancel_write(p);
    return;
  }

  /* Queue rate changes after the current audio block. This keeps the rate
   * reported by the preceding pull aligned with the block PulseAudio has
   * already received. */
  pulseaudio_submitRateUpdate(false);

  if (latencyValid)
  {
    const int64_t latencyNs = negative ? 0 : (int64_t)latency * 1000;
    atomic_store_explicit(
        &pa.sinkLatencyNs, latencyNs, memory_order_release);
  }
}

static void pulseaudio_underflow_cb(pa_stream * p, void * userdata)
{
  pulseaudio_noteError(&pa.sinkUnderflows);
}

static void pulseaudio_overflow_cb(pa_stream * p, void * userdata)
{
  pulseaudio_noteError(&pa.sinkOverflows);
}

static bool pulseaudio_setup(const LG_AudioFormat * format,
    int requestedPeriodFrames, bool requestResampler,
    bool * resamplerEnabled, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  *resamplerEnabled = false;
  pulseaudio_reportErrors(false);

  const int channels   = format->channelCount;
  const int sampleRate = format->sampleRate;

  const pa_sample_format_t sampleFormat =
    pulseaudio_sampleFormat(format->sampleFormat);
  if (sampleFormat == PA_SAMPLE_INVALID)
    return false;

  pa_sample_spec spec = {
    .format   = sampleFormat,
    .rate     = sampleRate,
    .channels = channels
  };
  if (!pa_sample_spec_valid(&spec))
    return false;

  pa_channel_map channelMap = { .channels = channels };
  for (uint8_t i = 0; i < format->channelCount; ++i)
    channelMap.map[i] = pulseaudio_channel(format->channels[i], i);

  const int stride = (int)pa_frame_size(&spec);
  int bufferSize = requestedPeriodFrames * 2 * stride;
  pa_buffer_attr attribs =
  {
    .maxlength = -1,
    .tlength   = bufferSize,
    .prebuf    = 0,
    .minreq    = (uint32_t)-1
  };

  pa_threaded_mainloop_lock(pa.loop);
  if (!pa.context ||
      pa_context_get_state(pa.context) != PA_CONTEXT_READY)
  {
    pulseaudio_sink_close_nl();
    pulseaudio_context_close_nl();
    if (!pulseaudio_contextConnect_nl())
    {
      pa_threaded_mainloop_unlock(pa.loop);
      return false;
    }
  }

  /* pa_stream_update_sample_rate requires protocol version 12. */
  const bool enableResampler = requestResampler &&
    pa_context_get_server_protocol_version(pa.context) >= 12;
  if (pa.sink && pa.context &&
      pa_stream_get_state(pa.sink) == PA_STREAM_READY &&
      pa_context_get_state(pa.context) == PA_CONTEXT_READY &&
      !pa.sinkRateFailed &&
      pa.sinkResamplerEnabled == enableResampler &&
      !pa.sinkRateOperation &&
      pa.sinkAppliedRate == pa.sinkNominalRate &&
      pa.sinkRequestedRate == pa.sinkNominalRate &&
      pulseaudio_audioFormatEqual(&pa.sinkFormat, format))
  {
    *resamplerEnabled = pa.sinkResamplerEnabled;
    *maxPeriodFrames  = pa.sinkMaxPeriodFrames;
    *startFrames      = pa.sinkStartFrames;
    pa_threaded_mainloop_unlock(pa.loop);
    return true;
  }

  pulseaudio_sink_close_nl();

  pa.sinkFormat             = *format;
  pa.sinkStride             = stride;
  pa.sinkPullFn             = pullFn;
  pa.sinkCorked             = true;
  pa.sinkResamplerEnabled   = enableResampler;
  pa.sinkRateFailed         = false;
  pa.sinkNominalRate        = sampleRate;
  pa.sinkAppliedRate        = sampleRate;
  pa.sinkPendingRate        = sampleRate;
  pa.sinkRequestedRate      = sampleRate;
  pa.sinkDeferredRate       = sampleRate;
  pa.sinkNextRateUpdate     = 0;
  pa.sinkRateDeferredSince  = 0;
  pa.sinkRateUpdateArmed    = false;

  pa.sink = pa_stream_new(
      pa.context, "Looking Glass", &spec, &channelMap);
  if (!pa.sink)
  {
    DEBUG_ERROR("Failed to create PulseAudio stream: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

  pa_stream_set_state_callback    (pa.sink, pulseaudio_state_cb    , NULL);
  pa_stream_set_write_callback    (pa.sink, pulseaudio_write_cb    , NULL);
  pa_stream_set_underflow_callback(pa.sink, pulseaudio_underflow_cb, NULL);
  pa_stream_set_overflow_callback (pa.sink, pulseaudio_overflow_cb , NULL);

  const pa_stream_flags_t flags =
    PA_STREAM_START_CORKED |
    PA_STREAM_ADJUST_LATENCY |
    PA_STREAM_INTERPOLATE_TIMING |
    PA_STREAM_AUTO_TIMING_UPDATE |
    (enableResampler ? PA_STREAM_VARIABLE_RATE : 0);
  if (pa_stream_connect_playback(
        pa.sink, NULL, &attribs, flags, NULL, NULL) < 0)
  {
    DEBUG_ERROR("Failed to connect PulseAudio stream: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pulseaudio_sink_close_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

  pa_stream  * sink    = pa.sink;
  pa_context * context = pa.context;
  struct PulseWait wait = {0};
  pa_time_event * timer = pa_context_rttime_new(context,
      pa_rtclock_now() + PULSEAUDIO_READY_TIMEOUT_US,
      pulseaudio_timeout_cb, &wait);
  if (!timer)
  {
    DEBUG_ERROR("Failed to create the PulseAudio stream setup timer");
    pulseaudio_sink_close_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

  while (!wait.timedOut && pa.sink == sink &&
      pa_stream_get_state(sink) == PA_STREAM_CREATING &&
      pa.context == context &&
      PA_CONTEXT_IS_GOOD(pa_context_get_state(context)))
    pa_threaded_mainloop_wait(pa.loop);

  const bool ready = pa.sink == sink && pa.context == context &&
    pa_stream_get_state(sink) == PA_STREAM_READY &&
    pa_context_get_state(context) == PA_CONTEXT_READY;
  pa.api->time_free(timer);

  if (!ready)
  {
    if (wait.timedOut)
      DEBUG_ERROR("Timed out setting up the PulseAudio stream");
    else
      DEBUG_ERROR("PulseAudio stream did not become ready: %s",
          pa_strerror(pa_context_errno(context)));
    pulseaudio_sink_close_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }
  pa.sinkIndex = pa_stream_get_index(pa.sink);

  const pa_buffer_attr * actual = pa_stream_get_buffer_attr(pa.sink);
  const uint64_t minRequestFrames =
    actual && actual->minreq != UINT32_MAX ?
      actual->minreq / (uint64_t)stride : requestedPeriodFrames;
  const uint64_t targetFrames =
    actual && actual->tlength != UINT32_MAX ?
      actual->tlength / (uint64_t)stride :
      (uint64_t)requestedPeriodFrames * 2;
  const int actualMinRequest =
    clamp(minRequestFrames, UINT64_C(1), (uint64_t)INT_MAX);
  const int actualTarget =
    clamp(max(targetFrames, minRequestFrames),
        UINT64_C(1), (uint64_t)INT_MAX);

  pa.sinkMaxPeriodFrames = actualMinRequest;
  pa.sinkStartFrames     = actualTarget;

  *maxPeriodFrames  = pa.sinkMaxPeriodFrames;
  *startFrames      = pa.sinkStartFrames;
  *resamplerEnabled = pa.sinkResamplerEnabled;

  atomic_store_explicit(
      &pa.sinkLatencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pa.sinkLatencyUpdateRequested, false, memory_order_relaxed);
  pa_threaded_mainloop_unlock(pa.loop);
  return true;
}

static bool pulseaudio_start(
    LG_AudioFailureFn failureFn, uint32_t failureCookie)
{
  pulseaudio_reportErrors(false);
  if (!pa.loop || pa_threaded_mainloop_in_thread(pa.loop))
    return false;

  pa_threaded_mainloop_lock(pa.loop);

  pa_stream  * sink    = pa.sink;
  pa_context * context = pa.context;
  if (!sink || !context ||
      pa_stream_get_state(sink) != PA_STREAM_READY ||
      pa_context_get_state(context) != PA_CONTEXT_READY)
  {
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

  pulseaudio_sinkDisarm_nl();
  pa.sinkFailureFn     = failureFn;
  pa.sinkFailureCookie = failureCookie;
  const uint32_t serial = pa.sinkStartSerial;
  pa.sinkStartTimer    = pa_context_rttime_new(context,
      pa_rtclock_now() + PULSEAUDIO_OPERATION_TIMEOUT_US,
      pulseaudio_startTimeout_cb, (void *)(uintptr_t)serial);
  if (!pa.sinkStartTimer)
  {
    pulseaudio_sinkDisarm_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

  pa.sinkStartOperation = pa_stream_cork(sink, 0,
      pulseaudio_start_cb, (void *)(uintptr_t)serial);
  if (!pa.sinkStartOperation)
  {
    pulseaudio_sinkDisarm_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }
  pa.sinkCorked = false;

  pa_threaded_mainloop_unlock(pa.loop);
  return true;
}

static void pulseaudio_stop(void)
{
  if (!pa.loop)
    return;

  bool needLock = !pa_threaded_mainloop_in_thread(pa.loop);
  if (needLock)
    pa_threaded_mainloop_lock(pa.loop);

  pulseaudio_sinkDisarm_nl();
  if (!pa.sink)
  {
    if (needLock)
      pa_threaded_mainloop_unlock(pa.loop);
    return;
  }

  pa.sinkCorked = true;
  if (pa_stream_get_state(pa.sink) == PA_STREAM_READY)
  {
    pulseaudio_trackControlOperation(
        pa_stream_cork(pa.sink, 1, pulseaudio_streamControl_cb, NULL));
    pulseaudio_trackControlOperation(
        pa_stream_flush(pa.sink, pulseaudio_streamControl_cb, NULL));
  }
  if (pa.sinkResamplerEnabled)
  {
    pa.sinkRequestedRate = pa.sinkNominalRate;
    pulseaudio_submitRateUpdate(true);
  }
  atomic_store_explicit(
      &pa.sinkLatencyNs, 0, memory_order_release);
  atomic_store_explicit(
      &pa.sinkLatencyUpdateRequested, false, memory_order_relaxed);

  if (needLock)
  {
    pa_threaded_mainloop_unlock(pa.loop);
    pulseaudio_reportErrors(false);
  }
}

static void pulseaudio_volume(int channels, const uint16_t volume[])
{
  struct pa_cvolume v = { .channels = channels };
  for(int i = 0; i < channels; ++i)
    v.values[i] = pa_sw_volume_from_linear(
      max(0.0,
        9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787));

  pa_threaded_mainloop_lock(pa.loop);
  if (pa.sink && pa.sinkIndex != PA_INVALID_INDEX)
    pulseaudio_trackControlOperation(pa_context_set_sink_input_volume(
        pa.context, pa.sinkIndex, &v, pulseaudio_contextControl_cb, NULL));
  pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_mute(bool mute)
{
  pa_threaded_mainloop_lock(pa.loop);
  if (pa.sink && pa.sinkIndex != PA_INVALID_INDEX)
    pulseaudio_trackControlOperation(pa_context_set_sink_input_mute(
        pa.context, pa.sinkIndex, mute, pulseaudio_contextControl_cb, NULL));
  pa_threaded_mainloop_unlock(pa.loop);
}

static bool pulseaudio_setRate(double * ratio)
{
  if (!pa.sink || !pa.sinkResamplerEnabled ||
      pa.sinkRateFailed || !ratio || !(*ratio > 0.0))
    return false;

  const double requestedRate = pa.sinkNominalRate / *ratio;
  if (requestedRate < 1.0 || requestedRate > PA_RATE_MAX)
    return false;

  pa.sinkRequestedRate = (uint32_t)llround(requestedRate);

  uint32_t scheduledRate;
  if (pa.sinkRateOperation)
  {
    scheduledRate = pa.sinkPendingRate;
    if (pa.sinkRequestedRate == scheduledRate)
      pa.sinkRateDeferredSince = 0;
    else if (!pa.sinkRateDeferredSince ||
        pa.sinkDeferredRate != pa.sinkRequestedRate)
    {
      pa.sinkDeferredRate = pa.sinkRequestedRate;
      pa.sinkRateDeferredSince = (int64_t)nanotime();
    }
  }
  else
  {
    scheduledRate = pa.sinkAppliedRate;
    pa.sinkRateUpdateArmed = false;
    if (pa.sinkRequestedRate == scheduledRate)
      pa.sinkRateDeferredSince = 0;
    else
    {
      const int64_t now = (int64_t)nanotime();
      if (!pa.sinkRateDeferredSince ||
          pa.sinkDeferredRate != pa.sinkRequestedRate)
      {
        pa.sinkDeferredRate = pa.sinkRequestedRate;
        pa.sinkRateDeferredSince = now;
      }

      const uint32_t difference = pa.sinkRequestedRate > scheduledRate ?
        pa.sinkRequestedRate - scheduledRate :
        scheduledRate - pa.sinkRequestedRate;
      /* Coalesce controller noise around an integer-Hz boundary while still
       * applying a persistent one-Hz correction. Larger corrections only
       * wait for the operation-rate limit. */
      if (now >= pa.sinkNextRateUpdate &&
          (difference > PULSEAUDIO_RATE_UPDATE_DEADBAND_HZ ||
           now - pa.sinkRateDeferredSince >=
             PULSEAUDIO_RATE_UPDATE_MAX_HOLD_NS))
      {
        pa.sinkRateUpdateArmed = true;
        scheduledRate = pa.sinkRequestedRate;
      }
    }
  }

  if (!scheduledRate)
    return false;

  *ratio = (double)pa.sinkNominalRate / scheduledRate;
  return true;
}

static uint64_t pulseaudio_latency(void)
{
  atomic_store_explicit(
      &pa.sinkLatencyUpdateRequested, true, memory_order_release);

  const int64_t latencyNs = atomic_load_explicit(
      &pa.sinkLatencyNs, memory_order_acquire);
  if (latencyNs <= 0)
    return 0;

  return latencyNs / 1000;
}

struct LG_AudioDevOps LGAD_PulseAudio =
{
  .name   = "PulseAudio",
  .init   = pulseaudio_init,
  .free   = pulseaudio_free,
  .playback =
  {
    .setup   = pulseaudio_setup,
    .start   = pulseaudio_start,
    .stop    = pulseaudio_stop,
    .volume  = pulseaudio_volume,
    .mute    = pulseaudio_mute,
    .setRate = pulseaudio_setRate,
    .latency = pulseaudio_latency
  }
};
