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

#include "interface/font.h"

typedef struct EGL_Help EGL_Help;

bool egl_help_init(EGL_Help ** help, const LG_Font * font, LG_FontObj fontObj);
void egl_help_free(EGL_Help ** help);

void egl_help_set_text(EGL_Help * help, const char * help_text);
void egl_help_set_font(EGL_Help * help, LG_FontObj fontObj);
void egl_help_render(EGL_Help * help, const float scaleX, const float scaleY);
