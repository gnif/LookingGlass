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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

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
  int width, height;
  if (libdecor_configuration_get_content_size(configuration, frame, &width, &height)) {
    wlWm.width = width;
    wlWm.height = height;
  }

  enum libdecor_window_state windowState;
  if (libdecor_configuration_get_window_state(configuration, &windowState))
    wlWm.fullscreen = windowState & LIBDECOR_WINDOW_STATE_FULLSCREEN;

  struct libdecor_state * state = libdecor_state_new(wlWm.width, wlWm.height);
  libdecor_frame_commit(frame, state, wlWm.configured ? NULL : configuration);
  libdecor_state_free(state);

  if (wlWm.configured)
  {
    wlWm.needsResize = true;
    wlWm.resizeSerial = configuration->serial;
  }
  else
    wlWm.configured = true;
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

bool waylandShellInit(const char * title, bool fullscreen, bool maximize, bool borderless)
{
  wlWm.libdecor = libdecor_new(wlWm.display, &libdecorListener);
  wlWm.libdecorFrame = libdecor_decorate(wlWm.libdecor, wlWm.surface, &libdecorFrameListener, NULL);

  libdecor_frame_set_app_id(wlWm.libdecorFrame, "looking-glass-client");
  libdecor_frame_set_title(wlWm.libdecorFrame, title);
  libdecor_frame_map(wlWm.libdecorFrame);

  while (!wlWm.configured)
    wl_display_roundtrip(wlWm.display);

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
}

bool waylandGetFullscreen(void)
{
  return wlWm.fullscreen;
}

void waylandMinimize(void)
{
  libdecor_frame_set_minimized(wlWm.libdecorFrame);
}
