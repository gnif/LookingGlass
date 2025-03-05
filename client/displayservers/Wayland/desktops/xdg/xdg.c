/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "wayland.h"
#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-xdg-decoration-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>

#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

// Maximum number of fds we can process at once in waylandWait
#define MAX_EPOLL_EVENTS 10

typedef struct XDGState
{
  bool configured;

  struct xdg_wm_base                 * wmBase;
  struct xdg_surface                 * surface;
  struct xdg_toplevel                * toplevel;
  struct zxdg_decoration_manager_v1  * decorationManager;
  struct zxdg_toplevel_decoration_v1 * toplevelDecoration;

  int32_t width, height;
  uint32_t resizeSerial;
  bool fullscreen;
  bool floating;
  int displayFd;
}
XDGState;

static XDGState state = {0};

// XDG WM base listeners.

static void xdgWmBasePing(void * data, struct xdg_wm_base * xdgWmBase, uint32_t serial)
{
  xdg_wm_base_pong(xdgWmBase, serial);
}

static const struct xdg_wm_base_listener xdgWmBaseListener = {
  .ping = xdgWmBasePing,
};

// XDG Surface listeners.

static void xdgSurfaceConfigure(void * data, struct xdg_surface * xdgSurface,
    uint32_t serial)
{
  if (state.configured)
  {
    state.resizeSerial = serial;
    waylandNeedsResize();
  }
  else
  {
    xdg_surface_ack_configure(xdgSurface, serial);
    state.configured = true;
  }
}

static const struct xdg_surface_listener xdgSurfaceListener = {
  .configure = xdgSurfaceConfigure,
};

// XDG Toplevel listeners.

static void xdgToplevelConfigure(void * data, struct xdg_toplevel * xdgToplevel,
    int32_t width, int32_t height, struct wl_array * states)
{
  state.width      = width;
  state.height     = height;
  state.fullscreen = false;
  state.floating   = true;

  enum xdg_toplevel_state * s;
  wl_array_for_each(s, states)
  {
    switch (*s)
    {
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
        state.fullscreen = true;
        // fallthrough
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
      case XDG_TOPLEVEL_STATE_TILED_LEFT:
      case XDG_TOPLEVEL_STATE_TILED_RIGHT:
      case XDG_TOPLEVEL_STATE_TILED_TOP:
      case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        state.floating = false;
        break;

      default:
        break;
    }
  }
}

static void xdgToplevelClose(void * data, struct xdg_toplevel * xdgToplevel)
{
  app_handleCloseEvent();
}

static const struct xdg_toplevel_listener xdgToplevelListener = {
  .configure = xdgToplevelConfigure,
  .close     = xdgToplevelClose,
};

bool xdg_shellInit(struct wl_display * display, struct wl_surface * surface,
    const char * title, const char * appId, bool fullscreen, bool maximize, bool borderless,
    bool resizable)
{
  if (!state.wmBase)
  {
    DEBUG_ERROR("Compositor missing xdg_wm_base, will not proceed");
    return false;
  }

  xdg_wm_base_add_listener(state.wmBase, &xdgWmBaseListener, NULL);

  state.surface = xdg_wm_base_get_xdg_surface(state.wmBase, surface);
  xdg_surface_add_listener(state.surface, &xdgSurfaceListener, NULL);

  state.toplevel = xdg_surface_get_toplevel(state.surface);
  xdg_toplevel_add_listener(state.toplevel, &xdgToplevelListener, NULL);
  xdg_toplevel_set_title(state.toplevel, title);
  xdg_toplevel_set_app_id(state.toplevel, appId);

  if (fullscreen)
    xdg_toplevel_set_fullscreen(state.toplevel, NULL);

  if (maximize)
    xdg_toplevel_set_maximized(state.toplevel);

  if (state.decorationManager)
  {
    state.toplevelDecoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        state.decorationManager, state.toplevel);
    if (state.toplevelDecoration)
    {
      zxdg_toplevel_decoration_v1_set_mode(state.toplevelDecoration,
          borderless ?
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }

  return true;
}

static void xdg_shellAckConfigureIfNeeded(void)
{
  if (state.resizeSerial)
  {
    xdg_surface_ack_configure(state.surface, state.resizeSerial);
    state.resizeSerial = 0;
  }
}

static void xdg_setFullscreen(bool fs)
{
  if (fs)
    xdg_toplevel_set_fullscreen(state.toplevel, NULL);
  else
    xdg_toplevel_unset_fullscreen(state.toplevel);
}

static bool xdg_getFullscreen(void)
{
  return state.fullscreen;
}

static void xdg_minimize(void)
{
  xdg_toplevel_set_minimized(state.toplevel);
}

static void xdg_shellResize(int w, int h)
{
  if (!state.floating)
    return;

  state.width  = w;
  state.height = h;
  xdg_surface_set_window_geometry(state.surface, 0, 0, w, h);

  waylandNeedsResize();
}

static void xdg_setSize(int w, int h)
{
  state.width  = w;
  state.height = h;
}

static void xdg_getSize(int * w, int * h)
{
  *w = state.width;
  *h = state.height;
}

static bool xdg_registryGlobalHandler(void * data,
    struct wl_registry * registry, uint32_t name, const char * interface,
    uint32_t version)
{
  if (!strcmp(interface, xdg_wm_base_interface.name))
  {
    state.wmBase = wl_registry_bind(registry, name,
        &xdg_wm_base_interface, 1);
    return true;
  }

  if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
  {
    state.decorationManager = wl_registry_bind(registry, name,
        &zxdg_decoration_manager_v1_interface, 1);
    return true;
  }

  return false;
}

static void waylandDisplayCallback(uint32_t events, void * opaque)
{
  struct wl_display * display = (struct wl_display *)opaque;
  if (events & EPOLLERR)
    wl_display_cancel_read(display);
  else
    wl_display_read_events(display);
  wl_display_dispatch_pending(display);
}

static bool xdg_pollInit(struct wl_display * display)
{
  state.displayFd = wl_display_get_fd(display);
  if (!waylandPollRegister(state.displayFd, waylandDisplayCallback,
        display, EPOLLIN))
  {
    DEBUG_ERROR("Failed register display to epoll: %s", strerror(errno));
    return false;
  }

  return true;
}

void xdg_pollWait(struct wl_display * display, int epollFd,
    unsigned int time)
{
  while (wl_display_prepare_read(display))
    wl_display_dispatch_pending(display);
  wl_display_flush(display);

  struct epoll_event events[MAX_EPOLL_EVENTS];
  int count;
  if ((count = epoll_wait(epollFd, events, MAX_EPOLL_EVENTS, time)) < 0)
  {
    if (errno != EINTR)
      DEBUG_INFO("epoll failed: %s", strerror(errno));
    wl_display_cancel_read(display);
    return;
  }

  bool sawDisplay = false;
  for (int i = 0; i < count; ++i) {
    struct WaylandPoll * poll = events[i].data.ptr;
    if (!poll->removed)
      poll->callback(events[i].events, poll->opaque);
    if (poll->fd == state.displayFd)
      sawDisplay = true;
  }

  if (!sawDisplay)
    wl_display_cancel_read(display);
}

WL_DesktopOps WLD_xdg =
{
  .name                      = "xdg",
  .compositor                = "",
  .shellInit                 = xdg_shellInit,
  .shellAckConfigureIfNeeded = xdg_shellAckConfigureIfNeeded,
  .setFullscreen             = xdg_setFullscreen,
  .getFullscreen             = xdg_getFullscreen,
  .minimize                  = xdg_minimize,
  .shellResize               = xdg_shellResize,
  .setSize                   = xdg_setSize,
  .getSize                   = xdg_getSize,
  .registryGlobalHandler     = xdg_registryGlobalHandler,
  .pollInit                  = xdg_pollInit,
  .pollWait                  = xdg_pollWait
};
