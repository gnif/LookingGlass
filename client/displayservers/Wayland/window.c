/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2021 Guanzhong Chen (quantum2048@gmail.com)
Copyright (C) 2021 Tudor Brindus (contact@tbrindus.ca)
https://looking-glass.io

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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

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

// Surface-handling listeners.

void waylandWindowUpdateScale(void)
{
  int32_t maxScale = 0;
  struct SurfaceOutput * node;

  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
  {
    int32_t scale = waylandOutputGetScale(node->output);
    if (scale > maxScale)
      maxScale = scale;
  }

  if (maxScale)
  {
    wlWm.scale = maxScale;
    wlWm.needsResize = true;
  }
}

static void wlSurfaceEnterHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node = malloc(sizeof(struct SurfaceOutput));
  node->output = output;
  wl_list_insert(&wlWm.surfaceOutputs, &node->link);
  waylandWindowUpdateScale();
}

static void wlSurfaceLeaveHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node;
  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
    if (node->output == output)
    {
      wl_list_remove(&node->link);
      break;
    }
  waylandWindowUpdateScale();
}

static const struct wl_surface_listener wlSurfaceListener = {
  .enter = wlSurfaceEnterHandler,
  .leave = wlSurfaceLeaveHandler,
};

// XDG Surface listeners.

static void xdgSurfaceConfigure(void * data, struct xdg_surface * xdgSurface,
    uint32_t serial)
{
  if (wlWm.configured)
  {
    wlWm.needsResize  = true;
    wlWm.resizeSerial = serial;
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

bool waylandWindowInit(const char * title, bool fullscreen, bool maximize, bool borderless)
{
  wlWm.scale = 1;

  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor (version 3+), will not proceed");
    return false;
  }

  if (!wlWm.xdgWmBase)
  {
    DEBUG_ERROR("Compositor missing xdg_wm_base, will not proceed");
    return false;
  }

  xdg_wm_base_add_listener(wlWm.xdgWmBase, &xdgWmBaseListener, NULL);

  wlWm.surface = wl_compositor_create_surface(wlWm.compositor);
  if (!wlWm.surface)
  {
    DEBUG_ERROR("Failed to create wl_surface");
    return false;
  }

  wl_surface_add_listener(wlWm.surface, &wlSurfaceListener, NULL);

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

  wl_surface_commit(wlWm.surface);

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

void waylandWindowFree(void)
{
  wl_surface_destroy(wlWm.surface);
}

void waylandSetWindowSize(int x, int y)
{
  // FIXME: implement.
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

bool waylandIsValidPointerPos(int x, int y)
{
  return x >= 0 && x < wlWm.width && y >= 0 && y < wlWm.height;
}


