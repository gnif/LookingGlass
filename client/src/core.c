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

#include "core.h"
#include "main.h"
#include "app.h"
#include "util.h"
#include "kb.h"
#include "message.h"

#include "common/time.h"
#include "common/debug.h"
#include "common/array.h"
#include "common/proctitle.h"
#include "common/stringutils.h"
#include "common/version.h"

#include <math.h>

#define RESIZE_TIMEOUT (10 * 1000) // 10ms

#define MTRACE(fmt, ...) \
  do \
  { \
    const uint64_t seq = app_mouseSeq(); \
    if (seq) \
      app_mouseTrace(__FILE__, __LINE__, __FUNCTION__, seq, \
          "core." fmt, ##__VA_ARGS__); \
  } \
  while (0)

static bool isInView(void)
{
  return
    g_cursor.pos.x >= g_state.dstRect.x                     &&
    g_cursor.pos.x <  g_state.dstRect.x + g_state.dstRect.w &&
    g_cursor.pos.y >= g_state.dstRect.y                     &&
    g_cursor.pos.y <  g_state.dstRect.y + g_state.dstRect.h;
}

static int exitPos(double pos, int edge)
{
  return pos < edge ? floor(pos) : ceil(pos);
}

bool core_inputEnabled(void)
{
  return g_params.useSpiceInput && !g_state.ignoreInput &&
    ((g_cursor.grab && g_params.captureInputOnly) || !g_params.captureInputOnly);
}

void core_invalidatePointer(bool detectInView)
{
  /* if the display server does not support warp, then we can not operate in
   * always relative mode and we should not grab the pointer */
  enum LG_DSWarpSupport warpSupport = LG_DS_WARP_NONE;
  app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

  MTRACE("invalidate detect=%d inWin=%d inView=%d grab=%d warp=%d "
      "support=%d", detectInView, g_cursor.inWindow, g_cursor.inView,
      g_cursor.grab, g_cursor.warpState, warpSupport);

  if (detectInView)
  {
    bool inView = isInView();
    // do not allow the view to become active if any mouse buttons are being held,
    // this fixes issues with meta window resizing.
    if (inView && g_cursor.buttons)
      return;

    g_cursor.inView = inView;
  }

  g_cursor.draw = (g_params.alwaysShowCursor || g_params.captureInputOnly)
    ? true : g_cursor.inView;
  g_cursor.redraw = true;

  g_cursor.warpState = g_cursor.inView ? WARP_STATE_ON : WARP_STATE_OFF;
  if (g_cursor.inView)
  {
    if (g_params.hideMouse)
      g_state.ds->setPointer(LG_POINTER_NONE);

    if (warpSupport != LG_DS_WARP_NONE && !g_params.captureInputOnly)
      g_state.ds->grabPointer();

    if (g_params.grabKeyboardOnFocus)
      g_state.ds->grabKeyboard();
  }
  else
  {
    if (g_params.hideMouse)
      g_state.ds->setPointer(LG_POINTER_SQUARE);

    if (warpSupport != LG_DS_WARP_NONE)
      g_state.ds->ungrabPointer();

    g_state.ds->ungrabKeyboard();
  }

  g_cursor.warpState = WARP_STATE_ON;
}

void core_setCursorInView(bool enable)
{
  MTRACE("view req=%d old=%d focus=%d inWin=%d grab=%d",
      enable, g_cursor.inView, g_state.focused, g_cursor.inWindow,
      g_cursor.grab);

  // if the state has not changed, don't do anything else
  if (g_cursor.inView == enable)
  {
    MTRACE("view skip=same value=%d", enable);
    return;
  }

  if (enable && !g_state.focused)
  {
    MTRACE("view skip=focus");
    return;
  }

  g_cursor.inView = enable;
  core_invalidatePointer(false);
}

void core_setGrab(bool enable)
{
  core_setGrabQuiet(enable);

  app_alert(
    g_cursor.grab ? LG_ALERT_SUCCESS  : LG_ALERT_WARNING,
    g_cursor.grab ? "Capture Enabled" : "Capture Disabled"
  );
}

void core_setGrabQuiet(bool enable)
{
  MTRACE("grab req=%d old=%d inWin=%d inView=%d focus=%d",
      enable, g_cursor.grab, g_cursor.inWindow, g_cursor.inView,
      g_state.focused);

  /* we always do this so that at init the cursor is in the right state */
  if (g_params.captureInputOnly && g_params.hideMouse)
    g_state.ds->setPointer(enable ? LG_POINTER_NONE : LG_POINTER_SQUARE);

  if (g_cursor.grab == enable)
    return;

  g_cursor.grab = enable;
  g_cursor.acc.x = 0.0;
  g_cursor.acc.y = 0.0;

  /* if the display server does not support warp we need to ungrab the pointer
   * here instead of in the move handler */
  enum LG_DSWarpSupport warpSupport = LG_DS_WARP_NONE;
  app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

  if (enable)
  {
    core_setCursorInView(true);
    g_state.ignoreInput = false;

    /* ensure the local mouse is inside the window before we capture, this fixes
     * odd UI behaviour if the user is using focus follows mouse and the window
     * was focused without the cursor being in window already */
    struct DoublePoint local;
    const bool valid = util_guestCurToLocal(&local);
    MTRACE("grab align valid=%d", valid);
    core_warpPointer(local.x, local.y, true);

    if (g_params.grabKeyboard)
      g_state.ds->grabKeyboard();

    g_state.ds->capturePointer();
  }
  else
  {
    if (g_params.grabKeyboard)
    {
      if (!g_params.grabKeyboardOnFocus ||
          !g_state.focused || g_params.captureInputOnly)
        g_state.ds->ungrabKeyboard();
    }

    g_state.ds->uncapturePointer();

    /* if exiting capture when input on capture only we need to align the local
     * cursor to the guest's location before it is shown. */
    if (g_params.captureInputOnly || !g_params.hideMouse)
      core_alignToGuest();
  }
}

bool core_warpPointer(int x, int y, bool exiting)
{
  if (!g_cursor.inWindow && !exiting)
  {
    MTRACE("warp drop=window target=%d,%d exit=%d", x, y, exiting);
    return false;
  }

  if (app_isOverlayMode())
  {
    MTRACE("warp drop=overlay target=%d,%d exit=%d", x, y, exiting);
    return false;
  }

  if (g_cursor.warpState == WARP_STATE_OFF)
  {
    MTRACE("warp drop=state target=%d,%d exit=%d", x, y, exiting);
    return false;
  }

  if (exiting)
    g_cursor.warpState = WARP_STATE_OFF;

  if (g_cursor.pos.x == x && g_cursor.pos.y == y)
  {
    MTRACE("warp same target=%d,%d exit=%d", x, y, exiting);
    return true;
  }

  MTRACE("warp send target=%d,%d exit=%d pos=%.3f,%.3f",
      x, y, exiting, g_cursor.pos.x, g_cursor.pos.y);
  g_state.ds->warpPointer(x, y, exiting);
  return true;
}

void core_onWindowSizeChanged(unsigned width, unsigned height)
{
  if (!g_state.transport ||
      !(g_state.transportFeatures & LG_TRANSPORT_FEATURE_WINDOW_SIZE))
    return;

  if (g_state.srcSize.x == width && g_state.srcSize.y == height)
    return;

  const LG_TransportControl control =
  {
    .type       = LG_TRANSPORT_CONTROL_WINDOW_SIZE,
    .windowSize = { .width = width, .height = height },
  };

  LG_TransportControlToken token;
  const LG_TransportStatus status = g_state.transportOps->sendControl(
      g_state.transport, &control, &token);
  if (status != LG_TRANSPORT_OK && status != LG_TRANSPORT_UNAVAILABLE)
    DEBUG_WARN("Window-size control failed with status %d", status);
}

void core_updatePositionInfo(void)
{
  if (g_params.setGuestRes &&
      g_state.transportFeatures & LG_TRANSPORT_FEATURE_WINDOW_SIZE)
  {
    LGMsg msg =
    {
      .type = LG_MSG_WINDOWSIZE,
      .windowSize =
      {
        .width  = round(g_state.windowW * g_state.windowScale),
        .height = round(g_state.windowH * g_state.windowScale)
      }
    };
    lgMessage_post(&msg);
  }

  if (!g_state.haveSrcSize)
    goto done;

  float srcW;
  float srcH;

  switch(g_params.winRotate)
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
      DEBUG_UNREACHABLE();
  }

  if (g_params.keepAspect)
  {
    const float srcAspect = srcH / srcW;
    const float wndAspect = (float)g_state.windowH / (float)g_state.windowW;
    bool force = true;

    if (g_params.dontUpscale &&
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
    if (g_params.intUpscale &&
        srcW <= g_state.windowW &&
        srcH <= g_state.windowH)
    {
      force = false;
      const int scale = min(
          floor(g_state.windowW / srcW),
          floor(g_state.windowH / srcH));
      g_state.dstRect.w = srcW * scale;
      g_state.dstRect.h = srcH * scale;
      g_state.dstRect.x = g_state.windowCX - g_state.dstRect.w / 2;
      g_state.dstRect.y = g_state.windowCY - g_state.dstRect.h / 2;
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

    if (g_params.dontUpscale && g_params.shrinkOnUpscale)
    {
      if (g_state.windowW > srcW)
      {
        force = true;
        g_state.dstRect.w = (int) (srcW + 0.5);
      }
      if (g_state.windowH > srcH)
      {
        force = true;
        g_state.dstRect.h = (int) (srcH + 0.5);
      }
    }

    if (force && g_params.forceAspect)
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
      srcW       != g_state.dstRect.w);

  g_cursor.scale.x  = (float)srcW / (float)g_state.dstRect.w;
  g_cursor.scale.y  = (float)srcH / (float)g_state.dstRect.h;

  if (!g_state.posInfoValid)
  {
    g_state.posInfoValid = true;
    g_state.ds->realignPointer();

    // g_cursor.guest.valid could have become true in the meantime.
    if (g_cursor.guest.valid)
    {
      // Since posInfoValid was false, core_handleGuestMouseUpdate becomes a
      // noop when called on the cursor thread, which means we need to call it
      // again in order for the cursor to show up.
      core_handleGuestMouseUpdate();

      // Similarly, the position needs to be valid before the initial mouse
      // move, otherwise we wouldn't know if the cursor is in the viewport.
      app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
    }
  }

done:
  MTRACE("geometry src=%dx%d dst=%d,%d,%d,%d win=%dx%d "
      "scale=%.4f,%.4f uiScale=%.4f rot=%d/%d border=%d,%d,%d,%d "
      "valid=%d", g_state.srcSize.x, g_state.srcSize.y,
      g_state.dstRect.x, g_state.dstRect.y, g_state.dstRect.w,
      g_state.dstRect.h, g_state.windowW, g_state.windowH,
      g_cursor.scale.x, g_cursor.scale.y, g_state.windowScale,
      g_state.rotate, g_params.winRotate, g_state.border.left,
      g_state.border.top, g_state.border.right, g_state.border.bottom,
      g_state.posInfoValid);
  app_updateMouseState();
  atomic_fetch_add(&g_state.lgrResize, 1);
}

void core_alignToGuest(void)
{
  if (!g_cursor.guest.valid || !g_state.focused)
    return;

  struct DoublePoint local;
  if (util_guestCurToLocal(&local))
    if (core_warpPointer(round(local.x), round(local.y), false))
      core_setCursorInView(true);
}

bool core_isValidPointerPos(int x, int y)
{
  return g_state.ds->isValidPointerPos(x, y);
}

bool core_startCursorThread(void)
{
  if (g_state.cursorThread)
    return true;

  g_state.stopVideo = false;
  if (!lgCreateThread("cursorThread", main_cursorThread, NULL,
        &g_state.cursorThread))
  {
    DEBUG_ERROR("cursor create thread failed");
    return false;
  }
  return true;
}

void core_stopCursorThread(void)
{
  g_state.stopVideo = true;
  if (g_state.cursorThread)
    lgJoinThread(g_state.cursorThread, NULL);

  g_state.cursorThread = NULL;
}

bool core_startFrameThread(void)
{
  if (g_state.frameThread)
    return true;

  g_state.stopVideo = false;
  if (!lgCreateThread("frameThread", main_frameThread, NULL,
        &g_state.frameThread))
  {
    DEBUG_ERROR("frame create thread failed");
    return false;
  }
  return true;
}

void core_stopFrameThread(void)
{
  g_state.stopVideo = true;
  if (g_state.frameThread)
    lgJoinThread(g_state.frameThread, NULL);

  g_state.frameThread = NULL;
}

void core_handleGuestMouseUpdate(void)
{
  struct DoublePoint localPos;
  if (!util_guestCurToLocal(&localPos))
  {
    MTRACE("guest drop=geometry guest=%d,%d hot=%d,%d valid=%d",
        g_cursor.guest.x, g_cursor.guest.y, g_cursor.guest.hx,
        g_cursor.guest.hy, g_cursor.guest.valid);
    return;
  }

  const bool overlay = app_isOverlayMode();
  if (overlay || !g_cursor.inView)
  {
    MTRACE("guest drop=%s guest=%d,%d local=%.3f,%.3f inWin=%d "
        "inView=%d", overlay ? "overlay" : "view", g_cursor.guest.x,
        g_cursor.guest.y, localPos.x, localPos.y, g_cursor.inWindow,
        g_cursor.inView);
    return;
  }

  MTRACE("guest send guest=%d,%d local=%.3f,%.3f inWin=%d inView=%d",
      g_cursor.guest.x, g_cursor.guest.y, localPos.x, localPos.y,
      g_cursor.inWindow, g_cursor.inView);
  g_state.ds->guestPointerUpdated(
    g_cursor.guest.x, g_cursor.guest.y,
    util_clamp(localPos.x, g_state.dstRect.x,
      g_state.dstRect.x + g_state.dstRect.w),
    util_clamp(localPos.y, g_state.dstRect.y,
      g_state.dstRect.y + g_state.dstRect.h)
  );
}

void core_handleMouseGrabbed(double ex, double ey)
{
  const bool active  = g_cursor.grab && g_state.ds->isPointerCaptured();
  const bool enabled = active && core_inputEnabled();
  MTRACE("captured delta=%.3f,%.3f active=%d enabled=%d", ex, ey,
      active, enabled);

  if (!enabled)
    return;

  int x, y;
  if (g_params.rawMouse && !g_cursor.sens)
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
    util_cursorToInt(ex, ey, &x, &y);
  }

  if (x == 0 && y == 0)
    return;

  if (!purespice_mouseMotion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}

void core_handleMouseNormal(double ex, double ey)
{
  MTRACE("normal delta=%.3f,%.3f guest=%d,%d valid=%d inWin=%d "
      "inView=%d", ex, ey, g_cursor.guest.x, g_cursor.guest.y,
      g_cursor.guest.valid, g_cursor.inWindow, g_cursor.inView);

  // prevent cursor handling outside of capture if the position is not known
  if (!g_cursor.guest.valid)
  {
    if (app_guestIsWindows())
    {
      // wiggle the mouse when the guest has not provided any information, we need
      // to do this because windows doesn't enable a cursor at all until it has
      // been moved for the first time.
      if (!purespice_mouseMotion(1, 1))
        DEBUG_ERROR("failed to send mouse motion message");
      if (!purespice_mouseMotion(-1, -1))
        DEBUG_ERROR("failed to send mouse motion message");
    }
    return;
  }

  if (g_cursor.realigning)
    return;

  if (!core_inputEnabled())
    return;

  /* scale the movement to the guest */
  if (g_cursor.useScale && g_params.scaleMouseInput)
  {
    ex *= g_cursor.scale.x;
    ey *= g_cursor.scale.y;
  }

  bool testExit = true;
  bool didExit = false;
  const bool inView = isInView();
  if (!g_cursor.inView)
  {
    if (inView)
      g_cursor.realign = true;
    else /* nothing to do if we are outside the viewport */
      return;
  }

  /*
   * do not pass mouse events to the guest if we do not have focus, this must be
   * done after the inView test has been performed so that when focus is gained
   * we know if we should be drawing the cursor.
   */
  if (!g_state.focused)
  {
    core_setCursorInView(inView);
    return;
  }

  /* if we have been instructed to realign */
  if (g_cursor.realign)
  {
    struct DoublePoint guest;
    util_localCurToGuest(&guest);

    if (!g_state.stopVideo &&
      g_state.transportFeatures & LG_TRANSPORT_FEATURE_SET_CURSOR_POS)
    {
      const LG_TransportControl control = {
        .type      = LG_TRANSPORT_CONTROL_SET_CURSOR_POS,
        .cursorPos = { .x = round(guest.x), .y = round(guest.y) },
      };

      LG_TransportControlToken token;
      LG_TransportStatus status = g_state.transportOps->sendControl(
          g_state.transport, &control, &token);
      if (status != LG_TRANSPORT_OK)
      {
        DEBUG_WARN("Cursor-position control failed with status %d", status);
        goto fallback;
      }
      else
      {
        /* wait for the move request to be processed */
        g_cursor.realigning = true;
        unsigned timeout = 200;
        do
        {
          status = g_state.transportOps->controlStatus(g_state.transport,
              token);
          if (status == LG_TRANSPORT_DISCONNECTED)
          {
            g_cursor.realigning = false;
            break;
          }
          if (status == LG_TRANSPORT_OK)
            break;
          if (status != LG_TRANSPORT_UNAVAILABLE)
          {
            g_cursor.realigning = false;
            return;
          }

          if (--timeout == 0)
          {
            DEBUG_ERROR("Cursor-position control was not acknowledged");
            break;
          }

          g_state.ds->wait(1);
        }
        while(app_isRunning());

        g_cursor.guest.x    = control.cursorPos.x;
        g_cursor.guest.y    = control.cursorPos.y;
        g_cursor.realign    = false;
        g_cursor.realigning = false;
        g_cursor.redraw     = true;

        if (!g_cursor.inWindow)
          return;

        core_setCursorInView(true);
        return;
      }
    }
    else
    {
fallback:
      /* add the difference to the offset */
      ex += guest.x - (g_cursor.guest.x + g_cursor.guest.hx);
      ey += guest.y - (g_cursor.guest.y + g_cursor.guest.hy);
      core_setCursorInView(true);
    }

    g_cursor.realign = false;

    /* don't test for an exit as we just entered, we can get into a enter/exit
     * loop otherwise */
    testExit = false;
  }

  /* if we are in "autoCapture" and the delta was large don't test for exit */
  if (g_params.autoCapture &&
      (fabs(ex) > 20.0 / g_cursor.scale.x || fabs(ey) > 20.0 / g_cursor.scale.y))
    testExit = false;

  /* if any buttons are held we should not allow exit to happen */
  if (g_cursor.buttons)
    testExit = false;

  if (testExit)
  {
    enum LG_DSWarpSupport warpSupport = LG_DS_WARP_NONE;
    app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

    /* translate the move to the guests orientation */
    struct DoublePoint move = {.x = ex, .y = ey};
    util_rotatePoint(&move);

    /* translate the guests position to our coordinate space */
    struct DoublePoint local;
    util_guestCurToLocal(&local);

    /* check if the move would push the cursor outside the guest's viewport */
    if (
        local.x + move.x <  g_state.dstRect.x ||
        local.y + move.y <  g_state.dstRect.y ||
        local.x + move.x >= g_state.dstRect.x + g_state.dstRect.w ||
        local.y + move.y >= g_state.dstRect.y + g_state.dstRect.h)
    {
      didExit = true;
      local.x += move.x;
      local.y += move.y;
      const int tx = exitPos(local.x, g_state.dstRect.x);
      const int ty = exitPos(local.y, g_state.dstRect.y);

      MTRACE("exit local=%.3f,%.3f move=%.3f,%.3f target=%d,%d "
          "dst=%d,%d,%d,%d support=%d", local.x - move.x,
          local.y - move.y, move.x, move.y, tx, ty, g_state.dstRect.x,
          g_state.dstRect.y, g_state.dstRect.w, g_state.dstRect.h,
          warpSupport);

      switch (warpSupport)
      {
        case LG_DS_WARP_NONE:
          break;

        case LG_DS_WARP_SURFACE:
          g_state.ds->ungrabPointer();
          core_warpPointer(tx, ty, true);

          if (!isInView() &&
              tx >= 0 && tx < g_state.windowW &&
              ty >= 0 && ty < g_state.windowH)
            core_setCursorInView(false);
          break;

        case LG_DS_WARP_SCREEN:
          if (core_isValidPointerPos(
                g_state.windowPos.x + g_state.border.left + tx,
                g_state.windowPos.y + g_state.border.top  + ty))
          {
            core_setCursorInView(false);

            /* preempt the window leave flag if the warp will leave our window */
            if (tx < 0 || ty < 0 || tx > g_state.windowW || ty > g_state.windowH)
              g_cursor.inWindow = false;

            /* ungrab the pointer and move the local cursor to the exit point */
            g_state.ds->ungrabPointer();
            core_warpPointer(tx, ty, true);
            return;
          }
      }
    }
    else if (warpSupport == LG_DS_WARP_SURFACE && isInView())
    {
      /* regrab the pointer in case the user did not move off the surface */
      MTRACE("regrab pos=%.3f,%.3f", g_cursor.pos.x, g_cursor.pos.y);
      g_state.ds->grabPointer();
      g_cursor.warpState = WARP_STATE_ON;
    }
  }

  int x, y;
  util_cursorToInt(ex, ey, &x, &y);

  if (x == 0 && y == 0)
    return;

  if (g_params.autoCapture)
  {
    g_cursor.delta.x += x;
    g_cursor.delta.y += y;

    if (fabs(g_cursor.delta.x) > 50.0 || fabs(g_cursor.delta.y) > 50.0)
    {
      g_cursor.delta.x = 0;
      g_cursor.delta.y = 0;
    }
  }
  else
  {
    /* assume the mouse will move to the location we attempt to move it to so we
     * avoid warp out of window issues. The cursorThread will correct this if
     * wrong after the movement has ocurred on the guest */
    g_cursor.guest.x += x;
    g_cursor.guest.y += y;
  }

  MTRACE("motion delta=%d,%d guest=%d,%d exit=%d test=%d warp=%d",
      x, y, g_cursor.guest.x, g_cursor.guest.y, didExit, testExit,
      g_cursor.warpState);

  if (!purespice_mouseMotion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}

void core_resetOverlayInputState(void)
{
  g_state.io->MouseDown[ImGuiMouseButton_Left  ] = false;
  g_state.io->MouseDown[ImGuiMouseButton_Right ] = false;
  g_state.io->MouseDown[ImGuiMouseButton_Middle] = false;
  for(int key = 0; key < KEY_MAX; key++)
    if (linux_to_imgui[key])
      ImGuiIO_AddKeyEvent(g_state.io, linux_to_imgui[key], false);
}

void core_updateOverlayState(void)
{
  g_state.cursorLast = -2;

  static bool wasGrabbed = false;
  if (app_isOverlayMode())
  {
    wasGrabbed = g_cursor.grab;

    g_state.io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    g_state.io->MousePos = (ImVec2) { g_cursor.pos.x, g_cursor.pos.y };

    core_setGrabQuiet(false);
    core_setCursorInView(false);
  }
  else
  {
    g_state.io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
    core_resetOverlayInputState();
    core_setGrabQuiet(wasGrabbed);
    core_invalidatePointer(true);
    app_invalidateWindow(false);

    if (!g_cursor.grab)
    {
      g_cursor.realign = true;
      core_handleMouseNormal(0, 0);
    }
  }
}

void core_setTitle(const char * title)
{
  if (!lgCanSetProcessTitle())
    return;

  char * string;
  if (alloc_sprintf(&string, "Looking Glass Client %s%s%s", BUILD_VERSION,
                    title && *title ? ": " : "", title ? title : "") >= 0)
  {
    lgSetProcessTitle(string);
    free(string);
  }
}
