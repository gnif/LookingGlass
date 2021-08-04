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
#include "texture_util.h"
#include "common/locking.h"

typedef struct TextureBuffer
{
  EGL_Texture base;
  bool free;

  EGL_TexFormat format;
  int           texCount;
  GLuint        tex[2];
  GLuint        sampler;
  EGL_TexBuffer buf[2];
  int           bufFree;
  GLsync        sync;
  LG_Lock       copyLock;
  int           bufIndex;
  int           rIndex;
}
TextureBuffer;

bool eglTexBuffer_init(EGL_Texture ** texture_, EGLDisplay * display);
void eglTexBuffer_free(EGL_Texture * texture_);
bool eglTexBuffer_setup(EGL_Texture * texture_, const EGL_TexSetup * setup);
EGL_TexStatus eglTexBuffer_process(EGL_Texture * texture_);
EGL_TexStatus eglTexBuffer_bind(EGL_Texture * texture_);

bool eglTexBuffer_stream_init(EGL_Texture ** texture_, EGLDisplay * display);
bool eglTexBuffer_stream_setup(EGL_Texture * texture_,
    const EGL_TexSetup * setup);
EGL_TexStatus eglTexBuffer_stream_process(EGL_Texture * texture_);
EGL_TexStatus eglTexBuffer_stream_bind(EGL_Texture * texture_);
