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

#define _GNU_SOURCE
#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "common/debug.h"

static const uint32_t cursorBitmap[] = {
  0x000000, 0x000000, 0x000000, 0x000000,
  0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000,
  0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000,
  0x000000, 0x000000, 0x000000, 0x000000,
};

static struct wl_buffer * createCursorBuffer(void)
{
  int fd = memfd_create("lg-cursor", 0);
  if (fd < 0)
  {
    DEBUG_ERROR("Failed to create cursor shared memory: %d", errno);
    return NULL;
  }

  struct wl_buffer * result = NULL;

  if (ftruncate(fd, sizeof cursorBitmap) < 0)
  {
    DEBUG_ERROR("Failed to ftruncate cursor shared memory: %d", errno);
    goto fail;
  }

  void * shm_data = mmap(NULL, sizeof cursorBitmap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_data == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to map memory for cursor: %d", errno);
    goto fail;
  }

  struct wl_shm_pool * pool = wl_shm_create_pool(wlWm.shm, fd, sizeof cursorBitmap);
  result = wl_shm_pool_create_buffer(pool, 0, 4, 4, 16, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);

  memcpy(shm_data, cursorBitmap, sizeof cursorBitmap);
  munmap(shm_data, sizeof cursorBitmap);

fail:
  close(fd);
  return result;
}

bool waylandCursorInit(void)
{
  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor, will not proceed");
    return false;
  }

  wlWm.cursorBuffer = createCursorBuffer();
  if (wlWm.cursorBuffer)
  {
    wlWm.cursor = wl_compositor_create_surface(wlWm.compositor);
    wl_surface_attach(wlWm.cursor, wlWm.cursorBuffer, 0, 0);
    wl_surface_commit(wlWm.cursor);
  }

  return true;
}

void waylandCursorFree(void)
{
  if (wlWm.cursor)
    wl_surface_destroy(wlWm.cursor);
  if (wlWm.cursorBuffer)
    wl_buffer_destroy(wlWm.cursorBuffer);
}

void waylandShowPointer(bool show)
{
  wlWm.showPointer = show;
  wl_pointer_set_cursor(wlWm.pointer, wlWm.pointerEnterSerial, show ? wlWm.cursor : NULL, 0, 0);
}
