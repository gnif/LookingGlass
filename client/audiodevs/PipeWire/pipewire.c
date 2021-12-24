/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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
#include <pipewire/pipewire.h>

#include "common/debug.h"
#include "common/ringbuffer.h"

struct PipeWire
{
  struct pw_loop        * loop;
  struct pw_thread_loop * thread;
  struct pw_stream      * stream;
  int    channels;
  int    stride;

  RingBuffer buffer;
};

static struct PipeWire pw = {0};

static void pipewire_on_process(void * userdata)
{
  struct pw_buffer * pbuf;

  const int avail = ringbuffer_getCount(pw.buffer);
  if (!avail)
    return;

  if (!(pbuf = pw_stream_dequeue_buffer(pw.stream))) {
    DEBUG_WARN("out of buffers");
    return;
  }

  struct spa_buffer * sbuf = pbuf->buffer;
  uint8_t * dst;

  if (!(dst = sbuf->datas[0].data))
    return;

  int frames = sbuf->datas[0].maxsize / pw.stride;
  if (frames > avail)
    frames = avail;

  for(int i = 0; i < frames; ++i)
  {
    ringbuffer_shift(pw.buffer, dst);
    dst += pw.stride;
  }

  sbuf->datas[0].chunk->offset = 0;
  sbuf->datas[0].chunk->stride = pw.stride;
  sbuf->datas[0].chunk->size   = frames * pw.stride;

  pw_stream_queue_buffer(pw.stream, pbuf);
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
  pw.thread = pw_thread_loop_new_full(pw.loop, "Playback", NULL);
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
  pw_deinit();
  return false;
}

static void pipewire_free(void)
{
  if (pw.thread)
  {
    pw_thread_loop_lock(pw.thread);
    if (pw.stream)
    {
      pw_stream_destroy(pw.stream);
      pw.stream = NULL;
    }

    pw_thread_loop_signal(pw.thread, true);
    pw_thread_loop_destroy(pw.thread);
    pw.loop = NULL;
  }

  ringbuffer_free(&pw.buffer);
  pw_deinit();
}

static void pipewire_start(int channels, int sampleRate)
{
  const struct spa_pod * params[1];
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  static const struct pw_stream_events events =
  {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = pipewire_on_process
  };

  pw.channels = channels;
  pw.stride   = sizeof(uint16_t) * channels;
  pw.buffer   = ringbuffer_new(sampleRate, channels * sizeof(uint16_t));

  pw_thread_loop_lock(pw.thread);
  pw.stream = pw_stream_new_simple(
    pw.loop,
    "LookingGlass",
    pw_properties_new(
      PW_KEY_MEDIA_TYPE    , "Audio",
      PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE    , "Music",
      NULL
    ),
    &events,
    NULL
  );

  if (!pw.stream)
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
      pw.stream,
      PW_DIRECTION_OUTPUT,
      PW_ID_ANY,
      PW_STREAM_FLAG_AUTOCONNECT |
      PW_STREAM_FLAG_MAP_BUFFERS |
      PW_STREAM_FLAG_RT_PROCESS,
      params, 1);

  pw_thread_loop_unlock(pw.thread);
}

static void pipewire_play(uint8_t * data, int size)
{
  if (!pw.stream)
    return;

  for(int i = 0; i < size; i += pw.stride)
    ringbuffer_push(pw.buffer, data + i);
}

static void pipewire_stop(void)
{
  if (!pw.stream)
    return;

  pw_thread_loop_lock(pw.thread);
  pw_stream_flush(pw.stream, true);
  pw_stream_destroy(pw.stream);
  pw.stream = NULL;
  pw_thread_loop_unlock(pw.thread);
}

struct LG_AudioDevOps LGAD_PipeWire =
{
  .name  = "PipeWire",
  .init  = pipewire_init,
  .free  = pipewire_free,
  .start = pipewire_start,
  .play  = pipewire_play,
  .stop  = pipewire_stop
};
