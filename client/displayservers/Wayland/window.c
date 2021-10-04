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
#include <string.h>

#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "common/event.h"

// Surface-handling listeners.

void waylandWindowUpdateScale(void)
{
  wl_fixed_t maxScale = 0;
  struct SurfaceOutput * node;

  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
  {
    wl_fixed_t scale = waylandOutputGetScale(node->output);
    if (scale > maxScale)
      maxScale = scale;
  }

  if (maxScale)
  {
    wlWm.scale = maxScale;
    wlWm.fractionalScale = wl_fixed_from_int(wl_fixed_to_int(maxScale)) != maxScale;
    wlWm.needsResize = true;
    waylandCursorScaleChange();
    app_invalidateWindow(true);
    waylandStopWaitFrame();
  }
}

static void wlSurfaceEnterHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node = malloc(sizeof(*node));
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

bool waylandWindowInit(const char * title, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  wlWm.scale = wl_fixed_from_int(1);

  wlWm.frameEvent = lgCreateEvent(true, 0);
  if (!wlWm.frameEvent)
  {
    DEBUG_ERROR("Failed to initialize event for waitFrame");
    return false;
  }
  lgSignalEvent(wlWm.frameEvent);

  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor (version 3+), will not proceed");
    return false;
  }

  wlWm.surface = wl_compositor_create_surface(wlWm.compositor);
  if (!wlWm.surface)
  {
    DEBUG_ERROR("Failed to create wl_surface");
    return false;
  }

  wl_surface_add_listener(wlWm.surface, &wlSurfaceListener, NULL);

  if (!waylandShellInit(title, fullscreen, maximize, borderless, resizable))
    return false;

  wl_surface_commit(wlWm.surface);
  return true;
}

void waylandWindowFree(void)
{
  wl_surface_destroy(wlWm.surface);
  lgFreeEvent(wlWm.frameEvent);
}

void waylandSetWindowSize(int x, int y)
{
    waylandShellResize(x, y);
}

bool waylandIsValidPointerPos(int x, int y)
{
  return x >= 0 && x < wlWm.width && y >= 0 && y < wlWm.height;
}

static void frameHandler(void * opaque, struct wl_callback * callback, unsigned int data)
{
  lgSignalEvent(wlWm.frameEvent);
  wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   .done = frameHandler,
};

bool waylandWaitFrame(void)
{
  lgWaitEvent(wlWm.frameEvent, TIMEOUT_INFINITE);

  struct wl_callback * callback = wl_surface_frame(wlWm.surface);
  if (callback)
    wl_callback_add_listener(callback, &frame_listener, NULL);

  return false;
}

void waylandSkipFrame(void)
{
  // If we decided to not render, we must commit the surface so that the callback is registered.
  wl_surface_commit(wlWm.surface);
}

void waylandStopWaitFrame(void)
{
  lgSignalEvent(wlWm.frameEvent);
}
