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
#include "common/windebug.h"
#include "windows/mousehook.h"
#include "windows/force_compose.h"
#include "common/array.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/event.h"
#include "common/rects.h"
#include "common/thread.h"
#include "common/KVMFR.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include "wrapper.h"

#define DIFF_MAP_DIM(x, shift) (((x) + (1 << (shift)) - 1) >> (shift))

struct FrameInfo
{
  unsigned int width;
  unsigned int height;
  bool wasFresh;
  uint8_t * diffMap;
};

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

  unsigned int formatVer;
  unsigned int grabWidth, grabHeight, grabStride;
  unsigned int shmStride;

  uint8_t * frameBuffer;
  uint8_t * diffMap;
  int       diffShift;

  NvFBCFrameGrabInfo grabInfo;

  LGEvent * cursorEvent;

  int mouseX, mouseY, mouseHotX, mouseHotY;
  bool mouseVisible, hasMousePosition;

  bool mouseHookCreated;
  bool forceCompositionCreated;

  struct FrameInfo frameInfo[LGMP_Q_FRAME_LEN];
};

static struct iface * this = NULL;

static bool nvfbc_deinit(void);
static void nvfbc_free(void);
static int pointerThread(void * unused);

static void getDesktopSize(unsigned int * width, unsigned int * height)
{
  HMONITOR    monitor     = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo = {
    .cbSize = sizeof(MONITORINFO)
  };

  GetMonitorInfo(monitor, &monitorInfo);

  *width  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  *height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
}

static void on_mouseMove(int x, int y)
{
  this->hasMousePosition = true;
  this->mouseX           = x;
  this->mouseY           = y;

  const CapturePointer pointer =
  {
    .positionUpdate = true,
    .visible        = this->mouseVisible,
    .x              = x - this->mouseHotX,
    .y              = y - this->mouseHotY
  };

  this->postPointerBufferFn(pointer);
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
    {
      .module         = "nvfbc",
      .name           = "diffRes",
      .description    = "The resolution of the diff map",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 128
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

  this = calloc(1, sizeof(*this));

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
    privData    = malloc(privDataLen);
    char hex[3] = {0};
    for (int i = 0; i < privDataLen; ++i)
    {
      memcpy(hex, &buffer[i*2], 2);
      privData[i] = (uint8_t)strtoul(hex, NULL, 16);
    }

    free(buffer);
  }

  // NOTE: Calling this on hardware that doesn't support NvFBC such as GeForce
  // causes a substantial performance pentalty even if it fails! As such we only
  // attempt NvFBC as a last resort, or if configured via the app:capture
  // option.
  if (!NvFBCToSysCreate(privData, privDataLen, &this->nvfbc, &this->maxWidth, &this->maxHeight))
  {
    free(privData);
    return false;
  }

  int diffRes = option_get_int("nvfbc", "diffRes");
  enum DiffMapBlockSize blockSize;
  NvFBCGetDiffMapBlockSize(diffRes, &blockSize, &this->diffShift, privData, privDataLen);
  free(privData);

  getDesktopSize(&this->width, &this->height);

  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    BUFFER_FMT_ARGB,
    !this->seperateCursor,
    this->seperateCursor,
    true,
    blockSize,
    (void **)&this->frameBuffer,
    (void **)&this->diffMap,
    &event
  ))
  {
    return false;
  }

  if (this->seperateCursor)
    this->cursorEvent = lgWrapEvent(event);

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

  if (diffRes != (1 << this->diffShift))
    DEBUG_WARN("DiffMap block size not supported: %dx%d", diffRes, diffRes);

  DEBUG_INFO("DiffMap block    : %dx%d", 1 << this->diffShift, 1 << this->diffShift);
  DEBUG_INFO("Cursor mode      : %s", this->seperateCursor ? "decoupled" : "integrated");

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    this->frameInfo[i].width    = 0;
    this->frameInfo[i].height   = 0;
    this->frameInfo[i].wasFresh = false;
    this->frameInfo[i].diffMap  = malloc(
      DIFF_MAP_DIM(this->maxWidth, this->diffShift) *
      DIFF_MAP_DIM(this->maxHeight, this->diffShift)
    );
    if (!this->frameInfo[i].diffMap)
    {
      DEBUG_ERROR("Failed to allocate memory for diffMaps");
      nvfbc_deinit();
      return false;
    }
  }

  Sleep(100);

  if (this->seperateCursor && !lgCreateThread("NvFBCPointer", pointerThread, NULL, &this->pointerThread))
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

  if (this->seperateCursor)
  {
    lgSignalEvent(this->cursorEvent);

    if (this->pointerThread)
    {
      lgJoinThread(this->pointerThread, NULL);
      this->pointerThread = NULL;
    }
  }
}

static bool nvfbc_deinit(void)
{
  this->cursorEvent = NULL;

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    free(this->frameInfo[i].diffMap);
    this->frameInfo[i].diffMap = NULL;
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
  if (this->mouseHookCreated)
    mouseHook_remove();

  if (this->forceCompositionCreated)
    dwmUnforceComposition();

  free(this);
  this = NULL;
  NvFBCFree();
}

static CaptureResult nvfbc_capture(void)
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

  bool changed = false;
  const unsigned int h = DIFF_MAP_DIM(this->height, this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(this->width,  this->diffShift);
  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
      if (this->diffMap[(y*w)+x])
      {
        changed = true;
        goto done;
      }

done:
  if (!changed)
    return CAPTURE_RESULT_TIMEOUT;

  memcpy(&this->grabInfo, &grabInfo, sizeof(grabInfo));
  return CAPTURE_RESULT_OK;
}

struct DisjointSet {
  int  id;
  bool use;
  bool row;
  int  x1;
  int  y1;
  int  x2;
  int  y2;
};

static int dsFind(struct DisjointSet * ds, int id)
{
  if (ds[id].id != id)
    ds[id].id = dsFind(ds, ds[id].id);
  return ds[id].id;
}

static void dsUnion(struct DisjointSet * ds, int a, int b)
{
  a = dsFind(ds, a);
  b = dsFind(ds, b);
  if (a == b)
    return;

  ds[b].id = a;
  ds[a].x2 = max(ds[a].x2, ds[b].x2);
  ds[a].y2 = max(ds[a].y2, ds[b].y2);
}

static void updateDamageRects(CaptureFrame * frame)
{
  const unsigned int h = DIFF_MAP_DIM(this->height, this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(this->width,  this->diffShift);

  struct DisjointSet ds[w * h];

  // init the ds usage
  for(unsigned int i = 0; i < ARRAY_LENGTH(ds); ++i)
    ds[i].use = (bool)this->diffMap[i];

  // reduce the number of resulting rectangles by filling in holes and merging
  // irregular shapes into contiguous rectangles
  bool resolve;
  do
  {
    resolve = false;
    for (unsigned int y = 0; y < h; ++y)
      for (unsigned int x = 0; x < w; ++x)
      {
        const int c = y * w + x;     // current
        if (ds[c].use)
          continue;

        const int l  = c - 1; // left
        const int r  = c + 1; // right
        const int u  = c - w; // up
        const int d  = c + w; // down

        if ((x < w - 1 && y < h - 1 && ds[r].use && ds[d].use) ||
            (x > 0     && y < h - 1 && ds[l].use && ds[d].use) ||
            (x < w - 1 && y > 0     && ds[r].use && ds[u].use) ||
            (x > 0     && y > 0     && ds[l].use && ds[u].use) ||
            (x > 0     && y > 0     &&
             x < w - 1 && y < h - 1 && ds[l].use && ds[u].use &&
                                       ds[r].use && ds[d].use)
            )
        {
          ds[c].use = true;
          resolve   = true;
        }
      }
  }
  while(resolve);

  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
    {
      const int c = y * w + x; // current
      const int l = c - 1;     // left
      const int u = c - w;     // up

      if (ds[c].use)
      {
        ds[c].id  = c;
        ds[c].row = false;
        ds[c].x1  = ds[c].x2 = x;
        ds[c].y1  = ds[c].y2 = y;

        if (y > 0 && ds[u].use)
        {
          bool ok = true;
          if (x > 0 && ds[l].id != ds[u].id)
          {
            // no need to use dsFind as the search order ensures that the id of
            // the block above has been fully resolved
            for(unsigned int j = ds[ds[u].id].x1; j <= ds[ds[u].id].x2; ++j)
              if (!ds[y * w + j].use)
              {
                ok = false;
                break;
              }
          }

          if (ok)
          {
            dsUnion(ds, u, c);
            ds[c].row = true;
            continue;
          }
        }

        if (x > 0 && ds[l].use && (ds[l].id == l || !ds[l].row))
          dsUnion(ds, l, c);
      }
    }

  int rectId = 0;
  for (unsigned int y = 0; y < h; ++y)
    for (unsigned int x = 0; x < w; ++x)
    {
      const int c = y * w + x;

      if (ds[c].use && ds[c].id == c)
      {
        if (rectId >= KVMFR_MAX_DAMAGE_RECTS)
        {
          rectId = 0;
          goto done;
        }

        int x1 = ds[c].x1 << this->diffShift;
        int y1 = ds[c].y1 << this->diffShift;
        int x2 = min((ds[c].x2 + 1) << this->diffShift, this->width);
        int y2 = min((ds[c].y2 + 1) << this->diffShift, this->height);
        frame->damageRects[rectId++] = (FrameDamageRect) {
          .x = x1,
          .y = y1,
          .width = x2 - x1,
          .height = y2 - y1,
        };
      }
    }

done:
  frame->damageRectsCount = rectId;
}

static CaptureResult nvfbc_waitFrame(CaptureFrame * frame,
    const size_t maxFrameSize)
{
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
    // Round up stride in IVSHMEM to avoid issues with dmabuf import.
    this->shmStride  = ALIGN_PAD(this->grabStride, 32);
    ++this->formatVer;
  }

  const unsigned int maxHeight = maxFrameSize / (this->shmStride * 4);

  frame->formatVer  = this->formatVer;
  frame->width      = this->grabWidth;
  frame->height     = maxHeight > this->grabHeight ? this->grabHeight : maxHeight;
  frame->realHeight = this->grabHeight;
  frame->pitch      = this->shmStride * 4;
  frame->stride     = this->shmStride;
  frame->rotation   = CAPTURE_ROT_0;

  updateDamageRects(frame);

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

static CaptureResult nvfbc_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  const unsigned int h = DIFF_MAP_DIM(this->height, this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(this->width,  this->diffShift);
  uint8_t * frameData = framebuffer_get_data(frame);
  struct FrameInfo * info = this->frameInfo + frameIndex;

  if (info->width == this->grabWidth && info->height == this->grabHeight)
  {
    const bool wasFresh = info->wasFresh;

    for (unsigned int y = 0; y < h; ++y)
    {
      const unsigned int ystart = y << this->diffShift;
      const unsigned int yend = min(height, (y + 1)  << this->diffShift);

      for (unsigned int x = 0; x < w; )
      {
        if ((wasFresh || !info->diffMap[y * w + x]) && !this->diffMap[y * w + x])
        {
          ++x;
          continue;
        }

        unsigned int x2 = x;
        while (x2 < w && ((!wasFresh && info->diffMap[y * w + x2]) || this->diffMap[y * w + x2]))
          ++x2;

        unsigned int width = (min(x2 << this->diffShift, this->grabWidth) - (x << this->diffShift)) * 4;
        rectCopyUnaligned(frameData, this->frameBuffer, ystart, yend, x << (2 + this->diffShift),
            this->shmStride * 4, this->grabStride * 4, width);

        x = x2;
      }
      framebuffer_set_write_ptr(frame, yend * this->shmStride * 4);
    }
  }
  else if (this->grabStride != this->shmStride)
  {
    for (int y = 0; y < height; y += 64)
    {
      int yend = min(height, y + 128);
      rectCopyUnaligned(frameData, this->frameBuffer, y, yend, 0, this->shmStride * 4,
        this->grabStride * 4, this->grabWidth * 4);
      framebuffer_set_write_ptr(frame, yend * this->shmStride * 4);
    }
  }
  else
    framebuffer_write(
      frame,
      this->frameBuffer,
      height * this->grabInfo.dwBufferWidth * 4
    );

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if (i == frameIndex)
    {
      this->frameInfo[i].width    = this->grabWidth;
      this->frameInfo[i].height   = this->grabHeight;
      this->frameInfo[i].wasFresh = true;
    }
    else if (this->frameInfo[i].width == this->grabWidth &&
          this->frameInfo[i].height == this->grabHeight)
    {
      if (this->frameInfo[i].wasFresh)
      {
        memcpy(this->frameInfo[i].diffMap, this->diffMap, h * w);
        this->frameInfo[i].wasFresh = false;
      }
      else
      {
        for (unsigned int j = 0; j < h * w; ++j)
          this->frameInfo[i].diffMap[j] |= this->diffMap[j];
      }
    }
    else
    {
      this->frameInfo[i].width  = 0;
      this->frameInfo[i].height = 0;
    }
  }
  return CAPTURE_RESULT_OK;
}

static int pointerThread(void * unused)
{
  while (!this->stop)
  {
    lgWaitEvent(this->cursorEvent, TIMEOUT_INFINITE);

    if (this->stop)
      break;

    CaptureResult  result;
    CapturePointer pointer = { 0 };

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

    pointer.positionUpdate = true;
    pointer.visible        = this->mouseVisible;
    pointer.x              = this->mouseX - pointer.hx;
    pointer.y              = this->mouseY - pointer.hy;

    this->postPointerBufferFn(pointer);
  }

  return 0;
}

struct CaptureInterface Capture_NVFBC =
{
  .shortName       = "NvFBC",
  .asyncCapture    = false,
  .getName         = nvfbc_getName,
  .initOptions     = nvfbc_initOptions,

  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .stop            = nvfbc_stop,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .capture         = nvfbc_capture,
  .waitFrame       = nvfbc_waitFrame,
  .getFrame        = nvfbc_getFrame
};
