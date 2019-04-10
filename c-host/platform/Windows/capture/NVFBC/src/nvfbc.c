/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "interface/capture.h"
#include "interface/platform.h"
#include "debug.h"
#include <assert.h>
#include <stdlib.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

struct iface
{
  bool         reinit;
  NvFBCToSys * nvfbc;

  void       * pointerShape;
  unsigned int pointerSize;
  unsigned int width, height;

  uint8_t * frameBuffer;
  uint8_t * diffMap;

  NvFBCFrameGrabInfo grabInfo;

  osEventHandle * frameEvent;
  osEventHandle * pointerEvent;
};

static struct iface * this = NULL;

static void nvfbc_free();

static void getDesktopSize(unsigned int * width, unsigned int * height)
{
  HMONITOR    monitor     = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo = {
    .cbSize = sizeof(MONITORINFO)
  };

  GetMonitorInfo(monitor, &monitorInfo);
  CloseHandle(monitor);

  *width  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  *height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
}

static const char * nvfbc_getName()
{
  return "NVFBC (NVidia Frame Buffer Capture)";
};

static bool nvfbc_create()
{
  if (!NvFBCInit())
    return false;

  this = (struct iface *)calloc(sizeof(struct iface), 1);

  if (!NvFBCToSysCreate(NULL, 0, &this->nvfbc))
  {
    nvfbc_free();
    return false;
  }

  this->frameEvent = os_createEvent(true);
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    nvfbc_free();
    return false;
  }

  this->pointerEvent = os_createEvent(true);
  if (!this->pointerEvent)
  {
    DEBUG_ERROR("failed to create the pointer event");
    nvfbc_free();
    return false;
  }

  return true;
}

static bool nvfbc_init(void * pointerShape, const unsigned int pointerSize)
{
  this->reinit       = false;
  this->pointerShape = pointerShape;
  this->pointerSize  = pointerSize;

  getDesktopSize(&this->width, &this->height);
  os_resetEvent(this->frameEvent);
  os_resetEvent(this->pointerEvent);

  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB,
    true,
    true,
    DIFFMAP_BLOCKSIZE_128X128,
    (void **)&this->frameBuffer,
    (void **)&this->diffMap
  ))
  {
    return false;
  }

  Sleep(100);

  return true;
}

static bool nvfbc_deinit()
{
  return true;
}

static void nvfbc_free()
{
  NvFBCToSysRelease(&this->nvfbc);

  if (this->frameEvent)
    os_freeEvent(this->frameEvent);

  if (this->pointerEvent)
    os_freeEvent(this->pointerEvent);

  free(this);
  this = NULL;
  NvFBCFree();
}

static unsigned int nvfbc_getMaxFrameSize()
{
  return this->width * this->height * 4;
}

static CaptureResult nvfbc_capture()
{
  // check if the resolution has changed, if it has we need to re-init to avoid capturing
  // black areas as NvFBC doesn't tell us about the change.
  unsigned int width, height;
  getDesktopSize(&width, &height);
  if (this->width != width || this->height != height)
  {
    DEBUG_INFO("Resolution change detected");

    this->reinit = true;
    os_signalEvent(this->frameEvent  );
    os_signalEvent(this->pointerEvent);

    return CAPTURE_RESULT_REINIT;
  }

  NvFBCFrameGrabInfo grabInfo;
  CaptureResult result = NvFBCToSysCapture(
    this->nvfbc,
    1000,
    0, 0,
    this->width,
    this->height,
    &grabInfo
  );

  if (result != CAPTURE_RESULT_OK)
    return result;

  // NvFBC doesn't tell us when a timeout occurs, so check the diff map
  // to see if anything actually changed

  const int dw = (grabInfo.dwWidth  + 0x7F) >> 7;
  const int dh = (grabInfo.dwHeight + 0x7F) >> 7;
  bool diff = false;
  for(int y = 0; y < dh && !diff; ++y)
    for(int x = 0; x < dw; ++x)
      if (this->diffMap[y * dw + x])
      {
        diff = true;
        break;
      }

  if (!diff)
    return CAPTURE_RESULT_TIMEOUT;

  memcpy(&this->grabInfo, &grabInfo, sizeof(grabInfo));
  os_signalEvent(this->frameEvent);
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getFrame(CaptureFrame * frame)
{
  if (!os_waitEvent(this->frameEvent, TIMEOUT_INFINITE))
  {
    DEBUG_ERROR("Failed to wait on the frame event");
    return CAPTURE_RESULT_ERROR;
  }

  if (this->reinit)
    return CAPTURE_RESULT_REINIT;

  frame->width  = this->grabInfo.dwWidth;
  frame->height = this->grabInfo.dwHeight;
  frame->pitch  = this->grabInfo.dwBufferWidth * 4;
  frame->stride = this->grabInfo.dwBufferWidth;
  frame->format = CAPTURE_FMT_BGRA;

  memcpy(frame->data, this->frameBuffer, frame->pitch * frame->height);
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getPointer(CapturePointer * pointer)
{
  if (!os_waitEvent(this->pointerEvent, TIMEOUT_INFINITE))
  {
    DEBUG_ERROR("Failed to wait on the pointer event");
    return CAPTURE_RESULT_ERROR;
  }

  if (this->reinit)
    return CAPTURE_RESULT_REINIT;

  return CAPTURE_RESULT_ERROR;
}

struct CaptureInterface Capture_NVFBC =
{
  .getName         = nvfbc_getName,
  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .getMaxFrameSize = nvfbc_getMaxFrameSize,
  .capture         = nvfbc_capture,
  .getFrame        = nvfbc_getFrame,
  .getPointer      = nvfbc_getPointer
};