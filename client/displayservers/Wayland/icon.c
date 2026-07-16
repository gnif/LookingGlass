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

// icondata is defined as an array of unsigned long, but on 64-bit platforms
// unsigned long is 8 bytes while the actual data is 32-bit ARGB packed in
// unsigned long values (the top 32 bits are zero). The first two values are
// width and height (both 64), followed by pixel rows.
// We need to convert this to a uint32_t buffer for wl_shm (ARGB8888).
//
// Note: sizeof(icondata) / sizeof(icondata[0]) gives the total array length,
// which includes the 2 header values + 64*64 = 4096 pixel values = 4098 total.

#define ICON_SIZE 64
// header: width and height, then pixel data
#define ICON_HEADER_WORDS 2
#define ICON_PIXEL_WORDS (ICON_SIZE * ICON_SIZE)
#define ICON_TOTAL_WORDS (ICON_HEADER_WORDS + ICON_PIXEL_WORDS)

static uint32_t g_iconPixels[ICON_SIZE * ICON_SIZE];

bool waylandIconInit(void)
{
  if (!wlWm.iconManager || !wlWm.xdgToplevel || !wlWm.shm)
    return true; // not an error, just no support

  for (size_t i = 0; i < ICON_TOTAL_WORDS; ++i)
  {
    if (i >= icondataSize / sizeof(icondata[0]))
    {
      DEBUG_ERROR("Icon data array is smaller than expected");
      return true;
    }
    uint32_t val = (uint32_t)icondata[i];
    // First two words are width/height, skip them
    if (i >= ICON_HEADER_WORDS)
      g_iconPixels[i - ICON_HEADER_WORDS] = val;
  }

  // Create shared memory buffer for the icon
  size_t dataSize = sizeof(g_iconPixels);
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

  memcpy(shm_data, g_iconPixels, dataSize);
  munmap(shm_data, dataSize);

  struct wl_shm_pool * pool = wl_shm_create_pool(wlWm.shm, fd, (int32_t)dataSize);
  buffer = wl_shm_pool_create_buffer(pool, 0, ICON_SIZE, ICON_SIZE,
      ICON_SIZE * 4, WL_SHM_FORMAT_ARGB8888);
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
  xdg_toplevel_icon_v1_destroy(icon);
  wl_buffer_destroy(buffer);

  close(fd);
  return true;

fail:
  if (icon)
    xdg_toplevel_icon_v1_destroy(icon);
  if (buffer)
    wl_buffer_destroy(buffer);
  close(fd);

  return true; // not fatal
}
