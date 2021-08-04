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

#include "common/debug.h"

static void registryGlobalHandler(void * data, struct wl_registry * registry,
    uint32_t name, const char * interface, uint32_t version)
{
  if (!strcmp(interface, wl_output_interface.name))
    waylandOutputBind(name, version);
  else if (!strcmp(interface, wl_seat_interface.name) && !wlWm.seat)
    wlWm.seat = wl_registry_bind(wlWm.registry, name, &wl_seat_interface, 1);
  else if (!strcmp(interface, wl_shm_interface.name))
    wlWm.shm = wl_registry_bind(wlWm.registry, name, &wl_shm_interface, 1);
  else if (!strcmp(interface, wl_compositor_interface.name) && version >= 3)
    wlWm.compositor = wl_registry_bind(wlWm.registry, name,
        // we only need v3 to run, but v4 can use eglSwapBuffersWithDamageKHR
        &wl_compositor_interface, version > 4 ? 4 : version);
#ifndef ENABLE_LIBDECOR
  else if (!strcmp(interface, xdg_wm_base_interface.name))
    wlWm.xdgWmBase = wl_registry_bind(wlWm.registry, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
    wlWm.xdgDecorationManager = wl_registry_bind(wlWm.registry, name,
        &zxdg_decoration_manager_v1_interface, 1);
#endif
  else if (!strcmp(interface, wp_presentation_interface.name))
    wlWm.presentation = wl_registry_bind(wlWm.registry, name,
        &wp_presentation_interface, 1);
  else if (!strcmp(interface, wp_viewporter_interface.name))
    wlWm.viewporter = wl_registry_bind(wlWm.registry, name,
        &wp_viewporter_interface, 1);
  else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name))
    wlWm.relativePointerManager = wl_registry_bind(wlWm.registry, name,
        &zwp_relative_pointer_manager_v1_interface, 1);
  else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name))
    wlWm.pointerConstraints = wl_registry_bind(wlWm.registry, name,
        &zwp_pointer_constraints_v1_interface, 1);
  else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
    wlWm.keyboardInhibitManager = wl_registry_bind(wlWm.registry, name,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
  else if (!strcmp(interface, wl_data_device_manager_interface.name) && version >= 3)
    wlWm.dataDeviceManager = wl_registry_bind(wlWm.registry, name,
        &wl_data_device_manager_interface, 3);
  else if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name))
    wlWm.idleInhibitManager = wl_registry_bind(wlWm.registry, name,
        &zwp_idle_inhibit_manager_v1_interface, 1);
  else if (!strcmp(interface, zxdg_output_manager_v1_interface.name) && version >= 2)
    wlWm.xdgOutputManager = wl_registry_bind(wlWm.registry, name,
        // we only need v2 to run, but v3 saves a callback
        &zxdg_output_manager_v1_interface, version > 3 ? 3 : version);
}

static void registryGlobalRemoveHandler(void * data,
    struct wl_registry * registry, uint32_t name)
{
  waylandOutputTryUnbind(name);
}

static const struct wl_registry_listener registryListener = {
  .global = registryGlobalHandler,
  .global_remove = registryGlobalRemoveHandler,
};

bool waylandRegistryInit(void)
{
  wlWm.registry = wl_display_get_registry(wlWm.display);
  if (!wlWm.registry)
  {
    DEBUG_ERROR("Unable to find wl_registry");
    return false;
  }

  wl_registry_add_listener(wlWm.registry, &registryListener, NULL);
  wl_display_roundtrip(wlWm.display);
  return true;
}

void waylandRegistryFree(void)
{
  wl_registry_destroy(wlWm.registry);
}
