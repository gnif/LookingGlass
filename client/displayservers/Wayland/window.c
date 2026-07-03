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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "common/event.h"

// Surface-handling listeners.

static void setScale(struct WaylandScale newScale)
{
  wlWm.scale = newScale;
  wlWm.fractionalScale = waylandScaleIsFractional(newScale);
  wlWm.needsResize = true;

  if (wlWm.desktop->configured())
  {
    waylandCursorScaleChange();
    app_invalidateWindow(true);
    waylandStopWaitFrame();
  }
}

void waylandWindowUpdateScale(void)
{
  if (wlWm.fractionalScaleInterface)
    return;

  struct WaylandScale maxScale = waylandScaleFromInt(0);
  struct SurfaceOutput * node;

  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
  {
    struct WaylandScale scale = waylandOutputGetScale(node->output);
    if (waylandScaleCmp(scale, maxScale) > 0)
      maxScale = scale;
  }

  if (waylandScaleValid(maxScale))
    setScale(maxScale);
}

static void wlSurfaceEnterHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node = malloc(sizeof(*node));
  if (!node)
  {
    DEBUG_ERROR("out of memory");
    return;
  }

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

static void fractionalScalePreferredScale(void * data,
    struct wp_fractional_scale_v1 * fractionalScale, uint32_t scale)
{
  setScale(waylandScaleFromRatio(scale, 120));
}

static const struct wp_fractional_scale_v1_listener fractionalScaleListener = {
  .preferred_scale = fractionalScalePreferredScale,
};

bool waylandWindowInit(const char * title, const char * appId, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  wlWm.scale = waylandScaleFromInt(1);

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

  if (wlWm.fractionalScaleManager)
  {
    wlWm.fractionalScaleInterface = wp_fractional_scale_manager_v1_get_fractional_scale(
        wlWm.fractionalScaleManager, wlWm.surface);
    wp_fractional_scale_v1_add_listener(wlWm.fractionalScaleInterface,
        &fractionalScaleListener, NULL);
  }
  else
    wl_surface_add_listener(wlWm.surface, &wlSurfaceListener, NULL);

  if (wlWm.contentTypeManager)
  {
    wlWm.contentType = wp_content_type_manager_v1_get_surface_content_type(
        wlWm.contentTypeManager, wlWm.surface);
    wp_content_type_v1_set_content_type(wlWm.contentType, WP_CONTENT_TYPE_V1_TYPE_GAME);
  }

  if (!wlWm.desktop->shellInit(wlWm.display, wlWm.surface,
        title, appId, fullscreen, maximize, borderless, resizable))
    return false;

  wl_surface_commit(wlWm.surface);
  return true;
}

void waylandWindowFree(void)
{
  if (wlWm.fractionalScaleInterface)
    wp_fractional_scale_v1_destroy(wlWm.fractionalScaleInterface);
  if (wlWm.contentType)
    wp_content_type_v1_destroy(wlWm.contentType);
  wl_surface_destroy(wlWm.surface);
  lgFreeEvent(wlWm.frameEvent);
}

void waylandSetWindowSize(int x, int y)
{
    wlWm.desktop->shellResize(x, y);
}

bool waylandIsValidPointerPos(int x, int y)
{
  int width, height;
  wlWm.desktop->getSize(&width, &height);
  return x >= 0 && x < width && y >= 0 && y < height;
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
