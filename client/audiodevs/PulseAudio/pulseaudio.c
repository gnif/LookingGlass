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

struct PulseAudio
{
  pa_threaded_mainloop * loop;
  pa_mainloop_api      * api;
  pa_context           * context;
  pa_operation         * contextSub;

  pa_stream            * sink;
  int                    sinkIndex;
  bool                   sinkCorked;
  bool                   sinkMuted;
  bool                   sinkStarting;
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
  pa_operation         * sinkRateOperation;
  LG_AudioPullFn         sinkPullFn;
  _Atomic(int64_t)       sinkPresentationDeadline;
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

static void pulseaudio_unrefOperation(pa_operation * operation)
{
  if (operation)
    pa_operation_unref(operation);
}

static bool pulseaudio_submitRateUpdate(void);

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
    DEBUG_ERROR("Failed to update PulseAudio sample rate: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pa.sinkRateFailed = true;
    return;
  }

  pa.sinkAppliedRate = pa.sinkPendingRate;
  if (pa.sinkCorked)
    pulseaudio_submitRateUpdate();
}

static bool pulseaudio_submitRateUpdate(void)
{
  if (pa.sinkRateFailed)
    return false;

  if (pa.sinkRateOperation ||
      pa.sinkRequestedRate == pa.sinkAppliedRate)
    return true;

  pa.sinkPendingRate = pa.sinkRequestedRate;
  pa.sinkRateOperation = pa_stream_update_sample_rate(pa.sink,
      pa.sinkPendingRate, pulseaudio_rateUpdate_cb, NULL);
  if (!pa.sinkRateOperation)
  {
    DEBUG_ERROR("Failed to request a PulseAudio sample rate update: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pa.sinkRateFailed = true;
    return false;
  }

  return true;
}

static void pulseaudio_sink_input_cb(pa_context *c, const pa_sink_input_info *i,
    int eol, void *userdata)
{
  if (eol < 0 || eol == 1)
    return;

  pa.sinkIndex = i->index;
}

static void pulseaudio_subscribe_cb(pa_context *c,
    pa_subscription_event_type_t t, uint32_t index, void *userdata)
{
  switch (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)
  {
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
      if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
        pa.sinkIndex = 0;
      else
      {
        pa_operation *o = pa_context_get_sink_input_info(c, index,
            pulseaudio_sink_input_cb, NULL);
        pulseaudio_unrefOperation(o);
      }
      break;
  }
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
      pa_context_set_subscribe_callback(c, pulseaudio_subscribe_cb, NULL);
      pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);
      pa_threaded_mainloop_signal(pa.loop, 0);
      break;

    case PA_CONTEXT_TERMINATED:
      if (pa.contextSub)
      {
        pa_operation_unref(pa.contextSub);
        pa.contextSub = NULL;
      }
      break;

    case PA_CONTEXT_FAILED:
    default:
      DEBUG_ERROR("context error: %s", pa_strerror(pa_context_errno(c)));
      break;
  }
}

static bool pulseaudio_init(void)
{
  pa.loop = pa_threaded_mainloop_new();
  if (!pa.loop)
  {
    DEBUG_ERROR("Failed to create the main loop");
    goto err;
  }

  pa.api = pa_threaded_mainloop_get_api(pa.loop);
  if (pa_signal_init(pa.api) != 0)
  {
    DEBUG_ERROR("Failed to init signals");
    goto err_loop;
  }

  if (pa_threaded_mainloop_start(pa.loop) < 0)
  {
    DEBUG_ERROR("Failed to start the main loop");
    goto err_loop;
  }

  pa_proplist * propList = pa_proplist_new();
  if (!propList)
  {
    DEBUG_ERROR("Failed to create the proplist");
    goto err_thread;
  }
  pa_proplist_sets(propList, PA_PROP_MEDIA_ROLE, "video");

  pa_threaded_mainloop_lock(pa.loop);
  pa.context = pa_context_new_with_proplist(
      pa.api,
      "Looking Glass",
      propList);
  if (!pa.context)
  {
    DEBUG_ERROR("Failed to create the context");
    goto err_context;
  }

  pa_context_set_state_callback(pa.context,
      pulseaudio_ctx_state_change_cb, NULL);

  if (pa_context_connect(pa.context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
  {
    DEBUG_ERROR("Failed to connect to the context server");
    goto err_context;
  }

  for(;;)
  {
    pa_context_state_t state = pa_context_get_state(pa.context);
    if(!PA_CONTEXT_IS_GOOD(state))
    {
      DEBUG_ERROR("Context is bad");
      goto err_context;
    }

    if (state == PA_CONTEXT_READY)
      break;

    pa_threaded_mainloop_wait(pa.loop);
  }

  pa_threaded_mainloop_unlock(pa.loop);
  pa_proplist_free(propList);
  return true;

err_context:
  pa_threaded_mainloop_unlock(pa.loop);
  pa_proplist_free(propList);

err_thread:
  pa_threaded_mainloop_stop(pa.loop);

err_loop:
  pa_threaded_mainloop_free(pa.loop);

err:
  return false;
}

static void pulseaudio_sink_close_nl(void)
{
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
  pulseaudio_unrefOperation(pa_stream_flush(pa.sink, NULL, NULL));
  pa_stream_unref(pa.sink);
  pa.sink = NULL;
  pa.sinkResamplerEnabled = false;
  pa.sinkRateFailed       = false;
  pa.sinkNominalRate      = 0;
  pa.sinkAppliedRate      = 0;
  pa.sinkPendingRate      = 0;
  pa.sinkRequestedRate    = 0;
  atomic_store_explicit(
      &pa.sinkPresentationDeadline, 0, memory_order_release);
}

static void pulseaudio_free(void)
{
  pa_threaded_mainloop_lock(pa.loop);

  pulseaudio_sink_close_nl();

  pa_context_set_state_callback(pa.context, NULL, NULL);
  pa_context_set_subscribe_callback(pa.context, NULL, NULL);
  pa_context_disconnect(pa.context);
  pa_context_unref(pa.context);

  if (pa.contextSub)
  {
    pa_operation_unref(pa.contextSub);
    pa.contextSub = NULL;
  }

  pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_state_cb(pa_stream * p, void * userdata)
{
  if (pa.sinkStarting && pa_stream_get_state(pa.sink) == PA_STREAM_READY)
  {
    pulseaudio_unrefOperation(pa_stream_cork(pa.sink, 0, NULL, NULL));
    pa.sinkCorked   = false;
    pa.sinkStarting = false;
  }

  pa_threaded_mainloop_signal(pa.loop, 0);
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
    DEBUG_ERROR("pa_stream_begin_write failed: %s",
        pa_strerror(pa_context_errno(pa.context)));
    return;
  }

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
    DEBUG_ERROR("pa_stream_write failed: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pa_stream_cancel_write(p);
    return;
  }

  /* Queue rate changes after the current audio block. This keeps the rate
   * reported by the preceding pull aligned with the block PulseAudio has
   * already received. */
  pulseaudio_submitRateUpdate();

  pa_usec_t latency;
  int negative;
  if (pa_stream_get_latency(p, &latency, &negative) == 0)
  {
    const int64_t latencyNs = negative ? 0 : (int64_t)latency * 1000;
    atomic_store_explicit(&pa.sinkPresentationDeadline,
        (int64_t)nanotime() + latencyNs, memory_order_release);
  }
}

static void pulseaudio_underflow_cb(pa_stream * p, void * userdata)
{
  DEBUG_WARN("Underflow");
}

static void pulseaudio_overflow_cb(pa_stream * p, void * userdata)
{
  DEBUG_WARN("Overflow");
}

static bool pulseaudio_setup(const LG_AudioFormat * format,
    int requestedPeriodFrames, bool requestResampler,
    bool * resamplerEnabled, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  *resamplerEnabled = false;

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
  /* pa_stream_update_sample_rate requires protocol version 12. */
  const bool enableResampler = requestResampler &&
    pa_context_get_server_protocol_version(pa.context) >= 12;
  if (pa.sink && !pa.sinkRateFailed &&
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
  pa.sinkStarting           = false;
  pa.sinkResamplerEnabled   = enableResampler;
  pa.sinkRateFailed         = false;
  pa.sinkNominalRate        = sampleRate;
  pa.sinkAppliedRate        = sampleRate;
  pa.sinkPendingRate        = sampleRate;
  pa.sinkRequestedRate      = sampleRate;

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

  while (pa_stream_get_state(pa.sink) == PA_STREAM_CREATING)
    pa_threaded_mainloop_wait(pa.loop);

  if (pa_stream_get_state(pa.sink) != PA_STREAM_READY)
  {
    DEBUG_ERROR("PulseAudio stream did not become ready: %s",
        pa_strerror(pa_context_errno(pa.context)));
    pulseaudio_sink_close_nl();
    pa_threaded_mainloop_unlock(pa.loop);
    return false;
  }

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
      &pa.sinkPresentationDeadline, 0, memory_order_release);
  pa_threaded_mainloop_unlock(pa.loop);
  return true;
}

static void pulseaudio_start(void)
{
  if (!pa.sink)
    return;

  pa_threaded_mainloop_lock(pa.loop);

  pa_stream_state_t state = pa_stream_get_state(pa.sink);
  if (state == PA_STREAM_CREATING)
    pa.sinkStarting = true;
  else
  {
    pulseaudio_unrefOperation(pa_stream_cork(pa.sink, 0, NULL, NULL));
    pa.sinkCorked = false;
  }

  pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_stop(void)
{
  if (!pa.sink)
    return;

  bool needLock = !pa_threaded_mainloop_in_thread(pa.loop);
  if (needLock)
    pa_threaded_mainloop_lock(pa.loop);

  pulseaudio_unrefOperation(pa_stream_cork(pa.sink, 1, NULL, NULL));
  pa.sinkCorked   = true;
  pa.sinkStarting = false;
  if (pa.sinkResamplerEnabled)
  {
    pa.sinkRequestedRate = pa.sinkNominalRate;
    pulseaudio_submitRateUpdate();
  }
  atomic_store_explicit(
      &pa.sinkPresentationDeadline, 0, memory_order_release);

  if (needLock)
    pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_volume(int channels, const uint16_t volume[])
{
  if (!pa.sink || !pa.sinkIndex)
    return;

  struct pa_cvolume v = { .channels = channels };
  for(int i = 0; i < channels; ++i)
    v.values[i] = pa_sw_volume_from_linear(
      9.3234e-7 * pow(1.000211902, volume[i]) - 0.000172787);

  pa_threaded_mainloop_lock(pa.loop);
  pulseaudio_unrefOperation(pa_context_set_sink_input_volume(
      pa.context, pa.sinkIndex, &v, NULL, NULL));
  pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_mute(bool mute)
{
  if (!pa.sink || !pa.sinkIndex || pa.sinkMuted == mute)
    return;

  pa.sinkMuted = mute;
  pa_threaded_mainloop_lock(pa.loop);
  pulseaudio_unrefOperation(pa_context_set_sink_input_mute(
      pa.context, pa.sinkIndex, mute, NULL, NULL));
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
  const uint32_t scheduledRate = pa.sinkRateOperation ?
    pa.sinkPendingRate : pa.sinkRequestedRate;
  if (!scheduledRate)
    return false;

  *ratio = (double)pa.sinkNominalRate / scheduledRate;
  return true;
}

static uint64_t pulseaudio_latency(void)
{
  const int64_t deadline = atomic_load_explicit(
      &pa.sinkPresentationDeadline, memory_order_acquire);
  if (deadline <= 0)
    return 0;

  return max(INT64_C(0), deadline - (int64_t)nanotime()) / 1000;
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
