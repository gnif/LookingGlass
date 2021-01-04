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

#include "main.h"
#include "config.h"

#include <getopt.h>
#include <signal.h>
#include <SDL2/SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>

#if SDL_VIDEO_DRIVER_X11_XINPUT2
// because SDL2 sucks and we need to turn it off
#include <X11/extensions/XInput2.h>
#endif

#include "common/debug.h"
#include "common/crash.h"
#include "common/KVMFR.h"
#include "common/stringutils.h"
#include "common/thread.h"
#include "common/locking.h"
#include "common/event.h"
#include "common/ivshmem.h"
#include "common/time.h"
#include "common/version.h"

#include "utils.h"
#include "kb.h"
#include "ll.h"

#define RESIZE_TIMEOUT (10 * 1000) // 10ms

// forwards
static int cursorThread(void * unused);
static int renderThread(void * unused);
static int frameThread (void * unused);

static LGEvent  *e_startup = NULL;
static LGEvent  *e_frame   = NULL;
static LGThread *t_spice   = NULL;
static LGThread *t_render  = NULL;
static LGThread *t_cursor  = NULL;
static LGThread *t_frame   = NULL;
static SDL_Cursor *cursor  = NULL;

struct AppState g_state;
struct CursorState g_cursor;

// this structure is initialized in config.c
struct AppParams params = { 0 };

static void handleMouseMoveEvent(int ex, int ey);

static void lgInit()
{
  g_state.state         = APP_STATE_RUNNING;
  g_state.resizeDone    = true;

  g_cursor.scale         = false;
  g_cursor.scaleX        = 1.0f;
  g_cursor.scaleY        = 1.0f;
  g_cursor.draw    = true;
  g_cursor.inView  = true;
  g_cursor.guest.valid   = false;
}

static void updatePositionInfo()
{
  if (g_state.haveSrcSize)
  {
    if (params.keepAspect)
    {
      const float srcAspect = (float)g_state.srcSize.y / (float)g_state.srcSize.x;
      const float wndAspect = (float)g_state.windowH / (float)g_state.windowW;
      bool force = true;

      if (params.dontUpscale &&
          g_state.srcSize.x <= g_state.windowW &&
          g_state.srcSize.y <= g_state.windowH)
      {
        force = false;
        g_state.dstRect.w = g_state.srcSize.x;
        g_state.dstRect.h = g_state.srcSize.y;
        g_state.dstRect.x = g_state.windowW / 2 - g_state.srcSize.x / 2;
        g_state.dstRect.y = g_state.windowH / 2 - g_state.srcSize.y / 2;
      }
      else
      if ((int)(wndAspect * 1000) == (int)(srcAspect * 1000))
      {
        force           = false;
        g_state.dstRect.w = g_state.windowW;
        g_state.dstRect.h = g_state.windowH;
        g_state.dstRect.x = 0;
        g_state.dstRect.y = 0;
      }
      else
      if (wndAspect < srcAspect)
      {
        g_state.dstRect.w = (float)g_state.windowH / srcAspect;
        g_state.dstRect.h = g_state.windowH;
        g_state.dstRect.x = (g_state.windowW >> 1) - (g_state.dstRect.w >> 1);
        g_state.dstRect.y = 0;
      }
      else
      {
        g_state.dstRect.w = g_state.windowW;
        g_state.dstRect.h = (float)g_state.windowW * srcAspect;
        g_state.dstRect.x = 0;
        g_state.dstRect.y = (g_state.windowH >> 1) - (g_state.dstRect.h >> 1);
      }

      if (force && params.forceAspect)
      {
        g_state.resizeTimeout = microtime() + RESIZE_TIMEOUT;
        g_state.resizeDone    = false;
      }
    }
    else
    {
      g_state.dstRect.x = 0;
      g_state.dstRect.y = 0;
      g_state.dstRect.w = g_state.windowW;
      g_state.dstRect.h = g_state.windowH;
    }
    g_state.dstRect.valid = true;

    g_cursor.scale = (
        g_state.srcSize.y != g_state.dstRect.h ||
        g_state.srcSize.x != g_state.dstRect.w);

    g_cursor.scaleX = (float)g_state.srcSize.y / (float)g_state.dstRect.h;
    g_cursor.scaleY = (float)g_state.srcSize.x / (float)g_state.dstRect.w;
  }

  g_state.lgrResize = true;
}

static int renderThread(void * unused)
{
  if (!g_state.lgr->render_startup(g_state.lgrData, g_state.window))
  {
    g_state.state = APP_STATE_SHUTDOWN;

    /* unblock threads waiting on the condition */
    lgSignalEvent(e_startup);
    return 1;
  }

  /* signal to other threads that the renderer is ready */
  lgSignalEvent(e_startup);

  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  while(g_state.state != APP_STATE_SHUTDOWN)
  {
    if (params.fpsMin != 0)
    {
      lgWaitEventAbs(e_frame, &time);
      clock_gettime(CLOCK_MONOTONIC, &time);
      tsAdd(&time, g_state.frameTime);
    }

    if (g_state.lgrResize)
    {
      if (g_state.lgr)
        g_state.lgr->on_resize(g_state.lgrData, g_state.windowW, g_state.windowH, g_state.dstRect);
      g_state.lgrResize = false;
    }

    if (!g_state.lgr->render(g_state.lgrData, g_state.window))
      break;

    if (params.showFPS)
    {
      const uint64_t t    = nanotime();
      g_state.renderTime   += t - g_state.lastFrameTime;
      g_state.lastFrameTime = t;
      ++g_state.renderCount;

      if (g_state.renderTime > 1e9)
      {
        const float avgUPS = 1000.0f / (((float)g_state.renderTime /
          atomic_exchange_explicit(&g_state.frameCount, 0, memory_order_acquire)) /
          1e6f);

        const float avgFPS = 1000.0f / (((float)g_state.renderTime /
          g_state.renderCount) /
          1e6f);

        g_state.lgr->update_fps(g_state.lgrData, avgUPS, avgFPS);

        g_state.renderTime  = 0;
        g_state.renderCount = 0;
      }
    }

    if (!g_state.resizeDone && g_state.resizeTimeout < microtime())
    {
      SDL_SetWindowSize(
        g_state.window,
        g_state.dstRect.w,
        g_state.dstRect.h
      );
      g_state.resizeDone = true;
    }
  }

  g_state.state = APP_STATE_SHUTDOWN;

  if (t_cursor)
    lgJoinThread(t_cursor, NULL);

  if (t_frame)
    lgJoinThread(t_frame, NULL);

  g_state.lgr->deinitialize(g_state.lgrData);
  g_state.lgr = NULL;
  return 0;
}

static int cursorThread(void * unused)
{
  LGMP_STATUS         status;
  PLGMPClientQueue    queue;
  LG_RendererCursor   cursorType     = LG_CURSOR_COLOR;

  lgWaitEvent(e_startup, TIMEOUT_INFINITE);

  // subscribe to the pointer queue
  while(g_state.state == APP_STATE_RUNNING)
  {
    status = lgmpClientSubscribe(g_state.lgmp, LGMP_Q_POINTER, &queue);
    if (status == LGMP_OK)
      break;

    if (status == LGMP_ERR_NO_SUCH_QUEUE)
    {
      usleep(1000);
      continue;
    }

    DEBUG_ERROR("lgmpClientSubscribe Failed: %s", lgmpStatusString(status));
    g_state.state = APP_STATE_SHUTDOWN;
    break;
  }

  while(g_state.state == APP_STATE_RUNNING)
  {
    LGMPMessage msg;
    if ((status = lgmpClientProcess(queue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
      {
        if (g_cursor.redraw)
        {
          g_cursor.redraw = false;
          g_state.lgr->on_mouse_event
          (
            g_state.lgrData,
            g_cursor.guest.visible && g_cursor.draw,
            g_cursor.guest.x,
            g_cursor.guest.y
          );

          lgSignalEvent(e_frame);
        }

        usleep(params.cursorPollInterval);
        continue;
      }

      if (status == LGMP_ERR_INVALID_SESSION)
        g_state.state = APP_STATE_RESTART;
      else
      {
        DEBUG_ERROR("lgmpClientProcess Failed: %s", lgmpStatusString(status));
        g_state.state = APP_STATE_SHUTDOWN;
      }
      break;
    }

    KVMFRCursor * cursor = (KVMFRCursor *)msg.mem;

    g_cursor.guest.visible =
      msg.udata & CURSOR_FLAG_VISIBLE;

    if (msg.udata & CURSOR_FLAG_SHAPE)
    {
      switch(cursor->type)
      {
        case CURSOR_TYPE_COLOR       : cursorType = LG_CURSOR_COLOR       ; break;
        case CURSOR_TYPE_MONOCHROME  : cursorType = LG_CURSOR_MONOCHROME  ; break;
        case CURSOR_TYPE_MASKED_COLOR: cursorType = LG_CURSOR_MASKED_COLOR; break;
        default:
          DEBUG_ERROR("Invalid cursor type");
          lgmpClientMessageDone(queue);
          continue;
      }

      g_cursor.guest.hx = cursor->hx;
      g_cursor.guest.hy = cursor->hy;

      const uint8_t * data = (const uint8_t *)(cursor + 1);
      if (!g_state.lgr->on_mouse_shape(
        g_state.lgrData,
        cursorType,
        cursor->width,
        cursor->height,
        cursor->pitch,
        data)
      )
      {
        DEBUG_ERROR("Failed to update mouse shape");
        lgmpClientMessageDone(queue);
        continue;
      }
    }

    if (msg.udata & CURSOR_FLAG_POSITION)
    {
      g_cursor.guest.x       = cursor->x;
      g_cursor.guest.y       = cursor->y;
      g_cursor.guest.valid = true;
    }

    lgmpClientMessageDone(queue);
    g_cursor.redraw = false;

    g_state.lgr->on_mouse_event
    (
      g_state.lgrData,
      g_cursor.guest.visible && g_cursor.draw,
      g_cursor.guest.x,
      g_cursor.guest.y
    );

    if (params.mouseRedraw)
      lgSignalEvent(e_frame);
  }

  lgmpClientUnsubscribe(&queue);
  return 0;
}

static int frameThread(void * unused)
{
  struct DMAFrameInfo
  {
    KVMFRFrame * frame;
    size_t       dataSize;
    int          fd;
  };

  LGMP_STATUS      status;
  PLGMPClientQueue queue;

  uint32_t          formatVer = 0;
  bool              formatValid = false;
  size_t            dataSize;
  LG_RendererFormat lgrFormat;

  struct DMAFrameInfo dmaInfo[LGMP_Q_FRAME_LEN] = {0};
  const bool useDMA =
    params.allowDMA &&
    ivshmemHasDMA(&g_state.shm) &&
    g_state.lgr->supports &&
    g_state.lgr->supports(g_state.lgrData, LG_SUPPORTS_DMABUF);

  if (useDMA)
    DEBUG_INFO("Using DMA buffer support");

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  lgWaitEvent(e_startup, TIMEOUT_INFINITE);
  if (g_state.state != APP_STATE_RUNNING)
    return 0;

  // subscribe to the frame queue
  while(g_state.state == APP_STATE_RUNNING)
  {
    status = lgmpClientSubscribe(g_state.lgmp, LGMP_Q_FRAME, &queue);
    if (status == LGMP_OK)
      break;

    if (status == LGMP_ERR_NO_SUCH_QUEUE)
    {
      usleep(1000);
      continue;
    }

    DEBUG_ERROR("lgmpClientSubscribe Failed: %s", lgmpStatusString(status));
    g_state.state = APP_STATE_SHUTDOWN;
    break;
  }

  while(g_state.state == APP_STATE_RUNNING && !g_state.stopVideo)
  {
    LGMPMessage msg;
    if ((status = lgmpClientProcess(queue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
      {
        usleep(params.framePollInterval);
        continue;
      }

      if (status == LGMP_ERR_INVALID_SESSION)
        g_state.state = APP_STATE_RESTART;
      else
      {
        DEBUG_ERROR("lgmpClientProcess Failed: %s", lgmpStatusString(status));
        g_state.state = APP_STATE_SHUTDOWN;
      }
      break;
    }

    KVMFRFrame * frame       = (KVMFRFrame *)msg.mem;
    struct DMAFrameInfo *dma = NULL;

    if (!formatValid || frame->formatVer != formatVer)
    {
      // setup the renderer format with the frame format details
      lgrFormat.type   = frame->type;
      lgrFormat.width  = frame->width;
      lgrFormat.height = frame->height;
      lgrFormat.stride = frame->stride;
      lgrFormat.pitch  = frame->pitch;

      bool error = false;
      switch(frame->type)
      {
        case FRAME_TYPE_RGBA:
        case FRAME_TYPE_BGRA:
        case FRAME_TYPE_RGBA10:
          dataSize       = lgrFormat.height * lgrFormat.pitch;
          lgrFormat.bpp  = 32;
          break;

        case FRAME_TYPE_RGBA16F:
          dataSize       = lgrFormat.height * lgrFormat.pitch;
          lgrFormat.bpp  = 64;
          break;

        case FRAME_TYPE_YUV420:
          dataSize       = lgrFormat.height * lgrFormat.width;
          dataSize      += (dataSize / 4) * 2;
          lgrFormat.bpp  = 12;
          break;

        default:
          DEBUG_ERROR("Unsupported frameType");
          error = true;
          break;
      }

      if (error)
      {
        lgmpClientMessageDone(queue);
        g_state.state = APP_STATE_SHUTDOWN;
        break;
      }

      formatValid = true;
      formatVer   = frame->formatVer;

      DEBUG_INFO("Format: %s %ux%u %u %u",
          FrameTypeStr[frame->type],
          frame->width, frame->height,
          frame->stride, frame->pitch);

      if (!g_state.lgr->on_frame_format(g_state.lgrData, lgrFormat, useDMA))
      {
        DEBUG_ERROR("renderer failed to configure format");
        g_state.state = APP_STATE_SHUTDOWN;
        break;
      }
    }

    if (useDMA)
    {
      /* find the existing dma buffer if it exists */
      for(int i = 0; i < sizeof(dmaInfo) / sizeof(struct DMAFrameInfo); ++i)
      {
        if (dmaInfo[i].frame == frame)
        {
          dma = &dmaInfo[i];
          /* if it's too small close it */
          if (dma->dataSize < dataSize)
          {
            close(dma->fd);
            dma->fd = -1;
          }
          break;
        }
      }

      /* otherwise find a free buffer for use */
      if (!dma)
        for(int i = 0; i < sizeof(dmaInfo) / sizeof(struct DMAFrameInfo); ++i)
        {
          if (!dmaInfo[i].frame)
          {
            dma = &dmaInfo[i];
            dma->frame = frame;
            dma->fd    = -1;
            break;
          }
        }

      /* open the buffer */
      if (dma->fd == -1)
      {
        const uintptr_t pos    = (uintptr_t)msg.mem - (uintptr_t)g_state.shm.mem;
        const uintptr_t offset = (uintptr_t)frame->offset + FrameBufferStructSize;

        dma->dataSize = dataSize;
        dma->fd       = ivshmemGetDMABuf(&g_state.shm, pos + offset, dataSize);
        if (dma->fd < 0)
        {
          DEBUG_ERROR("Failed to get the DMA buffer for the frame");
          g_state.state = APP_STATE_SHUTDOWN;
          break;
        }
      }
    }

    if (lgrFormat.width != g_state.srcSize.x || lgrFormat.height != g_state.srcSize.y)
    {
      g_state.srcSize.x = lgrFormat.width;
      g_state.srcSize.y = lgrFormat.height;
      g_state.haveSrcSize = true;
      if (params.autoResize)
        SDL_SetWindowSize(g_state.window, lgrFormat.width, lgrFormat.height);

      updatePositionInfo();
    }

    FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
    if (!g_state.lgr->on_frame(g_state.lgrData, fb, useDMA ? dma->fd : -1))
    {
      lgmpClientMessageDone(queue);
      DEBUG_ERROR("renderer on frame returned failure");
      g_state.state = APP_STATE_SHUTDOWN;
      break;
    }

    atomic_fetch_add_explicit(&g_state.frameCount, 1, memory_order_relaxed);
    lgSignalEvent(e_frame);
    lgmpClientMessageDone(queue);
  }

  lgmpClientUnsubscribe(&queue);
  g_state.lgr->on_restart(g_state.lgrData);

  if (useDMA)
  {
    for(int i = 0; i < sizeof(dmaInfo) / sizeof(struct DMAFrameInfo); ++i)
      if (dmaInfo[i].fd >= 0)
        close(dmaInfo[i].fd);
  }


  return 0;
}

int spiceThread(void * arg)
{
  while(g_state.state != APP_STATE_SHUTDOWN)
    if (!spice_process(1000))
    {
      if (g_state.state != APP_STATE_SHUTDOWN)
      {
        g_state.state = APP_STATE_SHUTDOWN;
        DEBUG_ERROR("failed to process spice messages");
      }
      break;
    }

  g_state.state = APP_STATE_SHUTDOWN;
  return 0;
}

static inline const uint32_t mapScancode(SDL_Scancode scancode)
{
  uint32_t ps2;
  if (scancode > (sizeof(usb_to_ps2) / sizeof(uint32_t)) || (ps2 = usb_to_ps2[scancode]) == 0)
  {
    DEBUG_WARN("Unable to map USB scan code: %x\n", scancode);
    return 0;
  }
  return ps2;
}

static LG_ClipboardData spice_type_to_clipboard_type(const SpiceDataType type)
{
  switch(type)
  {
    case SPICE_DATA_TEXT: return LG_CLIPBOARD_DATA_TEXT; break;
    case SPICE_DATA_PNG : return LG_CLIPBOARD_DATA_PNG ; break;
    case SPICE_DATA_BMP : return LG_CLIPBOARD_DATA_BMP ; break;
    case SPICE_DATA_TIFF: return LG_CLIPBOARD_DATA_TIFF; break;
    case SPICE_DATA_JPEG: return LG_CLIPBOARD_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid spice data type");
      return LG_CLIPBOARD_DATA_NONE;
  }
}

static SpiceDataType clipboard_type_to_spice_type(const LG_ClipboardData type)
{
  switch(type)
  {
    case LG_CLIPBOARD_DATA_TEXT: return SPICE_DATA_TEXT; break;
    case LG_CLIPBOARD_DATA_PNG : return SPICE_DATA_PNG ; break;
    case LG_CLIPBOARD_DATA_BMP : return SPICE_DATA_BMP ; break;
    case LG_CLIPBOARD_DATA_TIFF: return SPICE_DATA_TIFF; break;
    case LG_CLIPBOARD_DATA_JPEG: return SPICE_DATA_JPEG; break;
    default:
      DEBUG_ERROR("invalid clipboard data type");
      return SPICE_DATA_NONE;
  }
}

void clipboardRelease()
{
  if (!params.clipboardToVM)
    return;

  spice_clipboard_release();
}

void clipboardNotify(const LG_ClipboardData type, size_t size)
{
  if (!params.clipboardToVM)
    return;

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    spice_clipboard_release();
    return;
  }

  g_state.cbType    = clipboard_type_to_spice_type(type);
  g_state.cbChunked = size > 0;
  g_state.cbXfer    = size;

  spice_clipboard_grab(g_state.cbType);

  if (size)
    spice_clipboard_data_start(g_state.cbType, size);
}

void clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size)
{
  if (!params.clipboardToVM)
    return;

  if (g_state.cbChunked && size > g_state.cbXfer)
  {
    DEBUG_ERROR("refusing to send more then cbXfer bytes for chunked xfer");
    size = g_state.cbXfer;
  }

  if (!g_state.cbChunked)
    spice_clipboard_data_start(g_state.cbType, size);

  spice_clipboard_data(g_state.cbType, data, (uint32_t)size);
  g_state.cbXfer -= size;
}

void clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!params.clipboardToLocal)
    return;

  struct CBRequest * cbr = (struct CBRequest *)malloc(sizeof(struct CBRequest()));

  cbr->type    = g_state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(g_state.cbRequestList, cbr);

  spice_clipboard_request(g_state.cbType);
}

void spiceClipboardNotice(const SpiceDataType type)
{
  if (!params.clipboardToLocal)
    return;

  if (!g_state.lgc || !g_state.lgc->notice)
    return;

  g_state.cbType = type;
  g_state.lgc->notice(clipboardRequest, spice_type_to_clipboard_type(type));
}

void spiceClipboardData(const SpiceDataType type, uint8_t * buffer, uint32_t size)
{
  if (!params.clipboardToLocal)
    return;

  if (type == SPICE_DATA_TEXT)
  {
    // dos2unix
    uint8_t  * p       = buffer;
    uint32_t   newSize = size;
    for(uint32_t i = 0; i < size; ++i)
    {
      uint8_t c = buffer[i];
      if (c == '\r')
      {
        --newSize;
        continue;
      }
      *p++ = c;
    }
    size = newSize;
  }

  struct CBRequest * cbr;
  if (ll_shift(g_state.cbRequestList, (void **)&cbr))
  {
    cbr->replyFn(cbr->opaque, spice_type_to_clipboard_type(type), buffer, size);
    free(cbr);
  }
}

void spiceClipboardRelease()
{
  if (!params.clipboardToLocal)
    return;

  if (g_state.lgc && g_state.lgc->release)
    g_state.lgc->release();
}

void spiceClipboardRequest(const SpiceDataType type)
{
  if (!params.clipboardToVM)
    return;

  if (g_state.lgc && g_state.lgc->request)
    g_state.lgc->request(spice_type_to_clipboard_type(type));
}

static void warpMouse(int x, int y)
{
  if (!g_cursor.inWindow)
    return;

  if (g_cursor.warpState == WARP_STATE_WIN_EXIT)
  {
    SDL_WarpMouseInWindow(g_state.window, x, y);
    g_cursor.warpState = WARP_STATE_OFF;
    return;
  }

  if (g_cursor.warpState == WARP_STATE_ON)
  {
    g_cursor.warpTo.x   = x;
    g_cursor.warpTo.y   = y;
    g_cursor.warpState = WARP_STATE_ACTIVE;
    SDL_WarpMouseInWindow(g_state.window, x, y);
  }
}

static bool isValidCursorLocation(int x, int y)
{
  const int displays = SDL_GetNumVideoDisplays();
  for(int i = 0; i < displays; ++i)
  {
    SDL_Rect r;
    SDL_GetDisplayBounds(i, &r);
    if ((x >= r.x && x < r.x + r.w) &&
        (y >= r.y && y < r.y + r.h))
      return true;
  }
  return false;
}

static void handleMouseMoveEvent(int ex, int ey)
{
  SDL_Point delta = {
    .x = ex - g_cursor.last.x,
    .y = ey - g_cursor.last.y
  };

  if (delta.x == 0 && delta.y == 0)
    return;

  g_cursor.last.x = ex;
  g_cursor.last.y = ey;

  if (g_cursor.warpState == WARP_STATE_ACTIVE &&
      ex == g_cursor.warpTo.x && ey == g_cursor.warpTo.y)
  {
    g_cursor.warpState = WARP_STATE_ON;
    return;
  }

  if (!g_cursor.inWindow || g_state.ignoreInput || !params.useSpiceInput)
    return;

  /* if we don't have the current cursor pos just send cursor movements */
  if (!g_cursor.guest.valid)
  {
    if (g_cursor.grab)
    {
      g_cursor.inView = true;
      spice_mouse_motion(delta.x, delta.y);
      if (ex < g_state.windowCX - 25 || ex > g_state.windowCX + 25 ||
          ey < g_state.windowCY - 25 || ey > g_state.windowCY + 25)
        warpMouse(g_state.windowCX, g_state.windowCY);
    }

    return;
  }

  const bool inView = !(
      ex <  g_state.dstRect.x                   ||
      ex >= g_state.dstRect.x + g_state.dstRect.w ||
      ey <  g_state.dstRect.y                   ||
      ey >= g_state.dstRect.y + g_state.dstRect.h);

  /* if the cursor is to move in/outside the display area */
  if (g_cursor.inView != inView)
  {
    g_cursor.inView = inView;

    if (inView)
    {
      /* cursor moved in */
      if (params.hideMouse)
        SDL_ShowCursor(SDL_DISABLE);

      g_cursor.redraw = true;
      g_cursor.draw   = true;

      if (g_cursor.warpState == WARP_STATE_OFF)
        g_cursor.warpState = WARP_STATE_ON;

      warpMouse(g_state.windowW / 2, g_state.windowH / 2);

      /* convert guest to local and calculate the delta */
      const int lx = ((g_cursor.guest.x + g_cursor.guest.hx) / g_cursor.scaleX) + g_state.dstRect.x;
      const int ly = ((g_cursor.guest.y + g_cursor.guest.hy) / g_cursor.scaleY) + g_state.dstRect.y;
      delta.x = ex - lx;
      delta.y = ey - ly;
    }
    else
    {
      /* cursor moved out */
      SDL_ShowCursor(SDL_ENABLE);
      g_cursor.redraw = true;
      if (params.useSpiceInput && !params.alwaysShowCursor)
        g_cursor.draw = false;
    }
  }
  else if (inView)
  {
    if (ex < g_state.windowCX - 25 || ex > g_state.windowCX + 25 ||
        ey < g_state.windowCY - 25 || ey > g_state.windowCY + 25)
      warpMouse(g_state.windowCX, g_state.windowCY);
  }

  if (!inView)
  {
    /* cursor outside of the bounds, don't do anything */
    return;
  }

  if (g_cursor.scale && params.scaleMouseInput && !g_cursor.grab)
  {
    g_cursor.accX += (float)delta.x * g_cursor.scaleX;
    g_cursor.accY += (float)delta.y * g_cursor.scaleY;
    delta.x = floor(g_cursor.accX);
    delta.y = floor(g_cursor.accY);
    g_cursor.accX -= delta.x;
    g_cursor.accY -= delta.y;
  }

  if (g_cursor.grab && g_cursor.sens != 0)
  {
    g_cursor.sensX += ((float)delta.x / 10.0f) * (g_cursor.sens + 10);
    g_cursor.sensY += ((float)delta.y / 10.0f) * (g_cursor.sens + 10);
    delta.x = floor(g_cursor.sensX);
    delta.y = floor(g_cursor.sensY);
    g_cursor.sensX -= delta.x;
    g_cursor.sensY -= delta.y;
  }

  if (!g_cursor.grab && g_cursor.warpState == WARP_STATE_ON)
  {
    const float fx = (float)(g_cursor.guest.x + g_cursor.guest.hx + delta.x) /
      g_cursor.scaleX;
    const float fy = (float)(g_cursor.guest.y + g_cursor.guest.hy + delta.y) /
      g_cursor.scaleY;
    const SDL_Point newPos =
    {
      .x = fx < 0.0f ? floor(fx) : (fx >= g_state.dstRect.w ? ceil(fx) : round(fx)),
      .y = fy < 0.0f ? floor(fy) : (fy >= g_state.dstRect.h ? ceil(fy) : round(fy))
    };

    /* check if the movement would exit the window */
    if (newPos.x < 0 || newPos.x >= g_state.dstRect.w ||
        newPos.y < 0 || newPos.y >= g_state.dstRect.h)
    {
      const int nx = g_state.windowPos.x + g_state.border.x +
        g_state.dstRect.x + newPos.x;
      const int ny = g_state.windowPos.y + g_state.border.y +
        g_state.dstRect.y + newPos.y;

      if (isValidCursorLocation(nx, ny))
      {
        /* put the mouse where it should be and disable warp */
        g_cursor.warpState = WARP_STATE_WIN_EXIT;
        warpMouse(
          g_state.dstRect.x + newPos.x,
          g_state.dstRect.y + newPos.y
        );
        SDL_ShowCursor(SDL_ENABLE);
        return;
      }
    }
  }

  /* send the movement to the guest */
  if (!spice_mouse_motion(delta.x, delta.y))
    DEBUG_ERROR("failed to send mouse motion message");
}

static void handleResizeEvent(unsigned int w, unsigned int h)
{
  if (g_state.windowW == w && g_state.windowH == h)
    return;

  SDL_GetWindowBordersSize(g_state.window,
    &g_state.border.y,
    &g_state.border.x,
    &g_state.border.h,
    &g_state.border.w
  );

  g_state.windowW  = w;
  g_state.windowH  = h;
  g_state.windowCX = w / 2;
  g_state.windowCY = h / 2;
  updatePositionInfo();
}

static void handleWindowLeave()
{
  g_cursor.inWindow = false;

  if (!params.useSpiceInput)
    return;

  if (!params.alwaysShowCursor)
    g_cursor.draw = false;

  g_cursor.inView = false;
  g_cursor.redraw = true;
}

static void handleWindowEnter()
{
  g_cursor.inWindow = true;

  if (g_cursor.warpState == WARP_STATE_OFF)
    g_cursor.warpState = WARP_STATE_ON;

  if (!params.useSpiceInput)
    return;

  if (!g_cursor.guest.valid)
    return;

  g_cursor.draw   = true;
  g_cursor.redraw = true;
}

// only called for X11
static void keyboardGrab()
{
  if (!params.grabKeyboardOnFocus)
    return;

  // grab the keyboard so we can intercept WM keys
  XGrabKeyboard(
    g_state.wminfo.info.x11.display,
    g_state.wminfo.info.x11.window,
    true,
    GrabModeAsync,
    GrabModeAsync,
    CurrentTime
  );
}

// only called for X11
static void keyboardUngrab()
{
  if (!params.grabKeyboardOnFocus)
    return;

  // ungrab the keyboard
  XUngrabKeyboard(
    g_state.wminfo.info.x11.display,
    CurrentTime
  );
}

int eventFilter(void * userdata, SDL_Event * event)
{
  switch(event->type)
  {
    case SDL_QUIT:
    {
      if (!params.ignoreQuit)
      {
        DEBUG_INFO("Quit event received, exiting...");
        g_state.state = APP_STATE_SHUTDOWN;
      }
      return 0;
    }

    case SDL_WINDOWEVENT:
    {
      switch(event->window.event)
      {
        case SDL_WINDOWEVENT_ENTER:
          if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
            handleWindowEnter();
          break;

        case SDL_WINDOWEVENT_LEAVE:
          if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
            handleWindowLeave();
          break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
          if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
            handleResizeEvent(event->window.data1, event->window.data2);
          break;

        case SDL_WINDOWEVENT_MOVED:
          if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
          {
            g_state.windowPos.x = event->window.data1;
            g_state.windowPos.y = event->window.data2;
          }
          break;

        // allow a window close event to close the application even if ignoreQuit is set
        case SDL_WINDOWEVENT_CLOSE:
          g_state.state = APP_STATE_SHUTDOWN;
          break;
      }
      return 0;
    }

    case SDL_SYSWMEVENT:
    {
      // When the window manager forces the window size after calling SDL_SetWindowSize, SDL
      // ignores this update and caches the incorrect window size. As such all related details
      // are incorect including mouse movement information as it clips to the old window size.
      if (g_state.wminfo.subsystem == SDL_SYSWM_X11)
      {
        XEvent xe = event->syswm.msg->msg.x11.event;
        switch(xe.type)
        {
          case ConfigureNotify:
          {
            /* the window may have been re-parented so we need to translate to
             * ensure we get the screen top left position of the window */
            Window child;
            XTranslateCoordinates(g_state.wminfo.info.x11.display,
                g_state.wminfo.info.x11.window,
                DefaultRootWindow(g_state.wminfo.info.x11.display),
                0, 0, &g_state.windowPos.x, &g_state.windowPos.y,
                &child);

            handleResizeEvent(xe.xconfigure.width, xe.xconfigure.height);
            break;
          }

          case MotionNotify:
            handleMouseMoveEvent(xe.xmotion.x, xe.xmotion.y);
            break;

          case EnterNotify:
            if (xe.xcrossing.mode != NotifyNormal)
              break;

            g_cursor.last.x = xe.xcrossing.x;
            g_cursor.last.y = xe.xcrossing.y;
            handleWindowEnter();
            break;

          case LeaveNotify:
            if (xe.xcrossing.mode != NotifyNormal)
              break;

            g_cursor.last.x = xe.xcrossing.x;
            g_cursor.last.y = xe.xcrossing.y;
            handleWindowLeave();
            break;

          case FocusIn:
            if (!params.useSpiceInput)
              break;

            if (xe.xfocus.mode == NotifyNormal ||
                xe.xfocus.mode == NotifyUngrab)
              keyboardGrab();
            break;

          case FocusOut:
            if (!params.useSpiceInput)
              break;

            if (xe.xfocus.mode == NotifyNormal ||
                xe.xfocus.mode == NotifyWhileGrabbed)
              keyboardUngrab();
            break;
        }
      }

      if (params.useSpiceClipboard && g_state.lgc && g_state.lgc->wmevent)
        g_state.lgc->wmevent(event->syswm.msg);
      return 0;
    }

    case SDL_MOUSEMOTION:
      if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
        handleMouseMoveEvent(event->motion.x, event->motion.y);
      break;

    case SDL_KEYDOWN:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      if (sc == params.escapeKey)
      {
        g_state.escapeActive = true;
        g_state.escapeAction = -1;
        break;
      }

      if (g_state.escapeActive)
      {
        g_state.escapeAction = sc;
        break;
      }

      if (g_state.ignoreInput || !params.useSpiceInput)
        break;

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (!g_state.keyDown[sc])
      {
        if (spice_key_down(scancode))
          g_state.keyDown[sc] = true;
        else
        {
          DEBUG_ERROR("SDL_KEYDOWN: failed to send message");
          break;
        }
      }
      break;
    }

    case SDL_KEYUP:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      if (g_state.escapeActive)
      {
        if (g_state.escapeAction == -1)
        {
          if (params.useSpiceInput)
          {
            g_cursor.grab = !g_cursor.grab;

            if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
              SDL_SetWindowGrab(g_state.window, g_cursor.grab);
            else
            {
              if (g_cursor.grab)
              {
                XGrabPointer(
                    g_state.wminfo.info.x11.display,
                    g_state.wminfo.info.x11.window,
                    true,
                    None,
                    GrabModeAsync,
                    GrabModeAsync,
                    g_state.wminfo.info.x11.window,
                    None,
                    CurrentTime);

                XGrabKeyboard(
                  g_state.wminfo.info.x11.display,
                  g_state.wminfo.info.x11.window,
                  true,
                  GrabModeAsync,
                  GrabModeAsync,
                  CurrentTime);
              }
              else
              {
                XUngrabKeyboard(g_state.wminfo.info.x11.display, CurrentTime);
                XUngrabPointer(g_state.wminfo.info.x11.display,  CurrentTime);
              }
            }

            app_alert(
              g_cursor.grab ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
              g_cursor.grab ? "Capture Enabled" : "Capture Disabled"
            );
          }
        }
        else
        {
          KeybindHandle handle = g_state.bindings[sc];
          if (handle)
            handle->callback(sc, handle->opaque);
        }

        if (sc == params.escapeKey)
          g_state.escapeActive = false;
      }

      if (g_state.ignoreInput || !params.useSpiceInput)
        break;

      // avoid sending key up events when we didn't send a down
      if (!g_state.keyDown[sc])
        break;

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (spice_key_up(scancode))
        g_state.keyDown[sc] = false;
      else
      {
        DEBUG_ERROR("SDL_KEYUP: failed to send message");
        break;
      }
      break;
    }

    case SDL_MOUSEWHEEL:
      if (g_state.ignoreInput || !params.useSpiceInput || !g_cursor.inView)
        break;

      if (
        !spice_mouse_press  (event->wheel.y == 1 ? 4 : 5) ||
        !spice_mouse_release(event->wheel.y == 1 ? 4 : 5)
        )
      {
        DEBUG_ERROR("SDL_MOUSEWHEEL: failed to send messages");
        break;
      }
      break;

    case SDL_MOUSEBUTTONDOWN:
    {
      if (g_state.ignoreInput || !params.useSpiceInput || !g_cursor.inView)
        break;

      int button = event->button.button;
      if (button > 3)
        button += 2;

      if (!spice_mouse_press(button))
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONDOWN: failed to send message");
        break;
      }
      break;
    }

    case SDL_MOUSEBUTTONUP:
    {
      if (g_state.ignoreInput || !params.useSpiceInput || !g_cursor.inView)
        break;

      int button = event->button.button;
      if (button > 3)
        button += 2;

      if (!spice_mouse_release(button))
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONUP: failed to send message");
        break;
      }
      break;
    }
  }

  // consume all events
  return 0;
}

void int_handler(int signal)
{
  switch(signal)
  {
    case SIGINT:
    case SIGTERM:
      DEBUG_INFO("Caught signal, shutting down...");
      g_state.state = APP_STATE_SHUTDOWN;
      break;
  }
}

static bool try_renderer(const int index, const LG_RendererParams lgrParams, Uint32 * sdlFlags)
{
  const LG_Renderer *r = LG_Renderers[index];

  if (!IS_LG_RENDERER_VALID(r))
  {
    DEBUG_ERROR("FIXME: Renderer %d is invalid, skipping", index);
    return false;
  }

  // create the renderer
  g_state.lgrData = NULL;
  if (!r->create(&g_state.lgrData, lgrParams))
    return false;

  // initialize the renderer
  if (!r->initialize(g_state.lgrData, sdlFlags))
  {
    r->deinitialize(g_state.lgrData);
    return false;
  }

  DEBUG_INFO("Using Renderer: %s", r->get_name());
  return true;
}

static void toggle_fullscreen(SDL_Scancode key, void * opaque)
{
  SDL_SetWindowFullscreen(g_state.window, params.fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  params.fullscreen = !params.fullscreen;
}

static void toggle_video(SDL_Scancode key, void * opaque)
{
  g_state.stopVideo = !g_state.stopVideo;
  app_alert(
    LG_ALERT_INFO,
    g_state.stopVideo ? "Video Stream Disabled" : "Video Stream Enabled"
  );

  if (!g_state.stopVideo)
  {
    if (t_frame)
    {
      lgJoinThread(t_frame, NULL);
      t_frame = NULL;
    }

    if (!lgCreateThread("frameThread", frameThread, NULL, &t_frame))
      DEBUG_ERROR("frame create thread failed");
  }
}

static void toggle_input(SDL_Scancode key, void * opaque)
{
  g_state.ignoreInput = !g_state.ignoreInput;
  app_alert(
    LG_ALERT_INFO,
    g_state.ignoreInput ? "Input Disabled" : "Input Enabled"
  );
}

static void quit(SDL_Scancode key, void * opaque)
{
  g_state.state = APP_STATE_SHUTDOWN;
}

static void mouse_sens_inc(SDL_Scancode key, void * opaque)
{
  char * msg;
  if (g_cursor.sens < 9)
    ++g_cursor.sens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", g_cursor.sens > 0 ? "+" : "", g_cursor.sens);
  app_alert(
    LG_ALERT_INFO,
    msg
  );
  free(msg);
}

static void mouse_sens_dec(SDL_Scancode key, void * opaque)
{
  char * msg;

  if (g_cursor.sens > -9)
    --g_cursor.sens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", g_cursor.sens > 0 ? "+" : "", g_cursor.sens);
  app_alert(
    LG_ALERT_INFO,
    msg
  );
  free(msg);
}

static void ctrl_alt_fn(SDL_Scancode key, void * opaque)
{
  const uint32_t ctrl = mapScancode(SDL_SCANCODE_LCTRL);
  const uint32_t alt  = mapScancode(SDL_SCANCODE_LALT );
  const uint32_t fn   = mapScancode(key);

  spice_key_down(ctrl);
  spice_key_down(alt );
  spice_key_down(fn  );

  spice_key_up(ctrl);
  spice_key_up(alt );
  spice_key_up(fn  );
}

static void register_key_binds()
{
  g_state.kbFS           = app_register_keybind(SDL_SCANCODE_F     , toggle_fullscreen, NULL);
  g_state.kbVideo        = app_register_keybind(SDL_SCANCODE_V     , toggle_video     , NULL);
  g_state.kbInput        = app_register_keybind(SDL_SCANCODE_I     , toggle_input     , NULL);
  g_state.kbQuit         = app_register_keybind(SDL_SCANCODE_Q     , quit             , NULL);
  g_state.kbMouseSensInc = app_register_keybind(SDL_SCANCODE_INSERT, mouse_sens_inc   , NULL);
  g_state.kbMouseSensDec = app_register_keybind(SDL_SCANCODE_DELETE, mouse_sens_dec   , NULL);

  g_state.kbCtrlAltFn[0 ] = app_register_keybind(SDL_SCANCODE_F1 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[1 ] = app_register_keybind(SDL_SCANCODE_F2 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[2 ] = app_register_keybind(SDL_SCANCODE_F3 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[3 ] = app_register_keybind(SDL_SCANCODE_F4 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[4 ] = app_register_keybind(SDL_SCANCODE_F5 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[5 ] = app_register_keybind(SDL_SCANCODE_F6 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[6 ] = app_register_keybind(SDL_SCANCODE_F7 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[7 ] = app_register_keybind(SDL_SCANCODE_F8 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[8 ] = app_register_keybind(SDL_SCANCODE_F9 , ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[9 ] = app_register_keybind(SDL_SCANCODE_F10, ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[10] = app_register_keybind(SDL_SCANCODE_F11, ctrl_alt_fn, NULL);
  g_state.kbCtrlAltFn[11] = app_register_keybind(SDL_SCANCODE_F12, ctrl_alt_fn, NULL);
}

static void release_key_binds()
{
  app_release_keybind(&g_state.kbFS   );
  app_release_keybind(&g_state.kbVideo);
  app_release_keybind(&g_state.kbInput);
  app_release_keybind(&g_state.kbQuit );
  app_release_keybind(&g_state.kbMouseSensInc);
  app_release_keybind(&g_state.kbMouseSensDec);
  for(int i = 0; i < 12; ++i)
    app_release_keybind(&g_state.kbCtrlAltFn[i]);
}

static void initSDLCursor()
{
  const uint8_t data[4] = {0xf, 0x9, 0x9, 0xf};
  const uint8_t mask[4] = {0xf, 0xf, 0xf, 0xf};
  cursor = SDL_CreateCursor(data, mask, 8, 4, 4, 0);
  SDL_SetCursor(cursor);
}

static int lg_run()
{
  memset(&g_state, 0, sizeof(g_state));
  lgInit();

  g_cursor.sens = params.mouseSens;
       if (g_cursor.sens < -9) g_cursor.sens = -9;
  else if (g_cursor.sens >  9) g_cursor.sens =  9;

  char* XDG_SESSION_TYPE = getenv("XDG_SESSION_TYPE");

  if (XDG_SESSION_TYPE == NULL)
    XDG_SESSION_TYPE = "unspecified";

  if (strcmp(XDG_SESSION_TYPE, "wayland") == 0)
  {
     DEBUG_INFO("Wayland detected");
     if (getenv("SDL_VIDEODRIVER") == NULL)
     {
       int err = setenv("SDL_VIDEODRIVER", "wayland", 1);
       if (err < 0)
       {
         DEBUG_ERROR("Unable to set the env variable SDL_VIDEODRIVER: %d", err);
         return -1;
       }
       DEBUG_INFO("SDL_VIDEODRIVER has been set to wayland");
     }
  }

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return -1;
  }

  // override SDL's SIGINIT handler so that we can tell the difference between
  // SIGINT and the user sending a close event, such as ALT+F4
  signal(SIGINT , int_handler);
  signal(SIGTERM, int_handler);

  // try map the shared memory
  if (!ivshmemOpen(&g_state.shm))
  {
    DEBUG_ERROR("Failed to map memory");
    return -1;
  }

  // try to connect to the spice server
  if (params.useSpiceInput || params.useSpiceClipboard)
  {
    spice_set_clipboard_cb(
        spiceClipboardNotice,
        spiceClipboardData,
        spiceClipboardRelease,
        spiceClipboardRequest);

    if (!spice_connect(params.spiceHost, params.spicePort, ""))
    {
      DEBUG_ERROR("Failed to connect to spice server");
      return -1;
    }

    while(g_state.state != APP_STATE_SHUTDOWN && !spice_ready())
      if (!spice_process(1000))
      {
        g_state.state = APP_STATE_SHUTDOWN;
        DEBUG_ERROR("Failed to process spice messages");
        return -1;
      }

    spice_mouse_mode(true);
    if (!lgCreateThread("spiceThread", spiceThread, NULL, &t_spice))
    {
      DEBUG_ERROR("spice create thread failed");
      return -1;
    }
  }

  // select and init a renderer
  LG_RendererParams lgrParams;
  lgrParams.showFPS     = params.showFPS;
  lgrParams.quickSplash = params.quickSplash;
  Uint32 sdlFlags;

  if (params.forceRenderer)
  {
    DEBUG_INFO("Trying forced renderer");
    sdlFlags = 0;
    if (!try_renderer(params.forceRendererIndex, lgrParams, &sdlFlags))
    {
      DEBUG_ERROR("Forced renderer failed to iniailize");
      return -1;
    }
    g_state.lgr = LG_Renderers[params.forceRendererIndex];
  }
  else
  {
    // probe for a a suitable renderer
    for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    {
      sdlFlags = 0;
      if (try_renderer(i, lgrParams, &sdlFlags))
      {
        g_state.lgr = LG_Renderers[i];
        break;
      }
    }
  }

  if (!g_state.lgr)
  {
    DEBUG_INFO("Unable to find a suitable renderer");
    return -1;
  }

  // all our ducks are in a line, create the window
  g_state.window = SDL_CreateWindow(
    params.windowTitle,
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0) |
      (params.maximize    ? SDL_WINDOW_MAXIMIZED  : 0) |
      sdlFlags
    )
  );

  if (g_state.window == NULL)
  {
    DEBUG_ERROR("Could not create an SDL window: %s\n", SDL_GetError());
    return 1;
  }

  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
      params.minimizeOnFocusLoss ? "1" : "0");

  if (params.fullscreen)
    SDL_SetWindowFullscreen(g_state.window, SDL_WINDOW_FULLSCREEN_DESKTOP);

  if (!params.noScreensaver)
  {
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_EnableScreenSaver();
  }

  if (!params.center)
    SDL_SetWindowPosition(g_state.window, params.x, params.y);

  // ensure the initial window size is stored in the state
  SDL_GetWindowSize(g_state.window, &g_state.windowW, &g_state.windowH);

  // ensure renderer viewport is aware of the current window size
  updatePositionInfo();

  if (params.fpsMin <= 0)
  {
    // default 30 fps
    g_state.frameTime = 1000000000ULL / 30ULL;
  }
  else
  {
    DEBUG_INFO("Using the FPS minimum from args: %d", params.fpsMin);
    g_state.frameTime = 1000000000ULL / (unsigned long long)params.fpsMin;
  }

  register_key_binds();

  // set the compositor hint to bypass for low latency
  SDL_VERSION(&g_state.wminfo.version);
  if (SDL_GetWindowWMInfo(g_state.window, &g_state.wminfo))
  {
    if (g_state.wminfo.subsystem == SDL_SYSWM_X11)
    {
      // enable X11 events to work around SDL2 bugs
      SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

#if SDL_VIDEO_DRIVER_X11_XINPUT2
      // SDL2 bug, using xinput2 disables all motion notify events
      // we really don't care about touch, so turn it off and go back
      // to the default behaiovur.
      XIEventMask xinputmask =
      {
        .deviceid = XIAllMasterDevices,
        .mask     = 0,
        .mask_len = 0
      };

      XISelectEvents(
        g_state.wminfo.info.x11.display,
        g_state.wminfo.info.x11.window,
        &xinputmask,
        1
      );
#endif

      Atom NETWM_BYPASS_COMPOSITOR = XInternAtom(
        g_state.wminfo.info.x11.display,
        "NETWM_BYPASS_COMPOSITOR",
        False);

      unsigned long value = 1;
      XChangeProperty(
        g_state.wminfo.info.x11.display,
        g_state.wminfo.info.x11.window,
        NETWM_BYPASS_COMPOSITOR,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (unsigned char *)&value,
        1
      );

      g_state.lgc = LG_Clipboards[0];
    }
  } else {
    DEBUG_ERROR("Could not get SDL window information %s", SDL_GetError());
    return -1;
  }

  if (g_state.lgc)
  {
    DEBUG_INFO("Using Clipboard: %s", g_state.lgc->getName());
    if (!g_state.lgc->init(&g_state.wminfo, clipboardRelease, clipboardNotify, clipboardData))
    {
      DEBUG_WARN("Failed to initialize the clipboard interface, continuing anyway");
      g_state.lgc = NULL;
    }

    g_state.cbRequestList = ll_new();
  }

  initSDLCursor();
  if (params.hideMouse)
    SDL_ShowCursor(SDL_DISABLE);

  if (params.captureOnStart)
  {
    g_cursor.grab = true;
    if (g_state.wminfo.subsystem != SDL_SYSWM_X11)
      SDL_SetWindowGrab(g_state.window, true);
  }

  // setup the startup condition
  if (!(e_startup = lgCreateEvent(false, 0)))
  {
    DEBUG_ERROR("failed to create the startup event");
    return -1;
  }

  // setup the new frame event
  if (!(e_frame = lgCreateEvent(true, 0)))
  {
    DEBUG_ERROR("failed to create the frame event");
    return -1;
  }

  // start the renderThread so we don't just display junk
  if (!lgCreateThread("renderThread", renderThread, NULL, &t_render))
  {
    DEBUG_ERROR("render create thread failed");
    return -1;
  }

  // ensure mouse acceleration is identical in server mode
  SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);
  SDL_SetEventFilter(eventFilter, NULL);

  // wait for startup to complete so that any error messages below are output at
  // the end of the output
  lgWaitEvent(e_startup, TIMEOUT_INFINITE);

  LGMP_STATUS status;

  while(g_state.state == APP_STATE_RUNNING)
  {
    if ((status = lgmpClientInit(g_state.shm.mem, g_state.shm.size, &g_state.lgmp)) == LGMP_OK)
      break;

    DEBUG_ERROR("lgmpClientInit Failed: %s", lgmpStatusString(status));
    return -1;
  }

  /* this short timeout is to allow the LGMP host to update the timestamp before
   * we start checking for a valid session */
  SDL_WaitEventTimeout(NULL, 200);

  uint32_t udataSize;
  KVMFR *udata;
  int waitCount = 0;

restart:
  while(g_state.state == APP_STATE_RUNNING)
  {
    if ((status = lgmpClientSessionInit(g_state.lgmp, &udataSize, (uint8_t **)&udata)) == LGMP_OK)
      break;

    if (status != LGMP_ERR_INVALID_SESSION && status != LGMP_ERR_INVALID_MAGIC)
    {
      DEBUG_ERROR("lgmpClientSessionInit Failed: %s", lgmpStatusString(status));
      return -1;
    }

    if (waitCount++ == 0)
    {
      DEBUG_BREAK();
      DEBUG_INFO("The host application seems to not be running");
      DEBUG_INFO("Waiting for the host application to start...");
    }

    if (waitCount == 30)
    {
      DEBUG_BREAK();
      DEBUG_INFO("Please check the host application is running and is the correct version");
      DEBUG_INFO("Check the host log in your guest at %%TEMP%%\\looking-glass-host.txt");
      DEBUG_INFO("Continuing to wait...");
    }

    SDL_WaitEventTimeout(NULL, 1000);
  }

  if (g_state.state != APP_STATE_RUNNING)
    return -1;

  // dont show warnings again after the first startup
  waitCount = 100;

  const bool magicMatches = memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) == 0;
  if (udataSize != sizeof(KVMFR) || !magicMatches || udata->version != KVMFR_VERSION)
  {
    DEBUG_BREAK();
    DEBUG_ERROR("The host application is not compatible with this client");
    DEBUG_ERROR("This is not a Looking Glass error, do not report this");
    DEBUG_ERROR("Please install the matching host application for this client");

    if (magicMatches)
    {
      DEBUG_ERROR("Expected KVMFR version %d, got %d", KVMFR_VERSION, udata->version);
      if (udata->version >= 2)
        DEBUG_ERROR("Host version: %s", udata->hostver);
    }
    else
      DEBUG_ERROR("Invalid KVMFR magic");

    DEBUG_BREAK();
    return -1;
  }

  DEBUG_INFO("Host ready, reported version: %s", udata->hostver);
  DEBUG_INFO("Starting session");

  if (!lgCreateThread("cursorThread", cursorThread, NULL, &t_cursor))
  {
    DEBUG_ERROR("cursor create thread failed");
    return 1;
  }

  if (!lgCreateThread("frameThread", frameThread, NULL, &t_frame))
  {
    DEBUG_ERROR("frame create thread failed");
    return -1;
  }

  while(g_state.state == APP_STATE_RUNNING)
  {
    if (!lgmpClientSessionValid(g_state.lgmp))
    {
      g_state.state = APP_STATE_RESTART;
      break;
    }
    SDL_WaitEventTimeout(NULL, 100);
  }

  if (g_state.state == APP_STATE_RESTART)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(e_frame);
    lgJoinThread(t_frame , NULL);
    lgJoinThread(t_cursor, NULL);
    t_frame  = NULL;
    t_cursor = NULL;

    lgInit();

    g_state.lgr->on_restart(g_state.lgrData);

    DEBUG_INFO("Waiting for the host to restart...");
    goto restart;
  }

  return 0;
}

static void lg_shutdown()
{
  g_state.state = APP_STATE_SHUTDOWN;
  if (t_render)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(e_frame);
    lgJoinThread(t_render, NULL);
  }

  lgmpClientFree(&g_state.lgmp);

  if (e_frame)
  {
    lgFreeEvent(e_frame);
    e_frame = NULL;
  }

  if (e_startup)
  {
    lgFreeEvent(e_startup);
    e_startup = NULL;
  }

  // if spice is still connected send key up events for any pressed keys
  if (params.useSpiceInput && spice_ready())
  {
    for(int i = 0; i < SDL_NUM_SCANCODES; ++i)
      if (g_state.keyDown[i])
      {
        uint32_t scancode = mapScancode(i);
        if (scancode == 0)
          continue;
        g_state.keyDown[i] = false;
        spice_key_up(scancode);
      }

    spice_disconnect();
    if (t_spice)
      lgJoinThread(t_spice, NULL);
  }

  if (g_state.lgc)
  {
    g_state.lgc->free();

    struct CBRequest *cbr;
    while(ll_shift(g_state.cbRequestList, (void **)&cbr))
      free(cbr);
    ll_free(g_state.cbRequestList);
  }

  if (g_state.window)
    SDL_DestroyWindow(g_state.window);

  if (cursor)
    SDL_FreeCursor(cursor);

  ivshmemClose(&g_state.shm);

  release_key_binds();
  SDL_Quit();
}

int main(int argc, char * argv[])
{
  if (getuid() == 0)
  {
    DEBUG_ERROR("Do not run looking glass as root!");
    return -1;
  }

  DEBUG_INFO("Looking Glass (%s)", BUILD_VERSION);
  DEBUG_INFO("Locking Method: " LG_LOCK_MODE);

  if (!installCrashHandler("/proc/self/exe"))
    DEBUG_WARN("Failed to install the crash handler");

  config_init();
  ivshmemOptionsInit();

  // early renderer setup for option registration
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    LG_Renderers[i]->setup();

  if (!config_load(argc, argv))
    return -1;

  if (params.useSpiceInput && params.grabKeyboard)
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");

  const int ret = lg_run();
  lg_shutdown();

  config_free();
  return ret;

}
