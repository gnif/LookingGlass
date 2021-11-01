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

#include "main.h"
#include "config.h"

#include <getopt.h>
#include <signal.h>
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
#include <math.h>
#include <stdatomic.h>
#include <linux/input.h>

#include "common/array.h"
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
#include "common/paths.h"
#include "common/cpuinfo.h"

#include "core.h"
#include "app.h"
#include "keybind.h"
#include "clipboard.h"
#include "kb.h"
#include "ll.h"
#include "egl_dynprocs.h"
#include "gl_dynprocs.h"
#include "overlays.h"
#include "overlay_utils.h"
#include "util.h"

// forwards
static int renderThread(void * unused);

static LGEvent  *e_startup = NULL;
static LGThread *t_spice   = NULL;
static LGThread *t_render  = NULL;

struct AppState g_state = { 0 };
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
  if ((!g_params.useSpiceInput && g_params.hideMouse) || !g_params.showCursorDot)
    g_state.ds->setPointer(LG_POINTER_NONE);
  else
    g_state.ds->setPointer(LG_POINTER_SQUARE);
}

static bool fpsTimerFn(void * unused)
{
  static uint64_t last;
  if (!last)
  {
    last = nanotime();
    return true;
  }

  const uint64_t renderCount = atomic_exchange_explicit(&g_state.renderCount, 0,
      memory_order_acquire);

  float fps, ups;
  if (renderCount > 0)
  {
    const uint64_t frameCount = atomic_exchange_explicit(&g_state.frameCount, 0,
        memory_order_acquire);

    const uint64_t time      = nanotime();
    const uint64_t elapsedNs = time - last;
    const float    elapsedMs = (float)elapsedNs / 1e6f;

    last = time;
    fps  = 1e3f / (elapsedMs / (float)renderCount);
    ups  = 1e3f / (elapsedMs / (float)frameCount);
  }
  else
  {
    last = nanotime();
    fps  = 0.0f;
    ups  = 0.0f;
  }

  atomic_store_explicit(&g_state.fps, fps, memory_order_relaxed);
  atomic_store_explicit(&g_state.ups, ups, memory_order_relaxed);

  return true;
}

static void preSwapCallback(void * udata)
{
  const uint64_t * renderStart = (const uint64_t *)udata;
  ringbuffer_push(g_state.renderDuration,
      &(float) {(nanotime() - *renderStart) * 1e-6f});
}

static int renderThread(void * unused)
{
  if (!RENDERER(renderStartup, g_state.useDMA))
  {
    DEBUG_ERROR("EGL render failed to start");
    g_state.state = APP_STATE_SHUTDOWN;

    /* unblock threads waiting on the condition */
    lgSignalEvent(e_startup);
    return 1;
  }

  if (g_state.lgr->ops.supports && !RENDERER(supports, LG_SUPPORTS_DMABUF))
    g_state.useDMA = false;

  /* start up the fps timer */
  LGTimer * fpsTimer;
  if (!lgCreateTimer(500, fpsTimerFn, NULL, &fpsTimer))
  {
    DEBUG_ERROR("Failed to create the fps timer");
    return 1;
  }

  LG_LOCK_INIT(g_state.lgrLock);

  /* signal to other threads that the renderer is ready */
  lgSignalEvent(e_startup);

  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);

  while(g_state.state != APP_STATE_SHUTDOWN)
  {
    bool forceRender = false;
    if (g_state.jitRender)
      forceRender = g_state.ds->waitFrame();

    app_handleRenderEvent(microtime());
    if (g_state.jitRender)
    {
      const uint64_t pending =
        atomic_load_explicit(&g_state.pendingCount, memory_order_acquire);
      if (!lgResetEvent(g_state.frameEvent)
          && !forceRender
          && !pending
          && !app_overlayNeedsRender()
          && !RENDERER(needsRender))
      {
        if (g_state.ds->skipFrame)
          g_state.ds->skipFrame();
        continue;
      }

      if (pending > 0)
        atomic_fetch_sub(&g_state.pendingCount, 1);
    }
    else if (g_params.fpsMin != 0)
    {
      float ups = atomic_load_explicit(&g_state.ups, memory_order_relaxed);

      if (!lgWaitEventAbs(g_state.frameEvent, &time) || ups > g_params.fpsMin)
      {
        /* only update the time if we woke up early */
        clock_gettime(CLOCK_MONOTONIC, &time);
        tsAdd(&time, g_state.overlayInput ?
            g_state.overlayFrameTime : g_state.frameTime);
      }
    }

    int resize = atomic_load(&g_state.lgrResize);
    if (resize)
    {
      g_state.io->DisplaySize = (ImVec2) {
        .x = g_state.windowW,
        .y = g_state.windowH,
      };
      g_state.io->DisplayFramebufferScale = (ImVec2) {
        .x = g_state.windowScale,
        .y = g_state.windowScale,
      };
      g_state.io->FontGlobalScale = 1.0f / g_state.windowScale;

      ImFontAtlas_Clear(g_state.io->Fonts);
      ImFontAtlas_AddFontFromFileTTF(g_state.io->Fonts, g_state.fontName,
        g_params.uiSize * g_state.windowScale, NULL, NULL);
      g_state.fontLarge = ImFontAtlas_AddFontFromFileTTF(g_state.io->Fonts,
        g_state.fontName, 1.3f * g_params.uiSize * g_state.windowScale, NULL, NULL);
      if (!ImFontAtlas_Build(g_state.io->Fonts))
        DEBUG_FATAL("Failed to build font atlas: %s (%s)", g_params.uiFont, g_state.fontName);

      if (g_state.lgr)
        RENDERER(onResize, g_state.windowW, g_state.windowH,
            g_state.windowScale, g_state.dstRect, g_params.winRotate);
      atomic_compare_exchange_weak(&g_state.lgrResize, &resize, 0);
    }

    static uint64_t lastFrameCount = 0;
    const uint64_t frameCount =
      atomic_load_explicit(&g_state.frameCount, memory_order_relaxed);
    const bool newFrame = frameCount != lastFrameCount;
    lastFrameCount = frameCount;

    const bool invalidate = atomic_exchange(&g_state.invalidateWindow, false);

    const uint64_t renderStart = nanotime();
    LG_LOCK(g_state.lgrLock);
    if (!RENDERER(render, g_params.winRotate, newFrame, invalidate,
          preSwapCallback, (void *)&renderStart))
    {
      LG_UNLOCK(g_state.lgrLock);
      break;
    }
    LG_UNLOCK(g_state.lgrLock);

    const uint64_t t     = nanotime();
    const uint64_t delta = t - g_state.lastRenderTime;

    g_state.lastRenderTime = t;
    atomic_fetch_add_explicit(&g_state.renderCount, 1, memory_order_relaxed);

    if (g_state.lastRenderTimeValid)
    {
      const float fdelta = (float)delta / 1e6f;
      ringbuffer_push(g_state.renderTimings, &fdelta);
    }
    g_state.lastRenderTimeValid = true;

    const uint64_t now = microtime();
    if (!g_state.resizeDone && g_state.resizeTimeout < now)
    {
      if (g_params.autoResize)
      {
        g_state.ds->setWindowSize(
          g_state.dstRect.w,
          g_state.dstRect.h
        );
      }
      g_state.resizeDone = true;
    }
  }

  g_state.state = APP_STATE_SHUTDOWN;

  lgTimerDestroy(fpsTimer);

  core_stopCursorThread();
  core_stopFrameThread();

  RENDERER(deinitialize);
  g_state.lgr = NULL;
  LG_LOCK_FREE(g_state.lgrLock);

  return 0;
}

int main_cursorThread(void * unused)
{
  LGMP_STATUS         status;
  LG_RendererCursor   cursorType = LG_CURSOR_COLOR;
  KVMFRCursor *       cursor     = NULL;
  int                 cursorSize = 0;

  lgWaitEvent(e_startup, TIMEOUT_INFINITE);

  // subscribe to the pointer queue
  while(g_state.state == APP_STATE_RUNNING)
  {
    status = lgmpClientSubscribe(g_state.lgmp, LGMP_Q_POINTER,
        &g_state.pointerQueue);
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
    if ((status = lgmpClientProcess(g_state.pointerQueue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
      {
        if (g_cursor.redraw && g_cursor.guest.valid)
        {
          g_cursor.redraw = false;
          RENDERER(onMouseEvent,
            g_cursor.guest.visible && (g_cursor.draw || !g_params.useSpiceInput),
            g_cursor.guest.x,
            g_cursor.guest.y
          );

          if (!g_state.stopVideo)
            lgSignalEvent(g_state.frameEvent);
        }

        struct timespec req =
        {
          .tv_sec  = 0,
          .tv_nsec = g_params.cursorPollInterval * 1000L
        };

        struct timespec rem;
        while(nanosleep(&req, &rem) < 0)
        {
          if (errno != -EINTR)
          {
            DEBUG_ERROR("nanosleep failed");
            break;
          }
          req = rem;
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

    if (cursor && msg.size > cursorSize)
    {
      free(cursor);
      cursor = NULL;
    }

    /* copy and release the message ASAP */
    if (!cursor)
    {
      cursor = malloc(msg.size);
      if (!cursor)
      {
        DEBUG_ERROR("failed to allocate %d bytes for cursor", msg.size);
        g_state.state = APP_STATE_SHUTDOWN;
        break;
      }
      cursorSize = msg.size;
    }

    memcpy(cursor, msg.mem, msg.size);
    lgmpClientMessageDone(g_state.pointerQueue);

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
          lgmpClientMessageDone(g_state.pointerQueue);
          continue;
      }

      g_cursor.guest.hx = cursor->hx;
      g_cursor.guest.hy = cursor->hy;

      const uint8_t * data = (const uint8_t *)(cursor + 1);
      if (!RENDERER(onMouseShape,
        cursorType,
        cursor->width,
        cursor->height,
        cursor->pitch,
        data)
      )
      {
        DEBUG_ERROR("Failed to update mouse shape");
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
      if (valid != true && core_inputEnabled())
      {
        core_alignToGuest();
        app_resyncMouseBasic();
      }

      // tell the DS there was an update
      core_handleGuestMouseUpdate();
    }

    g_cursor.redraw = false;

    RENDERER(onMouseEvent,
      g_cursor.guest.visible && (g_cursor.draw || !g_params.useSpiceInput),
      g_cursor.guest.x,
      g_cursor.guest.y
    );

    if (g_params.mouseRedraw && g_cursor.guest.visible && !g_state.stopVideo)
      lgSignalEvent(g_state.frameEvent);
  }

  lgmpClientUnsubscribe(&g_state.pointerQueue);


  if (cursor)
  {
    free(cursor);
    cursor = NULL;
  }

  return 0;
}

int main_frameThread(void * unused)
{
  struct DMAFrameInfo
  {
    KVMFRFrame * frame;
    size_t       dataSize;
    int          fd;
  };

  LGMP_STATUS      status;
  PLGMPClientQueue queue;

  uint32_t          frameSerial = 0;
  uint32_t          formatVer   = 0;
  size_t            dataSize    = 0;
  LG_RendererFormat lgrFormat;

  struct DMAFrameInfo dmaInfo[LGMP_Q_FRAME_LEN] = {0};
  if (g_state.useDMA)
    DEBUG_INFO("Using DMA buffer support");

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
        struct timespec req =
        {
          .tv_sec  = 0,
          .tv_nsec = g_params.framePollInterval * 1000L
        };

        struct timespec rem;
        while(nanosleep(&req, &rem) < 0)
        {
          if (errno != -EINTR)
          {
            DEBUG_ERROR("nanosleep failed");
            break;
          }
          req = rem;
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

    KVMFRFrame * frame = (KVMFRFrame *)msg.mem;

    // ignore any repeated frames, this happens when a new client connects to
    // the same host application.
    if (frame->frameSerial == frameSerial && g_state.formatValid)
    {
      lgmpClientMessageDone(queue);
      continue;
    }
    frameSerial = frame->frameSerial;

    struct DMAFrameInfo *dma = NULL;

    if (!g_state.formatValid || frame->formatVer != formatVer)
    {
      // setup the renderer format with the frame format details
      lgrFormat.type   = frame->type;
      lgrFormat.width  = frame->width;
      lgrFormat.height = frame->height;
      lgrFormat.stride = frame->stride;
      lgrFormat.pitch  = frame->pitch;

      if (frame->height != frame->realHeight)
      {
        const float needed =
          ((frame->realHeight * frame->pitch * 2) / 1048576.0f) + 10.0f;
        const int   size   = (int)powf(2.0f, ceilf(logf(needed) / logf(2.0f)));

        DEBUG_BREAK();
        DEBUG_WARN("IVSHMEM too small, screen truncated");
        DEBUG_WARN("Recommend increase size to %d MiB", size);
        DEBUG_BREAK();

        app_alert(LG_ALERT_ERROR,
          "IVSHMEM too small, screen truncated\n"
          "Recommend increasing size to %d MiB",
          size);
      }

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

      LG_LOCK(g_state.lgrLock);
      if (!RENDERER(onFrameFormat, lgrFormat))
      {
        DEBUG_ERROR("renderer failed to configure format");
        g_state.state = APP_STATE_SHUTDOWN;
        LG_UNLOCK(g_state.lgrLock);
        break;
      }
      LG_UNLOCK(g_state.lgrLock);

      g_state.srcSize.x = lgrFormat.width;
      g_state.srcSize.y = lgrFormat.height;
      g_state.haveSrcSize = true;
      if (g_params.autoResize)
        g_state.ds->setWindowSize(lgrFormat.width, lgrFormat.height);

      core_updatePositionInfo();
    }

    if (g_state.useDMA)
    {
      /* find the existing dma buffer if it exists */
      for(int i = 0; i < ARRAY_LENGTH(dmaInfo); ++i)
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
        for(int i = 0; i < ARRAY_LENGTH(dmaInfo); ++i)
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
    if (!RENDERER(onFrame, fb, g_state.useDMA ? dma->fd : -1,
          frame->damageRects, frame->damageRectsCount))
    {
      lgmpClientMessageDone(queue);
      DEBUG_ERROR("renderer on frame returned failure");
      g_state.state = APP_STATE_SHUTDOWN;
      break;
    }

    if (g_params.autoScreensaver && g_state.autoIdleInhibitState != frame->blockScreensaver)
    {
      if (frame->blockScreensaver)
        g_state.ds->inhibitIdle();
      else
        g_state.ds->uninhibitIdle();
      g_state.autoIdleInhibitState = frame->blockScreensaver;
    }

    const uint64_t t      = nanotime();
    const uint64_t delta  = t - g_state.lastFrameTime;
    g_state.lastFrameTime = t;

    if (g_state.lastFrameTimeValid)
      ringbuffer_push(g_state.uploadTimings, &(float) { delta * 1e-6f });
    g_state.lastFrameTimeValid = true;

    atomic_fetch_add_explicit(&g_state.frameCount, 1, memory_order_relaxed);
    if (g_state.jitRender)
    {
      if (atomic_load_explicit(&g_state.pendingCount, memory_order_acquire) < 10)
        atomic_fetch_add_explicit(&g_state.pendingCount, 1,
            memory_order_release);
    }
    else
      lgSignalEvent(g_state.frameEvent);

    lgmpClientMessageDone(queue);
  }

  lgmpClientUnsubscribe(&queue);
  RENDERER(onRestart);

  if (g_state.useDMA)
  {
    for(int i = 0; i < ARRAY_LENGTH(dmaInfo); ++i)
      if (dmaInfo[i].fd >= 0)
        close(dmaInfo[i].fd);
  }


  return 0;
}

int spiceThread(void * arg)
{
  while(g_state.state != APP_STATE_SHUTDOWN)
    if (!spice_process(100))
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

void intHandler(int sig)
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

static bool tryRenderer(const int index, const LG_RendererParams lgrParams,
    bool * needsOpenGL)
{
  const LG_RendererOps *r = LG_Renderers[index];

  if (!IS_LG_RENDERER_VALID(r))
  {
    DEBUG_ERROR("FIXME: Renderer %d is invalid, skipping", index);
    return false;
  }

  // create the renderer
  g_state.lgr  = NULL;
  *needsOpenGL = false;
  if (!r->create(&g_state.lgr, lgrParams, needsOpenGL))
  {
    g_state.lgr = NULL;
    return false;
  }

  // init the ops member
  memcpy(&g_state.lgr->ops, r, sizeof(*r));

  // initialize the renderer
  if (!r->initialize(g_state.lgr))
  {
    r->deinitialize(g_state.lgr);
    g_state.lgr = NULL;
    return false;
  }

  DEBUG_INFO("Using Renderer: %s", r->getName());
  return true;
}

static void reportBadVersion()
{
  DEBUG_BREAK();
  DEBUG_ERROR("The host application is not compatible with this client");
  DEBUG_ERROR("This is not a Looking Glass error, do not report this");
  DEBUG_ERROR("Please install the matching host application for this client");
}

static int lg_run(void)
{
  g_cursor.sens = g_params.mouseSens;
       if (g_cursor.sens < -9) g_cursor.sens = -9;
  else if (g_cursor.sens >  9) g_cursor.sens =  9;

  /* setup imgui */
  igCreateContext(NULL);
  g_state.io    = igGetIO();
  g_state.style = igGetStyle();

  g_state.style->Colors[ImGuiCol_ModalWindowDimBg] = (ImVec4) { 0.0f, 0.0f, 0.0f, 0.4f };

  alloc_sprintf(&g_state.imGuiIni, "%s/imgui.ini", lgConfigDir());
  g_state.io->IniFilename   = g_state.imGuiIni;
  g_state.io->BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

  g_state.windowScale = 1.0;
  if (util_initUIFonts())
  {
    g_state.fontName = util_getUIFont(g_params.uiFont);
    DEBUG_INFO("Using font: %s", g_state.fontName);
  }

  app_initOverlays();

  // initialize metrics ringbuffers
  g_state.renderTimings  = ringbuffer_new(256, sizeof(float));
  g_state.uploadTimings  = ringbuffer_new(256, sizeof(float));
  g_state.renderDuration = ringbuffer_new(256, sizeof(float));
  overlayGraph_register("FRAME" , g_state.renderTimings , 0.0f, 50.0f);
  overlayGraph_register("UPLOAD", g_state.uploadTimings , 0.0f, 50.0f);
  overlayGraph_register("RENDER", g_state.renderDuration, 0.0f, 10.0f);

  initImGuiKeyMap(g_state.io->KeyMap);

  // search for the best displayserver ops to use
  for(int i = 0; i < LG_DISPLAYSERVER_COUNT; ++i)
    if (LG_DisplayServers[i]->probe())
    {
      g_state.ds = LG_DisplayServers[i];
      break;
    }

  DEBUG_ASSERT(g_state.ds);
  ASSERT_LG_DS_VALID(g_state.ds);

  if (g_params.jitRender)
  {
    if (g_state.ds->waitFrame)
      g_state.jitRender = true;
    else
      DEBUG_WARN("JIT render not supported on display server backend, disabled");
  }

  // init the subsystem
  if (!g_state.ds->earlyInit())
  {
    DEBUG_ERROR("Subsystem early init failed");
    return -1;
  }

  // override the SIGINIT handler so that we can tell the difference between
  // SIGINT and the user sending a close event, such as ALT+F4
  signal(SIGINT , intHandler);
  signal(SIGTERM, intHandler);

  // try map the shared memory
  if (!ivshmemOpen(&g_state.shm))
  {
    DEBUG_ERROR("Failed to map memory");
    return -1;
  }

  // try to connect to the spice server
  if (g_params.useSpiceInput || g_params.useSpiceClipboard)
  {
    if (g_params.useSpiceClipboard)
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
  bool needsOpenGL;
  LG_RendererParams lgrParams;
  lgrParams.quickSplash = g_params.quickSplash;

  if (g_params.forceRenderer)
  {
    DEBUG_INFO("Trying forced renderer");
    if (!tryRenderer(g_params.forceRendererIndex, lgrParams, &needsOpenGL))
    {
      DEBUG_ERROR("Forced renderer failed to iniailize");
      return -1;
    }
  }
  else
  {
    // probe for a a suitable renderer
    for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    {
      if (tryRenderer(i, lgrParams, &needsOpenGL))
        break;
    }
  }

  if (!g_state.lgr)
  {
    DEBUG_INFO("Unable to find a suitable renderer");
    return -1;
  }

  g_state.useDMA =
    g_params.allowDMA &&
    ivshmemHasDMA(&g_state.shm);

  // initialize the window dimensions at init for renderers
  g_state.windowW  = g_params.w;
  g_state.windowH  = g_params.h;
  g_state.windowCX = g_params.w / 2;
  g_state.windowCY = g_params.h / 2;
  core_updatePositionInfo();

  const LG_DSInitParams params =
  {
    .title               = g_params.windowTitle,
    .x                   = g_params.x,
    .y                   = g_params.y,
    .w                   = g_params.w,
    .h                   = g_params.h,
    .center              = g_params.center,
    .fullscreen          = g_params.fullscreen,
    .resizable           = g_params.allowResize,
    .borderless          = g_params.borderless,
    .maximize            = g_params.maximize,
    .opengl              = needsOpenGL,
    .jitRender           = g_params.jitRender
  };

  g_state.dsInitialized = g_state.ds->init(params);
  if (!g_state.dsInitialized)
  {
    DEBUG_ERROR("Failed to initialize the displayserver backend");
    return -1;
  }

  if (g_params.noScreensaver)
    g_state.ds->inhibitIdle();

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

  // when the overlay is shown we should run at a minimum of 60 fps for
  // interactivity.
  g_state.overlayFrameTime = min(g_state.frameTime, 1000000000ULL / 60ULL);

  keybind_register();

  // setup the startup condition
  if (!(e_startup = lgCreateEvent(false, 0)))
  {
    DEBUG_ERROR("failed to create the startup event");
    return -1;
  }

  // setup the new frame event
  if (!(g_state.frameEvent = lgCreateEvent(!g_state.jitRender, 0)))
  {
    DEBUG_ERROR("failed to create the frame event");
    return -1;
  }

  if (g_state.jitRender)
    DEBUG_INFO("Using JIT render mode");

  lgInit();

  // start the renderThread so we don't just display junk
  if (!lgCreateThread("renderThread", renderThread, NULL, &t_render))
  {
    DEBUG_ERROR("render create thread failed");
    return -1;
  }

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
  g_state.ds->wait(200);

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

    if (status == LGMP_ERR_INVALID_VERSION)
    {
      reportBadVersion();
      DEBUG_INFO("Waiting for you to upgrade the host application");
      while (g_state.state == APP_STATE_RUNNING &&
          lgmpClientSessionInit(g_state.lgmp, &udataSize, (uint8_t **)&udata) != LGMP_OK)
        g_state.ds->wait(1000);

      if (g_state.state != APP_STATE_RUNNING)
        return -1;

      break;
    }

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
      DEBUG_INFO("Check the host log in your guest at %%ProgramData%%\\Looking Glass (host)\\looking-glass-host.txt");
      DEBUG_INFO("Continuing to wait...");
    }

    g_state.ds->wait(1000);
  }

  if (g_state.state != APP_STATE_RUNNING)
    return -1;

  // dont show warnings again after the first startup
  waitCount = 100;

  const bool magicMatches = memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) == 0;
  if (udataSize != sizeof(KVMFR) || !magicMatches || udata->version != KVMFR_VERSION)
  {
    reportBadVersion();
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
        g_state.ds->wait(1000);

      if (g_state.state != APP_STATE_RUNNING)
        return -1;

      goto restart;
    }
    else
      return -1;
  }

  DEBUG_INFO("Host ready, reported version: %s", udata->hostver);
  DEBUG_INFO("Starting session");

  g_state.kvmfrFeatures = udata->features;

  if (!core_startCursorThread() || !core_startFrameThread())
    return -1;

  while(g_state.state == APP_STATE_RUNNING)
  {
    if (!lgmpClientSessionValid(g_state.lgmp))
    {
      g_state.state = APP_STATE_RESTART;
      break;
    }
    g_state.ds->wait(100);
  }

  if (g_state.state == APP_STATE_RESTART)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(g_state.frameEvent);

    core_stopFrameThread();
    core_stopCursorThread();

    lgInit();

    RENDERER(onRestart);

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
    if (g_state.jitRender && g_state.ds->stopWaitFrame)
      g_state.ds->stopWaitFrame();
    lgSignalEvent(e_startup);
    lgSignalEvent(g_state.frameEvent);
    lgJoinThread(t_render, NULL);
  }

  lgmpClientFree(&g_state.lgmp);

  if (g_state.frameEvent)
  {
    lgFreeEvent(g_state.frameEvent);
    g_state.frameEvent = NULL;
  }

  if (e_startup)
  {
    lgFreeEvent(e_startup);
    e_startup = NULL;
  }

  // if spice is still connected send key up events for any pressed keys
  if (g_params.useSpiceInput && spice_ready())
  {
    for(int scancode = 0; scancode < KEY_MAX; ++scancode)
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

  app_releaseAllKeybinds();

  if (g_state.dsInitialized)
    g_state.ds->free();

  if (g_state.overlays)
  {
    app_freeOverlays();
    ll_free(g_state.overlays);
    g_state.overlays = NULL;
  }

  ivshmemClose(&g_state.shm);

  // free metrics ringbuffers
  ringbuffer_free(&g_state.renderTimings);
  ringbuffer_free(&g_state.uploadTimings);
  ringbuffer_free(&g_state.renderDuration);

  free(g_state.fontName);
  igDestroyContext(NULL);
  free(g_state.imGuiIni);
}

int main(int argc, char * argv[])
{
  // initialize for DEBUG_* macros
  debug_init();

  if (getuid() == 0)
  {
    DEBUG_ERROR("Do not run looking glass as root!");
    return -1;
  }

  if (getuid() != geteuid())
  {
    DEBUG_ERROR("Do not run looking glass as setuid!");
    return -1;
  }

  DEBUG_INFO("Looking Glass (%s)", BUILD_VERSION);
  DEBUG_INFO("Locking Method: " LG_LOCK_MODE);
  lgDebugCPU();

  if (!installCrashHandler("/proc/self/exe"))
    DEBUG_WARN("Failed to install the crash handler");

  lgPathsInit("looking-glass");
  config_init();
  ivshmemOptionsInit();
  egl_dynProcsInit();
  gl_dynProcsInit();

  g_state.overlays = ll_new();
  app_registerOverlay(&LGOverlayConfig, NULL);
  app_registerOverlay(&LGOverlayAlert , NULL);
  app_registerOverlay(&LGOverlayFPS   , NULL);
  app_registerOverlay(&LGOverlayGraphs, NULL);
  app_registerOverlay(&LGOverlayHelp  , NULL);

  // early renderer setup for option registration
  for(unsigned int i = 0; i < LG_RENDERER_COUNT; ++i)
    LG_Renderers[i]->setup();

  for(unsigned int i = 0; i < LG_DISPLAYSERVER_COUNT; ++i)
    LG_DisplayServers[i]->setup();

  if (!config_load(argc, argv))
    return -1;

  const int ret = lg_run();
  lg_shutdown();

  config_free();

  util_freeUIFonts();
  cleanupCrashHandler();
  return ret;
}
