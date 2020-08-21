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

static atomic_uint a_framesPending = 0;

struct AppState state;

// this structure is initialized in config.c
struct AppParams params = { 0 };

static void handleMouseMoveEvent(int ex, int ey);
static void alignMouseWithGuest();
static void alignMouseWithHost();

static void updatePositionInfo()
{
  if (state.haveSrcSize)
  {
    if (params.keepAspect)
    {
      const float srcAspect = (float)state.srcSize.y / (float)state.srcSize.x;
      const float wndAspect = (float)state.windowH / (float)state.windowW;
      bool force = true;

      if ((int)(wndAspect * 1000) == (int)(srcAspect * 1000))
      {
        force           = false;
        state.dstRect.w = state.windowW;
        state.dstRect.h = state.windowH;
        state.dstRect.x = 0;
        state.dstRect.y = 0;
      }
      else
      if (wndAspect < srcAspect)
      {
        state.dstRect.w = (float)state.windowH / srcAspect;
        state.dstRect.h = state.windowH;
        state.dstRect.x = (state.windowW >> 1) - (state.dstRect.w >> 1);
        state.dstRect.y = 0;
      }
      else
      {
        state.dstRect.w = state.windowW;
        state.dstRect.h = (float)state.windowW * srcAspect;
        state.dstRect.x = 0;
        state.dstRect.y = (state.windowH >> 1) - (state.dstRect.h >> 1);
      }

      if (force && params.forceAspect)
      {
        state.resizeTimeout = microtime() + RESIZE_TIMEOUT;
        state.resizeDone    = false;
      }
    }
    else
    {
      state.dstRect.x = 0;
      state.dstRect.y = 0;
      state.dstRect.w = state.windowW;
      state.dstRect.h = state.windowH;
    }
    state.dstRect.valid = true;

    state.scaleX = (float)state.srcSize.y / (float)state.dstRect.h;
    state.scaleY = (float)state.srcSize.x / (float)state.dstRect.w;
  }

  state.lgrResize = true;
}

static int renderThread(void * unused)
{
  if (!state.lgr->render_startup(state.lgrData, state.window))
  {
    state.state = APP_STATE_SHUTDOWN;

    /* unblock threads waiting on the condition */
    lgSignalEvent(e_startup);
    return 1;
  }

  /* signal to other threads that the renderer is ready */
  lgSignalEvent(e_startup);

  int resyncCheck = 0;
  struct timespec time;
  clock_gettime(CLOCK_REALTIME, &time);

  while(state.state != APP_STATE_SHUTDOWN)
  {
    if (state.frameTime > 0)
    {
      if (++resyncCheck == 100)
      {
        resyncCheck = 0;
        clock_gettime(CLOCK_REALTIME, &time);
      }
      tsAdd(&time, state.frameTime);
    }

    if (state.lgrResize)
    {
      if (state.lgr)
        state.lgr->on_resize(state.lgrData, state.windowW, state.windowH, state.dstRect);
      state.lgrResize = false;
    }

    if (!state.lgr->render(state.lgrData, state.window))
      break;

    if (params.showFPS)
    {
      const uint64_t t    = nanotime();
      state.renderTime   += t - state.lastFrameTime;
      state.lastFrameTime = t;
      ++state.renderCount;

      if (state.renderTime > 1e9)
      {
        const float avgUPS = 1000.0f / (((float)state.renderTime / atomic_load_explicit(&state.frameCount, memory_order_acquire)) / 1e6f);
        const float avgFPS = 1000.0f / (((float)state.renderTime / state.renderCount) / 1e6f);
        state.lgr->update_fps(state.lgrData, avgUPS, avgFPS);

        atomic_store_explicit(&state.frameCount, 0, memory_order_release);
        state.renderTime  = 0;
        state.renderCount = 0;
      }
    }

    if (!state.resizeDone && state.resizeTimeout < microtime())
    {
      SDL_SetWindowSize(
        state.window,
        state.dstRect.w,
        state.dstRect.h
      );
      state.resizeDone = true;
    }

    if (state.frameTime > 0)
    {
      /* if there are frames pending already, don't wait on the event */
      if (atomic_load_explicit(&a_framesPending, memory_order_acquire) > 0)
        if (atomic_fetch_sub_explicit(&a_framesPending, 1, memory_order_release) > 1)
          continue;

      if (lgWaitEventAbs(e_frame, &time) && state.frameTime > 0)
      {
        /* only resync the timer if we got an early frame */
        struct timespec now, diff;
        clock_gettime(CLOCK_REALTIME, &now);
        tsDiff(&diff, &now, &time);
        if (diff.tv_sec == 0 && diff.tv_nsec < state.frameTime)
        {
          resyncCheck = 0;
          memcpy(&time, &now, sizeof(struct timespec));
          tsAdd(&time, state.frameTime);
        }
      }
    }
  }

  state.state = APP_STATE_SHUTDOWN;

  if (t_cursor)
    lgJoinThread(t_cursor, NULL);

  if (t_frame)
    lgJoinThread(t_frame, NULL);

  state.lgr->deinitialize(state.lgrData);
  state.lgr = NULL;
  return 0;
}

static int cursorThread(void * unused)
{
  LGMP_STATUS         status;
  PLGMPClientQueue    queue;
  LG_RendererCursor   cursorType     = LG_CURSOR_COLOR;

  lgWaitEvent(e_startup, TIMEOUT_INFINITE);

  // subscribe to the pointer queue
  while(state.state == APP_STATE_RUNNING)
  {
    status = lgmpClientSubscribe(state.lgmp, LGMP_Q_POINTER, &queue);
    if (status == LGMP_OK)
      break;

    if (status == LGMP_ERR_NO_SUCH_QUEUE)
    {
      usleep(1000);
      continue;
    }

    DEBUG_ERROR("lgmpClientSubscribe Failed: %s", lgmpStatusString(status));
    state.state = APP_STATE_SHUTDOWN;
    break;
  }

  while(state.state == APP_STATE_RUNNING)
  {
    LGMPMessage msg;
    if ((status = lgmpClientProcess(queue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
      {
        if (state.updateCursor)
        {
          state.updateCursor = false;
          state.lgr->on_mouse_event
          (
            state.lgrData,
            state.cursorVisible && state.drawCursor,
            state.cursor.x,
            state.cursor.y
          );

          lgSignalEvent(e_frame);
        }

        usleep(params.cursorPollInterval);
        continue;
      }

      if (status == LGMP_ERR_INVALID_SESSION)
        state.state = APP_STATE_RESTART;
      else
      {
        DEBUG_ERROR("lgmpClientProcess Failed: %s", lgmpStatusString(status));
        state.state = APP_STATE_SHUTDOWN;
      }
      break;
    }

    KVMFRCursor * cursor = (KVMFRCursor *)msg.mem;

    state.cursorVisible =
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

      state.cursor.hx = cursor->hx;
      state.cursor.hy = cursor->hy;

      const uint8_t * data = (const uint8_t *)(cursor + 1);
      if (!state.lgr->on_mouse_shape(
        state.lgrData,
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
      state.cursor.x      = cursor->x;
      state.cursor.y      = cursor->y;
      state.haveCursorPos = true;

      if (state.haveSrcSize && state.haveCurLocal && !state.serverMode)
        alignMouseWithGuest();
    }

    lgmpClientMessageDone(queue);
    state.updateCursor = false;

    state.lgr->on_mouse_event
    (
      state.lgrData,
      state.cursorVisible && state.drawCursor,
      state.cursor.x,
      state.cursor.y
    );

    if (params.mouseRedraw)
      lgSignalEvent(e_frame);
  }

  lgmpClientUnsubscribe(&queue);
  return 0;
}

static int frameThread(void * unused)
{
  LGMP_STATUS      status;
  PLGMPClientQueue queue;

  SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  lgWaitEvent(e_startup, TIMEOUT_INFINITE);
  if (state.state != APP_STATE_RUNNING)
    return 0;

  // subscribe to the frame queue
  while(state.state == APP_STATE_RUNNING)
  {
    status = lgmpClientSubscribe(state.lgmp, LGMP_Q_FRAME, &queue);
    if (status == LGMP_OK)
      break;

    if (status == LGMP_ERR_NO_SUCH_QUEUE)
    {
      usleep(1000);
      continue;
    }

    DEBUG_ERROR("lgmpClientSubscribe Failed: %s", lgmpStatusString(status));
    state.state = APP_STATE_SHUTDOWN;
    break;
  }

  while(state.state == APP_STATE_RUNNING)
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
        state.state = APP_STATE_RESTART;
      else
      {
        DEBUG_ERROR("lgmpClientProcess Failed: %s", lgmpStatusString(status));
        state.state = APP_STATE_SHUTDOWN;
      }
      break;
    }

    KVMFRFrame * frame = (KVMFRFrame *)msg.mem;

    // setup the renderer format with the frame format details
    LG_RendererFormat lgrFormat;
    lgrFormat.type   = frame->type;
    lgrFormat.width  = frame->width;
    lgrFormat.height = frame->height;
    lgrFormat.stride = frame->stride;
    lgrFormat.pitch  = frame->pitch;

    size_t dataSize;
    bool   error = false;
    switch(frame->type)
    {
      case FRAME_TYPE_RGBA:
      case FRAME_TYPE_BGRA:
      case FRAME_TYPE_RGBA10:
        dataSize       = lgrFormat.height * lgrFormat.pitch;
        lgrFormat.bpp  = 32;
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
      state.state = APP_STATE_SHUTDOWN;
      break;
    }

    if (frame->width != state.srcSize.x || frame->height != state.srcSize.y)
    {
      state.srcSize.x = frame->width;
      state.srcSize.y = frame->height;
      state.haveSrcSize = true;
      if (params.autoResize)
        SDL_SetWindowSize(state.window, frame->width, frame->height);

      updatePositionInfo();
    }

    FrameBuffer * fb = (FrameBuffer *)(((uint8_t*)frame) + frame->offset);
    if (!state.lgr->on_frame_event(state.lgrData, lgrFormat, fb))
    {
      DEBUG_ERROR("renderer on frame event returned failure");
      state.state = APP_STATE_SHUTDOWN;
      break;
    }

    atomic_fetch_add_explicit(&state.frameCount, 1, memory_order_relaxed);
    if (atomic_fetch_add_explicit(&a_framesPending, 1, memory_order_relaxed) == 0)
      lgSignalEvent(e_frame);

    lgmpClientMessageDone(queue);
  }

  lgmpClientUnsubscribe(&queue);
  return 0;
}

int spiceThread(void * arg)
{
  while(state.state != APP_STATE_SHUTDOWN)
    if (!spice_process(1000))
    {
      if (state.state != APP_STATE_SHUTDOWN)
      {
        state.state = APP_STATE_SHUTDOWN;
        DEBUG_ERROR("failed to process spice messages");
      }
      break;
    }

  state.state = APP_STATE_SHUTDOWN;
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

void clipboardNotify(const LG_ClipboardData type)
{
  if (!params.clipboardToVM)
    return;

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    spice_clipboard_release();
    return;
  }

  spice_clipboard_grab(clipboard_type_to_spice_type(type));
}

void clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size)
{
  if (!params.clipboardToVM)
    return;

  uint8_t * buffer = data;

  // unix2dos
  if (type == LG_CLIPBOARD_DATA_TEXT)
  {
    // TODO: make this more memory efficent
    size_t newSize = 0;
    buffer = malloc(size * 2);
    uint8_t * p = buffer;
    for(uint32_t i = 0; i < size; ++i)
    {
      uint8_t c = data[i];
      if (c == '\n')
      {
        *p++ = '\r';
        ++newSize;
      }
      *p++ = c;
      ++newSize;
    }
    size = newSize;
  }

  spice_clipboard_data(clipboard_type_to_spice_type(type), buffer, (uint32_t)size);
  if (buffer != data)
    free(buffer);
}

void clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!params.clipboardToLocal)
    return;

  struct CBRequest * cbr = (struct CBRequest *)malloc(sizeof(struct CBRequest()));

  cbr->type    = state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(state.cbRequestList, cbr);

  spice_clipboard_request(state.cbType);
}

void spiceClipboardNotice(const SpiceDataType type)
{
  if (!params.clipboardToLocal)
    return;

  if (!state.lgc || !state.lgc->notice)
    return;

  state.cbType = type;
  state.lgc->notice(clipboardRequest, spice_type_to_clipboard_type(type));
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
  if (ll_shift(state.cbRequestList, (void **)&cbr))
  {
    cbr->replyFn(cbr->opaque, type, buffer, size);
    free(cbr);
  }
}

void spiceClipboardRelease()
{
  if (!params.clipboardToLocal)
    return;

  if (state.lgc && state.lgc->release)
    state.lgc->release();
}

void spiceClipboardRequest(const SpiceDataType type)
{
  if (!params.clipboardToVM)
    return;

  if (state.lgc && state.lgc->request)
    state.lgc->request(spice_type_to_clipboard_type(type));
}

static void warpMouse(int x, int y)
{
  if (state.warpState != WARP_STATE_ON)
    return;

  state.warpFromX = state.curLastX;
  state.warpFromY = state.curLastY;
  state.warpToX   = x;
  state.warpToY   = y;
  state.warpState = WARP_STATE_ACTIVE;

  SDL_WarpMouseInWindow(state.window, x, y);
}

static void handleMouseMoveEvent(int ex, int ey)
{

  state.curLocalX    = ex;
  state.curLocalY    = ey;
  state.haveCurLocal = true;

  if (state.ignoreInput || !params.useSpiceInput)
    return;

  if (state.warpState == WARP_STATE_ACTIVE)
  {
    if (ex == state.warpToX && ey == state.warpToY)
    {
      state.curLastX += state.warpToX - state.warpFromX;
      state.curLastY += state.warpToY - state.warpFromY;
      state.warpState = WARP_STATE_ON;
    }
  }

  if (state.serverMode)
  {
    if (
        ex < 100 || ex > state.windowW - 100 ||
        ey < 100 || ey > state.windowH - 100)
    {
      warpMouse(state.windowW / 2, state.windowH / 2);
    }
  }
  else
  {
    if (ex < state.dstRect.x                   ||
        ex > state.dstRect.x + state.dstRect.w ||
        ey < state.dstRect.y                   ||
        ey > state.dstRect.y + state.dstRect.h)
    {
      state.cursorInView = false;
      state.updateCursor = true;
      state.warpState    = WARP_STATE_OFF;

      if (params.useSpiceInput)
        state.drawCursor = false;
      return;
    }
  }

  if (!state.cursorInView)
  {
    state.cursorInView = true;
    state.updateCursor = true;
    state.drawCursor   = true;
    if (state.warpState == WARP_STATE_ARMED)
      state.warpState = WARP_STATE_ON;
  }

  int rx = ex - state.curLastX;
  int ry = ey - state.curLastY;
  state.curLastX = ex;
  state.curLastY = ey;

  if (rx == 0 && ry == 0)
    return;

  if (params.scaleMouseInput && !state.serverMode)
  {
    state.accX += (float)rx * state.scaleX;
    state.accY += (float)ry * state.scaleY;
    rx = floor(state.accX);
    ry = floor(state.accY);
    state.accX -= rx;
    state.accY -= ry;
  }

  if (state.serverMode && state.mouseSens != 0)
  {
    state.sensX += ((float)rx / 10.0f) * (state.mouseSens + 10);
    state.sensY += ((float)ry / 10.0f) * (state.mouseSens + 10);
    rx = floor(state.sensX);
    ry = floor(state.sensY);
    state.sensX -= rx;
    state.sensY -= ry;
  }

  if (!spice_mouse_motion(rx, ry))
    DEBUG_ERROR("failed to send mouse motion message");
}

static void alignMouseWithGuest()
{
  if (state.ignoreInput || !params.useSpiceInput)
    return;

  state.curLastX = (int)round((float)(state.cursor.x + state.cursor.hx) / state.scaleX) + state.dstRect.x;
  state.curLastY = (int)round((float)(state.cursor.y + state.cursor.hy) / state.scaleY) + state.dstRect.y;
  warpMouse(state.curLastX, state.curLastY);
}

static void alignMouseWithHost()
{
  if (state.ignoreInput || !params.useSpiceInput)
    return;

  if (!state.haveCursorPos || state.serverMode)
    return;

  state.curLastX = (int)round((float)(state.cursor.x + state.cursor.hx) / state.scaleX) + state.dstRect.x;
  state.curLastY = (int)round((float)(state.cursor.y + state.cursor.hy) / state.scaleY) + state.dstRect.y;
  handleMouseMoveEvent(state.curLocalX, state.curLocalY);
}

static void handleResizeEvent(unsigned int w, unsigned int h)
{
  if (state.windowW == w && state.windowH == h)
    return;

  state.windowW = w;
  state.windowH = h;
  updatePositionInfo();
}

static void handleWindowLeave()
{
  if (!params.useSpiceInput)
    return;

  state.drawCursor   = false;
  state.cursorInView = false;
  state.updateCursor = true;
  state.warpState    = WARP_STATE_OFF;
}

static void handleWindowEnter()
{
  if (!params.useSpiceInput)
    return;

  alignMouseWithHost();
  state.drawCursor   = true;
  state.updateCursor = true;
  state.warpState    = WARP_STATE_ARMED;
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
        state.state = APP_STATE_SHUTDOWN;
      }
      return 0;
    }

    case SDL_WINDOWEVENT:
    {
      switch(event->window.event)
      {
        case SDL_WINDOWEVENT_ENTER:
          if (state.wminfo.subsystem != SDL_SYSWM_X11)
            handleWindowEnter();
          break;

        case SDL_WINDOWEVENT_LEAVE:
          if (state.wminfo.subsystem != SDL_SYSWM_X11)
            handleWindowLeave();
          break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
          if (state.wminfo.subsystem != SDL_SYSWM_X11)
            handleResizeEvent(event->window.data1, event->window.data2);
          break;

        // allow a window close event to close the application even if ignoreQuit is set
        case SDL_WINDOWEVENT_CLOSE:
          state.state = APP_STATE_SHUTDOWN;
          break;
      }
      return 0;
    }

    case SDL_SYSWMEVENT:
    {
      // When the window manager forces the window size after calling SDL_SetWindowSize, SDL
      // ignores this update and caches the incorrect window size. As such all related details
      // are incorect including mouse movement information as it clips to the old window size.
      if (state.wminfo.subsystem == SDL_SYSWM_X11)
      {
        XEvent xe = event->syswm.msg->msg.x11.event;
        switch(xe.type)
        {
          case ConfigureNotify:
            handleResizeEvent(xe.xconfigure.width, xe.xconfigure.height);
            break;

          case MotionNotify:
            handleMouseMoveEvent(xe.xmotion.x, xe.xmotion.y);
            break;

          case EnterNotify:
            state.curLocalX    = xe.xcrossing.x;
            state.curLocalY    = xe.xcrossing.y;
            state.haveCurLocal = true;
            handleWindowEnter();
            break;

          case LeaveNotify:
            state.curLocalX    = xe.xcrossing.x;
            state.curLocalY    = xe.xcrossing.y;
            state.haveCurLocal = true;
            handleWindowLeave();
            break;
        }
      }

      if (params.useSpiceClipboard && state.lgc && state.lgc->wmevent)
        state.lgc->wmevent(event->syswm.msg);
      return 0;
    }

    case SDL_MOUSEMOTION:
      if (state.wminfo.subsystem != SDL_SYSWM_X11)
        handleMouseMoveEvent(event->motion.x, event->motion.y);
      break;

    case SDL_KEYDOWN:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      if (sc == params.escapeKey)
      {
        state.escapeActive = true;
        state.escapeAction = -1;
        break;
      }

      if (state.escapeActive)
      {
        state.escapeAction = sc;
        break;
      }

      if (state.ignoreInput || !params.useSpiceInput)
        break;

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (!state.keyDown[sc])
      {
        if (spice_key_down(scancode))
          state.keyDown[sc] = true;
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
      if (state.escapeActive)
      {
        if (state.escapeAction == -1)
        {
          if (params.useSpiceInput)
          {
            state.serverMode = !state.serverMode;
            SDL_SetWindowGrab(state.window, state.serverMode);
            DEBUG_INFO("Server Mode: %s", state.serverMode ? "on" : "off");

            app_alert(
              state.serverMode ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
              state.serverMode ? "Capture Enabled" : "Capture Disabled"
            );

            if (state.serverMode)
              state.warpState = WARP_STATE_ON;
            else
              alignMouseWithGuest();
          }
        }
        else
        {
          KeybindHandle handle = state.bindings[sc];
          if (handle)
            handle->callback(sc, handle->opaque);
        }

        if (sc == params.escapeKey)
          state.escapeActive = false;
      }

      if (state.ignoreInput || !params.useSpiceInput)
        break;

      // avoid sending key up events when we didn't send a down
      if (!state.keyDown[sc])
        break;

      uint32_t scancode = mapScancode(sc);
      if (scancode == 0)
        break;

      if (spice_key_up(scancode))
        state.keyDown[sc] = false;
      else
      {
        DEBUG_ERROR("SDL_KEYUP: failed to send message");
        break;
      }
      break;
    }

    case SDL_MOUSEWHEEL:
      if (state.ignoreInput || !params.useSpiceInput || !state.cursorInView)
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
      if (state.ignoreInput || !params.useSpiceInput || !state.cursorInView)
        break;

      // The SPICE protocol doesn't support more than a standard PS/2 3 button mouse
      if (event->button.button > 3)
        break;
      if (
        !spice_mouse_position(event->button.x, event->button.y) ||
        !spice_mouse_press(event->button.button)
      )
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONDOWN: failed to send message");
        break;
      }
      break;

    case SDL_MOUSEBUTTONUP:
      if (state.ignoreInput || !params.useSpiceInput || !state.cursorInView)
        break;

      // The SPICE protocol doesn't support more than a standard PS/2 3 button mouse
      if (event->button.button > 3)
        break;
      if (
        !spice_mouse_position(event->button.x, event->button.y) ||
        !spice_mouse_release(event->button.button)
      )
      {
        DEBUG_ERROR("SDL_MOUSEBUTTONUP: failed to send message");
        break;
      }
      break;
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
      state.state = APP_STATE_SHUTDOWN;
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
  state.lgrData = NULL;
  if (!r->create(&state.lgrData, lgrParams))
    return false;

  // initialize the renderer
  if (!r->initialize(state.lgrData, sdlFlags))
  {
    r->deinitialize(state.lgrData);
    return false;
  }

  DEBUG_INFO("Using Renderer: %s", r->get_name());
  return true;
}

static void toggle_fullscreen(SDL_Scancode key, void * opaque)
{
  SDL_SetWindowFullscreen(state.window, params.fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  params.fullscreen = !params.fullscreen;
}

static void toggle_input(SDL_Scancode key, void * opaque)
{
  state.ignoreInput = !state.ignoreInput;
  app_alert(
    LG_ALERT_INFO,
    state.ignoreInput ? "Input Disabled" : "Input Enabled"
  );
}

static void quit(SDL_Scancode key, void * opaque)
{
  state.state = APP_STATE_SHUTDOWN;
}

static void mouse_sens_inc(SDL_Scancode key, void * opaque)
{
  char * msg;
  if (state.mouseSens < 9)
    ++state.mouseSens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", state.mouseSens > 0 ? "+" : "", state.mouseSens);
  app_alert(
    LG_ALERT_INFO,
    msg
  );
  free(msg);
}

static void mouse_sens_dec(SDL_Scancode key, void * opaque)
{
  char * msg;

  if (state.mouseSens > -9)
    --state.mouseSens;

  alloc_sprintf(&msg, "Sensitivity: %s%d", state.mouseSens > 0 ? "+" : "", state.mouseSens);
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
  state.kbFS           = app_register_keybind(SDL_SCANCODE_F     , toggle_fullscreen, NULL);
  state.kbInput        = app_register_keybind(SDL_SCANCODE_I     , toggle_input     , NULL);
  state.kbQuit         = app_register_keybind(SDL_SCANCODE_Q     , quit             , NULL);
  state.kbMouseSensInc = app_register_keybind(SDL_SCANCODE_INSERT, mouse_sens_inc   , NULL);
  state.kbMouseSensDec = app_register_keybind(SDL_SCANCODE_DELETE, mouse_sens_dec   , NULL);

  state.kbCtrlAltFn[0 ] = app_register_keybind(SDL_SCANCODE_F1 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[1 ] = app_register_keybind(SDL_SCANCODE_F2 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[2 ] = app_register_keybind(SDL_SCANCODE_F3 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[3 ] = app_register_keybind(SDL_SCANCODE_F4 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[4 ] = app_register_keybind(SDL_SCANCODE_F5 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[5 ] = app_register_keybind(SDL_SCANCODE_F6 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[6 ] = app_register_keybind(SDL_SCANCODE_F7 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[7 ] = app_register_keybind(SDL_SCANCODE_F8 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[8 ] = app_register_keybind(SDL_SCANCODE_F9 , ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[9 ] = app_register_keybind(SDL_SCANCODE_F10, ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[10] = app_register_keybind(SDL_SCANCODE_F11, ctrl_alt_fn, NULL);
  state.kbCtrlAltFn[11] = app_register_keybind(SDL_SCANCODE_F12, ctrl_alt_fn, NULL);
}

static void release_key_binds()
{
  app_release_keybind(&state.kbFS);
  app_release_keybind(&state.kbInput);
  app_release_keybind(&state.kbQuit);
  app_release_keybind(&state.kbMouseSensInc);
  app_release_keybind(&state.kbMouseSensDec);
  for(int i = 0; i < 12; ++i)
    app_release_keybind(&state.kbCtrlAltFn[i]);
}

static int lg_run()
{
  memset(&state, 0, sizeof(state));
  state.state      = APP_STATE_RUNNING;
  state.scaleX     = 1.0f;
  state.scaleY     = 1.0f;
  state.resizeDone = true;
  state.drawCursor = true;

  state.mouseSens = params.mouseSens;
       if (state.mouseSens < -9) state.mouseSens = -9;
  else if (state.mouseSens >  9) state.mouseSens =  9;

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
  if (!ivshmemOpen(&state.shm))
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

    while(state.state != APP_STATE_SHUTDOWN && !spice_ready())
      if (!spice_process(1000))
      {
        state.state = APP_STATE_SHUTDOWN;
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
  lgrParams.showFPS = params.showFPS;
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
    state.lgr = LG_Renderers[params.forceRendererIndex];
  }
  else
  {
    // probe for a a suitable renderer
    for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    {
      sdlFlags = 0;
      if (try_renderer(i, lgrParams, &sdlFlags))
      {
        state.lgr = LG_Renderers[i];
        break;
      }
    }
  }

  if (!state.lgr)
  {
    DEBUG_INFO("Unable to find a suitable renderer");
    return -1;
  }

  // all our ducks are in a line, create the window
  state.window = SDL_CreateWindow(
    params.windowTitle,
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_SHOWN |
      (params.fullscreen  ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) |
      (params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless  ? SDL_WINDOW_BORDERLESS : 0) |
      (params.maximize    ? SDL_WINDOW_MAXIMIZED  : 0) |
      sdlFlags
    )
  );

  if (state.window == NULL)
  {
    DEBUG_ERROR("Could not create an SDL window: %s\n", SDL_GetError());
    return 1;
  }

  if (params.fullscreen && !params.minimizeOnFocusLoss)
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

  if (!params.noScreensaver)
  {
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_EnableScreenSaver();
  }

  if (!params.center)
    SDL_SetWindowPosition(state.window, params.x, params.y);

  // ensure the initial window size is stored in the state
  SDL_GetWindowSize(state.window, &state.windowW, &state.windowH);

  // ensure renderer viewport is aware of the current window size
  updatePositionInfo();

  if (params.fpsMin == -1)
  {
      // minimum 60fps to keep interactivity decent
      state.frameTime = 1000000000ULL / 60ULL;
  }
  else
  {
      DEBUG_INFO("Using the FPS minimum from args: %d", params.fpsMin);
      state.frameTime = 1000000000ULL / (unsigned long long)params.fpsMin;
  }

  register_key_binds();

  // set the compositor hint to bypass for low latency
  SDL_VERSION(&state.wminfo.version);
  if (SDL_GetWindowWMInfo(state.window, &state.wminfo))
  {
    if (state.wminfo.subsystem == SDL_SYSWM_X11)
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
        state.wminfo.info.x11.display,
        state.wminfo.info.x11.window,
        &xinputmask,
        1
      );
#endif

      Atom NETWM_BYPASS_COMPOSITOR = XInternAtom(
        state.wminfo.info.x11.display,
        "NETWM_BYPASS_COMPOSITOR",
        False);

      unsigned long value = 1;
      XChangeProperty(
        state.wminfo.info.x11.display,
        state.wminfo.info.x11.window,
        NETWM_BYPASS_COMPOSITOR,
        XA_CARDINAL,
        32,
        PropModeReplace,
        (unsigned char *)&value,
        1
      );

      state.lgc = LG_Clipboards[0];
    }
  } else {
    DEBUG_ERROR("Could not get SDL window information %s", SDL_GetError());
    return -1;
  }

  if (state.lgc)
  {
    DEBUG_INFO("Using Clipboard: %s", state.lgc->getName());
    if (!state.lgc->init(&state.wminfo, clipboardRelease, clipboardNotify, clipboardData))
    {
      DEBUG_WARN("Failed to initialize the clipboard interface, continuing anyway");
      state.lgc = NULL;
    }

    state.cbRequestList = ll_new();
  }

  if (params.hideMouse)
  {
    // work around SDL_ShowCursor being non functional
    int32_t cursorData[2] = {0, 0};
    cursor = SDL_CreateCursor((uint8_t*)cursorData, (uint8_t*)cursorData, 8, 8, 4, 4);
    SDL_SetCursor(cursor);
    SDL_ShowCursor(SDL_DISABLE);
  }

  if (params.captureOnStart)
  {
    state.serverMode = true;
    SDL_SetWindowGrab(state.window, state.serverMode);
    DEBUG_INFO("Server Mode: %s", state.serverMode ? "on" : "off");
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

  while(state.state == APP_STATE_RUNNING)
  {
    if ((status = lgmpClientInit(state.shm.mem, state.shm.size, &state.lgmp)) == LGMP_OK)
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
  while(state.state == APP_STATE_RUNNING)
  {
    if ((status = lgmpClientSessionInit(state.lgmp, &udataSize, (uint8_t **)&udata)) == LGMP_OK)
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

  if (state.state != APP_STATE_RUNNING)
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

  while(state.state == APP_STATE_RUNNING)
  {
    if (!lgmpClientSessionValid(state.lgmp))
    {
      state.state = APP_STATE_RESTART;
      break;
    }
    SDL_WaitEventTimeout(NULL, 100);
  }

  if (state.state == APP_STATE_RESTART)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(e_frame);
    lgJoinThread(t_frame , NULL);
    lgJoinThread(t_cursor, NULL);
    t_frame  = NULL;
    t_cursor = NULL;

    state.state = APP_STATE_RUNNING;
    state.lgr->on_restart(state.lgrData);

    DEBUG_INFO("Waiting for the host to restart...");
    goto restart;
  }

  return 0;
}

static void lg_shutdown()
{
  state.state = APP_STATE_SHUTDOWN;
  if (t_render)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(e_frame);
    lgJoinThread(t_render, NULL);
  }

  lgmpClientFree(&state.lgmp);

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
      if (state.keyDown[i])
      {
        uint32_t scancode = mapScancode(i);
        if (scancode == 0)
          continue;
        state.keyDown[i] = false;
        spice_key_up(scancode);
      }

    spice_disconnect();
    if (t_spice)
      lgJoinThread(t_spice, NULL);
  }

  if (state.lgc)
  {
    state.lgc->free();

    struct CBRequest *cbr;
    while(ll_shift(state.cbRequestList, (void **)&cbr))
      free(cbr);
    ll_free(state.cbRequestList);
  }

  if (state.window)
    SDL_DestroyWindow(state.window);

  if (cursor)
    SDL_FreeCursor(cursor);

  ivshmemClose(&state.shm);

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

  DEBUG_INFO("Looking Glass (" BUILD_VERSION ")");
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

  if (params.grabKeyboard)
    SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");

  const int ret = lg_run();
  lg_shutdown();

  config_free();
  return ret;

}
