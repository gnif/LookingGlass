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

#include "portal.h"
#include "interface/capture.h"
#include "interface/platform.h"
#include "common/util.h"
#include "common/debug.h"
#include "common/stringutils.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>
#include <spa/param/format.h>
#include <spa/param/video/format-utils.h>

struct pipewire
{
  struct Portal         * portal;
  char                  * sessionHandle;
  struct pw_thread_loop * threadLoop;
  struct pw_context     * context;
  struct pw_core        * core;
  struct spa_hook         coreListener;
  struct pw_stream      * stream;
  struct spa_hook         streamListener;

  bool          stop;
  bool          hasFormat;
  bool          formatChanged;
  int           width, height;
  CaptureFormat format;
  uint8_t     * frameData;
  unsigned int  formatVer;
};

static struct pipewire * this = NULL;

// forwards

static bool pipewire_deinit(void);

// implementation

static const char * pipewire_getName(void)
{
  return "PipeWire";
}

static bool pipewire_create(CaptureGetPointerBuffer getPointerBufferFn, CapturePostPointerBuffer postPointerBufferFn)
{
  DEBUG_ASSERT(!this);
  pw_init(NULL, NULL);
  this = calloc(1, sizeof(*this));
  return true;
}

static void coreErrorCallback(void * opaque, uint32_t id, int seq, int res, const char * message)
{
  DEBUG_ERROR("pipewire error: id %" PRIu32 ", seq: %d, res: %d (%s): %s",
    id, seq, res, strerror(res), message);
}

static const struct pw_core_events coreEvents = {
  PW_VERSION_CORE_EVENTS,
  .error = coreErrorCallback,
};

static bool startStream(struct pw_stream * stream, uint32_t node)
{
  char buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod * param = spa_pod_builder_add_object(
    &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
    SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
    SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
    SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6,
      SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
      SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
      SPA_VIDEO_FORMAT_xBGR_210LE, SPA_VIDEO_FORMAT_RGBA_F16),
    SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
      &SPA_RECTANGLE(1920, 1080), &SPA_RECTANGLE(1, 1), &SPA_RECTANGLE(8192, 4320)),
    SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
      &SPA_FRACTION(60, 1), &SPA_FRACTION(0, 1), &SPA_FRACTION(360, 1)));

  return pw_stream_connect(stream, PW_DIRECTION_INPUT, node,
    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, &param, 1) >= 0;
}

static void streamProcessCallback(void * opaque)
{
  if (!this->hasFormat)
    return;

  struct pw_buffer * pwBuffer = NULL;

  // dequeue all buffers to get the latest one
  while (true)
  {
    struct pw_buffer * tmp = pw_stream_dequeue_buffer(this->stream);
    if (!tmp)
      break;
    if (pwBuffer)
      pw_stream_queue_buffer(this->stream, pwBuffer);
    pwBuffer = tmp;
  }

  if (!pwBuffer)
  {
    DEBUG_WARN("Pipewire out of buffers");
    return;
  }

  struct spa_buffer * buffer = pwBuffer->buffer;
  if (!buffer->datas[0].chunk->size)
    return;

  this->frameData = buffer->datas[0].data;

  pw_thread_loop_signal(this->threadLoop, true);
  pw_stream_queue_buffer(this->stream, pwBuffer);
}

static CaptureFormat convertSpaFormat(enum spa_video_format spa)
{
  switch (spa)
  {
    case SPA_VIDEO_FORMAT_RGBA:
    case SPA_VIDEO_FORMAT_RGBx:
      return CAPTURE_FMT_RGBA;

    case SPA_VIDEO_FORMAT_BGRA:
    case SPA_VIDEO_FORMAT_BGRx:
      return CAPTURE_FMT_BGRA;

    case SPA_VIDEO_FORMAT_xBGR_210LE:
      return CAPTURE_FMT_RGBA10;

    case SPA_VIDEO_FORMAT_RGBA_F16:
      return CAPTURE_FMT_RGBA16F;

    default:
      return -1;
  }
}

static void streamParamChangedCallback(void * opaque, uint32_t id,
  const struct spa_pod * param)
{
  if (!param || id != SPA_PARAM_Format)
    return;

  uint32_t mediaType, mediaSubtype;
  if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
      mediaType != SPA_MEDIA_TYPE_video ||
      mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
    return;

  struct spa_video_info_raw info;
  if (spa_format_video_raw_parse(param, &info) < 0)
  {
    DEBUG_ERROR("Failed to parse video info");
    return;
  }

  this->width  = info.size.width;
  this->height = info.size.height;
  this->format = convertSpaFormat(info.format);

  if (this->hasFormat)
  {
    this->formatChanged = true;
    return;
  }

  char buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  param = spa_pod_builder_add_object(
    &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
    SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_MemPtr));
  pw_stream_update_params(this->stream, &param, 1);

  this->hasFormat = true;
  pw_thread_loop_signal(this->threadLoop, true);
}

static void streamStateChangedCallback(void * opaque,
  enum pw_stream_state oldState, enum pw_stream_state newState,
  const char * error)
{
  DEBUG_INFO("PipeWire stream state change: %s -> %s",
    pw_stream_state_as_string(oldState), pw_stream_state_as_string(newState));

  if (newState == PW_STREAM_STATE_ERROR)
    DEBUG_ERROR("PipeWire stream error: %s", error);
}

static const struct pw_stream_events streamEvents = {
  PW_VERSION_STREAM_EVENTS,
  .process       = streamProcessCallback,
  .state_changed = streamStateChangedCallback,
  .param_changed = streamParamChangedCallback,
};

static bool pipewire_init(void)
{
  DEBUG_ASSERT(this);
  this->stop = false;

  this->portal = portal_create();
  if (!this->portal)
  {
    DEBUG_ERROR("Failed to create xdg-desktop-portal for screencasting");
    goto fail;
  }

  if (!portal_createScreenCastSession(this->portal, &this->sessionHandle))
  {
    DEBUG_ERROR("Failed to create ScreenCast session");
    goto fail;
  }

  DEBUG_INFO("Got session handle: %s", this->sessionHandle);

  if (!portal_selectSource(this->portal, this->sessionHandle))
  {
    DEBUG_ERROR("Failed to select source");
    goto fail;
  }

  uint32_t pipewireNode = portal_getPipewireNode(this->portal, this->sessionHandle);
  if (!pipewireNode)
  {
    DEBUG_ERROR("Failed to get pipewire node");
    goto fail;
  }

  int pipewireFd = portal_openPipewireRemote(this->portal, this->sessionHandle);
  if (pipewireFd < 0)
  {
    DEBUG_ERROR("Failed to get pipewire fd");
    goto fail;
  }

  this->threadLoop = pw_thread_loop_new("lg-pipewire-capture", NULL);
  if (!this->threadLoop)
  {
    DEBUG_ERROR("Failed to create pipewire thread loop");
    close(pipewireFd);
    goto fail;
  }

  this->context = pw_context_new(pw_thread_loop_get_loop(this->threadLoop), NULL, 0);
  if (!this->context)
  {
    DEBUG_ERROR("Failed to create pipewire context");
    close(pipewireFd);
    goto fail;
  }

  if (pw_thread_loop_start(this->threadLoop) < 0)
  {
    DEBUG_ERROR("Failed to start pipewire thread loop");
    close(pipewireFd);
    goto fail;
  }

  pw_thread_loop_lock(this->threadLoop);

  this->core = pw_context_connect_fd(this->context, pipewireFd, NULL, 0);
  if (!this->core)
  {
    DEBUG_ERROR("Failed to create pipewire core: %s", strerror(errno));
    pw_thread_loop_unlock(this->threadLoop);
    close(pipewireFd);
    goto fail;
  }

  pw_core_add_listener(this->core, &this->coreListener, &coreEvents, NULL);

  this->stream = pw_stream_new(this->core, "Looking Glass (host)", pw_properties_new(
    PW_KEY_MEDIA_TYPE, "Video",
    PW_KEY_MEDIA_CATEGORY, "Capture",
    PW_KEY_MEDIA_ROLE, "Screen", NULL));
  if (!this->stream)
  {
    DEBUG_ERROR("Failed to create pipewire stream");
    pw_thread_loop_unlock(this->threadLoop);
    goto fail;
  }

  this->hasFormat     = false;
  this->formatChanged = false;
  this->frameData     = NULL;
  pw_stream_add_listener(this->stream, &this->streamListener, &streamEvents, NULL);

  if (!startStream(this->stream, pipewireNode))
  {
    DEBUG_ERROR("Failed to start pipewire stream");
    pw_thread_loop_unlock(this->threadLoop);
    goto fail;
  }

  pw_thread_loop_unlock(this->threadLoop);

  while (!this->hasFormat)
    pw_thread_loop_wait(this->threadLoop);

  if (this->format < 0)
  {
    DEBUG_ERROR("Unknown frame format");
    pw_thread_loop_accept(this->threadLoop);
    goto fail;
  }

  DEBUG_INFO("Frame size       : %dx%d", this->width, this->height);

  pw_thread_loop_accept(this->threadLoop);

  return true;
fail:
  pipewire_deinit();
  return false;
}

static void pipewire_stop(void)
{
  this->stop = true;
  pw_stream_disconnect(this->stream);
  pw_thread_loop_signal(this->threadLoop, false);
}

static bool pipewire_deinit(void)
{
  if (this->stream)
  {
    pw_stream_disconnect(this->stream);
    this->stream = NULL;
  }

  if (this->core)
  {
    pw_core_disconnect(this->core);
    this->core = NULL;
  }

  if (this->threadLoop)
  {
    pw_thread_loop_stop(this->threadLoop);
    pw_thread_loop_destroy(this->threadLoop);
    this->threadLoop = NULL;
  }

  if (this->sessionHandle)
    portal_destroySession(this->portal, &this->sessionHandle);

  if (this->portal)
  {
    portal_free(this->portal);
    this->portal = NULL;
  }

  return true;
}

static void pipewire_free(void)
{
  DEBUG_ASSERT(this);
  pw_deinit();
  free(this);
  this = NULL;
}

static CaptureResult pipewire_capture(void)
{
  int result;

restart:
  result = pw_thread_loop_timed_wait(this->threadLoop, 1);

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  if (result == ETIMEDOUT)
    return CAPTURE_RESULT_TIMEOUT;

  if (this->formatChanged)
  {
    ++this->formatVer;
    this->formatChanged = false;
    pw_thread_loop_accept(this->threadLoop);
    goto restart;
  }

  return CAPTURE_RESULT_OK;
}

static CaptureResult pipewire_waitFrame(CaptureFrame * frame,
    const size_t maxFrameSize)
{
  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  const int bpp = this->format == CAPTURE_FMT_RGBA16F ? 8 : 4;
  const unsigned int maxHeight = maxFrameSize / (this->width * bpp);

  frame->formatVer    = this->formatVer;
  frame->format       = this->format;
  frame->screenWidth  = this->width;
  frame->screenHeight = this->height;
  frame->frameWidth   = this->width;
  frame->frameHeight  = min(maxHeight, this->height);
  frame->truncated    = maxHeight < this->height;
  frame->pitch        = this->width * bpp;
  frame->stride       = this->width;
  frame->rotation     = CAPTURE_ROT_0;

  // TODO: implement damage.
  frame->damageRectsCount = 0;

  return CAPTURE_RESULT_OK;
}

static CaptureResult pipewire_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  if (this->stop || !this->frameData)
    return CAPTURE_RESULT_REINIT;

  const int bpp = this->format == CAPTURE_FMT_RGBA16F ? 8 : 4;
  framebuffer_write(frame, this->frameData, height * this->width * bpp);

  pw_thread_loop_accept(this->threadLoop);
  return CAPTURE_RESULT_OK;
}

struct CaptureInterface Capture_pipewire =
{
  .shortName       = "pipewire",
  .asyncCapture    = false,
  .getName         = pipewire_getName,
  .create          = pipewire_create,
  .init            = pipewire_init,
  .stop            = pipewire_stop,
  .deinit          = pipewire_deinit,
  .free            = pipewire_free,
  .capture         = pipewire_capture,
  .waitFrame       = pipewire_waitFrame,
  .getFrame        = pipewire_getFrame
};
