/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

static struct wl_buffer * createSquareCursorBuffer(void)
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

static bool loadThemedCursor(const char * name, struct wl_surface ** surface,
    struct Point * hotspot)
{
  struct wl_cursor * cursor = wl_cursor_theme_get_cursor(wlWm.cursorTheme, name);
  if (!cursor)
    return false;

  struct wl_buffer * buffer = wl_cursor_image_get_buffer(cursor->images[0]);
  if (!buffer)
    return false;

  *surface = wl_compositor_create_surface(wlWm.compositor);
  if (!*surface)
    return NULL;

  wl_surface_attach(*surface, buffer, 0, 0);
  wl_surface_set_buffer_scale(*surface, wlWm.cursorScale);
  wl_surface_commit(*surface);

  *hotspot = (struct Point) {
    .x = cursor->images[0]->hotspot_x,
    .y = cursor->images[0]->hotspot_y,
  };
  return true;
}

static const char ** nameLists[LG_POINTER_COUNT] = {
  [LG_POINTER_ARROW      ] = (const char *[]) { "left_ptr", "arrow", NULL },
  [LG_POINTER_INPUT      ] = (const char *[]) { "text", "xterm", "ibeam", NULL },
  [LG_POINTER_MOVE       ] = (const char *[]) {
    "move", "4498f0e0c1937ffe01fd06f973665830", "9081237383d90e509aa00f00170e968f", NULL
  },
  [LG_POINTER_RESIZE_NS  ] = (const char *[]) {
    "sb_v_double_arrow", "size_ver", "v_double_arrow",
    "2870a09082c103050810ffdffffe0204", "00008160000006810000408080010102", NULL
  },
  [LG_POINTER_RESIZE_EW  ] = (const char *[]) {
    "sb_h_double_arrow", "size_hor", "h_double_arrow",
    "14fef782d02440884392942c11205230", "028006030e0e7ebffc7f7070c0600140", NULL
  },
  [LG_POINTER_RESIZE_NESW] = (const char *[]) {
    "fd_double_arrow", "size_bdiag", "fcf1c3c7cd4491d801f1e1c78f100000", NULL
  },
  [LG_POINTER_RESIZE_NWSE] = (const char *[]) {
    "bd_double_arrow", "size_fdiag", "c7088f0f3e6c8088236ef8e1e3e70000", NULL
  },
  [LG_POINTER_HAND       ] = (const char *[]) {
    "hand", "pointing_hand", "hand1", "hand2", "pointer",
    "e29285e634086352946a0e7090d73106", "9d800788f1b08800ae810202380a0822", NULL
  },
  [LG_POINTER_NOT_ALLOWED] = (const char *[]) { "crossed_circle", "not-allowed", NULL },
};

static void reloadCursors(void)
{
  if (wlWm.cursorTheme)
    for (LG_DSPointer pointer = LG_POINTER_ARROW; pointer < LG_POINTER_COUNT; ++pointer)
      for (const char ** names = nameLists[pointer]; *names; ++names)
        if (loadThemedCursor(*names, wlWm.cursors + pointer, wlWm.cursorHot + pointer))
          break;
}

bool waylandCursorInit(void)
{
  if (!wlWm.compositor)
  {
    DEBUG_ERROR("Compositor missing wl_compositor, will not proceed");
    return false;
  }

  wlWm.cursorSquareBuffer = createSquareCursorBuffer();
  if (wlWm.cursorSquareBuffer)
  {
    wlWm.cursors[LG_POINTER_SQUARE] = wl_compositor_create_surface(wlWm.compositor);
    wl_surface_attach(wlWm.cursors[LG_POINTER_SQUARE], wlWm.cursorSquareBuffer, 0, 0);
    wl_surface_commit(wlWm.cursors[LG_POINTER_SQUARE]);
  }

  wlWm.cursorThemeName = getenv("XCURSOR_THEME");
  wlWm.cursorSize      = 24;

  const char * cursorSizeEnv = getenv("XCURSOR_SIZE");
  if (cursorSizeEnv)
  {
    int size = atoi(cursorSizeEnv);
    if (size)
      wlWm.cursorSize = size;
  }

  wlWm.cursorTheme = wl_cursor_theme_load(wlWm.cursorThemeName, wlWm.cursorSize, wlWm.shm);
  wlWm.cursorScale = 1;
  reloadCursors();

  return true;
}

void waylandCursorFree(void)
{
  for (int i = 0; i < LG_POINTER_COUNT; ++i)
    if (wlWm.cursors[i])
      wl_surface_destroy(wlWm.cursors[i]);
  if (wlWm.cursorTheme)
    wl_cursor_theme_destroy(wlWm.cursorTheme);
  if (wlWm.cursorSquareBuffer)
    wl_buffer_destroy(wlWm.cursorSquareBuffer);
}

void waylandCursorScaleChange(void)
{
  int newScale = ceil(wl_fixed_to_double(wlWm.scale));
  if (newScale == wlWm.cursorScale)
    return;

  struct wl_cursor_theme * new = wl_cursor_theme_load(wlWm.cursorThemeName,
      wlWm.cursorSize * newScale, wlWm.shm);

  if (!new)
    return;

  struct wl_surface * old[LG_POINTER_COUNT];
  memcpy(old, wlWm.cursors, sizeof(old));
  memset(wlWm.cursors, 0, sizeof(wlWm.cursors));

  if (wlWm.cursorTheme)
    wl_cursor_theme_destroy(wlWm.cursorTheme);

  wlWm.cursorTheme = new;
  wlWm.cursorScale = newScale;
  reloadCursors();

  waylandSetPointer(wlWm.cursorId);

  for (int i = 0; i < LG_POINTER_COUNT; ++i)
    if (old[i])
      wl_surface_destroy(old[i]);
}

void waylandSetPointer(LG_DSPointer pointer)
{
  wlWm.cursorId   = pointer;
  wlWm.cursor     = wlWm.cursors[pointer];
  wlWm.cursorHotX = wlWm.cursorHot[pointer].x;
  wlWm.cursorHotY = wlWm.cursorHot[pointer].y;
  if (wlWm.pointer)
    wl_pointer_set_cursor(wlWm.pointer, wlWm.pointerEnterSerial, wlWm.cursor, wlWm.cursorHotX, wlWm.cursorHotY);
}
