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

static void egl_texDMABUFCleanup(TexDMABUF * this)
{
  for (size_t i = 0; i < this->imageUsed; ++i)
    eglDestroyImage(this->display, this->images[i].image);

  this->imageUsed = 0;
}

// dmabuf functions

static bool egl_texDMABUFInit(EGL_Texture ** texture, EGLDisplay * display)
{
  TexDMABUF * this = (TexDMABUF *)calloc(1, sizeof(*this));
  *texture = &this->base.base;

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  this->display = display;
  return true;
}

static void egl_texDMABUFFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(this);
  free(this->images);

  egl_texBufferFree(&parent->base);
  free(this);
}

static bool egl_texDMABUFSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(this);

  return egl_texBufferSetup(&parent->base, setup);
}

static bool egl_texDMABUFUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_DMABUF);

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
      EGL_WIDTH                    , texture->format.width,
      EGL_HEIGHT                   , texture->format.height,
      EGL_LINUX_DRM_FOURCC_EXT     , texture->format.fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT    , update->dmaFD,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT , texture->format.stride,
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

  INTERLOCKED_SECTION(parent->copyLock,
  {
    glBindTexture(GL_TEXTURE_2D, parent->tex[parent->bufIndex]);
    g_egl_dynProcs.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

    if (parent->sync)
      glDeleteSync(parent->sync);

    parent->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  });
  glFlush();
  return true;
}

static EGL_TexStatus egl_texDMABUFProcess(EGL_Texture * texture)
{
  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texDMABUFGet(EGL_Texture * texture, GLuint * tex)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  GLsync sync = 0;

  INTERLOCKED_SECTION(parent->copyLock,
  {
    if (parent->sync)
    {
      sync           = parent->sync;
      parent->sync   = 0;
      parent->rIndex = parent->bufIndex;
      if (++parent->bufIndex == parent->texCount)
        parent->bufIndex = 0;
    }
  });

  if (sync)
  {
    switch(glClientWaitSync(sync, 0, 20000000)) // 20ms
    {
      case GL_ALREADY_SIGNALED:
      case GL_CONDITION_SATISFIED:
        glDeleteSync(sync);
        break;

      case GL_TIMEOUT_EXPIRED:
        INTERLOCKED_SECTION(parent->copyLock,
        {
          if (!parent->sync)
            parent->sync = sync;
          else
            glDeleteSync(sync);
        });
        return EGL_TEX_STATUS_NOTREADY;

      case GL_WAIT_FAILED:
      case GL_INVALID_VALUE:
        glDeleteSync(sync);
        DEBUG_GL_ERROR("glClientWaitSync failed");
        return EGL_TEX_STATUS_ERROR;
    }
  }

  *tex = parent->tex[parent->rIndex];
  return EGL_TEX_STATUS_OK;
}

EGL_TextureOps EGL_TextureDMABUF =
{
  .init        = egl_texDMABUFInit,
  .free        = egl_texDMABUFFree,
  .setup       = egl_texDMABUFSetup,
  .update      = egl_texDMABUFUpdate,
  .process     = egl_texDMABUFProcess,
  .get         = egl_texDMABUFGet
};
