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

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/epoll.h>

#include <libdecor.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

struct libdecor_configuration {
  uint32_t serial;

  bool has_window_state;
  enum libdecor_window_state window_state;

  bool has_size;
  int window_width;
  int window_height;
};

static void libdecorHandleError(struct libdecor * context, enum libdecor_error error,
    const char *message)
{
  DEBUG_ERROR("Got libdecor error (%d): %s", error, message);
}

static void libdecorFrameConfigure(struct libdecor_frame * frame,
    struct libdecor_configuration * configuration, void * opaque)
{
  if (!wlWm.configured)
  {
    xdg_surface_ack_configure(libdecor_frame_get_xdg_surface(frame), configuration->serial);
    wlWm.configured = true;
    return;
  }

  int width, height;
  if (libdecor_configuration_get_content_size(configuration, frame, &width, &height))
  {
    wlWm.width = width;
    wlWm.height = height;

    struct libdecor_state * state = libdecor_state_new(wlWm.width, wlWm.height);
    libdecor_frame_commit(wlWm.libdecorFrame, state, NULL);
    libdecor_state_free(state);
  }

  enum libdecor_window_state windowState;
  if (libdecor_configuration_get_window_state(configuration, &windowState))
    wlWm.fullscreen = windowState & LIBDECOR_WINDOW_STATE_FULLSCREEN;

  wlWm.needsResize = true;
  wlWm.resizeSerial = configuration->serial;
  app_invalidateWindow(true);
  waylandStopWaitFrame();
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
static struct libdecor_interface libdecorListener = {
  libdecorHandleError,
};

static struct libdecor_frame_interface libdecorFrameListener = {
  libdecorFrameConfigure,
  libdecorFrameClose,
  libdecorFrameCommit,
};
#pragma GCC diagnostic pop

static void libdecorCallback(uint32_t events, void * opaque)
{
  libdecor_dispatch(wlWm.libdecor, 0);
}

bool waylandShellInit(const char * title, bool fullscreen, bool maximize, bool borderless, bool resizable)
{
  wlWm.libdecor = libdecor_new(wlWm.display, &libdecorListener);
  wlWm.libdecorFrame = libdecor_decorate(wlWm.libdecor, wlWm.surface, &libdecorFrameListener, NULL);

  libdecor_frame_set_app_id(wlWm.libdecorFrame, "looking-glass-client");
  libdecor_frame_set_title(wlWm.libdecorFrame, title);
  libdecor_frame_map(wlWm.libdecorFrame);

  if (resizable)
    libdecor_frame_set_capabilities(wlWm.libdecorFrame, LIBDECOR_ACTION_RESIZE);
  else
    libdecor_frame_unset_capabilities(wlWm.libdecorFrame, LIBDECOR_ACTION_RESIZE);

  while (!wlWm.configured)
    libdecor_dispatch(wlWm.libdecor, 0);

  if (!waylandPollRegister(libdecor_get_fd(wlWm.libdecor), libdecorCallback, NULL, EPOLLIN))
  {
    DEBUG_ERROR("Failed register display to epoll: %s", strerror(errno));
    return false;
  }
  return true;
}

void waylandShellAckConfigureIfNeeded(void)
{
  if (wlWm.resizeSerial)
  {
    xdg_surface_ack_configure(libdecor_frame_get_xdg_surface(wlWm.libdecorFrame), wlWm.resizeSerial);
    wlWm.resizeSerial = 0;
  }
}

void waylandSetFullscreen(bool fs)
{
  if (fs)
    libdecor_frame_set_fullscreen(wlWm.libdecorFrame, NULL);
  else
    libdecor_frame_unset_fullscreen(wlWm.libdecorFrame);

  libdecor_frame_set_visibility(wlWm.libdecorFrame, !fs);
}

bool waylandGetFullscreen(void)
{
  return wlWm.fullscreen;
}

void waylandMinimize(void)
{
  libdecor_frame_set_minimized(wlWm.libdecorFrame);
}

void waylandShellResize(int w, int h)
{
  if (!libdecor_frame_is_floating(wlWm.libdecorFrame))
    return;

  wlWm.width = w;
  wlWm.height = h;

  struct libdecor_state * state = libdecor_state_new(w, h);
  libdecor_frame_commit(wlWm.libdecorFrame, state, NULL);
  libdecor_state_free(state);

  wlWm.needsResize = true;
  app_invalidateWindow(true);
  waylandStopWaitFrame();
}
