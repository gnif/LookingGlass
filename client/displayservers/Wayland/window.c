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

#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"
#include "common/event.h"

#define MTRACE(fmt, ...) \
  do \
  { \
    const uint64_t seq = app_mouseSeq(); \
    if (seq) \
      app_mouseTrace(__FILE__, __LINE__, __FUNCTION__, seq, \
          "wl.window." fmt, ##__VA_ARGS__); \
  } \
  while (0)

// Surface-handling listeners.

static void setScale(struct WaylandScale newScale)
{
  const struct WaylandScale oldScale = wlWm.scale;
  wlWm.scale = newScale;
  wlWm.fractionalScale = waylandScaleIsFractional(newScale);
  wlWm.needsResize = true;

  const bool configured = wlWm.desktop->configured();
  MTRACE("scale old=%d/%d new=%d/%d configured=%d",
      oldScale.num, oldScale.den, newScale.num, newScale.den, configured);

  if (configured)
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
  waylandOutputUpdateHDRWhiteLevel();
  waylandOutputUpdateFramePeriod();
}

static void wlSurfaceLeaveHandler(void * data, struct wl_surface * surface, struct wl_output * output)
{
  struct SurfaceOutput * node;
  wl_list_for_each(node, &wlWm.surfaceOutputs, link)
    if (node->output == output)
    {
      wl_list_remove(&node->link);
      free(node);
      break;
    }
  waylandWindowUpdateScale();
  waylandOutputUpdateHDRWhiteLevel();
  waylandOutputUpdateFramePeriod();
}

static const struct wl_surface_listener wlSurfaceListener = {
  .enter = wlSurfaceEnterHandler,
  .leave = wlSurfaceLeaveHandler,
};

static void waylandSignalFrame(LG_DSWaitFrameResult result)
{
  atomic_fetch_or_explicit(
      &wlWm.frameEventFlags, result, memory_order_release);
  lgSignalEvent(wlWm.frameEvent);
}

static void fractionalScalePreferredScale(void * data,
    struct wp_fractional_scale_v1 * fractionalScale, uint32_t scale)
{
  setScale(waylandScaleFromRatio(scale, 120));
}

static const struct wp_fractional_scale_v1_listener fractionalScaleListener = {
  .preferred_scale = fractionalScalePreferredScale,
};

static void resizePollCallback(uint32_t events, void * opaque)
{
  eventfd_t value;
  eventfd_read(wlWm.resizeEventFd, &value);

  const uint64_t pending = atomic_exchange_explicit(&wlWm.pendingResize, 0,
      memory_order_acquire);
  if (!pending || !app_isRunning())
    return;

  wlWm.desktop->shellResize((int)(pending >> 32), (int)(uint32_t)pending);
}

static void resizeEventFdFree(void)
{
  if (wlWm.resizeEventFd < 0)
    return;

  waylandPollUnregister(wlWm.resizeEventFd);
  close(wlWm.resizeEventFd);
  wlWm.resizeEventFd = -1;
}

bool waylandWindowInit(const char * title, const char * appId, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  wlWm.scale = waylandScaleFromInt(1);

  wlWm.frameEvent = lgCreateEvent(true, 0);
  if (!wlWm.frameEvent)
  {
    DEBUG_ERROR("Failed to initialize event for waitFrame");
    goto fail;
  }

  waylandSignalFrame(LG_DS_WAIT_FRAME_INTERRUPTED);

  wlWm.resizeEventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (wlWm.resizeEventFd < 0)
  {
    DEBUG_ERROR("Failed to create the resize eventfd: %s", strerror(errno));
    goto fail;
  }

  if (!waylandPollRegister(wlWm.resizeEventFd, resizePollCallback, NULL,
        EPOLLIN))
  {
    DEBUG_ERROR("Failed to register the resize eventfd");
    close(wlWm.resizeEventFd);
    wlWm.resizeEventFd = -1;
    goto fail;
  }

  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor (version 3+), will not proceed");
    goto fail;
  }

  wlWm.surface = wl_compositor_create_surface(wlWm.compositor);
  if (!wlWm.surface)
  {
    DEBUG_ERROR("Failed to create wl_surface");
    goto fail;
  }

  wl_surface_add_listener(wlWm.surface, &wlSurfaceListener, NULL);

  if (wlWm.fractionalScaleManager)
  {
    wlWm.fractionalScaleInterface = wp_fractional_scale_manager_v1_get_fractional_scale(
        wlWm.fractionalScaleManager, wlWm.surface);
    wp_fractional_scale_v1_add_listener(wlWm.fractionalScaleInterface,
        &fractionalScaleListener, NULL);
  }

  if (wlWm.contentTypeManager)
  {
    wlWm.contentType = wp_content_type_manager_v1_get_surface_content_type(
        wlWm.contentTypeManager, wlWm.surface);
    wp_content_type_v1_set_content_type(wlWm.contentType, WP_CONTENT_TYPE_V1_TYPE_GAME);
  }

  if (!wlWm.desktop->shellInit(wlWm.display, wlWm.surface,
        title, appId, fullscreen, maximize, borderless, resizable))
    goto fail;

  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    wl_surface_commit(wlWm.surface);
  });

  // The initial configure supplies the compositor-selected size for states
  // such as fullscreen. It must be received before the first buffer is made.
  while (!wlWm.desktop->configured())
  {
    if (wl_display_roundtrip(wlWm.display) < 0)
    {
      DEBUG_ERROR("Failed waiting for the initial Wayland configure");
      goto fail;
    }
  }

  return true;

fail:
  waylandWindowFree();
  return false;
}

void waylandWindowFree(void)
{
  resizeEventFdFree();

  struct SurfaceOutput * output;
  struct SurfaceOutput * temp;
  wl_list_for_each_safe(output, temp, &wlWm.surfaceOutputs, link)
  {
    wl_list_remove(&output->link);
    free(output);
  }

  if (wlWm.fractionalScaleInterface)
  {
    wp_fractional_scale_v1_destroy(wlWm.fractionalScaleInterface);
    wlWm.fractionalScaleInterface = NULL;
  }

  if (wlWm.contentType)
  {
    wp_content_type_v1_destroy(wlWm.contentType);
    wlWm.contentType = NULL;
  }

  if (wlWm.surface)
  {
    wl_surface_destroy(wlWm.surface);
    wlWm.surface = NULL;
  }

  if (wlWm.frameEvent)
  {
    lgFreeEvent(wlWm.frameEvent);
    wlWm.frameEvent = NULL;
  }
}

void waylandSetWindowSize(int x, int y)
{
  // The shell resize must run on the event-loop thread; see resizePollCallback
  atomic_store_explicit(&wlWm.pendingResize,
      (uint64_t)(uint32_t)x << 32 | (uint32_t)y, memory_order_release);
  eventfd_write(wlWm.resizeEventFd, 1);
}

bool waylandIsValidPointerPos(int x, int y)
{
  int width, height;
  wlWm.desktop->getSize(&width, &height);
  return x >= 0 && x < width && y >= 0 && y < height;
}

static void frameHandler(void * opaque, struct wl_callback * callback, unsigned int data)
{
  waylandSignalFrame(LG_DS_WAIT_FRAME_CADENCE);
  wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = {
   .done = frameHandler,
};

LG_DSWaitFrameResult waylandWaitFrame(void)
{
  lgWaitEvent(wlWm.frameEvent, TIMEOUT_INFINITE);
  const LG_DSWaitFrameResult result = atomic_exchange_explicit(
      &wlWm.frameEventFlags, LG_DS_WAIT_FRAME_NONE, memory_order_acquire);

  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    struct wl_callback * callback = wl_surface_frame(wlWm.surface);
    if (callback)
      wl_callback_add_listener(callback, &frame_listener, NULL);
  });

  return result;
}

void waylandSkipFrame(void)
{
  // If we decided to not render, we must commit the surface so that the callback is registered.
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    wl_surface_commit(wlWm.surface);
  });
}

void waylandStopWaitFrame(void)
{
  waylandSignalFrame(LG_DS_WAIT_FRAME_INTERRUPTED);
}
