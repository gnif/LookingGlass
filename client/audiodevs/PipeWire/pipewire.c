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

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <pipewire/pipewire.h>
#include <math.h>

#include "common/debug.h"
#include "common/stringutils.h"
#include "common/util.h"

typedef enum
{
  STREAM_STATE_INACTIVE,
  STREAM_STATE_ACTIVE,
  STREAM_STATE_FLUSHING,
  STREAM_STATE_DRAINING,
  STREAM_STATE_RESTARTING
}
StreamState;

struct PipeWire
{
  struct pw_loop        * loop;
  struct pw_thread_loop * thread;

  struct
  {
    struct pw_stream * stream;
    struct spa_io_rate_match * rateMatch;

    int            channels;
    int            sampleRate;
    int            stride;
    LG_AudioPullFn pullFn;
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

    bool   active;
  }
  record;
};

static struct PipeWire pw = {0};

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

static void pipewire_onPlaybackProcess(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.playback.stream)))
  {
    DEBUG_WARN("out of buffers");
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  uint8_t * dst;

  if (!(dst = sbuf->datas[0].data))
    return;

  int frames = sbuf->datas[0].maxsize / pw.playback.stride;
  if (pw.playback.rateMatch && pw.playback.rateMatch->size > 0)
    frames = min(frames, pw.playback.rateMatch->size);

  frames = pw.playback.pullFn(dst, frames);
  if (!frames)
  {
    if (pw.playback.state == STREAM_STATE_FLUSHING)
    {
      pw_thread_loop_lock(pw.thread);
      pw_stream_flush(pw.playback.stream, true);
      pw.playback.state = STREAM_STATE_DRAINING;
      pw_thread_loop_unlock(pw.thread);
    }

    sbuf->datas[0].chunk->size = 0;
    pw_stream_queue_buffer(pw.playback.stream, pbuf);
    return;
  }

  sbuf->datas[0].chunk->offset = 0;
  sbuf->datas[0].chunk->stride = pw.playback.stride;
  sbuf->datas[0].chunk->size   = frames * pw.playback.stride;

  pw_stream_queue_buffer(pw.playback.stream, pbuf);
}

static void pipewire_onPlaybackDrained(void * userdata)
{
  pw_thread_loop_lock(pw.thread);

  if (pw.playback.state == STREAM_STATE_RESTARTING)
  {
    // A play command was received while we were in the middle of stopping;
    // switch straight back into playing
    pw_stream_set_active(pw.playback.stream, true);
    pw.playback.state = STREAM_STATE_ACTIVE;
  }
  else
  {
    pw_stream_set_active(pw.playback.stream, false);
    pw.playback.state = STREAM_STATE_INACTIVE;
  }

  pw_thread_loop_unlock(pw.thread);
}

static bool pipewire_init(void)
{
  pw_init(NULL, NULL);

  pw.loop = pw_loop_new(NULL);
  struct pw_context * context = pw_context_new(pw.loop, NULL, 0);
  if (!context)
  {
    DEBUG_ERROR("Failed to create a context");
    goto err;
  }

  /* this is just to test for PipeWire availabillity */
  struct pw_core * core = pw_context_connect(context, NULL, 0);
  if (!core)
    goto err_context;

  pw_context_destroy(context);

  /* PipeWire is available so create the loop thread and start it */
  pw.thread = pw_thread_loop_new_full(pw.loop, "PipeWire", NULL);
  if (!pw.thread)
  {
    DEBUG_ERROR("Failed to create the thread loop");
    goto err;
  }

  pw_thread_loop_start(pw.thread);
  return true;

err_context:
  pw_context_destroy(context);

err:
  pw_loop_destroy(pw.loop);
  pw_deinit();
  return false;
}

static void pipewire_playbackStopStream(void)
{
  if (!pw.playback.stream)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_destroy(pw.playback.stream);
  pw.playback.stream    = NULL;
  pw.playback.rateMatch = NULL;
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_playbackSetup(int channels, int sampleRate,
   LG_AudioPullFn pullFn)
{
  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version    = PW_VERSION_STREAM_EVENTS,
    .io_changed = pipewire_onPlaybackIoChanged,
    .process    = pipewire_onPlaybackProcess,
    .drained    = pipewire_onPlaybackDrained
  };

  if (pw.playback.stream &&
      pw.playback.channels == channels &&
      pw.playback.sampleRate == sampleRate)
    return;

  pipewire_playbackStopStream();

  int bufferFrames = sampleRate / 10;
  int maxLatencyFrames = bufferFrames / 2;
  char maxLatency[32];
  snprintf(maxLatency, sizeof(maxLatency), "%d/%d", maxLatencyFrames,
      sampleRate);

  pw.playback.channels    = channels;
  pw.playback.sampleRate  = sampleRate;
  pw.playback.stride      = sizeof(uint16_t) * channels;
  pw.playback.pullFn      = pullFn;
  pw.playback.startFrames = maxLatencyFrames;

  pw_thread_loop_lock(pw.thread);
  pw.playback.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    pw_properties_new(
      PW_KEY_NODE_NAME       , "Looking Glass",
      PW_KEY_MEDIA_TYPE      , "Audio",
      PW_KEY_MEDIA_CATEGORY  , "Playback",
      PW_KEY_MEDIA_ROLE      , "Music",
      PW_KEY_NODE_MAX_LATENCY, maxLatency,
      NULL
    ),
    &events,
    NULL
  );

  if (!pw.playback.stream)
  {
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create the stream");
    return;
  }

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
        .format   = SPA_AUDIO_FORMAT_S16,
        .channels = channels,
        .rate     = sampleRate
        ));

  pw_stream_connect(
      pw.playback.stream,
      PW_DIRECTION_OUTPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS  |
      PW_STREAM_FLAG_INACTIVE,
      params, 1);

  pw_thread_loop_unlock(pw.thread);
}

static bool pipewire_playbackStart(int framesBuffered)
{
  if (!pw.playback.stream)
    return false;

  bool start = false;

  if (pw.playback.state != STREAM_STATE_ACTIVE &&
    pw.playback.state != STREAM_STATE_RESTARTING)
  {
    pw_thread_loop_lock(pw.thread);

    switch (pw.playback.state)
    {
      case STREAM_STATE_INACTIVE:
        if (framesBuffered >= pw.playback.startFrames)
        {
          pw_stream_set_active(pw.playback.stream, true);
          pw.playback.state = STREAM_STATE_ACTIVE;
          start = true;
        }
        break;

      case STREAM_STATE_FLUSHING:
        // We were preparing to stop; just carry on as if nothing happened
        pw.playback.state = STREAM_STATE_ACTIVE;
        start = true;
        break;

      case STREAM_STATE_DRAINING:
        // We are in the middle of draining the PipeWire buffers; we will need
        // to reactivate the stream once this has completed
        pw.playback.state = STREAM_STATE_RESTARTING;
        break;

      default:
        DEBUG_UNREACHABLE();
    }

    pw_thread_loop_unlock(pw.thread);
  }

  return start;
}

static void pipewire_playbackStop(void)
{
  if (pw.playback.state != STREAM_STATE_ACTIVE &&
    pw.playback.state != STREAM_STATE_RESTARTING)
    return;

  pw_thread_loop_lock(pw.thread);

  switch (pw.playback.state)
  {
    case STREAM_STATE_ACTIVE:
      pw.playback.state = STREAM_STATE_FLUSHING;
      break;

    case STREAM_STATE_RESTARTING:
      // A stop was requested, and then a start while PipeWire was draining, and
      // now another stop. PipeWire hasn't finished draining yet so just switch
      // the state back
      pw.playback.state = STREAM_STATE_DRAINING;
      break;

    default:
      DEBUG_UNREACHABLE();
  }

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
  pw_stream_set_control(pw.playback.stream, SPA_PROP_mute, 1, (void *)&mute, 0);
  pw_thread_loop_unlock(pw.thread);
}

static size_t pipewire_playbackLatency(void)
{
  struct pw_time time = { 0 };

  pw_thread_loop_lock(pw.thread);
  if (pw_stream_get_time(pw.playback.stream, &time) < 0)
    DEBUG_ERROR("pw_stream_get_time failed");
  pw_thread_loop_unlock(pw.thread);

  return time.delay + time.queued / pw.playback.stride;
}

static void pipewire_recordStopStream(void)
{
  if (!pw.record.stream)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_destroy(pw.record.stream);
  pw.record.stream = NULL;
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_onRecordProcess(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.record.stream)))
  {
    DEBUG_WARN("out of buffers");
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  uint8_t * dst;

  if (!(dst = sbuf->datas[0].data))
    return;

  dst += sbuf->datas[0].chunk->offset;
  pw.record.pushFn(dst,
    min(
      sbuf->datas[0].chunk->size,
      sbuf->datas[0].maxsize - sbuf->datas[0].chunk->offset) / pw.record.stride
    );

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
    return;

  pipewire_recordStopStream();

  pw.record.channels   = channels;
  pw.record.sampleRate = sampleRate;
  pw.record.stride     = sizeof(uint16_t) * channels;
  pw.record.pushFn     = pushFn;

  pw_thread_loop_lock(pw.thread);
  pw.record.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    pw_properties_new(
      PW_KEY_NODE_NAME     , "Looking Glass",
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE    , "Music",
      NULL
    ),
    &events,
    NULL
  );

  if (!pw.record.stream)
  {
    pw_thread_loop_unlock(pw.thread);
    DEBUG_ERROR("Failed to create the stream");
    return;
  }

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
      &SPA_AUDIO_INFO_RAW_INIT(
        .format   = SPA_AUDIO_FORMAT_S16,
        .channels = channels,
        .rate     = sampleRate
        ));

  pw_stream_connect(
      pw.record.stream,
      PW_DIRECTION_INPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS,
      params, 1);

  pw_thread_loop_unlock(pw.thread);
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
  pw_stream_set_control(pw.record.stream, SPA_PROP_mute, 1, (void *)&mute, 0);
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_free(void)
{
  pipewire_playbackStopStream();
  pipewire_recordStopStream();
  pw_thread_loop_stop(pw.thread);
  pw_thread_loop_destroy(pw.thread);
  pw_loop_destroy(pw.loop);

  pw.loop   = NULL;
  pw.thread = NULL;

  pw_deinit();
}

struct LG_AudioDevOps LGAD_PipeWire =
{
  .name   = "PipeWire",
  .init   = pipewire_init,
  .free   = pipewire_free,
  .playback =
  {
    .setup   = pipewire_playbackSetup,
    .start   = pipewire_playbackStart,
    .stop    = pipewire_playbackStop,
    .volume  = pipewire_playbackVolume,
    .mute    = pipewire_playbackMute,
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
