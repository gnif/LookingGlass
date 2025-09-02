/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "common/array.h"
#include "egl_dynprocs.h"
#include "egldebug.h"

struct FdImage
{
  int fd;
  EGLImage image;
  GLsync   sync;
  int      texIndex;
};

typedef struct TexDMABUF
{
  TextureBuffer base;

  EGLDisplay display;

  struct FdImage images[2];
  int            lastIndex;

  EGL_PixelFormat pixFmt;
  unsigned        fourcc;
  unsigned        width;
  GLuint          format;
}
TexDMABUF;

EGL_TextureOps EGL_TextureDMABUF;

static bool initDone           = false;
static bool has24BitSupport    = true;
static bool hasImportModifiers = true;

// internal functions

static void egl_texDMABUFCleanup(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  for(int i = 0; i < ARRAY_LENGTH(this->images); ++i)
  {
    if (this->images[i].image != EGL_NO_IMAGE)
    {
      g_egl_dynProcs.eglDestroyImage(this->display, this->images[i].image);
      this->images[i].image = EGL_NO_IMAGE;
    }
    if (this->images[i].sync)
    {
      glDeleteSync(this->images[i].sync);
      this->images[i].sync = 0;
    }
    this->images[i].fd       = -1;
    this->images[i].texIndex = -1;
  }

  egl_texUtilFreeBuffers(parent->buf, parent->texCount);

  if (parent->tex[0])
    glDeleteTextures(parent->texCount, parent->tex);
}

// dmabuf functions

static bool egl_texDMABUFInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TexDMABUF * this = calloc(1, sizeof(*this));
  *texture = &this->base.base;

  for(int i = 0; i < ARRAY_LENGTH(this->images); ++i)
  {
    this->images[i].fd       = -1;
    this->images[i].image    = EGL_NO_IMAGE;
    this->images[i].sync     = 0;
    this->images[i].texIndex = -1;
  }
  this->lastIndex = -1;

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, type, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  this->display = display;

  if (!initDone)
  {
    const char * client_exts = eglQueryString(this->display, EGL_EXTENSIONS);
    hasImportModifiers =
      util_hasGLExt(client_exts, "EGL_EXT_image_dma_buf_import_modifiers");
    initDone = true;
  }

  return true;
}

static void egl_texDMABUFFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(texture);
  egl_texBufferFree(&parent->base);
  free(this);
}

static bool texDMABUFSetup(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  if (texture->format.pixFmt == EGL_PF_RGB_24 && !has24BitSupport)
  {
    this->pixFmt = EGL_PF_RGB_24_32;
    this->width  = texture->format.pitch / 4;
    this->fourcc = DRM_FORMAT_ARGB8888;
    this->format = GL_BGRA_EXT;
  }

  egl_texDMABUFCleanup(texture);

  glGenTextures(parent->texCount, parent->tex);
  parent->rIndex = -1;

  return true;
}

static bool egl_texDMABUFSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  this->pixFmt = texture->format.pixFmt;
  this->width  = texture->format.width;
  this->fourcc = texture->format.fourcc;
  this->format = texture->format.format;

  return texDMABUFSetup(texture);
}

static EGLImage createImage(EGL_Texture * texture, int fd)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  EGLAttrib attribs[] =
  {
    EGL_WIDTH                         , this->width ,
    EGL_HEIGHT                        , texture->format.height,
    EGL_LINUX_DRM_FOURCC_EXT          , this->fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT         , fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT     , 0,
    EGL_DMA_BUF_PLANE0_PITCH_EXT      , texture->format.pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (modifier & 0xffffffff),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (modifier >> 32),
    EGL_NONE                          , EGL_NONE
  };

  if (!hasImportModifiers)
    attribs[12] = attribs[13] =
    attribs[14] = attribs[15] = EGL_NONE;

  return g_egl_dynProcs.eglCreateImage(
      this->display,
      EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer)NULL,
      attribs);
}

static bool egl_texDMABUFUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_DMABUF);

  struct FdImage *fdImage =
    (this->images[0].fd == update->dmaFD) ? &this->images[0] :
    (this->images[1].fd == update->dmaFD) ? &this->images[1] :
    (this->images[0].fd == -1)            ? &this->images[0] :
                                            &this->images[1];
  EGLImage image = fdImage->image;
  if (unlikely(image == EGL_NO_IMAGE))
  {
    bool setup = false;
    if (texture->format.pixFmt == EGL_PF_RGB_24 && has24BitSupport)
    {
      image = createImage(texture, update->dmaFD);
      if (image == EGL_NO_IMAGE)
      {
        DEBUG_INFO("Using 24-bit in 32-bit for DMA");
        has24BitSupport = false;
        setup = true;
      }
    }

    if (!has24BitSupport && setup)
      texDMABUFSetup(texture);

    if (image == EGL_NO_IMAGE)
      image = createImage(texture, update->dmaFD);

    if (unlikely(image == EGL_NO_IMAGE))
    {
      DEBUG_EGL_ERROR("Failed to create EGLImage for DMA transfer");
      return false;
    }

    fdImage->fd    = update->dmaFD;
    fdImage->image = image;

    int slot = (fdImage == &this->images[0]) ? 0 : 1;
    fdImage->texIndex = slot;
    INTERLOCKED_SECTION(parent->copyLock,
    {
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, parent->tex[slot]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      g_egl_dynProcs.glEGLImageTargetTexture2DOES(
          GL_TEXTURE_EXTERNAL_OES, image);
    });
  }

  this->lastIndex = (fdImage == &this->images[0]) ? 0 : 1;
  INTERLOCKED_SECTION(parent->copyLock,
  {
    if (fdImage->sync)
      glDeleteSync(fdImage->sync);
    fdImage->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  });

  return true;
}

static EGL_TexStatus egl_texDMABUFProcess(EGL_Texture * texture)
{
  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texDMABUFGet(EGL_Texture * texture, GLuint * tex,
    EGL_PixelFormat * fmt)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  if (unlikely(this->lastIndex < 0))
    return EGL_TEX_STATUS_NOTREADY;

  struct FdImage *cur = &this->images[this->lastIndex];
  GLsync sync = 0;

  INTERLOCKED_SECTION(parent->copyLock, {
    if (cur->sync) {
      sync = cur->sync;
      cur->sync = 0;
    }
  });

  if (sync)
  {
    switch (glClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, 20000000)) //20ms
    {
      case GL_ALREADY_SIGNALED:
      case GL_CONDITION_SATISFIED:
        glDeleteSync(sync);
        break;

      case GL_TIMEOUT_EXPIRED:
        // Put it back for next try
        INTERLOCKED_SECTION(parent->copyLock,
        {
          if (!cur->sync)
            cur->sync = sync;
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

  *tex = parent->tex[cur->texIndex];

  if (fmt)
    *fmt = this->pixFmt;

  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texDMABUFBind(EGL_Texture * texture)
{
  GLuint tex;
  EGL_TexStatus status;

  if ((status = egl_texDMABUFGet(texture, &tex, NULL)) != EGL_TEX_STATUS_OK)
    return status;

  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  return EGL_TEX_STATUS_OK;
}

EGL_TextureOps EGL_TextureDMABUF =
{
  .init        = egl_texDMABUFInit,
  .free        = egl_texDMABUFFree,
  .setup       = egl_texDMABUFSetup,
  .update      = egl_texDMABUFUpdate,
  .process     = egl_texDMABUFProcess,
  .get         = egl_texDMABUFGet,
  .bind        = egl_texDMABUFBind
};
