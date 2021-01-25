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

#include "core.h"
#include "app.h"
#include "util.h"
#include "clipboard.h"
#include "ll.h"
#include "kb.h"

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
struct AppParams g_params = { 0 };

static void lgInit(void)
{
  g_state.state         = APP_STATE_RUNNING;
  g_state.formatValid   = false;
  g_state.resizeDone    = true;

  if (g_cursor.grab)
    core_setGrab(false);

  g_cursor.useScale      = false;
  g_cursor.scale.x       = 1.0;
  g_cursor.scale.y       = 1.0;
  g_cursor.draw          = false;
  g_cursor.inView        = false;
  g_cursor.guest.valid   = false;

  // if spice is not in use, hide the local cursor
  if (!app_inputEnabled() && g_params.hideMouse)
    SDL_ShowCursor(SDL_DISABLE);
  else
    SDL_ShowCursor(SDL_ENABLE);
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
    if (g_params.fpsMin != 0)
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
            g_state.dstRect, g_params.winRotate);
      atomic_compare_exchange_weak(&g_state.lgrResize, &resize, 0);
    }

    if (!g_state.lgr->render(g_state.lgrData, g_state.window, g_params.winRotate))
      break;

    if (g_params.showFPS)
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
            g_cursor.guest.visible && (g_cursor.draw || !g_params.useSpiceInput),
            g_cursor.guest.x,
            g_cursor.guest.y
          );

          lgSignalEvent(e_frame);
        }

        const struct timespec req =
        {
          .tv_sec  = 0,
          .tv_nsec = g_params.cursorPollInterval * 1000L
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
        core_alignToGuest();
    }

    lgmpClientMessageDone(queue);
    g_cursor.redraw = false;

    g_state.lgr->on_mouse_event
    (
      g_state.lgrData,
      g_cursor.guest.visible && (g_cursor.draw || !g_params.useSpiceInput),
      g_cursor.guest.x,
      g_cursor.guest.y
    );

    if (g_params.mouseRedraw && g_cursor.guest.visible)
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
    g_params.allowDMA &&
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
          .tv_nsec = g_params.framePollInterval * 1000L
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
      if (g_params.autoResize)
        SDL_SetWindowSize(g_state.window, lgrFormat.width, lgrFormat.height);

      g_cursor.guest.dpiScale = frame->mouseScalePercent;
      core_updatePositionInfo();
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

int eventFilter(void * userdata, SDL_Event * event)
{
  if (g_state.ds->eventFilter(event))
    return 0;

  // always include the default handler (SDL) for any unhandled events
  if (g_state.ds != LG_DisplayServers[0])
    if (LG_DisplayServers[0]->eventFilter(event))
      return 0;

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
  SDL_SetWindowFullscreen(g_state.window, g_params.fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
  g_params.fullscreen = !g_params.fullscreen;
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
  if (g_params.winRotate == LG_ROTATE_MAX-1)
    g_params.winRotate = 0;
  else
    ++g_params.winRotate;
  core_updatePositionInfo();
}

static void toggle_input(uint32_t scancode, void * opaque)
{
  g_state.ignoreInput = !g_state.ignoreInput;

  if (g_state.ignoreInput)
    core_setCursorInView(false);
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
  app_registerKeybind(KEY_F, toggle_fullscreen, NULL);
  app_registerKeybind(KEY_V, toggle_video     , NULL);
  app_registerKeybind(KEY_R, toggle_rotate    , NULL);
  app_registerKeybind(KEY_Q, quit             , NULL);

  if (g_params.useSpiceInput)
  {
    app_registerKeybind(KEY_I     , toggle_input     , NULL);
    app_registerKeybind(KEY_INSERT, mouse_sens_inc   , NULL);
    app_registerKeybind(KEY_DELETE, mouse_sens_dec   , NULL);

    app_registerKeybind(KEY_F1 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F2 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F3 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F4 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F5 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F6 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F7 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F8 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F9 , ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F10, ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F11, ctrl_alt_fn, NULL);
    app_registerKeybind(KEY_F12, ctrl_alt_fn, NULL);

    app_registerKeybind(KEY_LEFTMETA , key_passthrough, NULL);
    app_registerKeybind(KEY_RIGHTMETA, key_passthrough, NULL);
  }
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

  g_cursor.sens = g_params.mouseSens;
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
  SET_FALLBACK(showPointer);
  SET_FALLBACK(grabPointer);
  SET_FALLBACK(ungrabKeyboard);
  SET_FALLBACK(warpPointer);
  SET_FALLBACK(realignPointer);
  SET_FALLBACK(isValidPointerPos);
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
  if (g_params.useSpiceInput || g_params.useSpiceClipboard)
  {
    spice_set_clipboard_cb(
        cb_spiceNotice,
        cb_spiceData,
        cb_spiceRelease,
        cb_spiceRequest);

    if (!spice_connect(g_params.spiceHost, g_params.spicePort, ""))
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
  lgrParams.showFPS     = g_params.showFPS;
  lgrParams.quickSplash = g_params.quickSplash;
  Uint32 sdlFlags;

  if (g_params.forceRenderer)
  {
    DEBUG_INFO("Trying forced renderer");
    sdlFlags = 0;
    if (!try_renderer(g_params.forceRendererIndex, lgrParams, &sdlFlags))
    {
      DEBUG_ERROR("Forced renderer failed to iniailize");
      return -1;
    }
    g_state.lgr = LG_Renderers[g_params.forceRendererIndex];
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
    g_params.windowTitle,
    g_params.center ? SDL_WINDOWPOS_CENTERED : g_params.x,
    g_params.center ? SDL_WINDOWPOS_CENTERED : g_params.y,
    g_params.w,
    g_params.h,
    (
      SDL_WINDOW_HIDDEN |
      (g_params.allowResize ? SDL_WINDOW_RESIZABLE  : 0) |
      (g_params.borderless  ? SDL_WINDOW_BORDERLESS : 0) |
      (g_params.maximize    ? SDL_WINDOW_MAXIMIZED  : 0) |
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
      g_params.minimizeOnFocusLoss ? "1" : "0");

  if (g_params.fullscreen)
    SDL_SetWindowFullscreen(g_state.window, SDL_WINDOW_FULLSCREEN_DESKTOP);

  if (!g_params.center)
    SDL_SetWindowPosition(g_state.window, g_params.x, g_params.y);

  if (g_params.noScreensaver)
    g_state.ds->inhibitIdle();

  // ensure the initial window size is stored in the state
  SDL_GetWindowSize(g_state.window, &g_state.windowW, &g_state.windowH);

  // ensure renderer viewport is aware of the current window size
  core_updatePositionInfo();

  if (g_params.fpsMin <= 0)
  {
    // default 30 fps
    g_state.frameTime = 1000000000ULL / 30ULL;
  }
  else
  {
    DEBUG_INFO("Using the FPS minimum from args: %d", g_params.fpsMin);
    g_state.frameTime = 1000000000ULL / (unsigned long long)g_params.fpsMin;
  }

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

  if (g_params.captureOnStart)
    core_setGrab(true);

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
  if (g_params.useSpiceInput && spice_ready())
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

  // this must run last to ensure that we don't free any pointers still in use
  app_releaseAllKeybinds();

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

  cleanupCrashHandler();
  return ret;
}
