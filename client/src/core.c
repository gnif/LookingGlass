/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
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

#include "core.h"
#include "main.h"
#include "app.h"
#include "util.h"

#include "common/time.h"
#include "common/debug.h"

#include <assert.h>

#define RESIZE_TIMEOUT (10 * 1000) // 10ms

void core_setCursorInView(bool enable)
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
  g_cursor.draw   = (g_params.alwaysShowCursor || g_params.captureInputOnly)
    ? true : enable;
  g_cursor.redraw = true;

  /* if the display server does not support warp, then we can not operate in
   * always relative mode and we should not grab the pointer */
  bool warpSupport = true;
  app_getProp(LG_DS_WARP_SUPPORT, &warpSupport);

  g_cursor.warpState = enable ? WARP_STATE_ON : WARP_STATE_OFF;

  if (enable)
  {
    if (g_params.hideMouse)
      g_state.ds->showPointer(false);

    if (warpSupport && !g_params.captureInputOnly)
      g_state.ds->grabPointer();
  }
  else
  {
    if (g_params.hideMouse)
      g_state.ds->showPointer(true);

    if (warpSupport)
      g_state.ds->ungrabPointer();

    core_setGrabQuiet(false);
  }

  g_cursor.warpState = WARP_STATE_ON;
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
  /* we always do this so that at init the cursor is in the right state */
  if (g_params.captureInputOnly && g_params.hideMouse)
    g_state.ds->showPointer(!enable);

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
    core_setCursorInView(true);
    g_state.ignoreInput = false;

    if (g_params.grabKeyboard)
      g_state.ds->grabKeyboard();

    g_state.ds->grabPointer();
  }
  else
  {
    if (g_params.grabKeyboard)
    {
      if (!g_state.focused || !g_params.grabKeyboardOnFocus ||
          g_params.captureInputOnly)
        g_state.ds->ungrabKeyboard();
    }

    if (!warpSupport || g_params.captureInputOnly || !g_state.formatValid)
      g_state.ds->ungrabPointer();

    // if exiting capture when input on capture only, we want to show the cursor
    if (g_params.captureInputOnly || !g_params.hideMouse)
      core_alignToGuest();
  }
}

bool core_warpPointer(int x, int y, bool exiting)
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

void core_updatePositionInfo(void)
{
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
      assert(!"unreachable");
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
