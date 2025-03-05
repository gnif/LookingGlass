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

#include "interface/desktop.h"
#include "wayland-xdg-shell-client-protocol.h"

#include "wayland.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>

#include <libdecor.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

// Maximum number of fds we can process at once in waylandWait
#define MAX_EPOLL_EVENTS 10

typedef struct LibDecorState
{
  bool configured;
  struct libdecor       * libdecor;
  struct libdecor_frame * libdecorFrame;

  int32_t width, height;
  bool     needsResize;
  bool     fullscreen;
  uint32_t resizeSerial;
}
LibDecorState;

static LibDecorState state = {0};

struct libdecor_configuration
{
  uint32_t serial;

  bool has_window_state;
  enum libdecor_window_state window_state;

  bool has_size;
  int  window_width;
  int  window_height;
};

static void libdecorHandleError(struct libdecor * context, enum libdecor_error error,
    const char *message)
{
  DEBUG_ERROR("Got libdecor error (%d): %s", error, message);
}

static void libdecorFrameConfigure(struct libdecor_frame * frame,
    struct libdecor_configuration * configuration, void * opaque)
{
  if (!state.configured)
  {
    xdg_surface_ack_configure(libdecor_frame_get_xdg_surface(frame), configuration->serial);
    state.configured = true;
    return;
  }

  int width, height;
  if (libdecor_configuration_get_content_size(configuration, frame, &width, &height))
  {
    state.width  = width;
    state.height = height;

    struct libdecor_state * s = libdecor_state_new(width, height);
    libdecor_frame_commit(state.libdecorFrame, s, NULL);
    libdecor_state_free(s);
  }

  enum libdecor_window_state windowState;
  if (libdecor_configuration_get_window_state(configuration, &windowState))
    state.fullscreen = windowState & LIBDECOR_WINDOW_STATE_FULLSCREEN;

  state.resizeSerial = configuration->serial;
  waylandNeedsResize();
}

static void libdecorFrameClose(struct libdecor_frame * frame, void * opaque)
{
  app_handleCloseEvent();
}

static void libdecorFrameCommit(struct libdecor_frame * frame, void * opaque)
{
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct libdecor_interface libdecorListener =
{
  libdecorHandleError,
};

static struct libdecor_frame_interface libdecorFrameListener =
{
  libdecorFrameConfigure,
  libdecorFrameClose,
  libdecorFrameCommit,
};
#pragma GCC diagnostic pop

static void libdecorCallback(uint32_t events, void * opaque)
{
  libdecor_dispatch(state.libdecor, 0);
}

static bool libdecor_shellInit(
    struct wl_display * display, struct wl_surface * surface,
    const char * title, const char * appId, bool fullscreen,
    bool maximize, bool borderless, bool resizable)
{
  state.libdecor = libdecor_new(display, &libdecorListener);
  state.libdecorFrame = libdecor_decorate(state.libdecor, surface,
      &libdecorFrameListener, NULL);

  libdecor_frame_set_app_id(state.libdecorFrame, appId);
  libdecor_frame_set_title(state.libdecorFrame, title);
  libdecor_frame_map(state.libdecorFrame);

  if (fullscreen)
    libdecor_frame_set_fullscreen(state.libdecorFrame, NULL);

  if (maximize)
    libdecor_frame_set_maximized(state.libdecorFrame);

  if (resizable)
    libdecor_frame_set_capabilities(state.libdecorFrame,
        LIBDECOR_ACTION_RESIZE);
  else
    libdecor_frame_unset_capabilities(state.libdecorFrame,
        LIBDECOR_ACTION_RESIZE);

  while (!state.configured)
    libdecor_dispatch(state.libdecor, 0);

  if (!waylandPollRegister(libdecor_get_fd(state.libdecor),
        libdecorCallback, NULL, EPOLLIN))
  {
    DEBUG_ERROR("Failed register display to epoll: %s", strerror(errno));
    return false;
  }
  return true;
}

static void libdecor_shellAckConfigureIfNeeded(void)
{
  if (state.resizeSerial)
  {
    xdg_surface_ack_configure(
        libdecor_frame_get_xdg_surface(state.libdecorFrame), state.resizeSerial);
    state.resizeSerial = 0;
  }
}

static void libdecor_setFullscreen(bool fs)
{
  if (fs)
    libdecor_frame_set_fullscreen(state.libdecorFrame, NULL);
  else
    libdecor_frame_unset_fullscreen(state.libdecorFrame);

  libdecor_frame_set_visibility(state.libdecorFrame, !fs);
}

static bool libdecor_getFullscreen(void)
{
  return state.fullscreen;
}

static void libdecor_minimize(void)
{
  libdecor_frame_set_minimized(state.libdecorFrame);
}

static void libdecor_shellResize(int w, int h)
{
  if (!libdecor_frame_is_floating(state.libdecorFrame))
    return;

  state.width  = w;
  state.height = h;

  struct libdecor_state * s = libdecor_state_new(w, h);
  libdecor_frame_commit(state.libdecorFrame, s, NULL);
  libdecor_state_free(s);

  waylandNeedsResize();
}

static void libdecor_setSize(int w, int h)
{
  state.width  = w;
  state.height = h;
}

static void libdecor_getSize(int * w, int * h)
{
  *w = state.width;
  *h = state.height;
}

static bool libdecor_registryGlobalHandler(void * data,
    struct wl_registry * registry, uint32_t name, const char * interface,
    uint32_t version)
{
  return false;
}

bool libdecor_pollInit(struct wl_display * display)
{
  return true;
}

void libdecor_pollWait(struct wl_display * display, int epollFd,
    unsigned int time)
{
  libdecor_dispatch(state.libdecor, 0);

  struct epoll_event events[MAX_EPOLL_EVENTS];
  int count;
  if ((count = epoll_wait(epollFd, events, MAX_EPOLL_EVENTS, time)) < 0)
  {
    if (errno != EINTR)
      DEBUG_INFO("epoll failed: %s", strerror(errno));
    return;
  }

  for (int i = 0; i < count; ++i)
  {
    struct WaylandPoll * poll = events[i].data.ptr;
    if (!poll->removed)
      poll->callback(events[i].events, poll->opaque);
  }
}

WL_DesktopOps WLD_libdecor =
{
  .name                      = "libdecor",
  .compositor                = "gnome-shell",
  .shellInit                 = libdecor_shellInit,
  .shellAckConfigureIfNeeded = libdecor_shellAckConfigureIfNeeded,
  .setFullscreen             = libdecor_setFullscreen,
  .getFullscreen             = libdecor_getFullscreen,
  .minimize                  = libdecor_minimize,
  .shellResize               = libdecor_shellResize,
  .setSize                   = libdecor_setSize,
  .getSize                   = libdecor_getSize,
  .registryGlobalHandler     = libdecor_registryGlobalHandler,
  .pollInit                  = libdecor_pollInit,
  .pollWait                  = libdecor_pollWait
};
