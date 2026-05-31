/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include "common/util.h"
#include "common/array.h"
#include "common/option.h"
#include "common/framebuffer.h"
#include "common/event.h"
#include "common/rects.h"
#include "common/thread.h"
#include "common/KVMFR.h"
#include "common/vector.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <windows.h>
#include <dwmapi.h>
#include <d3d9.h>

#include "downsample_parser.h"

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
  bool                       dwmFlush;
  bool                       noHDR;
  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGThread                 * pointerThread;

  unsigned int maxWidth , maxHeight;
  unsigned int width    , height;
  unsigned int dataHeight;
  bool         resChanged, scale;
  unsigned int targetWidth, targetHeight;

  unsigned int  formatVer;
  unsigned int  grabWidth, grabHeight, grabStride;
  unsigned int  bpp;
  CaptureFormat format;
  unsigned int  shmStride;
  bool          isHDR;

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

  this->postPointerBufferFn(&pointer);
}

static const char * nvfbc_getName(void)
{
  return "NVFBC";
};

static Vector downsampleRules = {0};

static void nvfbc_initOptions(void)
{
  struct Option options[] =
  {
    DOWNSAMPLE_PARSER("nvfbc", &downsampleRules),
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
    {
      .module         = "nvfbc",
      .name           = "adapterIndex",
      .description    = "The index of the adapter to capture from",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = -1
    },
    {
      .module         = "nvfbc",
      .name           = "dwmFlush",
      .description    = "Use DwmFlush to sync the capture to the windows presentation inverval",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "nvfbc",
      .name           = "noHDR",
      .description    = "Capture HDR content as SDR",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {0}
  };

  option_register(options);
}

static bool nvfbc_create(
  CaptureGetPointerBuffer  getPointerBufferFn,
  CapturePostPointerBuffer postPointerBufferFn,
  unsigned                 frameBuffers)
{
  if (!NvFBCInit())
    return false;

  this = calloc(1, sizeof(*this));

  this->seperateCursor      = option_get_bool("nvfbc", "decoupleCursor");
  this->dwmFlush            = option_get_bool("nvfbc", "dwmFlush"      );
  this->noHDR               = option_get_bool("nvfbc", "noHDR"         );
  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  DEBUG_BREAK();
  DEBUG_WARN("NvFBC IS DEPRECATED by NVIDIA");
  DEBUG_WARN("As the new D12 capture engine is faster then NvFBC in every way");
  DEBUG_WARN("we HIGHLY recommend you migrate to it");
  DEBUG_BREAK();

  return true;
}

static void updateScale(void)
{
  DownsampleRule * rule = downsampleRule_match(&downsampleRules,
    this->width, this->height);
  if (rule)
  {
    this->scale        = true;
    this->targetWidth  = rule->targetX;
    this->targetHeight = rule->targetY;
    DEBUG_INFO("Downsampling to %dx%d", this->targetWidth, this->targetHeight);
    return;
  }

  this->scale        = false;
  this->targetWidth  = this->width;
  this->targetHeight = this->height;
}

static bool nvfbc_init(void * ivshmemBase, unsigned * alignSize)
{
  int adapterIndex = option_get_int("nvfbc", "adapterIndex");

  IDirect3D9 * d3d = Direct3DCreate9(D3D_SDK_VERSION);
  int adapterCount = IDirect3D9_GetAdapterCount(d3d);
  if (adapterIndex > adapterCount)
  {
    DEBUG_ERROR("Invalid adapterIndex specified");
    IDirect3D9_Release(d3d);
    return false;
  }

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

  D3DADAPTER_IDENTIFIER9 ident;
  bool created = false;
  for(int retry = 0; retry < 2; ++retry)
  {
    // NOTE: Calling this on hardware that doesn't support NvFBC such as GeForce
    // causes a substantial performance pentalty even if it fails! As such we only
    // attempt NvFBC as a last resort, or if configured via the app:capture
    // option.
    if (adapterIndex < 0)
    {
      for(int i = 0; i < adapterCount; ++i)
      {
        if (IDirect3D9_GetAdapterIdentifier(d3d, i, 0, &ident) != D3D_OK ||
          ident.VendorId != 0x10DE)
          continue;

        if (!NvFBCToSysCreate(i, privData, privDataLen, &this->nvfbc,
          &this->maxWidth, &this->maxHeight))
          continue;

        adapterIndex = i;
        created      = true;
        break;
      }
    }
    else
    {
      if (IDirect3D9_GetAdapterIdentifier(d3d, adapterIndex, 0, &ident) !=
        D3D_OK || ident.VendorId != 0x10DE)
      {
        DEBUG_ERROR("adapterIndex %d is not a NVidia device", adapterIndex);
        break;
      }

      if (!NvFBCToSysCreate(adapterIndex, privData, privDataLen, &this->nvfbc,
        &this->maxWidth, &this->maxHeight))
        continue;

      created = true;
    }

    if (created)
      break;

    //1000ms delay before retry
    nsleep(1000000000);
  }

  IDirect3D9_Release(d3d);

  if (!created)
  {
    free(privData);
    return false;
  }

  int diffRes = option_get_int("nvfbc", "diffRes");
  enum DiffMapBlockSize blockSize;
  NvFBCGetDiffMapBlockSize(diffRes, &blockSize, &this->diffShift, privData, privDataLen);
  free(privData);

  getDesktopSize(&this->width, &this->height);
  updateScale();

  this->bpp    = this->noHDR ? 3 : 4;
  this->format = this->noHDR ? CAPTURE_FMT_RGB_24 : CAPTURE_FMT_RGBA10;

  HANDLE event;
  if (!NvFBCToSysSetup(
    this->nvfbc,
    this->noHDR ? BUFFER_FMT_RGB : BUFFER_FMT_ARGB10,
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

  ++this->formatVer;
  this->stop = true;
  return true;
}

static bool nvfbc_start(void)
{
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

  if (!this->stop)
  {
    DEBUG_ERROR("BUG: start called when not stopped");
    return true;
  }

  this->stop = false;
  if (this->seperateCursor &&
      !lgCreateThread("NvFBCPointer", pointerThread, NULL, &this->pointerThread))
  {
    DEBUG_ERROR("Failed to create the NvFBCPointer thread");
    return false;
  }

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

static CaptureResult nvfbc_capture(unsigned frameBufferIndex,
  FrameBuffer * frameBuffer)
{
  // this is a bit of a hack as it causes this thread to block until the next
  // present keeping us locked with the refresh rate of the monitor being
  // captured
  if (this->dwmFlush)
    DwmFlush();

  unsigned int width, height;
  getDesktopSize(&width, &height);
  if (unlikely(this->width != width || this->height != height))
  {
    this->resChanged = true;
    this->width      = width;
    this->height     = height;
    updateScale();
  }

  NvFBCFrameGrabInfo grabInfo;
  CaptureResult result = NvFBCToSysCapture(
    this->nvfbc,
    1000,
    0, 0,
    this->targetWidth,
    this->targetHeight,
    this->scale,
    &grabInfo
  );

  if (unlikely(result != CAPTURE_RESULT_OK))
    return result;

  bool changed = false;
  const unsigned int h = DIFF_MAP_DIM(grabInfo.dwWidth , this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(grabInfo.dwHeight, this->diffShift);
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
  const unsigned int h = DIFF_MAP_DIM(this->grabHeight, this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(this->grabWidth,  this->diffShift);

  struct DisjointSet ds[w * h];

  // init the ds usage
  for(unsigned int i = 0; i < ARRAY_LENGTH(ds); ++i)
  {
    ds[i].id  = -1;
    ds[i].use = (bool)this->diffMap[i];
  }

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
        int x2 = min((ds[c].x2 + 1) << this->diffShift, this->grabWidth);
        int y2 = min((ds[c].y2 + 1) << this->diffShift, this->grabHeight);
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

static CaptureResult nvfbc_waitFrame(unsigned frameBufferIndex,
    CaptureFrame * frame, const size_t maxFrameSize)
{
  if (unlikely(this->stop))
    return CAPTURE_RESULT_REINIT;

  if (unlikely(
    this->grabInfo.dwWidth       != this->grabWidth  ||
    this->grabInfo.dwHeight      != this->grabHeight ||
    this->grabInfo.dwBufferWidth != this->grabStride ||
    this->grabInfo.bIsHDR        != this->isHDR      ||
    this->resChanged))
  {
    this->grabWidth  = this->grabInfo.dwWidth;
    this->grabHeight = this->grabInfo.dwHeight;
    this->grabStride = this->grabInfo.dwBufferWidth;
    this->isHDR      = this->grabInfo.bIsHDR;

    // Round up stride in IVSHMEM to avoid issues with dmabuf import.
    if (this->format == CAPTURE_FMT_RGB_24)
    {
      // EGLImage via DMABUF does not support 24-bit images, the client will
      // stuff this into a 32-bit texture so we need to ensure the padding is
      // wide enough to keep things aligned.
      this->shmStride = ALIGN_PAD(this->grabStride / 4, 64) * 4;
    }
    else
      this->shmStride = ALIGN_PAD(this->grabStride, 64);

    DEBUG_INFO("Mode: %dx%d, stride: %u, shmStride: %u",
      this->grabWidth , this->grabHeight,
      this->grabStride, this->shmStride);

    this->resChanged = false;
    ++this->formatVer;
  }

  const unsigned int maxHeight = maxFrameSize / (this->shmStride * this->bpp);
  this->dataHeight = min(maxHeight, this->grabHeight);

  frame->formatVer    = this->formatVer;
  frame->screenWidth  = this->width;
  frame->screenHeight = this->height;
  frame->frameWidth   = this->grabWidth;
  frame->frameHeight  = this->grabHeight;
  frame->dataWidth    = this->grabWidth;
  frame->dataHeight   = this->dataHeight;
  frame->truncated    = maxHeight < this->grabHeight;
  frame->pitch        = this->shmStride * this->bpp;
  frame->stride       = this->shmStride;
  frame->rotation     = CAPTURE_ROT_0;

  updateDamageRects(frame);

  frame->format = this->format;
  frame->hdr    = this->grabInfo.bIsHDR;
  frame->hdrPQ  = this->grabInfo.bIsHDR;

  return CAPTURE_RESULT_OK;
}

static CaptureResult nvfbc_getFrame(unsigned frameBufferIndex,
    FrameBuffer * frame, const size_t maxFrameSize)
{
  const unsigned int h = DIFF_MAP_DIM(this->grabHeight, this->diffShift);
  const unsigned int w = DIFF_MAP_DIM(this->grabWidth,  this->diffShift);
  uint8_t * frameData = framebuffer_get_data(frame);
  struct FrameInfo * info = this->frameInfo + frameBufferIndex;

  if (info->width == this->grabWidth && info->height == this->grabHeight)
  {
    const bool wasFresh = info->wasFresh;

    for (unsigned int y = 0; y < h; ++y)
    {
      const unsigned int ystart = y << this->diffShift;
      if (ystart > this->dataHeight)
        break;

      const unsigned int yend = min(
        this->dataHeight,
        (y + 1) << this->diffShift);

      for (unsigned int x = 0; x < w; )
      {
        if ((wasFresh || !info->diffMap[y * w + x]) && !this->diffMap[y * w + x])
        {
          ++x;
          continue;
        }

        unsigned int x2 = x;
        while (x2 < w && ((!wasFresh && info->diffMap[y * w + x2]) ||
          this->diffMap[y * w + x2]))
          ++x2;

        unsigned int xstart = x << this->diffShift;
        unsigned int xend   = min(x2 << this->diffShift, this->grabWidth);
        unsigned int width  = xend - xstart;

        rectCopyUnaligned(
          frameData,
          this->frameBuffer,
          ystart,
          yend,
          xstart           * this->bpp,
          this->shmStride  * this->bpp,
          this->grabStride * this->bpp,
          width            * this->bpp);

        x = x2;
      }
      framebuffer_set_write_ptr(frame, yend * this->shmStride * this->bpp);
    }
  }
  else if (this->grabStride != this->shmStride)
  {
    for (int y = 0; y < this->dataHeight; y += 64)
    {
      int yend = min(this->dataHeight, y + 128);
      rectCopyUnaligned(
        frameData,
        this->frameBuffer,
        y,
        yend,
        0,
        this->shmStride  * this->bpp,
        this->grabStride * this->bpp,
        this->grabWidth  * this->bpp);

      framebuffer_set_write_ptr(frame, yend * this->shmStride * this->bpp);
    }
  }
  else
    framebuffer_write(
      frame,
      this->frameBuffer,
      this->dataHeight * this->grabInfo.dwBufferWidth * this->bpp
    );

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    if (i == frameBufferIndex)
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
  while (likely(!this->stop))
  {
    lgWaitEvent(this->cursorEvent, TIMEOUT_INFINITE);

    if (unlikely(this->stop))
      break;

    CaptureResult  result;
    CapturePointer pointer = { 0 };

    void * data;
    uint32_t size;
    if (unlikely(!this->getPointerBufferFn(&data, &size)))
    {
      DEBUG_WARN("failed to get a pointer buffer");
      continue;
    }

    result = NvFBCToSysGetCursor(this->nvfbc, &pointer, data, size);
    if (unlikely(result != CAPTURE_RESULT_OK))
    {
      DEBUG_WARN("NvFBCToSysGetCursor failed");
      continue;
    }

    this->mouseVisible = pointer.visible;
    this->mouseHotX    = pointer.hx;
    this->mouseHotY    = pointer.hy;

    pointer.positionUpdate = true;
    pointer.x = this->mouseX - pointer.hx;
    pointer.y = this->mouseY - pointer.hy;

    this->postPointerBufferFn(&pointer);
  }

  return 0;
}

struct CaptureInterface Capture_NVFBC =
{
  .shortName       = "NvFBC",
  .asyncCapture    = false,
  .deprecated      = true,
  .getName         = nvfbc_getName,
  .initOptions     = nvfbc_initOptions,

  .create          = nvfbc_create,
  .init            = nvfbc_init,
  .start           = nvfbc_start,
  .stop            = nvfbc_stop,
  .deinit          = nvfbc_deinit,
  .free            = nvfbc_free,
  .capture         = nvfbc_capture,
  .waitFrame       = nvfbc_waitFrame,
  .getFrame        = nvfbc_getFrame
};
