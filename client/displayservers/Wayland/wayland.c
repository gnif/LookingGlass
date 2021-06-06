/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#define _GNU_SOURCE
#include "wayland.h"

#include <signal.h>
#include <string.h>
#include <wayland-client.h>

#include "common/debug.h"
#include "common/option.h"

static struct Option waylandOptions[] =
{
  {
    .module       = "wayland",
    .name         = "warpSupport",
    .description  = "Enable cursor warping",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true,
  },
  {0}
};

static bool waylandEarlyInit(void)
{
  // Request to receive EPIPE instead of SIGPIPE when one end of a pipe
  // disconnects while a write is pending. This is useful to the Wayland
  // clipboard backend, where an arbitrary application is on the other end of
  // that pipe.
  signal(SIGPIPE, SIG_IGN);

  return true;
}

static void waylandSetup(void)
{
  option_register(waylandOptions);
}

static bool waylandProbe(void)
{
  return getenv("WAYLAND_DISPLAY") != NULL;
}

static bool waylandInit(const LG_DSInitParams params)
{
  memset(&wlWm, 0, sizeof(wlWm));
  wl_list_init(&wlWm.surfaceOutputs);

  wlWm.warpSupport = option_get_bool("wayland", "warpSupport");

  wlWm.display = wl_display_connect(NULL);
  wlWm.width = params.w;
  wlWm.height = params.h;

  if (!waylandPollInit())
    return false;

  if (!waylandOutputInit())
    return false;

  if (!waylandRegistryInit())
    return false;

  if (!waylandIdleInit())
    return false;

  if (!waylandInputInit())
    return false;

  if (!waylandWindowInit(params.title, params.fullscreen, params.maximize, params.borderless))
    return false;

  if (!waylandEGLInit(params.w, params.h))
    return false;

  if (!waylandCursorInit())
    return false;

#ifdef ENABLE_OPENGL
  if (params.opengl && !waylandOpenGLInit())
    return false;
#endif

  wlWm.width = params.w;
  wlWm.height = params.h;

  return true;
}

static void waylandStartup(void)
{
}

static void waylandShutdown(void)
{
}

static void waylandFree(void)
{
  waylandIdleFree();
  waylandWindowFree();
  waylandInputFree();
  waylandOutputFree();
  waylandRegistryFree();
  waylandCursorFree();
  wl_display_disconnect(wlWm.display);
}

static bool waylandGetProp(LG_DSProperty prop, void * ret)
{
  if (prop == LG_DS_WARP_SUPPORT)
  {
    *(enum LG_DSWarpSupport*)ret = wlWm.warpSupport ? LG_DS_WARP_SURFACE : LG_DS_WARP_NONE;
    return true;
  }

  return false;
}

struct LG_DisplayServerOps LGDS_Wayland =
{
  .setup               = waylandSetup,
  .probe               = waylandProbe,
  .earlyInit           = waylandEarlyInit,
  .init                = waylandInit,
  .startup             = waylandStartup,
  .shutdown            = waylandShutdown,
  .free                = waylandFree,
  .getProp             = waylandGetProp,

#ifdef ENABLE_EGL
  .getEGLDisplay       = waylandGetEGLDisplay,
  .getEGLNativeWindow  = waylandGetEGLNativeWindow,
  .eglSwapBuffers      = waylandEGLSwapBuffers,
#endif

#ifdef ENABLE_OPENGL
  .glCreateContext     = waylandGLCreateContext,
  .glDeleteContext     = waylandGLDeleteContext,
  .glMakeCurrent       = waylandGLMakeCurrent,
  .glSetSwapInterval   = waylandGLSetSwapInterval,
  .glSwapBuffers       = waylandGLSwapBuffers,
#endif
  .guestPointerUpdated = waylandGuestPointerUpdated,
  .showPointer         = waylandShowPointer,
  .grabPointer         = waylandGrabPointer,
  .ungrabPointer       = waylandUngrabPointer,
  .capturePointer      = waylandCapturePointer,
  .uncapturePointer    = waylandUncapturePointer,
  .grabKeyboard        = waylandGrabKeyboard,
  .ungrabKeyboard      = waylandUngrabKeyboard,
  .warpPointer         = waylandWarpPointer,
  .realignPointer      = waylandRealignPointer,
  .isValidPointerPos   = waylandIsValidPointerPos,
  .inhibitIdle         = waylandInhibitIdle,
  .uninhibitIdle       = waylandUninhibitIdle,
  .wait                = waylandWait,
  .setWindowSize       = waylandSetWindowSize,
  .setFullscreen       = waylandSetFullscreen,
  .getFullscreen       = waylandGetFullscreen,
  .minimize            = waylandMinimize,

  .cbInit    = waylandCBInit,
  .cbNotice  = waylandCBNotice,
  .cbRelease = waylandCBRelease,
  .cbRequest = waylandCBRequest
};
