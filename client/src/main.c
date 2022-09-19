/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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
#include "common/ll.h"

#include "core.h"
#include "app.h"
#include "audio.h"
#include "keybind.h"
#include "clipboard.h"
#include "kb.h"
#include "egl_dynprocs.h"
#include "gl_dynprocs.h"
#include "overlays.h"
#include "overlay_utils.h"
#include "util.h"
#include "render_queue.h"

// forwards
static int renderThread(void * unused);

static LGEvent  *e_startup = NULL;
static LGEvent  *e_spice   = NULL;
static LGThread *t_spice   = NULL;
static LGThread *t_render  = NULL;

struct AppState g_state = { 0 };
struct CursorState g_cursor;

// this structure is initialized in config.c
struct AppParams g_params = { 0 };

static void lgInit(void)
{
  g_state.formatValid   = false;
  g_state.resizeDone    = true;

  core_setCursorInView(false);
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

static bool tickTimerFn(void * unused)
{
  static unsigned long long tickCount = 0;

  bool needsRender = false;
  struct Overlay * overlay;
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    if (overlay->ops->tick && overlay->ops->tick(overlay->udata, tickCount))
      needsRender = true;
  }
  ll_unlock(g_state.overlays);

  if (needsRender)
    app_invalidateWindow(false);

  ++tickCount;
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

  app_initOverlays();
  LGTimer * tickTimer;
  if (!lgCreateTimer(1000 / TICK_RATE, tickTimerFn, NULL, &tickTimer))
  {
    lgTimerDestroy(fpsTimer);
    DEBUG_ERROR("Failed to create the tick timer");
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
          && !app_overlayNeedsRender())
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
        tsAdd(&time, app_isOverlayMode() ?
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
        g_params.uiSize * g_state.windowScale, NULL, g_state.fontRange.Data);
      g_state.fontLarge = ImFontAtlas_AddFontFromFileTTF(g_state.io->Fonts,
        g_state.fontName, 1.3f * g_params.uiSize * g_state.windowScale, NULL, g_state.fontRange.Data);
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

    renderQueue_process();

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

  if (g_state.overlays)
  {
    app_freeOverlays();
    ll_free(g_state.overlays);
    g_state.overlays = NULL;
  }

  lgTimerDestroy(tickTimer);
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
            g_cursor.guest.y,
            g_cursor.guest.hx,
            g_cursor.guest.hy
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

    KVMFRCursor * tmp = (KVMFRCursor *)msg.mem;
    const int neededSize = sizeof(*tmp) +
      (msg.udata & CURSOR_FLAG_SHAPE ? tmp->height * tmp->pitch : 0);

    if (cursor && neededSize > cursorSize)
    {
      free(cursor);
      cursor = NULL;
    }

    /* copy and release the message ASAP */
    if (!cursor)
    {
      cursor = malloc(neededSize);
      if (!cursor)
      {
        DEBUG_ERROR("failed to allocate %d bytes for cursor", neededSize);
        g_state.state = APP_STATE_SHUTDOWN;
        break;
      }
      cursorSize = neededSize;
    }

    memcpy(cursor, msg.mem, neededSize);
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
      g_cursor.guest.y,
      g_cursor.guest.hx,
      g_cursor.guest.hy
    );

    if (g_params.mouseRedraw && g_cursor.guest.visible && !g_state.stopVideo)
      lgSignalEvent(g_state.frameEvent);
  }

  LG_LOCK(g_state.pointerQueueLock);
  lgmpClientUnsubscribe(&g_state.pointerQueue);
  LG_UNLOCK(g_state.pointerQueueLock);

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

  g_state.ds->requestActivation();

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
      lgrFormat.type         = frame->type;
      lgrFormat.screenWidth  = frame->screenWidth;
      lgrFormat.screenHeight = frame->screenHeight;
      lgrFormat.frameWidth   = frame->frameWidth;
      lgrFormat.frameHeight  = frame->frameHeight;
      lgrFormat.stride       = frame->stride;
      lgrFormat.pitch        = frame->pitch;

      if (frame->flags & FRAME_FLAG_TRUNCATED)
      {
        const float needed =
          ((frame->screenHeight * frame->pitch * 2) / 1048576.0f) + 10.0f;
        const int   size   = (int)powf(2.0f, ceilf(logf(needed) / logf(2.0f)));

        DEBUG_BREAK();
        DEBUG_WARN("IVSHMEM too small, screen truncated");
        DEBUG_WARN("Recommend increase size to %d MiB", size);
        DEBUG_BREAK();

        app_msgBox(
          "IVSHMEM too small",
          "IVSHMEM too small\n"
          "Please increase the size to %d MiB",
          size);
      }

      switch(frame->rotation)
      {
        case FRAME_ROT_0  : lgrFormat.rotate = LG_ROTATE_0  ; break;
        case FRAME_ROT_90 : lgrFormat.rotate = LG_ROTATE_90 ; break;
        case FRAME_ROT_180: lgrFormat.rotate = LG_ROTATE_180; break;
        case FRAME_ROT_270: lgrFormat.rotate = LG_ROTATE_270; break;

        default:
          DEBUG_ERROR("Unsupported/invalid frame rotation");
          lgrFormat.rotate = LG_ROTATE_0;
          break;
      }
      g_state.rotate = lgrFormat.rotate;

      bool error = false;
      switch(frame->type)
      {
        case FRAME_TYPE_RGBA:
        case FRAME_TYPE_BGRA:
        case FRAME_TYPE_RGBA10:
          dataSize       = lgrFormat.frameHeight * lgrFormat.pitch;
          lgrFormat.bpp  = 32;
          break;

        case FRAME_TYPE_RGBA16F:
          dataSize       = lgrFormat.frameHeight * lgrFormat.pitch;
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
          frame->frameWidth, frame->frameHeight,
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

      g_state.srcSize.x = lgrFormat.screenWidth;
      g_state.srcSize.y = lgrFormat.screenHeight;
      g_state.haveSrcSize = true;
      if (g_params.autoResize)
        g_state.ds->setWindowSize(lgrFormat.frameWidth, lgrFormat.frameHeight);

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
        const uintptr_t offset = (uintptr_t)frame->offset + sizeof(FrameBuffer);

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

    overlaySplash_show(false);

    if (frame->flags & FRAME_FLAG_REQUEST_ACTIVATION)
      g_state.ds->requestActivation();

    const bool blockScreensaver = frame->flags & FRAME_FLAG_BLOCK_SCREENSAVER;
    if (g_params.autoScreensaver && g_state.autoIdleInhibitState != blockScreensaver)
    {
      if (blockScreensaver)
        g_state.ds->inhibitIdle();
      else
        g_state.ds->uninhibitIdle();
      g_state.autoIdleInhibitState = blockScreensaver;
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

    // switch over to the LG stream
    app_useSpiceDisplay(false);
  }

  lgmpClientUnsubscribe(&queue);

  RENDERER(onRestart);

  if (g_state.state != APP_STATE_SHUTDOWN)
  {
    if (!app_useSpiceDisplay(true))
      overlaySplash_show(true);
  }

  if (g_state.useDMA)
  {
    for(int i = 0; i < ARRAY_LENGTH(dmaInfo); ++i)
      if (dmaInfo[i].fd >= 0)
        close(dmaInfo[i].fd);
  }

  return 0;
}

static void checkUUID(void)
{
  if (!g_state.spiceReady || !g_state.guestUUIDValid)
    return;

  if (memcmp(g_state.spiceUUID, g_state.guestUUID,
        sizeof(g_state.spiceUUID)) == 0)
    return;

  app_msgBox(
      "SPICE Configuration Error",
      "You have connected SPICE to the wrong guest.\n"
      "Input will not function until this is corrected.");

  g_params.useSpiceInput = false;
  g_state.spiceClose = true;
  purespice_disconnect();
}

void spiceReady(void)
{
  g_state.spiceReady = true;
  if (g_state.initialSpiceDisplay)
    app_useSpiceDisplay(true);

  // set the intial mouse mode
  purespice_mouseMode(true);

  PSServerInfo info;
  if (!purespice_getServerInfo(&info))
    return;

  bool uuidValid = false;
  for(int i = 0; i < sizeof(info.uuid); ++i)
    if (info.uuid[i])
    {
      uuidValid = true;
      break;
    }

  if (uuidValid)
  {
    memcpy(g_state.spiceUUID, info.uuid, sizeof(g_state.spiceUUID));
    checkUUID();
  }
  purespice_freeServerInfo(&info);

  if (g_params.useSpiceInput)
    keybind_spiceRegister();

  lgSignalEvent(e_spice);
}

static void spice_surfaceCreate(unsigned int surfaceId, PSSurfaceFormat format,
    unsigned int width, unsigned int height)
{
  DEBUG_INFO("Create SPICE surface: id: %d, size: %dx%d",
      surfaceId, width, height);

  g_state.srcSize.x   = width;
  g_state.srcSize.y   = height;
  g_state.haveSrcSize = true;
  core_updatePositionInfo();

  renderQueue_spiceConfigure(width, height);
  renderQueue_spiceDrawFill(0, 0, width, height, 0x0);
}

static void spice_surfaceDestroy(unsigned int surfaceId)
{
  DEBUG_INFO("Destroy spice surface %d", surfaceId);
  app_useSpiceDisplay(false);
}

static void spice_drawFill(unsigned int surfaceId, int x, int y, int width,
    int height, uint32_t color)
{
  renderQueue_spiceDrawFill(x, y, width, height, color);
}

static void spice_drawBitmap(unsigned int surfaceId, PSBitmapFormat format,
    bool topDown, int x, int y, int width, int height, int stride, void * data)
{
  renderQueue_spiceDrawBitmap(x, y, width, height, stride, data, topDown);
}

static void spice_setCursorRGBAImage(int width, int height, int hx, int hy,
    const void * data)
{
  g_state.spiceHotX = hx;
  g_state.spiceHotY = hy;

  void * copied = malloc(width * height * 4);
  memcpy(copied, data, width * height * 4);
  renderQueue_cursorImage(false, width, height, width * 4, copied);
}

static void spice_setCursorMonoImage(int width, int height, int hx, int hy,
    const void * xorMask, const void * andMask)
{
  g_state.spiceHotX = hx;
  g_state.spiceHotY = hy;

  int stride = (width + 7) / 8;
  uint8_t * buffer = malloc(stride * height * 2);
  memcpy(buffer, xorMask, stride * height);
  memcpy(buffer + stride * height, andMask, stride * height);
  renderQueue_cursorImage(true, width, height * 2, stride, buffer);
}

static void spice_setCursorState(bool visible, int x, int y)
{
  renderQueue_cursorState(visible, x, y, g_state.spiceHotX, g_state.spiceHotY);
}

int spiceThread(void * arg)
{
  if (g_params.useSpiceAudio)
    audio_init();

  const struct PSConfig config =
  {
    .host      = g_params.spiceHost,
    .port      = g_params.spicePort,
    .password  = "",
    .ready     = spiceReady,
    .inputs    =
    {
      .enable      = g_params.useSpiceInput,
      .autoConnect = true
    },
    .clipboard =
    {
      .enable  = g_params.useSpiceClipboard,
      .notice  = cb_spiceNotice,
      .data    = cb_spiceData,
      .release = cb_spiceRelease,
      .request = cb_spiceRequest
    },
    .display  =
    {
      .enable         = true,
      .autoConnect    = false,
      .surfaceCreate  = spice_surfaceCreate,
      .surfaceDestroy = spice_surfaceDestroy,
      .drawFill       = spice_drawFill,
      .drawBitmap     = spice_drawBitmap
    },
    .cursor   =
    {
      .enable       = true,
      .autoConnect  = false,
      .setRGBAImage = spice_setCursorRGBAImage,
      .setMonoImage = spice_setCursorMonoImage,
      .setState     = spice_setCursorState,
    },
#if ENABLE_AUDIO
    .playback =
    {
      .enable      = audio_supportsPlayback(),
      .autoConnect = true,
      .start       = audio_playbackStart,
      .volume      = audio_playbackVolume,
      .mute        = audio_playbackMute,
      .stop        = audio_playbackStop,
      .data        = audio_playbackData
    },
    .record =
    {
      .enable      = audio_supportsRecord(),
      .autoConnect = true,
      .start       = audio_recordStart,
      .volume      = audio_recordVolume,
      .mute        = audio_recordMute,
      .stop        = audio_recordStop
    }
#endif
  };

  if (!purespice_connect(&config))
  {
    DEBUG_ERROR("Failed to connect to spice server");
    lgSignalEvent(e_spice);
    goto end;
  }

  // process all spice messages
  while(g_state.state != APP_STATE_SHUTDOWN)
  {
    PSStatus status;
    if ((status = purespice_process(100)) != PS_STATUS_RUN)
    {
      if (status != PS_STATUS_SHUTDOWN)
        DEBUG_ERROR("failed to process spice messages");
      goto end;
    }
  }

  // send key up events for any pressed keys
  if (g_params.useSpiceInput)
  {
    for(int scancode = 0; scancode < KEY_MAX; ++scancode)
      if (g_state.keyDown[scancode])
      {
        g_state.keyDown[scancode] = false;
        purespice_keyUp(scancode);
      }
  }

  purespice_disconnect();

end:

  audio_free();

  // if the connection was disconnected intentionally we don't want to shutdown
  // so that the user can see the message box and take action
  if (!g_state.spiceClose)
    g_state.state = APP_STATE_SHUTDOWN;

  lgSignalEvent(e_spice);
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

static void reportBadVersion(void)
{
  DEBUG_BREAK();
  DEBUG_ERROR("The host application is not compatible with this client");
  DEBUG_ERROR("This is not a Looking Glass error, do not report this");
  DEBUG_ERROR("Please install the matching host application for this client");
}

static MsgBoxHandle showSpiceInputHelp(void)
{
  static bool done = false;
  if (!g_params.useSpiceInput || done)
    return NULL;

  done = true;
  return app_msgBox(
    "Information",
    "Please note you can still control your guest\n"
    "through SPICE if you press the capture key.");
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

  ImVector_ImWchar_Init(&g_state.fontRange);
  ImFontGlyphRangesBuilder * rangeBuilder =
    ImFontGlyphRangesBuilder_ImFontGlyphRangesBuilder();
  ImFontGlyphRangesBuilder_AddRanges(rangeBuilder, (ImWchar[]) {
    0x0020, 0x00FF, // Basic Latin + Latin Supplement
    0x2190, 0x2193, // four directional arrows
    0,
  });
  ImFontGlyphRangesBuilder_BuildRanges(rangeBuilder, &g_state.fontRange);
  ImFontGlyphRangesBuilder_destroy(rangeBuilder);

  // initialize metrics ringbuffers
  g_state.renderTimings  = ringbuffer_new(256, sizeof(float));
  g_state.uploadTimings  = ringbuffer_new(256, sizeof(float));
  g_state.renderDuration = ringbuffer_new(256, sizeof(float));
  overlayGraph_register("FRAME" , g_state.renderTimings , 0.0f, 50.0f, NULL);
  overlayGraph_register("UPLOAD", g_state.uploadTimings , 0.0f, 50.0f, NULL);
  overlayGraph_register("RENDER", g_state.renderDuration, 0.0f, 10.0f, NULL);

  initImGuiKeyMap(g_state.io->KeyMap);

  // unknown guest OS at this time
  g_state.guestOS = KVMFR_OS_OTHER;

  // search for the best displayserver ops to use
  for(int i = 0; i < LG_DISPLAYSERVER_COUNT; ++i)
    if (LG_DisplayServers[i]->probe())
    {
      g_state.ds = LG_DisplayServers[i];
      break;
    }

  if (!g_state.ds)
  {
    DEBUG_ERROR("No display servers available, tried:");
    for (int i = 0; i < LG_DISPLAYSERVER_COUNT; ++i)
      DEBUG_ERROR("* %s", LG_DisplayServers[i]->name);
    return -1;
  }

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

  // setup the spice startup condition
  if (!(e_spice = lgCreateEvent(false, 0)))
  {
    DEBUG_ERROR("failed to create the spice startup event");
    return -1;
  }

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

  //setup the render command queue
  renderQueue_init();

  const PSInit psInit =
  {
    .log =
    {
      .info  = debug_info,
      .warn  = debug_warn,
      .error = debug_error,
    }
  };
  purespice_init(&psInit);

  g_state.micDefaultState = g_params.micDefaultState;

  if (g_params.useSpiceInput     ||
      g_params.useSpiceClipboard ||
      g_params.useSpiceAudio)
  {
    if (!lgCreateThread("spiceThread", spiceThread, NULL, &t_spice))
    {
      DEBUG_ERROR("spice create thread failed");
      return -1;
    }

    lgWaitEvent(e_spice, TIMEOUT_INFINITE);
    if (!g_state.spiceReady)
      return -1;
  }

  // select and init a renderer
  bool needsOpenGL = false;
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
    DEBUG_ERROR("Unable to find a suitable renderer");
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

  keybind_commonRegister();

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

  MsgBoxHandle msgs[10];
  int msgsCount;

restart:
  msgsCount = 0;
  memset(msgs, 0, sizeof(msgs));

  uint64_t initialSpiceEnable = microtime() + 1000 * 1000;

  while(g_state.state == APP_STATE_RUNNING)
  {
    if (initialSpiceEnable && microtime() > initialSpiceEnable)
    {
      /* use the spice display until we get frames from the LG host application
       * it is safe to call this before connect as it will be delayed until
       * spiceReady is called */
      app_useSpiceDisplay(true);
      initialSpiceEnable = 0;
    }

    status = lgmpClientSessionInit(g_state.lgmp, &udataSize, (uint8_t **)&udata);
    switch(status)
    {
      case LGMP_OK:
        initialSpiceEnable = 0;
        break;

      case LGMP_ERR_INVALID_VERSION:
      {
        reportBadVersion();
        msgs[msgsCount++] = app_msgBox(
          "Incompatible LGMP Version",
          "The host application is not compatible with this client.\n"
          "Please download and install the matching version."
        );

        DEBUG_INFO("Waiting for you to upgrade the host application");
        while (g_state.state == APP_STATE_RUNNING &&
            lgmpClientSessionInit(g_state.lgmp, &udataSize, (uint8_t **)&udata) != LGMP_OK)
          g_state.ds->wait(1000);

        if (g_state.state != APP_STATE_RUNNING)
          return -1;

        continue;
      }

      case LGMP_ERR_INVALID_SESSION:
      case LGMP_ERR_INVALID_MAGIC:
      {
        if (waitCount++ == 0)
        {
          DEBUG_BREAK();
          DEBUG_INFO("The host application seems to not be running");
          DEBUG_INFO("Waiting for the host application to start...");
        }

        if (waitCount == 30)
        {
          DEBUG_BREAK();
          msgs[msgsCount++] = app_msgBox(
              "Host Application Not Running",
              "It seems the host application is not running or your\n"
              "virtual machine is still starting up\n"
              "\n"
              "If the the VM is running and booted please check the\n"
              "host application log for errors. You can find the\n"
              "log through the shortcut in your start menu\n"
              "\n"
              "Continuing to wait...");

          msgs[msgsCount++] = showSpiceInputHelp();

          DEBUG_INFO("Check the host log in your guest at %%ProgramData%%\\Looking Glass (host)\\looking-glass-host.txt");
          DEBUG_INFO("Continuing to wait...");
        }

        g_state.ds->wait(1000);
        continue;
      }

      default:
        DEBUG_ERROR("lgmpClientSessionInit Failed: %s", lgmpStatusString(status));
        return -1;
    }

    if (g_state.state != APP_STATE_RUNNING)
      return -1;

    // dont show warnings again after the first successful startup
    waitCount = 100;

    const bool magicMatches = memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) == 0;
    if (udataSize < sizeof(*udata) || !magicMatches || udata->version != KVMFR_VERSION)
    {
      static bool alertsDone = false;
      if (alertsDone)
      {
        if(g_state.state == APP_STATE_RUNNING)
          g_state.ds->wait(1000);
        continue;
      }

      reportBadVersion();
      if (magicMatches)
      {
        msgs[msgsCount++] = app_msgBox(
          "Incompatible KVMFR Version",
          "The host application is not compatible with this client.\n"
          "Please download and install the matching version.\n"
          "\n"
          "Client Version: %s\n"
          "Host Version: %s",
          BUILD_VERSION,
          udata->version >= 2 ? udata->hostver : NULL
        );

        DEBUG_ERROR("Expected KVMFR version %d, got %d", KVMFR_VERSION, udata->version);
        DEBUG_ERROR("Client version: %s", BUILD_VERSION);
        if (udata->version >= 2)
          DEBUG_ERROR("  Host version: %s", udata->hostver);
      }
      else
        DEBUG_ERROR("Invalid KVMFR magic");

      DEBUG_BREAK();

      msgs[msgsCount++] = showSpiceInputHelp();

      DEBUG_INFO("Waiting for you to upgrade the host application");

      alertsDone = true;
      if(g_state.state == APP_STATE_RUNNING)
        g_state.ds->wait(1000);

      continue;
    }

    break;
  }

  if(g_state.state != APP_STATE_RUNNING)
    return -1;

  /* close any informational message boxes from above as we now connected
   * successfully */

  for(int i = 0; i < msgsCount; ++i)
    if (msgs[i])
      app_msgBoxClose(msgs[i]);

  DEBUG_INFO("Guest Information:");
  DEBUG_INFO("Version  : %s", udata->hostver);

  /* parse the kvmfr records from the userdata */
  udataSize -= sizeof(*udata);
  uint8_t * p = (uint8_t *)(udata + 1);
  while(udataSize >= sizeof(KVMFRRecord))
  {
    KVMFRRecord * record = (KVMFRRecord *)p;
    p         += sizeof(*record);
    udataSize -= sizeof(*record);
    if (record->size > udataSize)
    {
      DEBUG_WARN("KVMFRecord size is invalid, aborting parsing.");
      break;
    }

    switch(record->type)
    {
      case KVMFR_RECORD_VMINFO:
      {
        KVMFRRecord_VMInfo * vmInfo = (KVMFRRecord_VMInfo *)p;
        DEBUG_INFO("UUID     : "
          "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
          vmInfo->uuid[ 0], vmInfo->uuid[ 1], vmInfo->uuid[ 2],
          vmInfo->uuid[ 3], vmInfo->uuid[ 4], vmInfo->uuid[ 5],
          vmInfo->uuid[ 6], vmInfo->uuid[ 7], vmInfo->uuid[ 8],
          vmInfo->uuid[ 9], vmInfo->uuid[10], vmInfo->uuid[11],
          vmInfo->uuid[12], vmInfo->uuid[13], vmInfo->uuid[14],
          vmInfo->uuid[15]);

        DEBUG_INFO("CPU Model: %s", vmInfo->model);
        DEBUG_INFO("CPU      : %u sockets, %u cores, %u threads",
            vmInfo->sockets, vmInfo->cores, vmInfo->cpus);
        DEBUG_INFO("Using    : %s", vmInfo->capture);

        bool uuidValid = false;
        for(int i = 0; i < sizeof(vmInfo->uuid); ++i)
         if (vmInfo->uuid[i])
         {
           uuidValid = true;
           break;
         }

        if (!uuidValid)
          break;

        memcpy(g_state.guestUUID, vmInfo->uuid, sizeof(g_state.guestUUID));
        g_state.guestUUIDValid = true;
        break;
      }

      case KVMFR_RECORD_OSINFO:
      {
        KVMFRRecord_OSInfo * osInfo = (KVMFRRecord_OSInfo *)p;
        static const char * typeStr[] =
        {
          "Linux",
          "BSD",
          "OSX",
          "Windows",
          "Other"
        };

        const char * type;
        if (osInfo->os >= ARRAY_LENGTH(typeStr))
          type = "Unknown";
        else
          type = typeStr[osInfo->os];

        DEBUG_INFO("OS       : %s", type);
        if (osInfo->name[0])
          DEBUG_INFO("OS Name  : %s", osInfo->name);

        g_state.guestOS = osInfo->os;

        if (g_state.spiceReady && g_params.useSpiceInput)
          keybind_spiceRegister();
        break;
      }

      default:
        DEBUG_WARN("Unhandled KVMFRecord type: %d", record->type);
        break;
    }

    p         += record->size;
    udataSize -= record->size;
  }

  checkUUID();

  if (g_state.state == APP_STATE_RUNNING)
  {
    DEBUG_INFO("Starting session");
    g_state.lgHostConnected = true;
  }

  g_state.kvmfrFeatures = udata->features;

  LG_LOCK_INIT(g_state.pointerQueueLock);
  if (!core_startCursorThread() || !core_startFrameThread())
  {
    LG_LOCK_FREE(g_state.pointerQueueLock);
    return -1;
  }

  while(g_state.state == APP_STATE_RUNNING)
  {
    if (!lgmpClientSessionValid(g_state.lgmp))
    {
      g_state.lgHostConnected = false;
      DEBUG_INFO("Waiting for the host to restart...");
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

    g_state.state = APP_STATE_RUNNING;
    lgInit();
    goto restart;
  }

  LG_LOCK_FREE(g_state.pointerQueueLock);
  return 0;
}

static void lg_shutdown(void)
{
  g_state.state = APP_STATE_SHUTDOWN;

  if (t_spice)
    lgJoinThread(t_spice, NULL);

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

  if (e_spice)
  {
    lgFreeEvent(e_spice);
    e_startup = NULL;
  }

  if (g_state.ds)
    g_state.ds->shutdown();

  if (g_state.cbRequestList)
  {
    ll_free(g_state.cbRequestList);
    g_state.cbRequestList = NULL;
  }

  app_releaseAllKeybinds();
  ll_free(g_state.bindings);

  if (g_state.dsInitialized)
    g_state.ds->free();

  ivshmemClose(&g_state.shm);

  renderQueue_free();

  // free metrics ringbuffers
  ringbuffer_free(&g_state.renderTimings);
  ringbuffer_free(&g_state.uploadTimings);
  ringbuffer_free(&g_state.renderDuration);

  free(g_state.fontName);
  ImVector_ImWchar_UnInit(&g_state.fontRange);
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

  g_state.bindings = ll_new();

  g_state.overlays = ll_new();
  app_registerOverlay(&LGOverlaySplash, NULL);
  app_registerOverlay(&LGOverlayConfig, NULL);
  app_registerOverlay(&LGOverlayAlert , NULL);
  app_registerOverlay(&LGOverlayFPS   , NULL);
  app_registerOverlay(&LGOverlayGraphs, NULL);
  app_registerOverlay(&LGOverlayHelp  , NULL);
  app_registerOverlay(&LGOverlayMsg   , NULL);
  app_registerOverlay(&LGOverlayStatus, NULL);

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
