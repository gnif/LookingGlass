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
#include <assert.h>
#include "shader.h"
#include "common/framebuffer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "texture_buffer.h"

extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;
extern const EGL_TextureOps EGL_TextureFrameBuffer;
extern const EGL_TextureOps EGL_TextureDMABUF;

bool egl_texture_init(EGL_Texture ** texture, EGLDisplay * display,
    EGL_TexType type, bool streaming)
{
  const EGL_TextureOps * ops;

  switch(type)
  {
    case EGL_TEXTYPE_BUFFER:
      ops = streaming ? &EGL_TextureBufferStream : &EGL_TextureBuffer;
      break;

    case EGL_TEXTYPE_FRAMEBUFFER:
      assert(streaming);
      ops = &EGL_TextureFrameBuffer;
      break;

    case EGL_TEXTYPE_DMABUF:
      assert(streaming);
      ops = &EGL_TextureDMABUF;
      break;

    default:
      return false;
  }

  *texture = NULL;
  if (!ops->init(texture, display))
    return false;

  (*texture)->ops = ops;
  return true;
}

void egl_texture_free(EGL_Texture ** tex)
{
  (*tex)->ops->free(*tex);
  *tex = NULL;
}

bool egl_texture_setup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt,
    size_t width, size_t height, size_t stride)
{
  const struct EGL_TexSetup setup =
  {
    .pixFmt = pixFmt,
    .width  = width,
    .height = height,
    .stride = stride
  };
  texture->size = height * stride;
  return texture->ops->setup(texture, &setup);
}

bool egl_texture_update(EGL_Texture * texture, const uint8_t * buffer)
{
  const struct EGL_TexUpdate update =
  {
    .type   = EGL_TEXTYPE_BUFFER,
    .buffer = buffer
  };
  return texture->ops->update(texture, &update);
}

bool egl_texture_update_from_frame(EGL_Texture * texture,
    const FrameBuffer * frame)
{
  const struct EGL_TexUpdate update =
  {
    .type  = EGL_TEXTYPE_FRAMEBUFFER,
    .frame = frame
  };
  return texture->ops->update(texture, &update);
}

bool egl_texture_update_from_dma(EGL_Texture * texture,
    const FrameBuffer * frame, const int dmaFd)
{
  const struct EGL_TexUpdate update =
  {
    .type  = EGL_TEXTYPE_DMABUF,
    .dmaFD = dmaFd
  };

  /* wait for completion */
  framebuffer_wait(frame, texture->size);

  return texture->ops->update(texture, &update);
}

enum EGL_TexStatus egl_texture_process(EGL_Texture * texture)
{
  return texture->ops->process(texture);
}

enum EGL_TexStatus egl_texture_bind(EGL_Texture * texture)
{
  return texture->ops->bind(texture);
}
