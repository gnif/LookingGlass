/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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
#include "common/windebug.h"
#include "windows/mousehook.h"
#include "windows/force_compose.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/event.h"
#include "common/thread.h"
#include "common/dpi.h"
#include <assert.h>
#include <stdlib.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

struct iface
{
  bool        stop;
  NvFBCHandle nvfbc;

  bool                       seperateCursor;
  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGThread                 * pointerThread;

  unsigned int maxWidth , maxHeight;
  unsigned int width    , height;
  unsigned int dpi;

  unsigned int formatVer;
  unsigned int grabWidth, grabHeight, grabStride;

  uint8_t * frameBuffer;
  uint8_t * diffMap;

  NvFBCFrameGrabInfo grabInfo;

  LGEvent * frameEvent;
  LGEvent * cursorEvents[2];

  int mouseX, mouseY, mouseHotX, mouseHotY;
  bool mouseVisible, hasMousePosition;

  bool mouseHookCreated;
  bool forceCompositionCreated;
};

static struct iface * this = NULL;

static bool nvfbc_deinit(void);
static void nvfbc_free(void);
static int pointerThread(void * unused);

static void getDesktopSize(unsigned int * width, unsigned int * height, unsigned int * dpi)
{
  HMONITOR    monitor     = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo = {
    .cbSize = sizeof(MONITORINFO)
  };

  GetMonitorInfo(monitor, &monitorInfo);
  *dpi = monitor_dpi(monitor);

  *width  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  *height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
}

static void on_mouseMove(int x, int y)
{
  this->hasMousePosition = true;
  this->mouseX           = x;
  this->mouseY           = y;
  lgSignalEvent(this->cursorEvents[0]);
}

static const char * nvfbc_getName(void)
{
  return "NVFBC (NVidia Frame Buffer Capture)";
};

static void nvfbc_initOptions(void)
{
  struct Option options[] =
  {
    {
      .module         = "nvfbc",
      .name           = "decoupleCursor",
      .description    = "Capture the cursor seperately",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {0}
  };

  option_register(options);
}

static bool nvfbc_create(
    CaptureGetPointerBuffer  getPointerBufferFn,
    CapturePostPointerBuffer postPointerBufferFn)
{
  if (!NvFBCInit())
    return false;

  this = (struct iface *)calloc(sizeof(struct iface), 1);
  this->frameEvent = lgCreateEvent(true, 17);
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    nvfbc_free();
    return false;
  }

  this->seperateCursor      = option_get_bool("nvfbc", "decoupleCursor");
  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  return true;
}

static bool nvfbc_init(void)
{
  this->stop = false;

  int       bufferLen   = GetEnvironmentVariable("NVFBC_PRIV_DATA", NULL, 0);
  uint8_t * privData    = NULL;
  int       privDataLen = 0;

  if (bufferLen)
  {
    char * buffer = malloc(bufferLen);
    GetEnvironmentVariable("NVFBC_PRIV_DATA", buffer, bufferLen);

    privDataLen = (bufferLen - 1) / 2;
    privData    = (uint8_t *)malloc(privDataLen);
    char hex[3] = {0};
    for(int i = 0; i < privDataLen; ++i)
    {
      memcpy(hex, &buffer[i*2], 2);
      privData[i] = (uint8_t)strtoul(hex, NULL, 16);
    }

    free(buffer);
  }

  if (!NvFBCToSysCreate(privData, privDataLen, &this->nvfbc, &this->maxWidth, &this->maxHeight))
  {
    free(privData);
    return false;
  }
  free(privData);

  getDesktopSize(&this->width, &this->height, &this->dpi);
  lgResetEvent(this->frameEvent);

  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB,
    !this->seperateCursor,
    this->seperateCursor,
    true,
    DIFFMAP_BLOCKSIZE_128X128,
    (void **)&this->frameBuffer,
    (void **)&this->diffMap,
    &event
  ))
  {
    return false;
  }

  this->cursorEvents[0] = lgCreateEvent(true, 10);

  if (this->seperateCursor)
    this->cursorEvents[1] = lgWrapEvent(event);

  if (!this->mouseHookCreated)
  {
    mouseHook_install(on_mouseMove);
    this->mouseHookCreated = true;
  }

  if (!this->forceCompositionCreated)
  {
    dwmForceComposition();
    this->forceCompositionCreated = true;
  }

  DEBUG_INFO("Cursor mode      : %s", this->seperateCursor ? "decoupled" : "integrated");

  Sleep(100);

  if (!lgCreateThread("NvFBCPointer", pointerThread, NULL, &this->pointerThread))
  {
    DEBUG_ERROR("Failed to create the NvFBCPointer thread");
    nvfbc_deinit();
    return false;
  }

  ++this->formatVer;
  return true;
}

static void nvfbc_stop(void)
{
  this->stop = true;

  lgSignalEvent(this->cursorEvents[0]);
  lgSignalEvent(this->frameEvent);

  if (this->pointerThread)
  {
    lgJoinThread(this->pointerThread, NULL);
    this->pointerThread = NULL;
  }
}

static bool nvfbc_deinit(void)
{
  if (this->cursorEvents[0])
  {
    lgFreeEvent(this->cursorEvents[0]);
    this->cursorEvents[0] = NULL;
  }

  if (this->nvfbc)
  {
    NvFBCToSysRelease(&this->nvfbc);
    this->nvfbc = NULL;
  }

  return true;
}

static void nvfbc_free(void)
{
  if (this->frameEvent)
    lgFreeEvent(this->frameEvent);

  if (this->mouseHookCreated)
    mouseHook_remove();

  if (this->forceCompositionCreated)
    dwmUnforceComposition();

  free(this);
  this = NULL;
  NvFBCFree();
}

static unsigned int nvfbc_getMaxFrameSize(void)
{
  return this->maxWidth * this->maxHeight * 4;
}

static unsigned int nvfbc_getMouseScale(void)
{
  return this->dpi * 100 / DPI_100_PERCENT;
}

static CaptureResult nvfbc_capture(void)
{
  getDesktopSize(&this->width, &this->height, &this->dpi);
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

  bool changed = false;
  const unsigned int h = (this->height + 127) / 128;
  const unsigned int w = (this->width  + 127) / 128;
  for(unsigned int y = 0; y < h; ++y)
    for(unsigned int x = 0; x < w; ++x)
      if (this->diffMap[(y*w)+x])
      {
        changed = true;
        break;
      }

  if (!changed)
    return CAPTURE_RESULT_TIMEOUT;

  memcpy(&this->grabInfo, &grabInfo, sizeof(grabInfo));
  lgSignalEvent(this->frameEvent);
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_waitFrame(CaptureFrame * frame)
{
  if (!lgWaitEvent(this->frameEvent, 1000))
    return CAPTURE_RESULT_TIMEOUT;

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  if (
    this->grabInfo.dwWidth       != this->grabWidth  ||
    this->grabInfo.dwHeight      != this->grabHeight ||
    this->grabInfo.dwBufferWidth != this->grabStride)
  {
    this->grabWidth  = this->grabInfo.dwWidth;
    this->grabHeight = this->grabInfo.dwHeight;
    this->grabStride = this->grabInfo.dwBufferWidth;
    ++this->formatVer;
  }

  frame->formatVer = this->formatVer;
  frame->width     = this->grabWidth;
  frame->height    = this->grabHeight;
  frame->pitch     = this->grabStride * 4;
  frame->stride    = this->grabStride;
  frame->rotation  = CAPTURE_ROT_0;

#if 0
  //NvFBC never sets bIsHDR so instead we check for any data in the alpha channel
  //If there is data, it's HDR. This is clearly suboptimal
  if (!this->grabInfo.bIsHDR)
    for(int y = 0; y < frame->height; ++y)
      for(int x = 0; x < frame->width; ++x)
      {
        int offset = (y * frame->pitch) + (x * 4);
        if (this->frameBuffer[offset + 3])
        {
          this->grabInfo.bIsHDR = 1;
          break;
        }
      }
#endif

  frame->format = this->grabInfo.bIsHDR ? CAPTURE_FMT_RGBA10 : CAPTURE_FMT_BGRA;
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getFrame(FrameBuffer * frame)
{
  framebuffer_write(
    frame,
    this->frameBuffer,
    this->grabInfo.dwHeight * this->grabInfo.dwBufferWidth * 4
  );
  return CAPTURE_RESULT_OK;
}

static int pointerThread(void * unused)
{
  lgSignalEvent(this->cursorEvents[1]);

  while(!this->stop)
  {
    LGEvent * events[2];
    memcpy(&events, &this->cursorEvents, sizeof(LGEvent *) * 2);
    if (!lgWaitEvents(events, this->seperateCursor ? 2 : 1, false, 1000))
      continue;

    if (this->stop)
      break;

    CaptureResult  result;
    CapturePointer pointer = { 0 };
    bool           hotspotUpdated = false;

    if (this->seperateCursor && events[1])
    {
      void * data;
      uint32_t size;
      if (!this->getPointerBufferFn(&data, &size))
      {
        DEBUG_WARN("failed to get a pointer buffer");
        continue;
      }

      result = NvFBCToSysGetCursor(this->nvfbc, &pointer, data, size);
      if (result != CAPTURE_RESULT_OK)
      {
        DEBUG_WARN("NvFBCToSysGetCursor failed");
        continue;
      }

      this->mouseVisible = pointer.visible;
      this->mouseHotX    = pointer.hx;
      this->mouseHotY    = pointer.hy;
      hotspotUpdated     = true;
    }

    if (events[0] || (hotspotUpdated && this->hasMousePosition))
    {
      pointer.positionUpdate = true;
      pointer.visible        = this->mouseVisible;
      pointer.x              = this->mouseX - this->mouseHotX;
      pointer.y              = this->mouseY - this->mouseHotY;
    }

    this->postPointerBufferFn(pointer);
  }

  return 0;
}

struct CaptureInterface Capture_NVFBC =
{
  .getName         = nvfbc_getName,
  .initOptions     = nvfbc_initOptions,

  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .stop            = nvfbc_stop,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .getMaxFrameSize = nvfbc_getMaxFrameSize,
  .getMouseScale   = nvfbc_getMouseScale,
  .capture         = nvfbc_capture,
  .waitFrame       = nvfbc_waitFrame,
  .getFrame        = nvfbc_getFrame
};
