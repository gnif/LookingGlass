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
#include "texture_buffer.h"

#include <assert.h>

#include "egl_dynprocs.h"
#include "egldebug.h"

typedef struct TexDMABUF
{
  TextureBuffer base;

  EGLDisplay display;

  size_t imageCount;
  size_t imageUsed;
  struct
  {
    int fd;
    EGLImage image;
  }
  * images;
}
TexDMABUF;

EGL_TextureOps EGL_TextureDMABUF;

// internal functions

static void eglTexDMABUF_cleanup(TexDMABUF * this)
{
  for (size_t i = 0; i < this->imageUsed; ++i)
    eglDestroyImage(this->display, this->images[i].image);

  this->imageUsed = 0;
}

// dmabuf functions

static bool eglTexDMABUF_init(EGL_Texture ** texture, EGLDisplay * display)
{
  TexDMABUF * this = (TexDMABUF *)calloc(sizeof(*this), 1);
  *texture = &this->base.base;

  EGL_Texture * parent = &this->base.base;
  if (!eglTexBuffer_init(&parent, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  this->display = display;
  return true;
}

static void eglTexDMABUF_free(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  eglTexDMABUF_cleanup(this);
  free(this->images);

  eglTexBuffer_free(&parent->base);
  free(this);
}

static bool eglTexDMABUF_setup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  eglTexDMABUF_cleanup(this);

  if (!eglTexBuffer_setup(&parent->base, setup))
    return false;

  glBindTexture(GL_TEXTURE_2D, parent->tex[0]);
  glTexImage2D(GL_TEXTURE_2D,
      0,
      parent->format.intFormat,
      parent->format.width,
      parent->format.height,
      0,
      parent->format.format,
      parent->format.dataType,
      NULL);

  return true;
}

static bool eglTexDMABUF_update(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);
  assert(update->type == EGL_TEXTYPE_DMABUF);

  EGLImage image = EGL_NO_IMAGE;

  for(int i = 0; i < this->imageUsed; ++i)
    if (this->images[i].fd == update->dmaFD)
    {
      image = this->images[i].image;
      break;
    }

  if (image == EGL_NO_IMAGE)
  {
    EGLAttrib const attribs[] =
    {
      EGL_WIDTH                    , parent->format.width,
      EGL_HEIGHT                   , parent->format.height,
      EGL_LINUX_DRM_FOURCC_EXT     , parent->format.fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT    , update->dmaFD,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT , parent->format.stride,
      EGL_NONE                     , EGL_NONE
    };

    image = eglCreateImage(
        this->display,
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        (EGLClientBuffer)NULL,
        attribs);

    if (image == EGL_NO_IMAGE)
    {
      DEBUG_EGL_ERROR("Failed to create EGLImage for DMA transfer");
      return false;
    }

    if (this->imageUsed == this->imageCount)
    {
      size_t newCount = this->imageCount * 2 + 2;
      void * new = realloc(this->images, newCount * sizeof(*this->images));
      if (!new)
      {
        DEBUG_ERROR("Failed to allocate memory");
        eglDestroyImage(this->display, image);
        return false;
      }

      this->imageCount = newCount;
      this->images     = new;
    }

    const size_t index = this->imageUsed++;
    this->images[index].fd    = update->dmaFD;
    this->images[index].image = image;
  }

  glBindTexture(GL_TEXTURE_2D, parent->tex[0]);
  g_egl_dynProcs.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
  return true;
}

static EGL_TexStatus eglTexDMABUF_process(EGL_Texture * texture)
{
  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus eglTexDMABUF_bind(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, parent->tex[0]);
  glBindSampler(0, parent->sampler);

  return EGL_TEX_STATUS_OK;
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
