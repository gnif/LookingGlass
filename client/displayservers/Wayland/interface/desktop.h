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

#ifndef _H_WAYLAND_DESKTOP_H_
#define _H_WAYLAND_DESKTOP_H_

#include <stdbool.h>
#include <wayland-client.h>

typedef struct WL_DesktopOps
{
  // the friendly name
  const char * name;

  // the compositor process name to match
  const char * compositor;

  bool (*shellInit)(
    struct wl_display * display, struct wl_surface * surface,
    const char * title, const char * appId, bool fullscreen, bool maximize,
    bool borderless, bool resizable);

  void (*shellAckConfigureIfNeeded)(void);

  void (*setFullscreen)(bool fs);

  bool (*getFullscreen)(void);

  void (*minimize)(void);

  void (*shellResize)(int w, int h);

  void (*setSize)(int w, int h);

  void (*getSize)(int * w, int * h);

  bool (*registryGlobalHandler)(
    void * data, struct wl_registry * registry,
    uint32_t name, const char * interface, uint32_t version);

  bool (*pollInit)(struct wl_display * display);

  void (*pollWait)(struct wl_display * display, int epollFd, unsigned int time);
}
WL_DesktopOps;

#endif
