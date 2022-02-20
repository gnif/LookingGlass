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

#include "interface/audiodev.h"

#include <pulse/pulseaudio.h>
#include <string.h>
#include <math.h>

#include "common/debug.h"

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
  int                    sinkMaxPeriodFrames;
  int                    sinkStartFrames;
  int                    sinkSampleRate;
  int                    sinkChannels;
  int                    sinkStride;
  LG_AudioPullFn         sinkPullFn;
};

static struct PulseAudio pa = {0};

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
        pa_operation_unref(o);
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

  pa_stream_set_write_callback(pa.sink, NULL, NULL);
  pa_stream_flush(pa.sink, NULL, NULL);
  pa_stream_unref(pa.sink);
  pa.sink = NULL;
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
    pa_stream_cork(pa.sink, 0, NULL, NULL);
    pa.sinkCorked   = false;
    pa.sinkStarting = false;
  }
}

static void pulseaudio_write_cb(pa_stream * p, size_t nbytes, void * userdata)
{
  // PulseAudio tries to pull data from the stream as soon as it is created for
  // some reason, even though it is corked
  if (pa.sinkCorked)
    return;

  uint8_t * dst;

  pa_stream_begin_write(p, (void **)&dst, &nbytes);

  int frames = nbytes / pa.sinkStride;
  frames = pa.sinkPullFn(dst, frames);

  pa_stream_write(p, dst, frames * pa.sinkStride, NULL, 0, PA_SEEK_RELATIVE);
}

static void pulseaudio_underflow_cb(pa_stream * p, void * userdata)
{
  DEBUG_WARN("Underflow");
}

static void pulseaudio_overflow_cb(pa_stream * p, void * userdata)
{
  DEBUG_WARN("Overflow");
}

static void pulseaudio_setup(int channels, int sampleRate,
    int requestedPeriodFrames, int * maxPeriodFrames, int * startFrames,
    LG_AudioPullFn pullFn)
{
  if (pa.sink && pa.sinkChannels == channels && pa.sinkSampleRate == sampleRate)
  {
    *maxPeriodFrames = pa.sinkMaxPeriodFrames;
    *startFrames     = pa.sinkStartFrames;
    return;
  }

  pa_sample_spec spec = {
    .format   = PA_SAMPLE_FLOAT32,
    .rate     = sampleRate,
    .channels = channels
  };

  int stride = channels * sizeof(float);
  int bufferSize = requestedPeriodFrames * 2 * stride;
  pa_buffer_attr attribs =
  {
    .maxlength = -1,
    .tlength   = bufferSize,
    .prebuf    = 0,
    .minreq    = (uint32_t)-1
  };

  pa_threaded_mainloop_lock(pa.loop);
  pulseaudio_sink_close_nl();

  pa.sinkChannels   = channels;
  pa.sinkSampleRate = sampleRate;

  pa.sink = pa_stream_new(pa.context, "Looking Glass", &spec, NULL);
  pa_stream_set_state_callback    (pa.sink, pulseaudio_state_cb    , NULL);
  pa_stream_set_write_callback    (pa.sink, pulseaudio_write_cb    , NULL);
  pa_stream_set_underflow_callback(pa.sink, pulseaudio_underflow_cb, NULL);
  pa_stream_set_overflow_callback (pa.sink, pulseaudio_overflow_cb , NULL);

  pa_stream_connect_playback(pa.sink, NULL, &attribs, PA_STREAM_START_CORKED,
    NULL, NULL);

  pa.sinkStride          = stride;
  pa.sinkPullFn          = pullFn;
  pa.sinkMaxPeriodFrames = requestedPeriodFrames;
  pa.sinkCorked          = true;
  pa.sinkStarting        = false;

  // If something else is, or was recently using a small latency value,
  // PulseAudio can request way more data at startup than is reasonable
  pa.sinkStartFrames = requestedPeriodFrames * 4;

  *maxPeriodFrames = requestedPeriodFrames;
  *startFrames     = pa.sinkStartFrames;

  pa_threaded_mainloop_unlock(pa.loop);
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
    pa_stream_cork(pa.sink, 0, NULL, NULL);
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

  pa_stream_cork(pa.sink, 1, NULL, NULL);
  pa.sinkCorked   = true;
  pa.sinkStarting = false;

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
  pa_context_set_sink_input_volume(pa.context, pa.sinkIndex, &v, NULL, NULL);
  pa_threaded_mainloop_unlock(pa.loop);
}

static void pulseaudio_mute(bool mute)
{
  if (!pa.sink || !pa.sinkIndex || pa.sinkMuted == mute)
    return;

  pa.sinkMuted = mute;
  pa_threaded_mainloop_lock(pa.loop);
  pa_context_set_sink_input_mute(pa.context, pa.sinkIndex, mute, NULL, NULL);
  pa_threaded_mainloop_unlock(pa.loop);
}

struct LG_AudioDevOps LGAD_PulseAudio =
{
  .name   = "PulseAudio",
  .init   = pulseaudio_init,
  .free   = pulseaudio_free,
  .playback =
  {
    .setup  = pulseaudio_setup,
    .start  = pulseaudio_start,
    .stop   = pulseaudio_stop,
    .volume = pulseaudio_volume,
    .mute   = pulseaudio_mute
  }
};
