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

#include "texture.h"

#include <stdbool.h>
#include <string.h>
#include "shader.h"
#include "common/framebuffer.h"
#include "common/debug.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "texture_buffer.h"

extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;
extern const EGL_TextureOps EGL_TextureFrameBuffer;
extern const EGL_TextureOps EGL_TextureDMABUF;

bool egl_textureInit(EGL_Texture ** texture_, EGLDisplay * display,
    EGL_TexType type, bool streaming)
{
  const EGL_TextureOps * ops;

  switch(type)
  {
    case EGL_TEXTYPE_BUFFER:
      ops = streaming ? &EGL_TextureBufferStream : &EGL_TextureBuffer;
      break;

    case EGL_TEXTYPE_FRAMEBUFFER:
      DEBUG_ASSERT(streaming);
      ops = &EGL_TextureFrameBuffer;
      break;

    case EGL_TEXTYPE_DMABUF:
      DEBUG_ASSERT(streaming);
      ops = &EGL_TextureDMABUF;
      break;

    default:
      return false;
  }

  *texture_ = NULL;
  if (!ops->init(texture_, display))
    return false;

  EGL_Texture * this = *texture_;
  memcpy(&this->ops, ops, sizeof(*ops));

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
  return true;
}

void egl_textureFree(EGL_Texture ** tex)
{
  EGL_Texture * this = *tex;

  glDeleteSamplers(1, &this->sampler);

  this->ops.free(this);
  *tex = NULL;
}

bool egl_textureSetup(EGL_Texture * this, enum EGL_PixelFormat pixFmt,
    size_t width, size_t height, size_t stride)
{
  const struct EGL_TexSetup setup =
  {
    .pixFmt = pixFmt,
    .width  = width,
    .height = height,
    .stride = stride
  };

  if (!egl_texUtilGetFormat(&setup, &this->format))
    return false;

  return this->ops.setup(this, &setup);
}

bool egl_textureUpdate(EGL_Texture * this, const uint8_t * buffer)
{
  const struct EGL_TexUpdate update =
  {
    .type   = EGL_TEXTYPE_BUFFER,
    .buffer = buffer
  };

  return this->ops.update(this, &update);
}

bool egl_textureUpdateFromFrame(EGL_Texture * this,
    const FrameBuffer * frame, const FrameDamageRect * damageRects,
    int damageRectsCount)
{
  const struct EGL_TexUpdate update =
  {
    .type      = EGL_TEXTYPE_FRAMEBUFFER,
    .frame     = frame,
    .rects     = damageRects,
    .rectCount = damageRectsCount,
  };

  return this->ops.update(this, &update);
}

bool egl_textureUpdateFromDMA(EGL_Texture * this,
    const FrameBuffer * frame, const int dmaFd)
{
  const struct EGL_TexUpdate update =
  {
    .type  = EGL_TEXTYPE_DMABUF,
    .dmaFD = dmaFd
  };

  /* wait for completion */
  framebuffer_wait(frame, this->format.bufferSize);

  return this->ops.update(this, &update);
}

enum EGL_TexStatus egl_textureProcess(EGL_Texture * this)
{
  return this->ops.process(this);
}

enum EGL_TexStatus egl_textureBind(EGL_Texture * this)
{
  GLuint tex;
  EGL_TexStatus status;

  if ((status = this->ops.get(this, &tex)) != EGL_TEX_STATUS_OK)
    return status;

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glBindSampler(0, this->sampler);
  return EGL_TEX_STATUS_OK;
}
