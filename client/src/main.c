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
#include <linux/input.h>

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

#include "app.h"
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

static Uint32 e_SDLEvent; // our SDL event

enum
{
  LG_EVENT_ALIGN_TO_GUEST
};

struct AppState g_state;
struct CursorState g_cursor;

// this structure is initialized in config.c
struct AppParams params = { 0 };

static void setGrab(bool enable);
static void setGrabQuiet(bool enable);
static void setCursorInView(bool enable);

static void lgInit(void)
{
  g_state.state         = APP_STATE_RUNNING;
  g_state.formatValid   = false;
  g_state.resizeDone    = true;

  if (g_cursor.grab)
    setGrab(false);

  g_cursor.useScale      = false;
  g_cursor.scale.x       = 1.0;
  g_cursor.scale.y       = 1.0;
  g_cursor.draw          = false;
  g_cursor.inView        = false;
  g_cursor.guest.valid   = false;

  // if spice is not in use, hide the local cursor
  if (!app_inputEnabled() && params.hideMouse)
    SDL_ShowCursor(SDL_DISABLE);
  else
    SDL_ShowCursor(SDL_ENABLE);
}

bool app_getProp(LG_DSProperty prop, void * ret)
{
  return g_state.ds->getProp(prop, ret);
}

SDL_Window * app_getWindow(void)
{
  return g_state.window;
}

bool app_inputEnabled(void)
{
  return params.useSpiceInput && !g_state.ignoreInput &&
    ((g_cursor.grab && params.captureInputOnly) || !params.captureInputOnly);
}

bool app_cursorInWindow(void)
{
  return g_cursor.inWindow;
}

bool app_cursorIsGrabbed(void)
{
  return g_cursor.grab;
}

bool app_cursorWantsRaw(void)
{
  return params.rawMouse;
}

void app_updateCursorPos(double x, double y)
{
  g_cursor.pos.x = x;
  g_cursor.pos.y = y;
  g_cursor.valid = true;
}

void app_handleFocusEvent(bool focused)
{
  g_state.focused = focused;
  if (!app_inputEnabled())
    return;

  if (!focused)
  {
    setGrabQuiet(false);
    setCursorInView(false);
  }

  g_cursor.realign = true;
  g_state.ds->realignPointer();
}

void app_handleCloseEvent(void)
{
  if (!params.ignoreQuit || !g_cursor.inView)
    g_state.state = APP_STATE_SHUTDOWN;
}

static void alignToGuest(void)
{
  if (SDL_HasEvent(e_SDLEvent))
    return;

  SDL_Event event;
  SDL_memset(&event, 0, sizeof(event));
  event.type      = e_SDLEvent;
  event.user.code = LG_EVENT_ALIGN_TO_GUEST;

  SDL_PushEvent(&event);
}

static void updatePositionInfo(void)
{
  if (!g_state.haveSrcSize)
    goto done;

  float srcW;
  float srcH;
  switch(params.winRotate)
  {
    case LG_ROTATE_0:
    case LG_ROTATE_180:
      srcW = g_state.srcSize.x;
      srcH = g_state.srcSize.y;
      break;

    case LG_ROTATE_90:
    case LG_ROTATE_270:
      srcW = g_state.srcSize.y;
      srcH = g_state.srcSize.x;
      break;

    default:
      assert(!"unreachable");
  }

  if (params.keepAspect)
  {
    const float srcAspect = srcH / srcW;
    const float wndAspect = (float)g_state.windowH / (float)g_state.windowW;
    bool force = true;

    if (params.dontUpscale &&
        srcW <= g_state.windowW &&
        srcH <= g_state.windowH)
    {
      force = false;
      g_state.dstRect.w = srcW;
      g_state.dstRect.h = srcH;
      g_state.dstRect.x = g_state.windowCX - srcW / 2;
      g_state.dstRect.y = g_state.windowCY - srcH / 2;
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

  g_cursor.useScale = (
      srcH       != g_state.dstRect.h ||
      srcW       != g_state.dstRect.w ||
      g_cursor.guest.dpiScale != 100);

  g_cursor.scale.x  = (float)srcW / (float)g_state.dstRect.w;
  g_cursor.scale.y  = (float)srcH / (float)g_state.dstRect.h;
  g_cursor.dpiScale = g_cursor.guest.dpiScale / 100.0f;

  if (!g_state.posInfoValid)
  {
    g_state.posInfoValid = true;
    g_state.ds->realignPointer();
  }

done:
  atomic_fetch_add(&g_state.lgrResize, 1);
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

    int resize = atomic_load(&g_state.lgrResize);
    if (resize)
    {
      if (g_state.lgr)
        g_state.lgr->on_resize(g_state.lgrData, g_state.windowW, g_state.windowH,
            g_state.dstRect, params.winRotate);
      atomic_compare_exchange_weak(&g_state.lgrResize, &resize, 0);
    }

    if (!g_state.lgr->render(g_state.lgrData, g_state.window, params.winRotate))
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
        if (g_cursor.redraw && g_cursor.guest.valid)
        {
          g_cursor.redraw = false;
          g_state.lgr->on_mouse_event
          (
            g_state.lgrData,
            g_cursor.guest.visible && (g_cursor.draw || !params.useSpiceInput),
            g_cursor.guest.x,
            g_cursor.guest.y
          );

          lgSignalEvent(e_frame);
        }

        const struct timespec req =
        {
          .tv_sec  = 0,
          .tv_nsec = params.cursorPollInterval * 1000L
        };

        struct timespec rem;
        while(nanosleep(&req, &rem) < 0)
          if (errno != -EINTR)
          {
            DEBUG_ERROR("nanosleep failed");
            break;
          }

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
      bool valid = g_cursor.guest.valid;
      g_cursor.guest.x     = cursor->x;
      g_cursor.guest.y     = cursor->y;
      g_cursor.guest.valid = true;

      // if the state just became valid
      if (valid != true && app_inputEnabled())
        alignToGuest();
    }

    lgmpClientMessageDone(queue);
    g_cursor.redraw = false;

    g_state.lgr->on_mouse_event
    (
      g_state.lgrData,
      g_cursor.guest.visible && (g_cursor.draw || !params.useSpiceInput),
      g_cursor.guest.x,
      g_cursor.guest.y
    );

    if (params.mouseRedraw && g_cursor.guest.visible)
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
  size_t            dataSize  = 0;
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
        const struct timespec req =
        {
          .tv_sec  = 0,
          .tv_nsec = params.framePollInterval * 1000L
        };

        struct timespec rem;
        while(nanosleep(&req, &rem) < 0)
          if (errno != -EINTR)
          {
            DEBUG_ERROR("nanosleep failed");
            break;
          }

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

    if (!g_state.formatValid || frame->formatVer != formatVer)
    {
      // setup the renderer format with the frame format details
      lgrFormat.type   = frame->type;
      lgrFormat.width  = frame->width;
      lgrFormat.height = frame->height;
      lgrFormat.stride = frame->stride;
      lgrFormat.pitch  = frame->pitch;

      switch(frame->rotation)
      {
        case FRAME_ROT_0  : lgrFormat.rotate = LG_ROTATE_0  ; break;
        case FRAME_ROT_90 : lgrFormat.rotate = LG_ROTATE_90 ; break;
        case FRAME_ROT_180: lgrFormat.rotate = LG_ROTATE_180; break;
        case FRAME_ROT_270: lgrFormat.rotate = LG_ROTATE_270; break;
      }
      g_state.rotate = lgrFormat.rotate;

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

      g_state.formatValid = true;
      formatVer = frame->formatVer;

      DEBUG_INFO("Format: %s %ux%u stride:%u pitch:%u rotation:%d",
          FrameTypeStr[frame->type],
          frame->width, frame->height,
          frame->stride, frame->pitch,
          frame->rotation);

      if (!g_state.lgr->on_frame_format(g_state.lgrData, lgrFormat, useDMA))
      {
        DEBUG_ERROR("renderer failed to configure format");
        g_state.state = APP_STATE_SHUTDOWN;
        break;
      }

      g_state.srcSize.x = lgrFormat.width;
      g_state.srcSize.y = lgrFormat.height;
      g_state.haveSrcSize = true;
      if (params.autoResize)
        SDL_SetWindowSize(g_state.window, lgrFormat.width, lgrFormat.height);

      g_cursor.guest.dpiScale = frame->mouseScalePercent;
      updatePositionInfo();
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

void app_clipboardRelease(void)
{
  if (!params.clipboardToVM)
    return;

  spice_clipboard_release();
}

void app_clipboardNotify(const LG_ClipboardData type, size_t size)
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

void app_clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size)
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

void app_clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!params.clipboardToLocal)
    return;

  struct CBRequest * cbr = (struct CBRequest *)malloc(sizeof(struct CBRequest));

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

  if (!g_state.cbAvailable)
    return;

  g_state.cbType = type;
  g_state.ds->cbNotice(spice_type_to_clipboard_type(type));
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

void spiceClipboardRelease(void)
{
  if (!params.clipboardToLocal)
    return;

  if (g_state.cbAvailable)
    g_state.ds->cbRelease();
}

void spiceClipboardRequest(const SpiceDataType type)
{
  if (!params.clipboardToVM)
    return;

  if (g_state.cbAvailable)
    g_state.ds->cbRequest(spice_type_to_clipboard_type(type));
}

static bool warpPointer(int x, int y, bool exiting)
{
  if (!g_cursor.inWindow && !exiting)
    return false;

  if (g_cursor.warpState == WARP_STATE_OFF)
    return false;

  if (exiting)
    g_cursor.warpState = WARP_STATE_OFF;

  if (g_cursor.pos.x == x && g_cursor.pos.y == y)
    return true;

  g_state.ds->warpPointer(x, y, exiting);
  return true;
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

static void cursorToInt(double ex, double ey, int *x, int *y)
{
  /* only smooth if enabled and not using raw mode */
  if (params.mouseSmoothing && !(g_cursor.grab && params.rawMouse))
  {
    static struct DoublePoint last = { 0 };

    /* only apply smoothing to small deltas */
    if (fabs(ex - last.x) < 5.0 && fabs(ey - last.y) < 5.0)
    {
      ex = last.x = (last.x + ex) / 2.0;
      ey = last.y = (last.y + ey) / 2.0;
    }
    else
    {
      last.x = ex;
      last.y = ey;
    }
  }

  /* convert to int accumulating the fractional error */
  ex += g_cursor.acc.x;
  ey += g_cursor.acc.y;
  g_cursor.acc.x = modf(ex, &ex);
  g_cursor.acc.y = modf(ey, &ey);

  *x = (int)ex;
  *y = (int)ey;
}

static void setCursorInView(bool enable)
{
  // if the state has not changed, don't do anything else
  if (g_cursor.inView == enable)
    return;

  if (enable && !g_state.focused)
    return;

  // do not allow the view to become active if any mouse buttons are being held,
  // this fixes issues with meta window resizing.
  if (enable && g_cursor.buttons)
    return;

  g_cursor.inView = enable;
  g_cursor.draw   = (params.alwaysShowCursor || params.captureInputOnly)
    ? true : enable;
  g_cursor.redraw = true;

  /* if the display server does not support warp, then we can not operate in
   * always relative mode and we should not grab the pointer */
  bool warpSupport = true;
  app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

  g_cursor.warpState = enable ? WARP_STATE_ON : WARP_STATE_OFF;

  if (enable)
  {
    if (params.hideMouse)
      SDL_ShowCursor(SDL_DISABLE);

    if (warpSupport && !params.captureInputOnly)
      g_state.ds->grabPointer();

    if (params.grabKeyboardOnFocus)
      g_state.ds->grabKeyboard();
  }
  else
  {
    if (params.hideMouse)
      SDL_ShowCursor(SDL_ENABLE);

    if (warpSupport)
      g_state.ds->ungrabPointer();

    g_state.ds->ungrabKeyboard();
  }

  g_cursor.warpState = WARP_STATE_ON;
}

void app_handleMouseGrabbed(double ex, double ey)
{
  if (!app_inputEnabled())
    return;

  int x, y;
  if (params.rawMouse && !g_cursor.useScale)
  {
    /* raw unscaled input are always round numbers */
    x = floor(ex);
    y = floor(ey);
  }
  else
  {
    /* apply sensitivity */
    ex = (ex / 10.0) * (g_cursor.sens + 10);
    ey = (ey / 10.0) * (g_cursor.sens + 10);
    cursorToInt(ex, ey, &x, &y);
  }

  if (x == 0 && y == 0)
    return;

  if (!spice_mouse_motion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}

void app_handleButtonPress(int button)
{
  if (!app_inputEnabled() || !g_cursor.inView)
    return;

  g_cursor.buttons |= (1U << button);

  if (!spice_mouse_press(button))
    DEBUG_ERROR("SDL_MOUSEBUTTONDOWN: failed to send message");
}

void app_handleButtonRelease(int button)
{
  if (!app_inputEnabled())
    return;

  g_cursor.buttons &= ~(1U << button);

  if (!spice_mouse_release(button))
    DEBUG_ERROR("SDL_MOUSEBUTTONUP: failed to send message");
}

void app_handleKeyPress(int sc)
{
  if (sc == params.escapeKey && !g_state.escapeActive)
  {
    g_state.escapeActive = true;
    g_state.escapeAction = -1;
    return;
  }

  if (g_state.escapeActive)
  {
    g_state.escapeAction = sc;
    return;
  }

  if (!app_inputEnabled())
    return;

  if (params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  if (!g_state.keyDown[sc])
  {
    uint32_t ps2 = xfree86_to_ps2[sc];
    if (!ps2)
      return;

    if (spice_key_down(ps2))
      g_state.keyDown[sc] = true;
    else
    {
      DEBUG_ERROR("SDL_KEYDOWN: failed to send message");
      return;
    }
  }
}

void app_handleKeyRelease(int sc)
{
  if (g_state.escapeActive)
  {
    if (g_state.escapeAction == -1)
    {
      if (params.useSpiceInput)
        setGrab(!g_cursor.grab);
    }
    else
    {
      KeybindHandle handle = g_state.bindings[sc];
      if (handle)
      {
        handle->callback(sc, handle->opaque);
        return;
      }
    }

    if (sc == params.escapeKey)
      g_state.escapeActive = false;
  }

  if (!app_inputEnabled())
    return;

  // avoid sending key up events when we didn't send a down
  if (!g_state.keyDown[sc])
    return;

  if (params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  uint32_t ps2 = xfree86_to_ps2[sc];
  if (!ps2)
    return;

  if (spice_key_up(ps2))
    g_state.keyDown[sc] = false;
  else
  {
    DEBUG_ERROR("SDL_KEYUP: failed to send message");
    return;
  }
}

static void rotatePoint(struct DoublePoint *point)
{
  double temp;

  switch((g_state.rotate + params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      break;

    case LG_ROTATE_90:
      temp = point->x;
      point->x =  point->y;
      point->y = -temp;
      break;

    case LG_ROTATE_180:
      point->x = -point->x;
      point->y = -point->y;
      break;

    case LG_ROTATE_270:
      temp = point->x;
      point->x = -point->y;
      point->y =  temp;
      break;
  }
}

static bool guestCurToLocal(struct DoublePoint *local)
{
  if (!g_cursor.guest.valid || !g_state.posInfoValid)
    return false;

  const struct DoublePoint point =
  {
    .x = g_cursor.guest.x + g_cursor.guest.hx,
    .y = g_cursor.guest.y + g_cursor.guest.hy
  };

  switch((g_state.rotate + params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      local->x = (point.x / g_cursor.scale.x) + g_state.dstRect.x;
      local->y = (point.y / g_cursor.scale.y) + g_state.dstRect.y;;
      break;

    case LG_ROTATE_90:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.y / g_cursor.scale.y;
      local->y = (point.x / g_cursor.scale.x) + g_state.dstRect.y;
      break;

    case LG_ROTATE_180:
      local->x = (g_state.dstRect.x + g_state.dstRect.w) -
        point.x / g_cursor.scale.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.y / g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      local->x = (point.y / g_cursor.scale.y) + g_state.dstRect.x;
      local->y = (g_state.dstRect.y + g_state.dstRect.h) -
        point.x / g_cursor.scale.x;
      break;
  }

  return true;
}

inline static void localCurToGuest(struct DoublePoint *guest)
{
  const struct DoublePoint point =
    g_cursor.pos;

  switch((g_state.rotate + params.winRotate) % LG_ROTATE_MAX)
  {
    case LG_ROTATE_0:
      guest->x = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      guest->y = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      break;

    case LG_ROTATE_90:
      guest->x = (point.y - g_state.dstRect.y) * g_cursor.scale.y;
      guest->y = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      break;

    case LG_ROTATE_180:
      guest->x = (g_state.dstRect.w - point.x + g_state.dstRect.x)
        * g_cursor.scale.x;
      guest->y = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      break;

    case LG_ROTATE_270:
      guest->x = (g_state.dstRect.h - point.y + g_state.dstRect.y)
        * g_cursor.scale.y;
      guest->y = (point.x - g_state.dstRect.x) * g_cursor.scale.x;
      break;

    default:
      assert(!"unreachable");
  }
}

void app_handleMouseNormal(double ex, double ey)
{
  // prevent cursor handling outside of capture if the position is not known
  if (!g_cursor.guest.valid)
    return;

  if (!app_inputEnabled())
    return;

  /* scale the movement to the guest */
  if (g_cursor.useScale && params.scaleMouseInput)
  {
    ex *= g_cursor.scale.x / g_cursor.dpiScale;
    ey *= g_cursor.scale.y / g_cursor.dpiScale;
  }

  bool testExit = true;
  if (!g_cursor.inView)
  {
    const bool inView =
      g_cursor.pos.x >= g_state.dstRect.x                     &&
      g_cursor.pos.x <  g_state.dstRect.x + g_state.dstRect.w &&
      g_cursor.pos.y >= g_state.dstRect.y                     &&
      g_cursor.pos.y <  g_state.dstRect.y + g_state.dstRect.h;

    setCursorInView(inView);
    if (inView)
      g_cursor.realign = true;
  }

  /* nothing to do if we are outside the viewport */
  if (!g_cursor.inView)
    return;

  /*
   * do not pass mouse events to the guest if we do not have focus, this must be
   * done after the inView test has been performed so that when focus is gained
   * we know if we should be drawing the cursor.
   */
  if (!g_state.focused)
    return;

  /* if we have been instructed to realign */
  if (g_cursor.realign)
  {
    g_cursor.realign = false;

    struct DoublePoint guest;
    localCurToGuest(&guest);

    /* add the difference to the offset */
    ex += guest.x - (g_cursor.guest.x + g_cursor.guest.hx);
    ey += guest.y - (g_cursor.guest.y + g_cursor.guest.hy);

    /* don't test for an exit as we just entered, we can get into a enter/exit
     * loop otherwise */
    testExit = false;
  }

 /* if we are in "autoCapture" and the delta was large don't test for exit */
  if (params.autoCapture &&
      (fabs(ex) > 100.0 / g_cursor.scale.x || fabs(ey) > 100.0 / g_cursor.scale.y))
    testExit = false;

  /* if any buttons are held we should not allow exit to happen */
  if (g_cursor.buttons)
    testExit = false;

  if (testExit)
  {
    /* translate the move to the guests orientation */
    struct DoublePoint move = {.x = ex, .y = ey};
    rotatePoint(&move);

    /* translate the guests position to our coordinate space */
    struct DoublePoint local;
    guestCurToLocal(&local);

    /* check if the move would push the cursor outside the guest's viewport */
    if (
        local.x + move.x <  g_state.dstRect.x ||
        local.y + move.y <  g_state.dstRect.y ||
        local.x + move.x >= g_state.dstRect.x + g_state.dstRect.w ||
        local.y + move.y >= g_state.dstRect.y + g_state.dstRect.h)
    {
      local.x += move.x;
      local.y += move.y;
      const int tx = (local.x <= 0.0) ? floor(local.x) : ceil(local.x);
      const int ty = (local.y <= 0.0) ? floor(local.y) : ceil(local.y);

      if (isValidCursorLocation(
            g_state.windowPos.x + g_state.border.x + tx,
            g_state.windowPos.y + g_state.border.y + ty))
      {
        setCursorInView(false);

        /* preempt the window leave flag if the warp will leave our window */
        if (tx < 0 || ty < 0 || tx > g_state.windowW || ty > g_state.windowH)
          g_cursor.inWindow = false;

        /* ungrab the pointer and move the local cursor to the exit point */
        g_state.ds->ungrabPointer();
        warpPointer(tx, ty, true);
        return;
      }
    }
  }

  int x, y;
  cursorToInt(ex, ey, &x, &y);

  if (x == 0 && y == 0)
    return;

  if (params.autoCapture)
  {
    g_cursor.delta.x += x;
    g_cursor.delta.y += y;

    if (fabs(g_cursor.delta.x) > 50.0 || fabs(g_cursor.delta.y) > 50.0)
    {
      g_cursor.delta.x = 0;
      g_cursor.delta.y = 0;
      warpPointer(g_state.windowCX, g_state.windowCY, false);
    }

    g_cursor.guest.x = g_state.srcSize.x / 2;
    g_cursor.guest.y = g_state.srcSize.y / 2;
  }
  else
  {
    /* assume the mouse will move to the location we attempt to move it to so we
     * avoid warp out of window issues. The cursorThread will correct this if
     * wrong after the movement has ocurred on the guest */
    g_cursor.guest.x += x;
    g_cursor.guest.y += y;
  }

  if (!spice_mouse_motion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}


// On some display servers normal cursor logic does not work due to the lack of
// cursor warp support. Instead, we attempt a best-effort emulation which works
// with a 1:1 mouse movement patch applied in the guest. For anything fancy, use
// capture mode.
void app_handleMouseBasic()
{
  /* do not pass mouse events to the guest if we do not have focus */
  if (!g_state.focused)
    return;

  if (!app_inputEnabled())
    return;

  const bool inView =
    g_cursor.pos.x >= g_state.dstRect.x                     &&
    g_cursor.pos.x <  g_state.dstRect.x + g_state.dstRect.w &&
    g_cursor.pos.y >= g_state.dstRect.y                     &&
    g_cursor.pos.y <  g_state.dstRect.y + g_state.dstRect.h;

  setCursorInView(inView);

  if (g_cursor.guest.dpiScale == 0)
    return;

  double px = g_cursor.pos.x;
  double py = g_cursor.pos.y;

  if (px < g_state.dstRect.x)
    px = g_state.dstRect.x;
  else if (px > g_state.dstRect.x + g_state.dstRect.w)
    px = g_state.dstRect.x + g_state.dstRect.w;

  if (py < g_state.dstRect.y)
    py = g_state.dstRect.y;
  else if (py > g_state.dstRect.y + g_state.dstRect.h)
    py = g_state.dstRect.y + g_state.dstRect.h;

  /* translate the guests position to our coordinate space */
  struct DoublePoint local;
  guestCurToLocal(&local);

  int x = (int) round((px - local.x) / g_cursor.dpiScale);
  int y = (int) round((py - local.y) / g_cursor.dpiScale);

  if (!x && !y)
    return;

  g_cursor.guest.x += x;
  g_cursor.guest.y += y;

  if (!spice_mouse_motion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}

void app_updateWindowPos(int x, int y)
{
  g_state.windowPos.x = x;
  g_state.windowPos.y = y;
}

void app_handleResizeEvent(int w, int h)
{
  SDL_GetWindowBordersSize(g_state.window,
    &g_state.border.y,
    &g_state.border.x,
    &g_state.border.h,
    &g_state.border.w
  );

  /* don't do anything else if the window dimensions have not changed */
  if (g_state.windowW == w && g_state.windowH == h)
    return;

  g_state.windowW  = w;
  g_state.windowH  = h;
  g_state.windowCX = w / 2;
  g_state.windowCY = h / 2;
  updatePositionInfo();

  if (app_inputEnabled())
  {
    /* if the window is moved/resized causing a loss of focus while grabbed, it
     * makes it impossible to re-focus the window, so we quietly re-enter
     * capture if we were already in it */
    if (g_cursor.grab)
    {
      setGrabQuiet(false);
      setGrabQuiet(true);
    }
    alignToGuest();
  }
}

void app_handleWindowLeave(void)
{
  g_cursor.inWindow = false;
  setCursorInView(false);

  if (!app_inputEnabled())
    return;

  if (!params.alwaysShowCursor)
    g_cursor.draw = false;
  g_cursor.redraw = true;
}

void app_handleWindowEnter(void)
{
  g_cursor.inWindow = true;
  if (!app_inputEnabled())
    return;

  g_cursor.realign = true;
}

static void setGrab(bool enable)
{
  setGrabQuiet(enable);

  app_alert(
    g_cursor.grab ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
    g_cursor.grab ? "Capture Enabled" : "Capture Disabled"
  );
}

static void setGrabQuiet(bool enable)
{
  /* we always do this so that at init the cursor is in the right state */
  if (params.captureInputOnly && params.hideMouse)
    SDL_ShowCursor(enable ? SDL_DISABLE : SDL_ENABLE);

  if (g_cursor.grab == enable)
    return;

  g_cursor.grab = enable;
  g_cursor.acc.x = 0.0;
  g_cursor.acc.y = 0.0;

  /* if the display server does not support warp we need to ungrab the pointer
   * here instead of in the move handler */
  bool warpSupport = true;
  app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

  if (enable)
  {
    setCursorInView(true);
    g_state.ignoreInput = false;

    if (params.grabKeyboard)
      g_state.ds->grabKeyboard();

    g_state.ds->grabPointer();
  }
  else
  {
    if (params.grabKeyboard)
    {
      if (!params.grabKeyboardOnFocus || !g_state.focused || params.captureInputOnly)
        g_state.ds->ungrabKeyboard();
    }

    if (!warpSupport || params.captureInputOnly || !g_state.formatValid)
      g_state.ds->ungrabPointer();

    // if exiting capture when input on capture only, we want to show the cursor
    if (params.captureInputOnly || !params.hideMouse)
      alignToGuest();
  }
}

int eventFilter(void * userdata, SDL_Event * event)
{
  if (g_state.ds->eventFilter(event))
    return 0;

  // always include the default handler (SDL) for any unhandled events
  if (g_state.ds != LG_DisplayServers[0])
    if (LG_DisplayServers[0]->eventFilter(event))
      return 0;

  if (event->type == e_SDLEvent)
  {
    switch(event->user.code)
    {
      case LG_EVENT_ALIGN_TO_GUEST:
      {
        if (!g_cursor.guest.valid || !g_state.focused)
          break;

        struct DoublePoint local;
        if (guestCurToLocal(&local))
          if (warpPointer(round(local.x), round(local.y), false))
            setCursorInView(true);
        break;
      }
    }
    return 0;
  }

  // consume all events
  return 0;
}

void int_handler(int sig)
{
  switch(sig)
  {
    case SIGINT:
    case SIGTERM:
      if (g_state.state != APP_STATE_SHUTDOWN)
      {
        DEBUG_INFO("Caught signal, shutting down...");
        g_state.state = APP_STATE_SHUTDOWN;
      }
      else
      {
        DEBUG_INFO("Caught second signal, force quitting...");
        signal(sig, SIG_DFL);
        raise(sig);
      }
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

static void toggle_fullscreen(uint32_t scancode, void * opaque)
{
  SDL_SetWindowFullscreen(g_state.window, params.fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  params.fullscreen = !params.fullscreen;
}

static void toggle_video(uint32_t scancode, void * opaque)
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

static void toggle_rotate(uint32_t scancode, void * opaque)
{
  if (params.winRotate == LG_ROTATE_MAX-1)
    params.winRotate = 0;
  else
    ++params.winRotate;
  updatePositionInfo();
}

static void toggle_input(uint32_t scancode, void * opaque)
{
  g_state.ignoreInput = !g_state.ignoreInput;

  if (g_state.ignoreInput)
    setCursorInView(false);
  else
    g_state.ds->realignPointer();

  app_alert(
    LG_ALERT_INFO,
    g_state.ignoreInput ? "Input Disabled" : "Input Enabled"
  );
}

static void quit(uint32_t scancode, void * opaque)
{
  g_state.state = APP_STATE_SHUTDOWN;
}

static void mouse_sens_inc(uint32_t scancode, void * opaque)
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

static void mouse_sens_dec(uint32_t scancode, void * opaque)
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

static void ctrl_alt_fn(uint32_t key, void * opaque)
{
  const uint32_t ctrl = xfree86_to_ps2[KEY_LEFTCTRL];
  const uint32_t alt  = xfree86_to_ps2[KEY_LEFTALT ];
  const uint32_t fn   = xfree86_to_ps2[key];
  spice_key_down(ctrl);
  spice_key_down(alt );
  spice_key_down(fn  );

  spice_key_up(ctrl);
  spice_key_up(alt );
  spice_key_up(fn  );
}

static void key_passthrough(uint32_t sc, void * opaque)
{
  sc = xfree86_to_ps2[sc];
  spice_key_down(sc);
  spice_key_up  (sc);
}

static void register_key_binds(void)
{
  g_state.kbFS           = app_register_keybind(KEY_F     , toggle_fullscreen, NULL);
  g_state.kbVideo        = app_register_keybind(KEY_V     , toggle_video     , NULL);
  g_state.kbRotate       = app_register_keybind(KEY_R     , toggle_rotate    , NULL);
  g_state.kbQuit         = app_register_keybind(KEY_Q     , quit             , NULL);

  if (params.useSpiceInput)
  {
    g_state.kbInput        = app_register_keybind(KEY_I     , toggle_input     , NULL);
    g_state.kbMouseSensInc = app_register_keybind(KEY_INSERT, mouse_sens_inc   , NULL);
    g_state.kbMouseSensDec = app_register_keybind(KEY_DELETE, mouse_sens_dec   , NULL);

    g_state.kbCtrlAltFn[0 ] = app_register_keybind(KEY_F1 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[1 ] = app_register_keybind(KEY_F2 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[2 ] = app_register_keybind(KEY_F3 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[3 ] = app_register_keybind(KEY_F4 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[4 ] = app_register_keybind(KEY_F5 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[5 ] = app_register_keybind(KEY_F6 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[6 ] = app_register_keybind(KEY_F7 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[7 ] = app_register_keybind(KEY_F8 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[8 ] = app_register_keybind(KEY_F9 , ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[9 ] = app_register_keybind(KEY_F10, ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[10] = app_register_keybind(KEY_F11, ctrl_alt_fn, NULL);
    g_state.kbCtrlAltFn[11] = app_register_keybind(KEY_F12, ctrl_alt_fn, NULL);

    g_state.kbPass[0] = app_register_keybind(KEY_LEFTMETA , key_passthrough, NULL);
    g_state.kbPass[1] = app_register_keybind(KEY_RIGHTMETA, key_passthrough, NULL);
  }
}

static void release_key_binds(void)
{
  app_release_keybind(&g_state.kbFS   );
  app_release_keybind(&g_state.kbVideo);
  app_release_keybind(&g_state.kbInput);
  app_release_keybind(&g_state.kbQuit );
  app_release_keybind(&g_state.kbMouseSensInc);
  app_release_keybind(&g_state.kbMouseSensDec);
  for(int i = 0; i < 12; ++i)
    app_release_keybind(&g_state.kbCtrlAltFn[i]);
  for(int i = 0; i < 2; ++i)
    app_release_keybind(&g_state.kbPass[i]);
}

static void initSDLCursor(void)
{
  const uint8_t data[4] = {0xf, 0x9, 0x9, 0xf};
  const uint8_t mask[4] = {0xf, 0xf, 0xf, 0xf};
  cursor = SDL_CreateCursor(data, mask, 8, 4, 4, 0);
  SDL_SetCursor(cursor);
}

static int lg_run(void)
{
  memset(&g_state, 0, sizeof(g_state));

  g_cursor.sens = params.mouseSens;
       if (g_cursor.sens < -9) g_cursor.sens = -9;
  else if (g_cursor.sens >  9) g_cursor.sens =  9;

  // try to early detect the platform
  SDL_SYSWM_TYPE subsystem = SDL_SYSWM_UNKNOWN;
       if (getenv("WAYLAND_DISPLAY")) subsystem = SDL_SYSWM_WAYLAND;
  else if (getenv("DISPLAY"        )) subsystem = SDL_SYSWM_X11;
  else
    DEBUG_WARN("Unknown subsystem, falling back to SDL default");

  // search for the best displayserver ops to use
  for(int i = 0; i < LG_DISPLAYSERVER_COUNT; ++i)
    if (LG_DisplayServers[i]->subsystem == subsystem)
    {
      g_state.ds = LG_DisplayServers[i];
      break;
    }

  assert(g_state.ds);

  // set any null methods to the fallback
#define SET_FALLBACK(x) \
  if (!g_state.ds->x) g_state.ds->x = LG_DisplayServers[0]->x;
  SET_FALLBACK(earlyInit);
  SET_FALLBACK(getProp);
  SET_FALLBACK(init);
  SET_FALLBACK(startup);
  SET_FALLBACK(shutdown);
  SET_FALLBACK(free);
  SET_FALLBACK(eventFilter);
  SET_FALLBACK(grabPointer);
  SET_FALLBACK(ungrabKeyboard);
  SET_FALLBACK(warpPointer);
  SET_FALLBACK(realignPointer);
  SET_FALLBACK(inhibitIdle);
  SET_FALLBACK(uninhibitIdle);
  SET_FALLBACK(cbInit);
  SET_FALLBACK(cbNotice);
  SET_FALLBACK(cbRelease);
  SET_FALLBACK(cbRequest);
#undef SET_FALLBACK

  // init the subsystem
  if (!g_state.ds->earlyInit())
  {
    DEBUG_ERROR("Subsystem early init failed");
    return -1;
  }

  // Allow screensavers for now: we will enable and disable as needed.
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

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
      SDL_WINDOW_HIDDEN |
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

  SDL_VERSION(&g_state.wminfo.version);
  if (!SDL_GetWindowWMInfo(g_state.window, &g_state.wminfo))
  {
    DEBUG_ERROR("Could not get SDL window information %s", SDL_GetError());
    return -1;
  }

  // enable WM events
  SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);

  g_state.ds->init(&g_state.wminfo);

  // now init has been done we can show the window, doing this before init for
  // X11 causes us to miss the first focus event
  SDL_ShowWindow(g_state.window);

  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS,
      params.minimizeOnFocusLoss ? "1" : "0");

  if (params.fullscreen)
    SDL_SetWindowFullscreen(g_state.window, SDL_WINDOW_FULLSCREEN_DESKTOP);

  if (!params.center)
    SDL_SetWindowPosition(g_state.window, params.x, params.y);

  if (params.noScreensaver)
    g_state.ds->inhibitIdle();

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

  // create our custom event
  e_SDLEvent = SDL_RegisterEvents(1);

  register_key_binds();

  initSDLCursor();

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

  lgInit();

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

  g_state.ds->startup();
  g_state.cbAvailable = g_state.ds->cbInit && g_state.ds->cbInit();
  if (g_state.cbAvailable)
    g_state.cbRequestList = ll_new();

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

  if (params.captureOnStart)
    setGrab(true);

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
      DEBUG_INFO("Check the host log in your guest at: "
        "%%ProgramData%%\\Looking Glass (host)\\looking-glass-host.txt");
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
      DEBUG_ERROR("Client version: %s", BUILD_VERSION);
      if (udata->version >= 2)
        DEBUG_ERROR("  Host version: %s", udata->hostver);
    }
    else
      DEBUG_ERROR("Invalid KVMFR magic");

    DEBUG_BREAK();

    if (magicMatches)
    {
      DEBUG_INFO("Waiting for you to upgrade the host application");
      while (g_state.state == APP_STATE_RUNNING && udata->version != KVMFR_VERSION)
        SDL_WaitEventTimeout(NULL, 1000);

      if (g_state.state != APP_STATE_RUNNING)
        return -1;

      goto restart;
    }
    else
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

static void lg_shutdown(void)
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
    for(uint32_t scancode = 0; scancode < KEY_MAX; ++scancode)
      if (g_state.keyDown[scancode])
      {
        g_state.keyDown[scancode] = false;
        spice_key_up(scancode);
      }

    spice_disconnect();
    if (t_spice)
      lgJoinThread(t_spice, NULL);
  }

  if (g_state.ds)
    g_state.ds->shutdown();

  if (g_state.cbRequestList)
  {
    ll_free(g_state.cbRequestList);
    g_state.cbRequestList = NULL;
  }

  if (g_state.window)
  {
    g_state.ds->free();
    SDL_DestroyWindow(g_state.window);
  }

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

  const int ret = lg_run();
  lg_shutdown();

  config_free();
  return ret;

}
