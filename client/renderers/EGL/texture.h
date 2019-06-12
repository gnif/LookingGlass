/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#pragma once

#include <stdbool.h>
#include "shader.h"

#include <GL/gl.h>

typedef struct EGL_Texture EGL_Texture;

enum EGL_PixelFormat
{
  EGL_PF_RGBA,
  EGL_PF_BGRA,
  EGL_PF_RGBA10,
  EGL_PF_YUV420
};

enum EGL_TexStatus
{
  EGL_TEX_STATUS_NOTREADY,
  EGL_TEX_STATUS_OK,
  EGL_TEX_STATUS_ERROR
};

bool egl_texture_init(EGL_Texture ** tex);
void egl_texture_free(EGL_Texture ** tex);

bool               egl_texture_setup  (EGL_Texture * texture, enum EGL_PixelFormat pixfmt, size_t width, size_t height, size_t stride, bool streaming);
bool               egl_texture_update (EGL_Texture * texture, const uint8_t * buffer);
enum EGL_TexStatus egl_texture_process(EGL_Texture * texture);
enum EGL_TexStatus egl_texture_bind          (EGL_Texture * texture);
int                egl_texture_count         (EGL_Texture * texture);