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

#define _GNU_SOURCE
#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "common/debug.h"
#include "resources/icondata.h"

static const uint32_t iconPixels[] = { LG_ICON_PIXELS };

bool waylandIconInit(void)
{
  if (!wlWm.iconManager || !wlWm.xdgToplevel || !wlWm.shm)
    return true; // not an error, just no support

  // Create shared memory buffer for the icon
  size_t dataSize = sizeof(iconPixels);
  int fd = memfd_create("lg-icon", 0);
  if (fd < 0)
  {
    DEBUG_ERROR("Failed to create icon shared memory: %d", errno);
    return true; // not fatal
  }

  struct wl_buffer * buffer = NULL;
  struct xdg_toplevel_icon_v1 * icon = NULL;

  if (ftruncate(fd, (off_t)dataSize) < 0)
  {
    DEBUG_ERROR("Failed to ftruncate icon shared memory: %d", errno);
    goto fail;
  }

  void * shm_data = mmap(NULL, dataSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_data == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to mmap icon shared memory: %d", errno);
    goto fail;
  }

  memcpy(shm_data, iconPixels, dataSize);
  munmap(shm_data, dataSize);

  struct wl_shm_pool * pool = wl_shm_create_pool(wlWm.shm, fd, (int32_t)dataSize);
  buffer = wl_shm_pool_create_buffer(pool, 0, LG_ICON_WIDTH, LG_ICON_HEIGHT,
      LG_ICON_WIDTH * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);

  if (!buffer)
  {
    DEBUG_ERROR("Failed to create wl_buffer for icon");
    goto fail;
  }

  icon = xdg_toplevel_icon_manager_v1_create_icon(wlWm.iconManager);
  if (!icon)
  {
    DEBUG_ERROR("Failed to create icon object");
    goto fail;
  }

  xdg_toplevel_icon_v1_add_buffer(icon, buffer, 1);
  xdg_toplevel_icon_manager_v1_set_icon(wlWm.iconManager,
      (struct xdg_toplevel *)wlWm.xdgToplevel, icon);

  // After set_icon, the icon object is immutable. We can destroy it;
  // the compositor retains its data.

fail:
  if (icon)
    xdg_toplevel_icon_v1_destroy(icon);
  if (buffer)
    wl_buffer_destroy(buffer);
  close(fd);

  return true; // not fatal even if failed
}
