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

#define EGL_TEX_BUFFER_MAX 2

typedef struct TextureBuffer
{
  EGL_Texture base;
  bool free;

  int           texCount;
  GLuint        tex[EGL_TEX_BUFFER_MAX];
  GLuint        sampler;
  EGL_TexBuffer buf[EGL_TEX_BUFFER_MAX];
  int           bufFree;
  GLsync        sync;
  LG_Lock       copyLock;
  int           bufIndex;
  int           rIndex;
}
TextureBuffer;

bool egl_texBufferInit(EGL_Texture ** texture_, EGLDisplay * display);
void egl_texBufferFree(EGL_Texture * texture_);
bool egl_texBufferSetup(EGL_Texture * texture_, const EGL_TexSetup * setup);
EGL_TexStatus egl_texBufferProcess(EGL_Texture * texture_);
EGL_TexStatus egl_texBufferGet(EGL_Texture * texture_, GLuint * tex);

bool egl_texBufferStreamInit(EGL_Texture ** texture_, EGLDisplay * display);
bool egl_texBufferStreamSetup(EGL_Texture * texture_,
    const EGL_TexSetup * setup);
EGL_TexStatus egl_texBufferStreamProcess(EGL_Texture * texture_);
EGL_TexStatus egl_texBufferStreamGet(EGL_Texture * texture_, GLuint * tex);
