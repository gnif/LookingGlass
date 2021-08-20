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

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/debug.h"
#include "common/event.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <xcb/shm.h>
#include <xcb/xfixes.h>
#include <sys/ipc.h>
#include <sys/shm.h>

struct xcb
{
  bool               initialized;
  xcb_connection_t * xcb;
  xcb_screen_t     * xcbScreen;
  uint32_t           seg;
  int                shmID;
  void             * data;
  LGEvent          * frameEvent;

  unsigned int width;
  unsigned int height;

  bool                                 hasFrame;
  xcb_shm_get_image_cookie_t           imgC;
  xcb_xfixes_get_cursor_image_cookie_t curC;
};

static struct xcb * this = NULL;

// forwards

static bool xcb_deinit();

// implementation

static const char * xcb_getName(void)
{
  return "XCB";
}

static bool xcb_create(CaptureGetPointerBuffer getPointerBufferFn, CapturePostPointerBuffer postPointerBufferFn)
{
  DEBUG_ASSERT(!this);
  this             = calloc(1, sizeof(*this));
  this->shmID      = -1;
  this->data       = (void *)-1;
  this->frameEvent = lgCreateEvent(true, 20);

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

  this->initialized = true;
  return true;
fail:
  xcb_deinit();
  return false;
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
  return false;
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

  frame->width      = this->width;
  frame->height     = maxHeight > this->height ? this->height : maxHeight;
  frame->realHeight = this->height;
  frame->pitch      = this->width * 4;
  frame->stride     = this->width;
  frame->format     = CAPTURE_FMT_BGRA;
  frame->rotation   = CAPTURE_ROT_0;

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

struct CaptureInterface Capture_XCB =
{
  .shortName       = "XCB",
  .asyncCapture    = true,
  .getName         = xcb_getName,
  .create          = xcb_create,
  .init            = xcb_init,
  .deinit          = xcb_deinit,
  .free            = xcb_free,
  .capture         = xcb_capture,
  .waitFrame       = xcb_waitFrame,
  .getFrame        = xcb_getFrame
};
