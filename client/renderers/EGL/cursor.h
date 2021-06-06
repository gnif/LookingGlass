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

#pragma once

#include <stdbool.h>

#include "interface/renderer.h"

typedef struct EGL_Cursor EGL_Cursor;

struct CursorState {
  bool visible;
  struct Rect rect;
};

bool egl_cursor_init(EGL_Cursor ** cursor);
void egl_cursor_free(EGL_Cursor ** cursor);

bool egl_cursor_set_shape(
    EGL_Cursor * cursor,
    const LG_RendererCursor type,
    const int width,
    const int height,
    const int stride,
    const uint8_t * data);

void egl_cursor_set_size(EGL_Cursor * cursor, const float x, const float y);

void egl_cursor_set_state(EGL_Cursor * cursor, const bool visible,
    const float x, const float y);

struct CursorState egl_cursor_get_state(EGL_Cursor * cursor, int width, int height);

void egl_cursor_render(EGL_Cursor * cursor, LG_RendererRotate rotate);
