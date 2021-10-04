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

#include "wayland.h"

#include <stdbool.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

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
  if (wlWm.configured)
  {
    wlWm.needsResize  = true;
    wlWm.resizeSerial = serial;
    app_invalidateWindow(true);
    waylandStopWaitFrame();
  }
  else
  {
    xdg_surface_ack_configure(xdgSurface, serial);
    wlWm.configured = true;
  }
}

static const struct xdg_surface_listener xdgSurfaceListener = {
  .configure = xdgSurfaceConfigure,
};

// XDG Toplevel listeners.

static void xdgToplevelConfigure(void * data, struct xdg_toplevel * xdgToplevel,
    int32_t width, int32_t height, struct wl_array * states)
{
  wlWm.width = width;
  wlWm.height = height;
  wlWm.fullscreen = false;

  enum xdg_toplevel_state * state;
  wl_array_for_each(state, states)
  {
    if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
      wlWm.fullscreen = true;
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

bool waylandShellInit(const char * title, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  if (!wlWm.xdgWmBase)
  {
    DEBUG_ERROR("Compositor missing xdg_wm_base, will not proceed");
    return false;
  }

  xdg_wm_base_add_listener(wlWm.xdgWmBase, &xdgWmBaseListener, NULL);

  wlWm.xdgSurface = xdg_wm_base_get_xdg_surface(wlWm.xdgWmBase, wlWm.surface);
  xdg_surface_add_listener(wlWm.xdgSurface, &xdgSurfaceListener, NULL);

  wlWm.xdgToplevel = xdg_surface_get_toplevel(wlWm.xdgSurface);
  xdg_toplevel_add_listener(wlWm.xdgToplevel, &xdgToplevelListener, NULL);
  xdg_toplevel_set_title(wlWm.xdgToplevel, title);
  xdg_toplevel_set_app_id(wlWm.xdgToplevel, "looking-glass-client");

  if (fullscreen)
    xdg_toplevel_set_fullscreen(wlWm.xdgToplevel, NULL);

  if (maximize)
    xdg_toplevel_set_maximized(wlWm.xdgToplevel);

  if (wlWm.xdgDecorationManager)
  {
    wlWm.xdgToplevelDecoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        wlWm.xdgDecorationManager, wlWm.xdgToplevel);
    if (wlWm.xdgToplevelDecoration)
    {
      zxdg_toplevel_decoration_v1_set_mode(wlWm.xdgToplevelDecoration,
          borderless ?
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }

  return true;
}

void waylandShellAckConfigureIfNeeded(void)
{
  if (wlWm.resizeSerial)
  {
    xdg_surface_ack_configure(wlWm.xdgSurface, wlWm.resizeSerial);
    wlWm.resizeSerial = 0;
  }
}

void waylandSetFullscreen(bool fs)
{
  if (fs)
    xdg_toplevel_set_fullscreen(wlWm.xdgToplevel, NULL);
  else
    xdg_toplevel_unset_fullscreen(wlWm.xdgToplevel);
}

bool waylandGetFullscreen(void)
{
  return wlWm.fullscreen;
}

void waylandMinimize(void)
{
  xdg_toplevel_set_minimized(wlWm.xdgToplevel);
}

void waylandShellResize(int w, int h)
{
  //TODO: Implement resize for XDG.
}
