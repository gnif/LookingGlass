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

#include <assert.h>

#include "texture_buffer.h"
#include "common/debug.h"

static bool eglTexFB_update(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  assert(update->type == EGL_TEXTYPE_FRAMEBUFFER);

  LG_LOCK(this->copyLock);

  framebuffer_read(
    update->frame,
    this->buf[this->bufIndex].map,
    this->format.stride,
    this->format.height,
    this->format.width,
    this->format.bpp,
    this->format.stride
  );

  this->buf[this->bufIndex].updated = true;
  LG_UNLOCK(this->copyLock);

  return true;
}

EGL_TextureOps EGL_TextureFrameBuffer =
{
  .init        = eglTexBuffer_stream_init,
  .free        = eglTexBuffer_free,
  .setup       = eglTexBuffer_stream_setup,
  .update      = eglTexFB_update,
  .process     = eglTexBuffer_stream_process,
  .bind        = eglTexBuffer_stream_bind
};
