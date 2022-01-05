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

#pragma once

#include "texture.h"

typedef struct EGL_Framebuffer EGL_Framebuffer;

bool egl_framebufferInit(EGL_Framebuffer ** fb);
void egl_framebufferFree(EGL_Framebuffer ** fb);

bool egl_framebufferSetup(EGL_Framebuffer * this, enum EGL_PixelFormat pixFmt,
    unsigned int width, unsigned int height);

void egl_framebufferBind(EGL_Framebuffer * this);

GLuint egl_framebufferGetTexture(EGL_Framebuffer * this);
