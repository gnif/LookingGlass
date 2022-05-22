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

#include "egltypes.h"

//typedef struct EGL_TexSetup EGL_TexSetup;

typedef struct EGL_TexFormat
{
  EGL_PixelFormat pixFmt;

  size_t       bpp;
  GLenum       format;
  GLenum       intFormat;
  GLenum       dataType;
  unsigned int fourcc;
  size_t       bufferSize;

  size_t       width , height;
  size_t       stride, pitch;
}
EGL_TexFormat;

typedef struct EGL_TexBuffer
{
  size_t size;
  GLuint pbo;
  void * map;
  bool   updated;
}
EGL_TexBuffer;

bool egl_texUtilGetFormat(const EGL_TexSetup * setup, EGL_TexFormat * fmt);
bool egl_texUtilGenBuffers(const EGL_TexFormat * fmt, EGL_TexBuffer * buffers,
    int count);
void egl_texUtilFreeBuffers(EGL_TexBuffer * buffers, int count);
bool egl_texUtilMapBuffer(EGL_TexBuffer * buffer);
void egl_texUtilUnmapBuffer(EGL_TexBuffer * buffer);
