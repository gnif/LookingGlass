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

#include "texture.h"

typedef struct TexDMABUF
{
  EGL_Texture base;
}
TexDMABUF;

EGL_TextureOps EGL_TextureDMABUF;

static bool eglTexDMABUF_init(EGL_Texture ** texture_, EGLDisplay * display)
{
  TexDMABUF * texture = (TexDMABUF *)calloc(sizeof(*texture), 1);
  *texture_ = &texture->base;

  return true;
}

static void eglTexDMABUF_free(EGL_Texture * texture_)
{
  TexDMABUF * texture = UPCAST(TexDMABUF, texture_);

  free(texture);
}

static bool eglTexDMABUF_setup(EGL_Texture * texture_,
    const EGL_TexSetup * setup)
{
  TexDMABUF * texture = UPCAST(TexDMABUF, texture_);
  (void)texture;

  return false;
}

static bool eglTexDMABUF_update(EGL_Texture * texture_,
    const EGL_TexUpdate * update)
{
  TexDMABUF * texture = UPCAST(TexDMABUF, texture_);
  (void)texture;

  return false;
}

static EGL_TexStatus eglTexDMABUF_process(EGL_Texture * texture_)
{
  TexDMABUF * texture = UPCAST(TexDMABUF, texture_);
  (void)texture;

  return EGL_TEX_STATUS_ERROR;
}

static EGL_TexStatus eglTexDMABUF_bind(EGL_Texture * texture_)
{
  TexDMABUF * texture = UPCAST(TexDMABUF, texture_);
  (void)texture;

  return EGL_TEX_STATUS_ERROR;
}

EGL_TextureOps EGL_TextureDMABUF =
{
  .init        = eglTexDMABUF_init,
  .free        = eglTexDMABUF_free,
  .setup       = eglTexDMABUF_setup,
  .update      = eglTexDMABUF_update,
  .process     = eglTexDMABUF_process,
  .bind        = eglTexDMABUF_bind
};
