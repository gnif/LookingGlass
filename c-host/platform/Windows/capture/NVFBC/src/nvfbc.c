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
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/event.h"
#include <assert.h>
#include <stdlib.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

struct iface
{
  bool        stop;
  NvFBCHandle nvfbc;

  bool         seperateCursor;
  void       * pointerShape;
  unsigned int pointerSize;
  unsigned int maxWidth, maxHeight;
  unsigned int width   , height;

  uint8_t * frameBuffer;

  NvFBCFrameGrabInfo grabInfo;

  LGEvent * frameEvent;
  LGEvent * cursorEvents[2];

  int mouseX, mouseY, mouseHotX, mouseHotY;
  bool mouseVisible;
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

static void on_mouseMove(int x, int y)
{
  this->mouseX = x;
  this->mouseY = y;
  lgSignalEvent(this->cursorEvents[0]);
}

static const char * nvfbc_getName()
{
  return "NVFBC (NVidia Frame Buffer Capture)";
};

static void nvfbc_initOptions()
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

static bool nvfbc_create()
{
  if (!NvFBCInit())
    return false;

  int       bufferLen   = GetEnvironmentVariable("NVFBC_PRIV_DATA", NULL, 0);
  uint8_t * privData    = NULL;
  int       privDataLen = 0;

  if(bufferLen)
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

  this = (struct iface *)calloc(sizeof(struct iface), 1);
  if (!NvFBCToSysCreate(privData, privDataLen, &this->nvfbc, &this->maxWidth, &this->maxHeight))
  {
    free(privData);
    nvfbc_free();
    return false;
  }
  free(privData);

  this->frameEvent = lgCreateEvent(true, 17);
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    nvfbc_free();
    return false;
  }

  this->seperateCursor = option_get_bool("nvfbc", "decoupleCursor");

  return true;
}

static bool nvfbc_init(void * pointerShape, const unsigned int pointerSize)
{
  this->stop         = false;
  this->pointerShape = pointerShape;
  this->pointerSize  = pointerSize;

  getDesktopSize(&this->width, &this->height);
  lgResetEvent(this->frameEvent);


  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB,
    !this->seperateCursor,
    this->seperateCursor,
    false,
    0,
    (void **)&this->frameBuffer,
    NULL,
    &event
  ))
  {
    return false;
  }

  this->cursorEvents[0] = lgCreateEvent(true, 10);
  mouseHook_install(on_mouseMove);

  if (this->seperateCursor)
    this->cursorEvents[1] = lgWrapEvent(event);

  DEBUG_INFO("Cursor mode      : %s", this->seperateCursor ? "decoupled" : "integrated");

  Sleep(100);
  return true;
}

static void nvfbc_stop()
{
  this->stop = true;
  lgSignalEvent(this->cursorEvents[0]);
  lgSignalEvent(this->frameEvent);
}

static bool nvfbc_deinit()
{
  mouseHook_remove();
  return true;
}

static void nvfbc_free()
{
  NvFBCToSysRelease(&this->nvfbc);

  if (this->frameEvent)
    lgFreeEvent(this->frameEvent);

  free(this);
  this = NULL;
  NvFBCFree();
}

static unsigned int nvfbc_getMaxFrameSize()
{
  return this->maxWidth * this->maxHeight * 4;
}

static CaptureResult nvfbc_capture()
{
  getDesktopSize(&this->width, &this->height);
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

  frame->width  = this->grabInfo.dwWidth;
  frame->height = this->grabInfo.dwHeight;
  frame->pitch  = this->grabInfo.dwBufferWidth * 4;
  frame->stride = this->grabInfo.dwBufferWidth;

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

static CaptureResult nvfbc_getFrame(FrameBuffer frame)
{
  framebuffer_write(
    frame,
    this->frameBuffer,
    this->grabInfo.dwHeight * this->grabInfo.dwBufferWidth * 4
  );
  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getPointer(CapturePointer * pointer)
{
  LGEvent * events[2];
  memcpy(&events, &this->cursorEvents, sizeof(LGEvent *) * 2);
  if (!lgWaitEvents(events, this->seperateCursor ? 2 : 1, false, 1000))
    return CAPTURE_RESULT_TIMEOUT;

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  CaptureResult result;
  pointer->shapeUpdate = false;
  if (this->seperateCursor && events[1])
  {
    result = NvFBCToSysGetCursor(this->nvfbc, pointer, this->pointerShape, this->pointerSize);
    this->mouseVisible = pointer->visible;
    this->mouseHotX    = pointer->x;
    this->mouseHotY    = pointer->y;
    if (result != CAPTURE_RESULT_OK)
      return result;
  }

  pointer->visible = this->mouseVisible;
  pointer->x       = this->mouseX - this->mouseHotX;
  pointer->y       = this->mouseY - this->mouseHotY;
  return CAPTURE_RESULT_OK;
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
  .capture         = nvfbc_capture,
  .waitFrame       = nvfbc_waitFrame,
  .getFrame        = nvfbc_getFrame,
  .getPointer      = nvfbc_getPointer
};