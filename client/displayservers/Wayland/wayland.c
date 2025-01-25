/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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
#include <sys/socket.h>

#include "common/debug.h"
#include "common/option.h"

#include "dynamic/wayland_desktops.h"

static struct Option waylandOptions[] =
{
  {
    .module       = "wayland",
    .name         = "warpSupport",
    .description  = "Enable cursor warping",
    .type         = OPTION_TYPE_BOOL,
    .value.x_bool = true,
  },
  {
    .module       = "wayland",
    .name         = "fractionScale",
    .description  = "Enable fractional scale",
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

static bool getCompositor(char * dst, size_t size)
{
  int fd = wl_display_get_fd(wlWm.display);
  struct ucred ucred;
  socklen_t len = sizeof(struct ucred);

  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1)
  {
    DEBUG_ERROR("Failed to get the pid of the socket");
    return false;
  }

  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/comm", ucred.pid);
  FILE *fp = fopen(path, "r");
  if (!fp)
  {
    DEBUG_ERROR("Failed to open %s", path);
    return false;
  }

  if (!fgets(dst, size, fp))
  {
    DEBUG_ERROR("Failed to read %s", path);
    fclose(fp);
    return false;
  }
  fclose(fp);

  dst[strlen(dst) - 1] = 0;
  return true;
}

static bool waylandInit(const LG_DSInitParams params)
{
  memset(&wlWm, 0, sizeof(wlWm));
  wlWm.desktop = WL_Desktops[0];

  wlWm.display = wl_display_connect(NULL);
  if (!wlWm.display)
    return false;

  // select the desktop interface based on the compositor process name
  char compositor[1024];
  if (getCompositor(compositor, sizeof(compositor)))
  {
    DEBUG_INFO("Compositor: %s", compositor);
    for(int i = 0; i < WL_DESKTOP_COUNT; ++i)
      if (strcmp(WL_Desktops[i]->compositor, compositor) == 0)
      {
        wlWm.desktop = WL_Desktops[i];
        break;
      }
  }
  else
    DEBUG_WARN("Compositor: UNKNOWN");
  DEBUG_INFO("Selected  : %s", wlWm.desktop->name);

  wl_list_init(&wlWm.surfaceOutputs);

  wlWm.warpSupport        = option_get_bool("wayland", "warpSupport");
  wlWm.useFractionalScale = option_get_bool("wayland", "fractionScale");

  if (!waylandPollInit())
    return false;

  if (!waylandOutputInit())
    return false;

  if (!waylandRegistryInit())
    return false;

  if (!waylandActivationInit())
    return false;

  if (!waylandIdleInit())
    return false;

  if (!waylandPresentationInit())
    return false;

  if (!waylandCursorInit())
    return false;

  if (!waylandInputInit())
    return false;

  wlWm.desktop->setSize(params.w, params.h);
  if (!waylandWindowInit(params.title, params.appId, params.fullscreen, params.maximize,
        params.borderless, params.resizable))
    return false;

  if (!waylandEGLInit(params.w, params.h))
    return false;

#ifdef ENABLE_OPENGL
  if (params.opengl && !waylandOpenGLInit())
    return false;
#endif

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
  waylandPresentationFree();
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

void waylandNeedsResize(void)
{
  wlWm.needsResize = true;
  app_invalidateWindow(true);
  waylandStopWaitFrame();
}

static void waylandSetFullscreen(bool fs)
{
  wlWm.desktop->setFullscreen(fs);
}

static bool waylandGetFullscreen(void)
{
  return wlWm.desktop->getFullscreen();
}

static void waylandMinimize(void)
{
  wlWm.desktop->minimize();
}

struct LG_DisplayServerOps LGDS_Wayland =
{
  .name                = "Wayland",
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

#ifdef ENABLE_VULKAN
  .createVulkanSurface = waylandCreateVulkanSurface,
#endif
  .waitFrame           = waylandWaitFrame,
  .skipFrame           = waylandSkipFrame,
  .stopWaitFrame       = waylandStopWaitFrame,
  .guestPointerUpdated = waylandGuestPointerUpdated,
  .setPointer          = waylandSetPointer,
  .grabPointer         = waylandGrabPointer,
  .ungrabPointer       = waylandUngrabPointer,
  .capturePointer      = waylandCapturePointer,
  .uncapturePointer    = waylandUncapturePointer,
  .grabKeyboard        = waylandGrabKeyboard,
  .ungrabKeyboard      = waylandUngrabKeyboard,
  .warpPointer         = waylandWarpPointer,
  .realignPointer      = waylandRealignPointer,
  .isValidPointerPos   = waylandIsValidPointerPos,
  .requestActivation   = waylandActivationRequestActivation,
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
