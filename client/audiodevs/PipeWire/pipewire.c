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
#include "common/ringbuffer.h"

struct PipeWire
{
  struct pw_loop        * loop;
  struct pw_thread_loop * thread;

  struct
  {
    struct pw_stream * stream;

    int    channels;
    int    sampleRate;
    int    stride;

    RingBuffer buffer;
    bool       active;
  }
  playback;
};

static struct PipeWire pw = {0};

static void pipewire_on_process(void * userdata)
{
  struct pw_buffer * pbuf;

  if (!ringbuffer_getCount(pw.playback.buffer))
    return;

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
  void * values = ringbuffer_consume(pw.playback.buffer, &frames);
  memcpy(dst, values, frames * pw.playback.stride);

  sbuf->datas[0].chunk->offset = 0;
  sbuf->datas[0].chunk->stride = pw.playback.stride;
  sbuf->datas[0].chunk->size   = frames * pw.playback.stride;

  pw_stream_queue_buffer(pw.playback.stream, pbuf);
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

static void pipewire_playbackStop_stream(void)
{
  if (!pw.playback.stream)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_flush(pw.playback.stream, true);
  pw_stream_destroy(pw.playback.stream);
  pw.playback.stream = NULL;
  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_free(void)
{
  pipewire_playbackStop_stream();
  pw_thread_loop_stop(pw.thread);
  pw_thread_loop_destroy(pw.thread);
  pw_loop_destroy(pw.loop);

  pw.loop   = NULL;
  pw.thread = NULL;

  ringbuffer_free(&pw.playback.buffer);
  pw_deinit();
}

static void pipewire_playbackPlaybackStart(int channels, int sampleRate)
{
  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = pipewire_on_process
  };

  if (pw.playback.stream &&
      pw.playback.channels == channels &&
      pw.playback.sampleRate == sampleRate)
    return;

  pipewire_playbackStop_stream();

  pw.playback.channels   = channels;
  pw.playback.sampleRate = sampleRate;
  pw.playback.stride     = sizeof(uint16_t) * channels;
  pw.playback.buffer     = ringbuffer_new(sampleRate / 10, channels * sizeof(uint16_t));

  pw_thread_loop_lock(pw.thread);
  pw.playback.stream = pw_stream_new_simple(
    pw.loop,
    "Looking Glass",
    pw_properties_new(
      PW_KEY_NODE_NAME     , "Looking Glass",
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE    , "Music",
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

static void pipewire_playbackPlay(uint8_t * data, size_t size)
{
  if (!pw.playback.stream)
    return;

  // if the buffer fill is higher then the average skip the update to reduce lag
  static unsigned int ttlSize = 0;
  static unsigned int count   = 0;
  ttlSize += size;
  if (++count > 100 && ringbuffer_getCount(pw.playback.buffer) > ttlSize / count)
  {
    count   = 0;
    ttlSize = 0;
    return;
  }

  ringbuffer_append(pw.playback.buffer, data, size / pw.playback.stride);

  if (!pw.playback.active)
  {
    pw_thread_loop_lock(pw.thread);
    pw_stream_set_active(pw.playback.stream, true);
    pw.playback.active = true;
    pw_thread_loop_unlock(pw.thread);
  }
}

static void pipewire_playbackStop(void)
{
  if (!pw.playback.active)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_set_active(pw.playback.stream, false);
  pw.playback.active = false;
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

struct LG_AudioDevOps LGAD_PipeWire =
{
  .name   = "PipeWire",
  .init   = pipewire_init,
  .free   = pipewire_free,
  .playback =
  {
    .start  = pipewire_playbackPlaybackStart,
    .play   = pipewire_playbackPlay,
    .stop   = pipewire_playbackStop,
    .volume = pipewire_playbackVolume,
    .mute   = pipewire_playbackMute
  }
};
