/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

typedef struct EGL_TexFormat
{
  size_t       bpp;
  GLenum       format;
  GLenum       intFormat;
  GLenum       dataType;
  unsigned int fourcc;
  size_t       bufferSize;

  size_t       width, height;
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

bool eglTexUtilGetFormat(const EGL_TexSetup * setup, EGL_TexFormat * fmt);
bool eglTexUtilGenBuffers(const EGL_TexFormat * fmt, EGL_TexBuffer * buffers,
    int count);
void eglTexUtilFreeBuffers(EGL_TexBuffer * buffers, int count);
bool eglTexUtilMapBuffer(EGL_TexBuffer * buffer);
void eglTexUtilUnmapBuffer(EGL_TexBuffer * buffer);
