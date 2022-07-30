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

#include "texture.h"
#include "texture_buffer.h"
#include "util.h"

#include "common/vector.h"
#include "egl_dynprocs.h"
#include "egldebug.h"

struct FdImage
{
  int fd;
  EGLImage image;
};

typedef struct TexDMABUF
{
  TextureBuffer base;

  EGLDisplay display;
  bool hasImportModifiers;
  Vector images;
}
TexDMABUF;

EGL_TextureOps EGL_TextureDMABUF;

// internal functions

static void egl_texDMABUFCleanup(TexDMABUF * this)
{
  struct FdImage * image;
  vector_forEachRef(image, &this->images)
    g_egl_dynProcs.eglDestroyImage(this->display, image->image);
  vector_clear(&this->images);
}

// dmabuf functions

static bool egl_texDMABUFInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TexDMABUF * this = calloc(1, sizeof(*this));
  *texture = &this->base.base;

  if (!vector_create(&this->images, sizeof(struct FdImage), 2))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, type, display))
  {
    vector_destroy(&this->images);
    free(this);
    *texture = NULL;
    return false;
  }

  this->display = display;

  const char * client_exts = eglQueryString(this->display, EGL_EXTENSIONS);
  this->hasImportModifiers =
    util_hasGLExt(client_exts, "EGL_EXT_image_dma_buf_import_modifiers");

  return true;
}

static void egl_texDMABUFFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(this);
  vector_destroy(&this->images);

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

  struct FdImage * fdImage;
  vector_forEachRef(fdImage, &this->images)
    if (fdImage->fd == update->dmaFD)
    {
      image = fdImage->image;
      break;
    }

  if (image == EGL_NO_IMAGE)
  {
    const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    EGLAttrib attribs[] =
    {
      EGL_WIDTH                         , texture->format.width,
      EGL_HEIGHT                        , texture->format.height,
      EGL_LINUX_DRM_FOURCC_EXT          , texture->format.fourcc,
      EGL_DMA_BUF_PLANE0_FD_EXT         , update->dmaFD,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT     , 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT      , texture->format.stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (modifier & 0xffffffff),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (modifier >> 32),
      EGL_NONE                          , EGL_NONE
    };

    if (!this->hasImportModifiers)
      attribs[12] = attribs[13] =
      attribs[14] = attribs[15] = EGL_NONE;

    image = g_egl_dynProcs.eglCreateImage(
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

    if (!vector_push(&this->images, &(struct FdImage) {
      .fd    = update->dmaFD,
      .image = image,
    }))
    {
      DEBUG_ERROR("Failed to store EGLImage");
      g_egl_dynProcs.eglDestroyImage(this->display, image);
      return false;
    }
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
