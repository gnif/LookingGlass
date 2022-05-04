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

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/util.h"
#include "common/option.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/thread.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <sys/ipc.h>
#include <sys/shm.h>

struct xcb
{
  bool                        initialized;
  bool                        stop;
  xcb_connection_t          * xcb;
  xcb_screen_t              * xcbScreen;
  uint32_t                    seg;
  int                         shmID;
  void                      * data;
  LGEvent                   * frameEvent;

  CaptureGetPointerBuffer     getPointerBufferFn;
  CapturePostPointerBuffer    postPointerBufferFn;
  LGThread                  * pointerThread;

  unsigned int width;
  unsigned int height;

  int mouseX, mouseY, mouseHotX, mouseHotY;

  bool                                 hasFrame;
  xcb_shm_get_image_cookie_t           imgC;
  xcb_xfixes_get_cursor_image_cookie_t curC;
};

static struct xcb * this = NULL;

static int pointerThread(void * unused);

// forwards

static bool xcb_deinit(void);

// implementation

static const char * xcb_getName(void)
{
  return "XCB";
}

static void xcb_initOptions(void)
{
  struct Option options[] =
  {
    {0}
  };

  option_register(options);
}

static bool xcb_create(CaptureGetPointerBuffer getPointerBufferFn, CapturePostPointerBuffer postPointerBufferFn)
{
  DEBUG_ASSERT(!this);
  this             = calloc(1, sizeof(*this));
  this->shmID      = -1;
  this->data       = (void *)-1;
  this->frameEvent = lgCreateEvent(true, 20);

  this->getPointerBufferFn = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  if (!this->frameEvent)
  {
    DEBUG_ERROR("Failed to create the frame event");
    free(this);
    return false;
  }

  return true;
}

static bool xcb_init(void)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(!this->initialized);

  lgResetEvent(this->frameEvent);

  this->stop = false;
  this->xcb = xcb_connect(NULL, NULL);
  if (!this->xcb || xcb_connection_has_error(this->xcb))
  {
    DEBUG_ERROR("Unable to open the X display");
    goto fail;
  }

  if (!xcb_get_extension_data(this->xcb, &xcb_shm_id)->present)
  {
    DEBUG_ERROR("Missing the SHM extension");
    goto fail;
  }

  xcb_screen_iterator_t iter;
  iter            = xcb_setup_roots_iterator(xcb_get_setup(this->xcb));
  this->xcbScreen = iter.data;
  this->width     = iter.data->width_in_pixels;
  this->height    = iter.data->height_in_pixels;
  DEBUG_INFO("Frame Size       : %u x %u", this->width, this->height);

  this->seg   = xcb_generate_id(this->xcb);
  const size_t maxFrameSize = this->width * this->height * 4;
  this->shmID = shmget(IPC_PRIVATE, maxFrameSize, IPC_CREAT | 0777);
  if (this->shmID == -1)
  {
    DEBUG_ERROR("shmget failed");
    goto fail;
  }

  xcb_shm_attach(this->xcb, this->seg ,this->shmID, false);
  this->data = shmat(this->shmID, NULL, 0);
  if ((uintptr_t)this->data == -1)
  {
    DEBUG_ERROR("shmat failed");
    goto fail;
  }
  DEBUG_INFO("Frame Data       : 0x%" PRIXPTR, (uintptr_t)this->data);

  xcb_query_extension_cookie_t extension_cookie =
		xcb_query_extension(this->xcb, strlen("XFIXES"), "XFIXES");
  xcb_query_extension_reply_t * extension_reply =
		xcb_query_extension_reply(this->xcb, extension_cookie, NULL);
  if(!extension_reply)
  {
    DEBUG_ERROR("Extension \"XFIXES\" isn't available");
    free(extension_reply);
    goto fail;
  }
  free(extension_reply);

  xcb_xfixes_query_version_cookie_t version_cookie =
		xcb_xfixes_query_version(this->xcb, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
  xcb_xfixes_query_version_reply_t * version_reply =
		xcb_xfixes_query_version_reply(this->xcb, version_cookie, NULL);
  if(!version_reply)
  {
    DEBUG_ERROR("Extension \"XFIXES\" isn't available");
    free(version_reply);
    goto fail;
  }
  free(version_reply);

  this->initialized = true;
  return true;
fail:
  xcb_deinit();
  return false;
}

static bool xcb_start(void)
{
  this->stop = false;

  if (!lgCreateThread("XCBPointer", pointerThread, NULL, &this->pointerThread))
  {
    DEBUG_ERROR("Failed to create the XCBPointer thread");
    return false;
  }

  return true;
}

static void xcb_stop(void)
{
  this->stop = true;

  if(this->pointerThread)
  {
    lgJoinThread(this->pointerThread, NULL);
    this->pointerThread = NULL;
  }
}

static bool xcb_deinit(void)
{
  DEBUG_ASSERT(this);

  if ((uintptr_t)this->data != -1)
  {
    shmdt(this->data);
    this->data = (void *)-1;
  }

  if (this->shmID != -1)
  {
    shmctl(this->shmID, IPC_RMID, NULL);
    this->shmID = -1;
  }

  if (this->xcb)
  {
    xcb_disconnect(this->xcb);
    this->xcb = NULL;
  }

  this->initialized = false;
  return true;
}

static void xcb_free(void)
{
  lgFreeEvent(this->frameEvent);
  free(this);
  this = NULL;
}

static CaptureResult xcb_capture(void)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  if (!this->hasFrame)
  {
    this->imgC = xcb_shm_get_image_unchecked(
        this->xcb,
        this->xcbScreen->root,
        0, 0,
        this->width,
        this->height,
        ~0,
        XCB_IMAGE_FORMAT_Z_PIXMAP,
        this->seg,
        0);

    this->hasFrame  = true;
    lgSignalEvent(this->frameEvent);
  }

  return CAPTURE_RESULT_OK;
}

static CaptureResult xcb_waitFrame(CaptureFrame * frame,
    const size_t maxFrameSize)
{
  lgWaitEvent(this->frameEvent, TIMEOUT_INFINITE);

  const unsigned int maxHeight = maxFrameSize / (this->width * 4);

  frame->screenWidth  = this->width;
  frame->screenHeight = this->height;
  frame->frameWidth   = this->width;
  frame->frameHeight  = min(maxHeight, this->height);
  frame->truncated    = maxHeight < this->height;
  frame->pitch        = this->width * 4;
  frame->stride       = this->width;
  frame->format       = CAPTURE_FMT_BGRA;
  frame->rotation     = CAPTURE_ROT_0;

  return CAPTURE_RESULT_OK;
}

static CaptureResult xcb_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  xcb_shm_get_image_reply_t * img;
  img = xcb_shm_get_image_reply(this->xcb, this->imgC, NULL);
  if (!img)
  {
    DEBUG_ERROR("Failed to get image reply");
    return CAPTURE_RESULT_ERROR;
  }

  framebuffer_write(frame, this->data, this->width * height * 4);
  free(img);

  this->hasFrame = false;
  return CAPTURE_RESULT_OK;
}

static int pointerThread(void * unused)
{
  while (!this->stop)
  {
    if (this->stop)
      break;

    CapturePointer pointer = { 0 };

    this->curC = xcb_xfixes_get_cursor_image_unchecked(this->xcb);

    void * data;
    uint32_t size;
    if (!this->getPointerBufferFn(&data, &size))
    {
      DEBUG_WARN("failed to get a pointer buffer");
      continue;
    }

    xcb_xfixes_get_cursor_image_reply_t * curReply;
    curReply = xcb_xfixes_get_cursor_image_reply(this->xcb, this->curC, NULL);
    if (curReply == NULL)
    {
      DEBUG_WARN("Failed to get cursor reply");
      continue;
    }

    if(curReply->xhot != this->mouseHotX || curReply->yhot != this->mouseHotY)
    {
      pointer.shapeUpdate = true;
      this->mouseHotX = curReply->xhot;
      this->mouseHotY = curReply->yhot;

      uint32_t * src = xcb_xfixes_get_cursor_image_cursor_image(curReply);
      uint32_t * dst = (uint32_t *) (data);
      memcpy(dst, src, curReply->width * curReply->height * sizeof(uint32_t));
    }
    else
      pointer.shapeUpdate = false;

    if(curReply->x != this->mouseX || curReply->y != this->mouseY)
    {
      pointer.positionUpdate = true;
      this->mouseX = curReply->x;
      this->mouseY = curReply->y;
    }
    else
      pointer.positionUpdate = false;

    if(pointer.positionUpdate || pointer.shapeUpdate)
    {
      pointer.hx      = curReply->xhot;
      pointer.hy      = curReply->yhot;
      pointer.visible = true;
      pointer.x       = curReply->x - curReply->xhot;
      pointer.y       = curReply->y - curReply->yhot;
      pointer.format  = CAPTURE_FMT_COLOR;
      pointer.width   = curReply->width;
      pointer.height  = curReply->height;
      pointer.pitch   = curReply->width * 4;

      this->postPointerBufferFn(pointer);
    }

    free(curReply);
    usleep(1000);
  }

  return 0;
}

struct CaptureInterface Capture_XCB =
{
  .shortName       = "XCB",
  .asyncCapture    = true,
  .initOptions     = xcb_initOptions,
  .getName         = xcb_getName,
  .create          = xcb_create,
  .init            = xcb_init,
  .start           = xcb_start,
  .stop            = xcb_stop,
  .deinit          = xcb_deinit,
  .free            = xcb_free,
  .capture         = xcb_capture,
  .waitFrame       = xcb_waitFrame,
  .getFrame        = xcb_getFrame
};
