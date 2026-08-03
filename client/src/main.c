/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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
#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <linux/input.h>

#include "common/array.h"
#include "common/debug.h"
#include "common/crash.h"
#include "common/stringutils.h"
#include "common/thread.h"
#include "common/locking.h"
#include "common/event.h"
#include "common/time.h"
#include "common/version.h"
#include "common/paths.h"
#include "common/cpuinfo.h"
#include "common/ll.h"
#include "common/option.h"
#include "common/proctitle.h"

#include "message.h"
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
#include "evdev.h"

#ifdef ENABLE_TESTS
#include "interface/test_capture.h"
_Static_assert((int)LG_CAPTURE_RGBA8 == (int)LG_TEST_CAPTURE_RGBA8,
    "capture format mismatch");
_Static_assert((int)LG_CAPTURE_RGB10_A2 == (int)LG_TEST_CAPTURE_RGB10_A2,
    "capture format mismatch");
_Static_assert((int)LG_CAPTURE_RGBA32F == (int)LG_TEST_CAPTURE_RGBA32F,
    "capture format mismatch");
#endif

// forwards
static int renderThread(void * unused);

static LGEvent  *e_startup = NULL;
static LGEvent  *e_spice   = NULL;
static LGThread *t_spice   = NULL;
static LGThread *t_render  = NULL;

_Atomic(enum RunState) p_appState = APP_STATE_RUNNING;

struct AppState g_state =
{
  .cbRemoteType = SPICE_DATA_NONE,
  .cbWriteType  = SPICE_DATA_NONE,
};
struct CursorState g_cursor;

// this structure is initialized in config.c
struct AppParams g_params = { 0 };

#ifdef ENABLE_TESTS
static struct
{
  const char * path;
  uint64_t     targetSerial;
  uint64_t     lastSerial;
  unsigned     delay;
  unsigned     stableRenders;
  bool         enabled;
  bool         complete;
}
l_testCapture;

static atomic_uint_least64_t l_testFrameSerial;
static _Atomic(FrameType)    l_testFrameType;
#endif

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

struct RenderTiming
{
  uint64_t renderStart;
  uint64_t captureTime;
  uint64_t postProcessTime;
  uint64_t copyTime;
  uint64_t readyTime;
  uint64_t importTime;
};

static struct RenderTiming frameTimingLoad(void)
{
  struct RenderTiming timing = {};

  for (;;)
  {
    const unsigned sequence = atomic_load_explicit(
        &g_state.frameTimingSequence, memory_order_seq_cst);
    if (sequence & 1)
      continue;

    timing.captureTime     = atomic_load_explicit(
        &g_state.producerCaptureTime, memory_order_seq_cst);
    timing.postProcessTime = atomic_load_explicit(
        &g_state.producerPostProcessTime, memory_order_seq_cst);
    timing.copyTime        = atomic_load_explicit(
        &g_state.producerCopyTime, memory_order_seq_cst);
    timing.readyTime       = atomic_load_explicit(
        &g_state.producerReadyTime, memory_order_seq_cst);
    timing.importTime      = atomic_load_explicit(
        &g_state.clientImportTime, memory_order_seq_cst);

    if (sequence == atomic_load_explicit(
          &g_state.frameTimingSequence, memory_order_seq_cst))
      return timing;
  }
}

static void frameTimingStore(const LG_TransportFrameTiming * timing,
    uint64_t importTime)
{
  atomic_fetch_add_explicit(
      &g_state.frameTimingSequence, 1, memory_order_seq_cst);
  atomic_store_explicit(&g_state.producerCaptureTime,
      timing->captureTime, memory_order_seq_cst);
  atomic_store_explicit(&g_state.producerPostProcessTime,
      timing->postProcessTime, memory_order_seq_cst);
  atomic_store_explicit(&g_state.producerCopyTime,
      timing->copyTime, memory_order_seq_cst);
  atomic_store_explicit(&g_state.producerReadyTime,
      timing->readyTime, memory_order_seq_cst);
  atomic_store_explicit(&g_state.clientImportTime,
      importTime, memory_order_seq_cst);
  atomic_fetch_add_explicit(
      &g_state.frameTimingSequence, 1, memory_order_seq_cst);
}

static void preSwapCallback(void * udata)
{
  const struct RenderTiming * timing = (const struct RenderTiming *)udata;
  const uint64_t timestamp  = nanotime();
  const uint64_t renderTime = timestamp - timing->renderStart;
  if (timing->captureTime || timing->postProcessTime || timing->copyTime ||
      timing->readyTime || timing->importTime)
  {
    const OverlayFrameTiming frameTiming = {
      .timestamp   = timestamp,
      .capture     = timing->captureTime     * 1e-6f,
      .postProcess = timing->postProcessTime * 1e-6f,
      .copy        = timing->copyTime        * 1e-6f,
      .ready       = timing->readyTime       * 1e-6f,
      .import      = timing->importTime      * 1e-6f,
      .render      = renderTime              * 1e-6f,
    };
    ringbuffer_push(g_state.frameLatency, &frameTiming);
  }

#ifdef ENABLE_TESTS
  if (!l_testCapture.enabled || l_testCapture.complete)
    return;

  const uint64_t serial =
    atomic_load_explicit(&l_testFrameSerial, memory_order_acquire);
  if (serial < l_testCapture.targetSerial)
    return;

  if (l_testCapture.lastSerial != serial)
  {
    l_testCapture.lastSerial    = serial;
    l_testCapture.stableRenders = 0;
    return;
  }

  if (l_testCapture.stableRenders++ < l_testCapture.delay)
    return;

  l_testCapture.complete = true;
  LG_RendererCapture capture;
  if (!g_state.lgr->ops.capture ||
      !RENDERER(capture, &capture))
  {
    DEBUG_ERROR("The selected renderer cannot capture its framebuffer");
    app_setState(APP_STATE_SHUTDOWN);
    return;
  }

  const FrameType sourceType =
    atomic_load_explicit(&l_testFrameType, memory_order_acquire);
  const LG_TestCaptureHeader header = {
    .magic         = LG_TEST_CAPTURE_MAGIC,
    .version       = LG_TEST_CAPTURE_VERSION,
    .headerSize    = sizeof(header),
    .frameSerial   = serial,
    .sourceType    = sourceType,
    .captureFormat = capture.format,
    .width         = capture.width,
    .height        = capture.height,
    .stride        = capture.stride,
    .flags         = (capture.hdr       ? LG_TEST_CAPTURE_HDR        : 0) |
                     (capture.hdrPQ     ? LG_TEST_CAPTURE_HDR_PQ     : 0) |
                     (capture.nativeHDR ? LG_TEST_CAPTURE_NATIVE_HDR : 0) |
                     LG_TEST_CAPTURE_BOTTOM_UP,
    .dataSize      = capture.dataSize,
  };

  FILE * file = fopen(l_testCapture.path, "wb");
  bool written = false;
  if (file)
  {
    written =
      fwrite(&header, sizeof(header), 1, file) == 1 &&
      fwrite(capture.data, capture.dataSize, 1, file) == 1;
    if (fclose(file) != 0)
      written = false;
  }
  free(capture.data);

  if (!written)
    DEBUG_ERROR("Failed to write test capture to: %s", l_testCapture.path);
  else
    DEBUG_INFO("Wrote test capture for frame %" PRIu64 " to: %s",
        serial, l_testCapture.path);
  app_setState(APP_STATE_SHUTDOWN);
#endif
}

static int renderThread(void * unused)
{
  if (!RENDERER(renderStartup, g_state.useDMA))
  {
    DEBUG_ERROR("EGL render failed to start");
    app_setState(APP_STATE_SHUTDOWN);

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

  while(likely(app_getState() != APP_STATE_SHUTDOWN))
  {
    if (g_state.jitRender)
    {
      const bool forceRender = g_state.ds->waitFrame();
      app_handleRenderEvent(microtime());

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
      app_handleRenderEvent(microtime());

      float ups = atomic_load_explicit(&g_state.ups, memory_order_relaxed);

      if (unlikely(
            !lgWaitEventAbs(g_state.frameEvent, &time) ||
            ups > g_params.fpsMin))
      {
        /* only update the time if we woke up early */
        clock_gettime(CLOCK_MONOTONIC, &time);
        tsAdd(&time, app_isOverlayMode() ?
            g_state.overlayFrameTime : g_state.frameTime);
      }
    }

    int resize = atomic_load(&g_state.lgrResize);
    if (unlikely(resize))
    {
      g_state.io->DisplaySize = (ImVec2) {
        .x = g_state.windowW,
        .y = g_state.windowH,
      };
      g_state.io->DisplayFramebufferScale = (ImVec2) {
        .x = g_state.windowScale,
        .y = g_state.windowScale,
      };
      igGetStyle()->FontScaleMain = 1.0f / g_state.windowScale;
    }

    const bool fontDirty = atomic_exchange(&g_state.fontDirty, false);
    if (unlikely(fontDirty || g_state.fontScale != g_state.windowScale))
    {
      if (!util_buildUIFontAtlas(g_state.io->Fonts,
            g_params.uiSize * g_state.windowScale, &g_state.fontLarge))
        DEBUG_FATAL("Failed to build font atlas: %s (%s)", g_params.uiFont, g_state.fontName);

      if (g_state.lgr && !RENDERER(onFontUpdate))
        DEBUG_FATAL("Failed to upload the ImGui font atlas");

      g_state.fontScale = g_state.windowScale;
    }

    if (unlikely(resize))
    {
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

    struct RenderTiming renderTiming =
      newFrame ? frameTimingLoad() : (struct RenderTiming) {};
    renderTiming.renderStart = nanotime();
    LG_LOCK(g_state.lgrLock);

    renderQueue_process();

    if (unlikely(!RENDERER(render, g_params.winRotate, newFrame, invalidate,
          preSwapCallback, (void *)&renderTiming)))
    {
      LG_UNLOCK(g_state.lgrLock);
      break;
    }
    LG_UNLOCK(g_state.lgrLock);

    const uint64_t t     = nanotime();
    const uint64_t delta = t - g_state.lastRenderTime;

    g_state.lastRenderTime = t;
    atomic_fetch_add_explicit(&g_state.renderCount, 1, memory_order_relaxed);

    if (likely(g_state.lastRenderTimeValid))
    {
      const float fdelta = (float)delta / 1e6f;
      ringbuffer_push(g_state.renderTimings, &fdelta);
    }
    g_state.lastRenderTimeValid = true;

    const uint64_t now = microtime();
    if (unlikely(
          !g_state.resizeDone &&
          g_state.resizeTimeout < now))
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

  app_setState(APP_STATE_SHUTDOWN);

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

  if (g_state.transportOps && g_state.transportOps->detachRenderer)
    g_state.transportOps->detachRenderer(g_state.transport);

  RENDERER(deinitialize);
  g_state.lgr = NULL;
  LG_LOCK_FREE(g_state.lgrLock);

  return 0;
}


int main_cursorThread(void * unused)
{
  LG_RendererCursor cursorType = LG_CURSOR_COLOR;

  lgWaitEvent(e_startup, TIMEOUT_INFINITE);

  // subscribe to the pointer queue
  while(app_getState() == APP_STATE_RUNNING && !g_state.stopVideo)
  {
    LG_TransportPointer pointer;
    const LG_TransportStatus status = g_state.transportOps->nextPointer(
        g_state.transport, &pointer);
    if (status != LG_TRANSPORT_OK)
    {
      if (status == LG_TRANSPORT_TIMEOUT || status == LG_TRANSPORT_UNAVAILABLE)
      {
        if (g_cursor.redraw && g_cursor.guest.valid)
        {
          g_cursor.redraw = false;
          RENDERER(onMouseEvent,
            g_cursor.guest.visible &&
              (g_cursor.draw || !g_params.useSpiceInput),
            g_cursor.guest.x, g_cursor.guest.y,
            g_cursor.guest.hx, g_cursor.guest.hy);
          if (!g_state.stopVideo)
            lgSignalEvent(g_state.frameEvent);
        }
        continue;
      }

      app_setState(status == LG_TRANSPORT_DISCONNECTED ?
        APP_STATE_RESTART : APP_STATE_SHUTDOWN);
      if (status != LG_TRANSPORT_DISCONNECTED)
        DEBUG_ERROR("Pointer transport failed with status %d", status);
      break;
    }

    if (pointer.flags & LG_TRANSPORT_POINTER_VISIBLE_VALID)
      g_cursor.guest.visible =
        pointer.flags & LG_TRANSPORT_POINTER_VISIBLE;

    if (pointer.flags & LG_TRANSPORT_POINTER_SHAPE)
    {
      switch (pointer.type)
      {
        case CURSOR_TYPE_COLOR       : cursorType = LG_CURSOR_COLOR       ; break;
        case CURSOR_TYPE_MONOCHROME  : cursorType = LG_CURSOR_MONOCHROME  ; break;
        case CURSOR_TYPE_MASKED_COLOR: cursorType = LG_CURSOR_MASKED_COLOR; break;
        default:
          DEBUG_ERROR("Invalid cursor type");
          g_state.transportOps->releasePointer(g_state.transport, &pointer);
          continue;
      }

      g_cursor.guest.hx = pointer.hx;
      g_cursor.guest.hy = pointer.hy;
      if (!RENDERER(onMouseShape, cursorType, pointer.width, pointer.height,
            pointer.pitch, pointer.shape))
      {
        DEBUG_ERROR("Failed to update mouse shape");
        g_state.transportOps->releasePointer(g_state.transport, &pointer);
        continue;
      }
    }

    if ((pointer.flags & LG_TRANSPORT_POINTER_COLOR_TRANSFORM) &&
        g_state.lgr->ops.onMouseColorTransform)
      g_state.lgr->ops.onMouseColorTransform(g_state.lgr,
          pointer.colorTransform);

    if ((pointer.flags & LG_TRANSPORT_POINTER_VISIBLE_VALID) &&
        pointer.sdrWhiteLevel && g_state.lgr->ops.onMouseWhiteLevel)
      g_state.lgr->ops.onMouseWhiteLevel(g_state.lgr,
          pointer.sdrWhiteLevel);

    if (pointer.flags & LG_TRANSPORT_POINTER_POSITION)
    {
      const bool wasValid = g_cursor.guest.valid;
      g_cursor.guest.x     = pointer.x;
      g_cursor.guest.y     = pointer.y;
      g_cursor.guest.valid = true;
      if (!wasValid && core_inputEnabled())
      {
        core_alignToGuest();
        app_resyncMouseBasic();
      }
      core_handleGuestMouseUpdate();
    }

    app_updateMouseState();
    g_cursor.redraw = false;
    RENDERER(onMouseEvent,
      g_cursor.guest.visible && (g_cursor.draw || !g_params.useSpiceInput),
      g_cursor.guest.x, g_cursor.guest.y,
      g_cursor.guest.hx, g_cursor.guest.hy);

    if ((g_params.mouseRedraw ||
         (pointer.flags & LG_TRANSPORT_POINTER_COLOR_TRANSFORM)) &&
        g_cursor.guest.visible && !g_state.stopVideo)
      lgSignalEvent(g_state.frameEvent);

    g_state.transportOps->releasePointer(g_state.transport, &pointer);
  }

  if (g_state.transportOps->stopPointer)
    g_state.transportOps->stopPointer(g_state.transport);

  return 0;
}

int main_frameThread(void * unused)
{
  uint64_t          frameSerial   = 0;
  uint32_t          formatVersion = 0;
  LG_RendererFormat rendererFormat;

  if (g_state.useDMA)
    DEBUG_INFO("Using DMA buffer support");

  lgWaitEvent(e_startup, TIMEOUT_INFINITE);
  if (app_getState() != APP_STATE_RUNNING)
  {
    if (g_state.transportOps->stopFrame)
      g_state.transportOps->stopFrame(g_state.transport);
    return 0;
  }

  while(app_getState() == APP_STATE_RUNNING && !g_state.stopVideo)
  {
    LG_TransportFrame frame;
    const LG_TransportStatus status = g_state.transportOps->nextFrame(
        g_state.transport, g_state.useDMA, &frame);
    if (status != LG_TRANSPORT_OK)
    {
      if (status == LG_TRANSPORT_TIMEOUT || status == LG_TRANSPORT_UNAVAILABLE)
        continue;
      if (status == LG_TRANSPORT_DISCONNECTED)
        app_setState(APP_STATE_RESTART);
      else if (status == LG_TRANSPORT_END)
        app_setState(APP_STATE_SHUTDOWN);
      else
      {
        DEBUG_ERROR("Frame transport failed with status %d", status);
        app_setState(APP_STATE_SHUTDOWN);
      }
      break;
    }

    if (frame.serial == frameSerial && g_state.formatValid)
    {
      g_state.transportOps->releaseFrame(g_state.transport, &frame);
      continue;
    }
    frameSerial = frame.serial;

    const LG_TransportFrameFormat * format = frame.format;
    if (!format)
    {
      DEBUG_ERROR("Transport returned a frame without format metadata");
      g_state.transportOps->releaseFrame(g_state.transport, &frame);
      app_setState(APP_STATE_SHUTDOWN);
      break;
    }
    if (!g_state.formatValid || format->version != formatVersion)
    {
      memset(&rendererFormat, 0, sizeof(rendererFormat));
      rendererFormat.type          = format->type;
      rendererFormat.screenWidth   = format->screenWidth;
      rendererFormat.screenHeight  = format->screenHeight;
      rendererFormat.dataWidth     = format->dataWidth;
      rendererFormat.dataHeight    = format->dataHeight;
      rendererFormat.frameWidth    = format->frameWidth;
      rendererFormat.frameHeight   = format->frameHeight;
      rendererFormat.stride        = format->stride;
      rendererFormat.pitch         = format->pitch;
      rendererFormat.hdr           = format->hdr;
      rendererFormat.hdrPQ         = format->hdrPQ;
      rendererFormat.hdrMetadata   = format->hdrMetadata;
      rendererFormat.sdrWhiteLevel = format->sdrWhiteLevel;

      if (rendererFormat.hdrMetadata)
      {
        memcpy(rendererFormat.hdrDisplayPrimary, format->hdrDisplayPrimary,
            sizeof(rendererFormat.hdrDisplayPrimary));
        memcpy(rendererFormat.hdrWhitePoint, format->hdrWhitePoint,
            sizeof(rendererFormat.hdrWhitePoint));
        rendererFormat.hdrMaxDisplayLuminance       =
          format->hdrMaxDisplayLuminance;
        rendererFormat.hdrMinDisplayLuminance       =
          format->hdrMinDisplayLuminance;
        rendererFormat.hdrMaxContentLightLevel      =
          format->hdrMaxContentLightLevel;
        rendererFormat.hdrMaxFrameAverageLightLevel =
          format->hdrMaxFrameAverageLightLevel;
      }

      if (frame.flags & LG_TRANSPORT_FRAME_TRUNCATED)
      {
        DEBUG_BREAK();
        DEBUG_WARN("Transport buffer too small, screen truncated");
        DEBUG_BREAK();
        app_msgBox("Transport buffer too small",
            "The transport buffer is too small for this frame.");
      }

      switch (format->rotation)
      {
        case FRAME_ROT_0  : rendererFormat.rotate = LG_ROTATE_0  ; break;
        case FRAME_ROT_90 : rendererFormat.rotate = LG_ROTATE_90 ; break;
        case FRAME_ROT_180: rendererFormat.rotate = LG_ROTATE_180; break;
        case FRAME_ROT_270: rendererFormat.rotate = LG_ROTATE_270; break;
        default:
          DEBUG_ERROR("Unsupported frame rotation");
          rendererFormat.rotate = LG_ROTATE_0;
          break;
      }
      g_state.rotate = rendererFormat.rotate;

      bool invalid = false;
      switch (format->type)
      {
        case FRAME_TYPE_RGBA:
        case FRAME_TYPE_BGRA:
        case FRAME_TYPE_RGBA10:
          rendererFormat.bpp = 32;
          break;
        case FRAME_TYPE_RGBA16F:
          rendererFormat.bpp = 64;
          break;
        case FRAME_TYPE_BGR_32:
        case FRAME_TYPE_RGB_24:
          rendererFormat.bpp = 24;
          break;
        default:
          invalid = true;
          break;
      }

      if (invalid)
      {
        DEBUG_ERROR("Unsupported frame type");
        g_state.transportOps->releaseFrame(g_state.transport, &frame);
        app_setState(APP_STATE_SHUTDOWN);
        break;
      }

      g_state.formatValid = true;
      formatVersion       = format->version;
#ifdef ENABLE_TESTS
      atomic_store_explicit(&l_testFrameType, format->type,
          memory_order_release);
#endif
      DEBUG_INFO("Format: %s %ux%u (%ux%u) stride:%u pitch:%u rotation:%d hdr:%d pq:%d sdrWhite:%u nits",
          FrameTypeStr[format->type], format->frameWidth, format->frameHeight,
          format->dataWidth, format->dataHeight, format->stride, format->pitch,
          format->rotation, format->hdr ? 1 : 0, format->hdrPQ ? 1 : 0,
          rendererFormat.sdrWhiteLevel);

      LG_LOCK(g_state.lgrLock);
      if (!RENDERER(onFrameFormat, rendererFormat))
      {
        LG_UNLOCK(g_state.lgrLock);
        DEBUG_ERROR("Renderer failed to configure format");
        g_state.transportOps->releaseFrame(g_state.transport, &frame);
        app_setState(APP_STATE_SHUTDOWN);
        break;
      }
      const bool rendererSupportsNativeHDR = !rendererFormat.hdr ||
        !g_state.lgr->ops.supports || RENDERER(supports,
          rendererFormat.hdrPQ ? LG_SUPPORTS_HDR_PQ : LG_SUPPORTS_HDR_SCRGB);
      // Publish the matching surface format before allowing the render thread
      // to consume the renderer format. Otherwise it can present one frame in
      // the new encoding while the display server still has the old image
      // description attached.
      renderQueue_surfaceFormat(rendererFormat, rendererSupportsNativeHDR);
      LG_UNLOCK(g_state.lgrLock);

      g_state.srcSize.x    = rendererFormat.screenWidth;
      g_state.srcSize.y    = rendererFormat.screenHeight;
      g_state.haveSrcSize = true;
      if (g_params.autoResize)
        g_state.ds->setWindowSize(rendererFormat.frameWidth,
            rendererFormat.frameHeight);
      core_updatePositionInfo();
    }

    uint32_t damageCount = frame.damageRectsCount <=
      LG_TRANSPORT_MAX_DAMAGE_RECTS ? frame.damageRectsCount : 0;
    bool invalidDamage = damageCount != frame.damageRectsCount ||
      (damageCount && !frame.damageRects);
    for (uint32_t i = 0; !invalidDamage && i < damageCount; ++i)
    {
      const FrameDamageRect * rect = &frame.damageRects[i];
      invalidDamage = rect->x > format->frameWidth ||
        rect->y > format->frameHeight ||
        rect->width > format->frameWidth - rect->x ||
        rect->height > format->frameHeight - rect->y;
    }
    if (invalidDamage)
    {
      DEBUG_WARN("Invalid damage rectangles, forcing a full update");
      damageCount = 0;
    }

    g_state.frameImportTime = 0;
    if (!RENDERER(onFrame, frame.framebuffer, frame.dmaFD,
          frame.damageRects, damageCount))
    {
      g_state.transportOps->releaseFrame(g_state.transport, &frame);
      DEBUG_ERROR("Renderer onFrame returned failure");
      app_setState(APP_STATE_SHUTDOWN);
      break;
    }

    LG_TransportFrameTiming timing = {};
    if (g_state.transportOps->getFrameTiming)
      g_state.transportOps->getFrameTiming(
          g_state.transport, &frame, &timing);

    frameTimingStore(&timing, g_state.frameImportTime);

    overlaySplash_show(false);
    if ((frame.flags & LG_TRANSPORT_FRAME_REQUEST_ACTIVATION) &&
        g_params.requestActivation)
      g_state.ds->requestActivation();

    const bool blockScreensaver =
      frame.flags & LG_TRANSPORT_FRAME_BLOCK_SCREENSAVER;
    if (g_params.autoScreensaver &&
        g_state.autoIdleInhibitState != blockScreensaver)
    {
      if (blockScreensaver)
        g_state.ds->inhibitIdle();
      else
        g_state.ds->uninhibitIdle();
      g_state.autoIdleInhibitState = blockScreensaver;
    }

    atomic_fetch_add_explicit(&g_state.frameCount, 1, memory_order_relaxed);
#ifdef ENABLE_TESTS
    atomic_store_explicit(&l_testFrameSerial, frame.serial,
        memory_order_release);
#endif
    if (g_state.jitRender)
    {
      if (atomic_load_explicit(&g_state.pendingCount, memory_order_acquire) < 10)
        atomic_fetch_add_explicit(&g_state.pendingCount, 1,
            memory_order_release);
    }
    else
      lgSignalEvent(g_state.frameEvent);

    g_state.transportOps->releaseFrame(g_state.transport, &frame);
    app_useSpiceDisplay(false);
  }

  if (g_state.transportOps->stopFrame)
    g_state.transportOps->stopFrame(g_state.transport);

  RENDERER(onRestart);

  if (app_getState() != APP_STATE_SHUTDOWN)
    if (!app_useSpiceDisplay(true))
      overlaySplash_show(true);
  return 0;
}

static void checkUUID(void)
{
  if (!atomic_load_explicit(&g_state.spiceReady, memory_order_acquire) ||
      !g_state.guestUUIDValid)
    return;

  if (memcmp(g_state.spiceUUID, g_state.guestUUID,
        sizeof(g_state.spiceUUID)) == 0)
    return;

  app_msgBox(
      "SPICE Configuration Error",
      "You have connected SPICE to the wrong guest.\n"
      "Input will not function until this is corrected.");

  g_params.useSpiceInput = false;
  atomic_store_explicit(&g_state.spiceClose, true, memory_order_release);
  purespice_disconnect();
}

void spiceReady(void)
{
  atomic_store_explicit(&g_state.spiceReady, true, memory_order_release);
  if (atomic_load_explicit(&g_state.spiceDisplayRequested,
        memory_order_acquire))
    app_useSpiceDisplay(true);

  // set the intial mouse mode
  purespice_mouseMode(true);

  PSServerInfo info;
  if (purespice_getServerInfo(&info))
  {
    core_setTitle(info.name);

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
  }
  else
    DEBUG_WARN("Failed to obtain SPICE server information");

  if (g_params.useSpiceInput)
    keybind_spiceRegister();

  lgSignalEvent(e_spice);
}

static void spice_surfaceCreate(unsigned int surfaceId, PSSurfaceFormat format,
    unsigned int width, unsigned int height)
{
  if (surfaceId != 0)
  {
    DEBUG_INFO("Ignoring secondary SPICE surface: id: %u, size: %ux%u",
        surfaceId, width, height);
    return;
  }

  switch(format)
  {
    case PS_SURFACE_FMT_32_xRGB:
    case PS_SURFACE_FMT_32_ARGB:
      break;

    default:
      DEBUG_ERROR("Unsupported primary SPICE surface format: %d", format);
      g_state.spicePrimarySurfaceValid = false;
      app_useSpiceDisplay(false);
      return;
  }

  DEBUG_INFO("Create primary SPICE surface: id: %u, size: %ux%u",
      surfaceId, width, height);

  g_state.spicePrimarySurfaceValid = true;

  g_state.srcSize.x   = width;
  g_state.srcSize.y   = height;
  g_state.haveSrcSize = true;
  core_updatePositionInfo();

  renderQueue_spiceConfigure(width, height);
  renderQueue_spiceDrawFill(0, 0, width, height, 0x0);
}

static void spice_surfaceDestroy(unsigned int surfaceId)
{
  if (!g_state.spicePrimarySurfaceValid || surfaceId != 0)
  {
    DEBUG_INFO("Ignoring destruction of inactive or secondary SPICE surface %u",
        surfaceId);
    return;
  }

  DEBUG_INFO("Destroy primary SPICE surface %u", surfaceId);
  g_state.spicePrimarySurfaceValid = false;
  app_useSpiceDisplay(false);
}

static void spice_drawFill(unsigned int surfaceId, int x, int y, int width,
    int height, uint32_t color)
{
  if (!g_state.spicePrimarySurfaceValid || surfaceId != 0)
    return;

  renderQueue_spiceDrawFill(x, y, width, height, color);
}

static void spice_drawBitmap(unsigned int surfaceId, PSBitmapFormat format,
    bool topDown, int x, int y, int width, int height, int stride, void * data)
{
  if (!g_state.spicePrimarySurfaceValid || surfaceId != 0)
    return;

  switch(format)
  {
    case PS_BITMAP_FMT_32BIT:
    case PS_BITMAP_FMT_RGBA:
      break;

    default:
      DEBUG_ERROR("Unsupported SPICE bitmap format: %d", format);
      return;
  }

  renderQueue_spiceDrawBitmap(x, y, width, height, stride, data, topDown);
}

static void spice_setCursorRGBAImage(int width, int height, int hx, int hy,
    const void * data)
{
  g_state.spiceHotX = hx;
  g_state.spiceHotY = hy;

  const uint8_t * rgba = data;
  uint8_t * bgra = malloc(width * height * 4);
  for (int i = 0; i < width * height; ++i)
  {
    bgra[i * 4 + 0] = rgba[i * 4 + 2];
    bgra[i * 4 + 1] = rgba[i * 4 + 1];
    bgra[i * 4 + 2] = rgba[i * 4 + 0];
    bgra[i * 4 + 3] = rgba[i * 4 + 3];
  }
  renderQueue_cursorImage(
      LG_CURSOR_COLOR, width, height, width * 4, bgra);
}

static void spice_setCursorMonoImage(int width, int height, int hx, int hy,
    const void * xorMask, const void * andMask)
{
  g_state.spiceHotX = hx;
  g_state.spiceHotY = hy;

  int stride = (width + 7) / 8;
  uint8_t * buffer = malloc(stride * height * 2);
  memcpy(buffer, andMask, stride * height);
  memcpy(buffer + stride * height, xorMask, stride * height);
  renderQueue_cursorImage(
      LG_CURSOR_MONOCHROME, width, height * 2, stride, buffer);
}

static void spice_setCursorColorImage(int width, int height, int hx, int hy,
    const void * data, const void * maskData)
{
  g_state.spiceHotX = hx;
  g_state.spiceHotY = hy;

  const uint8_t * rgba = data;
  const uint8_t * andMask = maskData;
  const int maskStride = (width + 7) / 8;
  uint8_t * bgra = malloc(width * height * 4);
  for (int y = 0; y < height; ++y)
  {
    for (int x = 0; x < width; ++x)
    {
      const int i = y * width + x;
      bgra[i * 4 + 0] = rgba[i * 4 + 2];
      bgra[i * 4 + 1] = rgba[i * 4 + 1];
      bgra[i * 4 + 2] = rgba[i * 4 + 0];
      bgra[i * 4 + 3] =
        andMask[y * maskStride + x / 8] & (0x80U >> (x % 8)) ? 255 : 0;
    }
  }

  renderQueue_cursorImage(
      LG_CURSOR_MASKED_COLOR, width, height, width * 4, bgra);
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
      .enable        = true,
      .autoConnect   = false,
      .setRGBAImage  = spice_setCursorRGBAImage,
      .setMonoImage  = spice_setCursorMonoImage,
      .setColorImage = spice_setCursorColorImage,
      .setState      = spice_setCursorState,
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
  while(app_getState() != APP_STATE_SHUTDOWN)
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
      if (atomic_load_explicit(
            &g_state.keyDown[scancode], memory_order_relaxed))
      {
        const uint32_t ps2 = linux_to_ps2[scancode];
        if (ps2 && purespice_keyUp(ps2))
          atomic_store_explicit(
              &g_state.keyDown[scancode], false, memory_order_relaxed);
        else
          DEBUG_ERROR("Failed to release key %d during SPICE shutdown",
              scancode);
      }
  }

  purespice_disconnect();

end:

  audio_free();

  // if the connection was disconnected intentionally we don't want to shutdown
  // so that the user can see the message box and take action
  if (!atomic_load_explicit(&g_state.spiceClose, memory_order_acquire))
    app_setState(APP_STATE_SHUTDOWN);

  lgSignalEvent(e_spice);
  return 0;
}

void intHandler(int sig)
{
  switch(sig)
  {
    case SIGINT:
    case SIGTERM:
      if (app_getState() != APP_STATE_SHUTDOWN)
      {
        DEBUG_INFO("Caught signal, shutting down...");
        app_setState(APP_STATE_SHUTDOWN);
      }
      else
      {
        DEBUG_INFO("Caught second signal, force quitting...");
        printAllThreadBacktraces();
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

struct TransportSessionProbe
{
  LG_Transport * transport;
  const LG_TransportOps * ops;
  LG_TransportSession session;
  LG_TransportStatus status;
  atomic_bool done;
};

static int transportSessionProbe(void * opaque)
{
  struct TransportSessionProbe * probe = opaque;
  probe->status = probe->ops->connect(probe->transport, &probe->session);
  atomic_store_explicit(&probe->done, true, memory_order_release);
  return 0;
}

static int lg_run(void)
{
#ifdef ENABLE_TESTS
  memset(&l_testCapture, 0, sizeof(l_testCapture));
  atomic_store_explicit(&l_testFrameSerial, 0, memory_order_relaxed);
  atomic_store_explicit(&l_testFrameType, FRAME_TYPE_INVALID,
      memory_order_relaxed);
  if (strcmp(g_params.transport, "test") == 0)
  {
    const char * capturePath = option_get_string("test", "captureFile");
    const int captureFrame   = option_get_int("test", "captureFrame");
    const int captureDelay   = option_get_int("test", "captureDelay");
    if (capturePath)
    {
      if (captureFrame < 1 || captureDelay < 0)
      {
        DEBUG_ERROR("test capture requires captureFrame >= 1 and "
            "captureDelay >= 0");
        return -1;
      }
      l_testCapture.path         = capturePath;
      l_testCapture.targetSerial = captureFrame;
      l_testCapture.delay        = captureDelay;
      l_testCapture.enabled      = true;
    }
  }
#endif

  g_cursor.sens = g_params.mouseSens;
       if (g_cursor.sens < -9) g_cursor.sens = -9;
  else if (g_cursor.sens >  9) g_cursor.sens =  9;

  /* setup imgui */
  igCreateContext(NULL);
  g_state.io    = igGetIO_Nil();
  g_state.style = igGetStyle();

  g_state.style->Colors[ImGuiCol_ModalWindowDimBg] = (ImVec4) { 0.0f, 0.0f, 0.0f, 0.4f };

  alloc_sprintf(&g_state.imGuiIni, "%s/imgui.ini", lgConfigDir());
  g_state.io->IniFilename   = g_state.imGuiIni;
  g_state.io->BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

  g_state.windowScale = 1.0;
  if (!util_initUIFonts())
  {
    DEBUG_ERROR("Failed to initialize UI fonts");
    return -1;
  }

  g_state.fontName = util_getUIFont(g_params.uiFont);
  if (!g_state.fontName)
  {
    DEBUG_ERROR("Failed to load UI font: %s", g_params.uiFont);
    return -1;
  }
  DEBUG_INFO("Using font: %s", g_state.fontName);

  // initialize metrics ringbuffers
  g_state.renderTimings = ringbuffer_new(256, sizeof(float));
  g_state.frameLatency  = ringbuffer_new(4096, sizeof(OverlayFrameTiming));
  overlayGraph_setCompact(overlayGraph_register(
        "FRAME", g_state.renderTimings, 0.0f, 50.0f, NULL), true);
  overlayGraph_registerFrameTiming("FRAME LATENCY", g_state.frameLatency);

  // unknown guest OS at this time
  g_state.guestOS = LG_TRANSPORT_OS_OTHER;

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

  if (evdev_start())
    DEBUG_INFO("Using evdev for keyboard capture");

  // override the SIGINIT handler so that we can tell the difference between
  // SIGINT and the user sending a close event, such as ALT+F4
  signal(SIGINT , intHandler);
  signal(SIGTERM, intHandler);

  if (!lgTransport_create(g_params.transport, &g_state.transport,
        &g_state.transportOps))
  {
    DEBUG_ERROR("Failed to create transport: %s", g_params.transport);
    return -1;
  }
  DEBUG_INFO("Using Transport: %s", g_state.transportOps->name);

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

  if (g_params.useSpice)
  {
    if (!lgCreateThread("spiceThread", spiceThread, NULL, &t_spice))
    {
      DEBUG_ERROR("spice create thread failed");
      return -1;
    }

    lgWaitEvent(e_spice, TIMEOUT_INFINITE);
    if (!atomic_load_explicit(&g_state.spiceReady, memory_order_acquire))
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

  g_state.useDMA = g_state.transportOps->supportsDMA(g_state.transport);

  // initialize the window dimensions at init for renderers
  g_state.windowW  = g_params.w;
  g_state.windowH  = g_params.h;
  g_state.windowCX = g_params.w / 2;
  g_state.windowCY = g_params.h / 2;
  core_updatePositionInfo();

  const LG_DSInitParams params =
  {
    .title               = g_params.windowTitle,
    .appId               = g_params.appId,
    .x                   = g_params.x,
    .y                   = g_params.y,
    .w                   = g_params.w,
    .h                   = g_params.h,
    .center              = g_params.center,
    .fullscreen          = g_params.fullscreen,
    .resizable           = g_params.allowResize,
    .borderless          = g_params.borderless,
    .maximize            = g_params.maximize,
    .largeCursorDot      = g_params.largeCursorDot,
    .allowNoInput        = strcmp(g_params.transport, "test") == 0,
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

  LG_RendererInterop interop = {
    .version = LG_RENDERER_INTEROP_VERSION,
    .size    = sizeof(interop),
  };
  const LG_RendererInterop * interopPtr = NULL;
  if (g_state.lgr->ops.getInterop &&
      g_state.lgr->ops.getInterop(g_state.lgr, &interop))
    interopPtr = &interop;
  if (g_state.transportOps->attachRenderer &&
      !g_state.transportOps->attachRenderer(g_state.transport, interopPtr))
  {
    DEBUG_ERROR("Failed to attach the renderer to the transport");
    return -1;
  }

  g_state.ds->startup();
  g_state.cbAvailable = g_state.ds->cbInit && g_state.ds->cbInit();
  if (g_state.cbAvailable)
    g_state.cbRequestList = ll_new();

  if (g_params.captureOnStart)
    core_setGrab(true);

  int waitCount = 0;
  LG_TransportSession session;
  MsgBoxHandle msgs[10];
  int msgsCount;

restart:
  msgsCount = 0;
  memset(msgs, 0, sizeof(msgs));

  uint64_t initialSpiceEnable = microtime() + 1000 * 1000;

  while(app_getState() == APP_STATE_RUNNING)
  {
    if (initialSpiceEnable && microtime() > initialSpiceEnable)
    {
      app_useSpiceDisplay(true);
      initialSpiceEnable = 0;
    }

    struct TransportSessionProbe probe = {
      .transport = g_state.transport,
      .ops       = g_state.transportOps,
      .done      = false,
    };
    LGThread * probeThread;
    if (!lgCreateThread("transportSession", transportSessionProbe, &probe,
          &probeThread))
    {
      DEBUG_ERROR("Failed to create transport session probe thread");
      return -1;
    }

    while (app_getState() == APP_STATE_RUNNING &&
        !atomic_load_explicit(&probe.done, memory_order_acquire))
      g_state.ds->wait(100);

    if (!lgJoinThread(probeThread, NULL))
    {
      DEBUG_ERROR("Failed to join transport session probe thread");
      return -1;
    }

    if (app_getState() != APP_STATE_RUNNING)
      return -1;

    if (probe.status == LG_TRANSPORT_OK)
    {
      session = probe.session;
      initialSpiceEnable = 0;
      break;
    }

    if (probe.status == LG_TRANSPORT_INVALID_VERSION)
    {
      if (waitCount++ == 0)
      {
        reportBadVersion();
        msgs[msgsCount++] = app_msgBox(
            "Incompatible Transport Version",
            "The selected transport source is not compatible with this client.\n"
            "Please install matching versions.");
        DEBUG_INFO("Remote transport version: %u", probe.session.remoteVersion);
      }

      g_state.ds->wait(1000);
      continue;
    }

    if (probe.status == LG_TRANSPORT_UNAVAILABLE ||
        probe.status == LG_TRANSPORT_DISCONNECTED)
    {
      if (waitCount++ == 0)
      {
        DEBUG_BREAK();
        DEBUG_INFO("The transport source is not available");
        DEBUG_INFO("Waiting for the source to start...");
      }
      if (waitCount == 30 && !g_params.disableWaitingMessage)
      {
        msgs[msgsCount++] = app_msgBox(
            "Transport Source Not Available",
            "The selected transport source is not available.\n"
            "Continuing to wait...");
        msgs[msgsCount++] = showSpiceInputHelp();
      }
      g_state.ds->wait(1000);
      continue;
    }

    DEBUG_ERROR("Transport connection failed with status %d", probe.status);
    return -1;
  }

  if (app_getState() != APP_STATE_RUNNING)
    return -1;

  waitCount = 100;
  for (int i = 0; i < msgsCount; ++i)
    if (msgs[i])
      app_msgBoxClose(msgs[i]);

  DEBUG_INFO("Guest Information:");
  DEBUG_INFO("Version  : %s", session.version[0] ? session.version : "unknown");
  if (session.cpuModel[0])
    DEBUG_INFO("CPU Model: %s", session.cpuModel);
  if (session.cpus)
    DEBUG_INFO("CPU      : %u sockets, %u cores, %u threads",
        session.sockets, session.cores, session.cpus);
  if (session.capture[0])
    DEBUG_INFO("Using    : %s", session.capture);

  if (session.cpuModel[0] && session.cpus && session.cores)
  {
    char hostModel[1024] = {};
    int hostProcs   = 0;
    int hostCores   = 0;
    int hostSockets = 0;
    if (cpuInfo_get(hostModel, sizeof(hostModel), &hostProcs, &hostCores,
        &hostSockets))
    {
      if (hostProcs > hostCores && session.cpus <= session.cores)
      {
        DEBUG_BREAK();
        DEBUG_WARN("The host CPU has hardware threads but the guest is not aware of them");
        DEBUG_WARN("This can degrade latency-sensitive tasks including Looking Glass");
        if (strncmp(hostModel, "AMD ", 4) == 0)
          DEBUG_WARN("For AMD CPUs, enable the `topoext` CPU feature for the virtual machine");
        DEBUG_BREAK();
      }

      if (strcmp(session.cpuModel, hostModel) != 0)
      {
        DEBUG_BREAK();
        DEBUG_WARN("The guest is unaware of the acceleration features of the host CPU");
        DEBUG_WARN("Set the virtual machine CPU type to `host-passthrough`");
        DEBUG_BREAK();
      }
    }
  }

  static const char * osNames[] = { "Linux", "BSD", "OSX", "Windows", "Other" };
  const char * os = session.os < LG_TRANSPORT_OS_OTHER + 1 ?
    osNames[session.os] : "Unknown";
  DEBUG_INFO("OS       : %s", os);
  if (session.osName[0])
    DEBUG_INFO("OS Name  : %s", session.osName);

  g_state.guestOS        = session.os;
  g_state.guestUUIDValid = session.uuidValid;
  if (session.uuidValid)
    memcpy(g_state.guestUUID, session.uuid, sizeof(g_state.guestUUID));
  g_state.transportFeatures = session.features;

  if (g_state.spiceReady && g_params.useSpiceInput)
    keybind_spiceRegister();
  checkUUID();
  DEBUG_INFO("Starting session");
  atomic_store_explicit(
      &g_state.lgHostConnected, true, memory_order_release);

  g_state.lgHostConnected = true;

  if (!core_startCursorThread() || !core_startFrameThread())
    return -1;

  while(likely(app_getState() == APP_STATE_RUNNING))
  {
    if (unlikely(!g_state.transportOps->sessionValid(g_state.transport)))
    {
      atomic_store_explicit(
          &g_state.lgHostConnected, false, memory_order_release);
      DEBUG_INFO("Waiting for the host to restart...");
      app_setState(APP_STATE_RESTART);
      break;
    }
    lgMessage_process();
    g_state.ds->wait(100);
  }

  if (app_getState() == APP_STATE_RESTART)
  {
    lgSignalEvent(e_startup);
    lgSignalEvent(g_state.frameEvent);

    core_stopFrameThread();
    core_stopCursorThread();
    g_state.transportOps->disconnect(g_state.transport);

    app_setState(APP_STATE_RUNNING);
    lgInit();
    goto restart;
  }

  return 0;
}

static void lg_shutdown(void)
{
  app_setState(APP_STATE_SHUTDOWN);

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

  if (g_state.transportOps)
    g_state.transportOps->destroy(&g_state.transport);

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

  if (g_state.ds && g_state.dsInitialized)
    g_state.ds->free();

  renderQueue_free();

  // free metrics ringbuffers
  ringbuffer_free(&g_state.renderTimings);
  ringbuffer_free(&g_state.frameLatency);

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

  lgInitProcessTitle(argc, &argv);
  core_setTitle(NULL);

  DEBUG_INFO("Looking Glass (%s)", BUILD_VERSION);
  DEBUG_INFO("Locking Method: " LG_LOCK_MODE);
  cpuInfo_log();

  if (!installCrashHandler("/proc/self/exe", argv[0]))
    DEBUG_WARN("Failed to install the crash handler");

  lgPathsInit("looking-glass");
  config_init();
  lgTransport_setup();
  egl_dynProcsInit();
  gl_dynProcsInit();

  if (!lgMessage_init())
    return -1;

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

  for(unsigned int i = 0; LG_AudioDevs[i]; ++i)
    if (LG_AudioDevs[i]->earlyInit)
      LG_AudioDevs[i]->earlyInit();

  evdev_earlyInit();

  if (!config_load(argc, argv))
    return -1;

  const int ret = lg_run();
  lg_shutdown();
  lgMessage_deinit();

  config_free();

  util_freeUIFonts();
  cleanupCrashHandler();
  return ret;
}
