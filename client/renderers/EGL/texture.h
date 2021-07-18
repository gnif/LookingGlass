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
#include "shader.h"
#include "common/framebuffer.h"

#include <GL/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

typedef struct EGL_Texture EGL_Texture;

enum EGL_PixelFormat
{
  EGL_PF_RGBA,
  EGL_PF_BGRA,
  EGL_PF_RGBA10,
  EGL_PF_RGBA16F,
  EGL_PF_YUV420
};

enum EGL_TexStatus
{
  EGL_TEX_STATUS_NOTREADY,
  EGL_TEX_STATUS_OK,
  EGL_TEX_STATUS_ERROR
};

bool egl_texture_init(EGL_Texture ** texture, EGLDisplay * display);
void egl_texture_free(EGL_Texture ** tex);

bool               egl_texture_setup  (EGL_Texture * texture, enum EGL_PixelFormat pixfmt, size_t width, size_t height, size_t stride, bool streaming, bool useDMA);
bool               egl_texture_update (EGL_Texture * texture, const uint8_t * buffer);
bool               egl_texture_update_from_frame(EGL_Texture * texture, const FrameBuffer * frame);
bool               egl_texture_update_from_dma  (EGL_Texture * texture, const FrameBuffer * frmame, const int dmaFd);
enum EGL_TexStatus egl_texture_process(EGL_Texture * texture);
enum EGL_TexStatus egl_texture_bind          (EGL_Texture * texture);
int                egl_texture_count         (EGL_Texture * texture);
