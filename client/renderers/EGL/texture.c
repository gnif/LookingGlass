/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "texture.h"
#include "common/debug.h"
#include "common/framebuffer.h"
#include "debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <libdrm/drm_fourcc.h>

#include <SDL2/SDL_egl.h>

/* this must be a multiple of 2 */
#define TEXTURE_COUNT 2

struct Tex
{
  GLuint   t;
  bool     hasPBO;
  GLuint   pbo;
  void *   map;
  GLsync   sync;
};

struct TexState
{
  _Atomic(uint8_t) w, u, s, d;
};

struct EGL_Texture
{
  EGLDisplay * display;

  enum   EGL_PixelFormat pixFmt;
  size_t bpp;
  bool   streaming;
  bool   dma;
  bool   ready;

  GLuint       sampler;
  size_t       width, height, stride, pitch;
  GLenum       intFormat;
  GLenum       format;
  GLenum       dataType;
  unsigned int fourcc;
  size_t       pboBufferSize;

  struct TexState state;
  int             textureCount;
  struct Tex      tex[TEXTURE_COUNT];
};

bool egl_texture_init(EGL_Texture ** texture, EGLDisplay * display)
{
  *texture = (EGL_Texture *)malloc(sizeof(EGL_Texture));
  if (!*texture)
  {
    DEBUG_ERROR("Failed to malloc EGL_Texture");
    return false;
  }

  memset(*texture, 0, sizeof(EGL_Texture));
  (*texture)->display = display;
  return true;
}

void egl_texture_free(EGL_Texture ** texture)
{
  if (!*texture)
    return;

  glDeleteSamplers(1, &(*texture)->sampler);

  for(int i = 0; i < (*texture)->textureCount; ++i)
  {
    struct Tex * t = &(*texture)->tex[i];
    if (t->hasPBO)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, t->pbo);
      if ((*texture)->tex[i].map)
      {
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        (*texture)->tex[i].map = NULL;
      }
      glDeleteBuffers(1, &t->pbo);
      if (t->sync)
        glDeleteSync(t->sync);
    }

    glDeleteTextures(1, &t->t);
  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  free(*texture);
  *texture = NULL;
}

static bool egl_texture_map(EGL_Texture * texture, uint8_t i)
{
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[i].pbo);
  texture->tex[i].map = glMapBufferRange(
    GL_PIXEL_UNPACK_BUFFER,
    0,
    texture->pboBufferSize,
    GL_MAP_WRITE_BIT             |
    GL_MAP_UNSYNCHRONIZED_BIT    |
    GL_MAP_INVALIDATE_BUFFER_BIT
  );
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  if (!texture->tex[i].map)
  {
    EGL_ERROR("glMapBufferRange failed for %d of %lu bytes", i, texture->pboBufferSize);
    return false;
  }

  return true;
}

static void egl_texture_unmap(EGL_Texture * texture, uint8_t i)
{
  if (!texture->tex[i].map)
    return;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[i].pbo);
  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  texture->tex[i].map = NULL;
}

bool egl_texture_setup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt, size_t width, size_t height, size_t stride, bool streaming, bool useDMA)
{
  if (texture->streaming)
  {
    for(int i = 0; i < texture->textureCount; ++i)
    {
      if (!useDMA)
      {
        egl_texture_unmap(texture, i);
        if (texture->tex[i].hasPBO)
        {
          glDeleteBuffers(1, &texture->tex[i].pbo);
          texture->tex[i].hasPBO = false;
        }
      }
    }
  }

  texture->pixFmt       = pixFmt;
  texture->width        = width;
  texture->height       = height;
  texture->stride       = stride;
  texture->streaming    = streaming;
  texture->textureCount = streaming ? TEXTURE_COUNT : 1;
  texture->ready        = false;

  atomic_store_explicit(&texture->state.w, 0, memory_order_relaxed);
  atomic_store_explicit(&texture->state.u, 0, memory_order_relaxed);
  atomic_store_explicit(&texture->state.s, 0, memory_order_relaxed);
  atomic_store_explicit(&texture->state.d, 0, memory_order_relaxed);

  switch(pixFmt)
  {
    case EGL_PF_BGRA:
      texture->bpp           = 4;
      texture->format        = GL_BGRA;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->fourcc        = DRM_FORMAT_ARGB8888;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA:
      texture->bpp           = 4;
      texture->format        = GL_RGBA;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->fourcc        = DRM_FORMAT_ABGR8888;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA10:
      texture->bpp           = 4;
      texture->format        = GL_RGBA;
      texture->intFormat     = GL_RGB10_A2;
      texture->dataType      = GL_UNSIGNED_INT_2_10_10_10_REV;
      texture->fourcc        = DRM_FORMAT_BGRA1010102;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA16F:
      texture->bpp           = 8;
      texture->format        = GL_RGBA;
      texture->intFormat     = GL_RGBA16F;
      texture->dataType      = GL_HALF_FLOAT;
      texture->fourcc        = DRM_FORMAT_ABGR16161616F;
      texture->pboBufferSize = height * stride;
      break;

    default:
      DEBUG_ERROR("Unsupported pixel format");
      return false;
  }

  texture->pitch = stride / texture->bpp;

  for(int i = 0; i < texture->textureCount; ++i)
  {
    if (texture->tex[0].t)
      glDeleteTextures(1, &texture->tex[i].t);
    glGenTextures(1, &texture->tex[i].t);
  }

  if (!texture->sampler)
  {
    glGenSamplers(1, &texture->sampler);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
  }

  if (useDMA)
    return true;

  for(int i = 0; i < texture->textureCount; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, texture->tex[i].t);
    glTexImage2D(GL_TEXTURE_2D, 0, texture->intFormat, texture->width,
      texture->height, 0, texture->format, texture->dataType, NULL);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!streaming)
    return true;

  for(int i = 0; i < texture->textureCount; ++i)
  {
    glGenBuffers(1, &texture->tex[i].pbo);
    texture->tex[i].hasPBO = true;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[i].pbo);
    glBufferStorage(
      GL_PIXEL_UNPACK_BUFFER,
      texture->pboBufferSize,
      NULL,
      GL_MAP_WRITE_BIT
    );
  }

  return true;
}

static void egl_warn_slow()
{
  static bool warnDone = false;
  if (!warnDone)
  {
    warnDone = true;
    DEBUG_BREAK();
    DEBUG_WARN("The guest is providing updates faster then your computer can display them");
    DEBUG_WARN("This is a hardware limitation, expect microstutters & frame skips");
    DEBUG_BREAK();
  }
}

bool egl_texture_update(EGL_Texture * texture, const uint8_t * buffer)
{
  if (texture->streaming)
  {
    const uint8_t sw =
      atomic_load_explicit(&texture->state.w, memory_order_acquire);

    if (atomic_load_explicit(&texture->state.u, memory_order_acquire) == (uint8_t)(sw + 1))
    {
      egl_warn_slow();
      return true;
    }

    const uint8_t t = sw % TEXTURE_COUNT;
    if (!egl_texture_map(texture, t))
      return EGL_TEX_STATUS_ERROR;

    memcpy(texture->tex[t].map, buffer, texture->pboBufferSize);
    atomic_fetch_add_explicit(&texture->state.w, 1, memory_order_release);
    egl_texture_unmap(texture, t);
  }
  else
  {
    glBindTexture(GL_TEXTURE_2D, texture->tex[0].t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->pitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height,
        texture->format, texture->dataType, buffer);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  return true;
}

bool egl_texture_update_from_frame(EGL_Texture * texture, const FrameBuffer * frame)
{
  if (!texture->streaming)
    return false;

  const uint8_t sw =
    atomic_load_explicit(&texture->state.w, memory_order_acquire);

  if (atomic_load_explicit(&texture->state.u, memory_order_acquire) == (uint8_t)(sw + 1))
  {
    egl_warn_slow();
    return true;
  }

  const uint8_t t = sw % TEXTURE_COUNT;
  if (!egl_texture_map(texture, t))
    return EGL_TEX_STATUS_ERROR;

  framebuffer_read(
    frame,
    texture->tex[t].map,
    texture->stride,
    texture->height,
    texture->width,
    texture->bpp,
    texture->stride
  );

  atomic_fetch_add_explicit(&texture->state.w, 1, memory_order_release);
  egl_texture_unmap(texture, t);

  return true;
}

bool egl_texture_update_from_dma(EGL_Texture * texture, const FrameBuffer * frame, const int dmaFd)
{
  if (!texture->streaming)
    return false;

  const uint8_t sw =
    atomic_load_explicit(&texture->state.w, memory_order_acquire);

  if (atomic_load_explicit(&texture->state.u, memory_order_acquire) == (uint8_t)(sw + 1))
  {
    egl_warn_slow();
    return true;
  }

  const uint8_t t = sw % TEXTURE_COUNT;
  EGLAttrib const attribs[] =
  {
    EGL_WIDTH                    , texture->width,
    EGL_HEIGHT                   , texture->height,
    EGL_LINUX_DRM_FOURCC_EXT     , texture->fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT    , dmaFd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
    EGL_DMA_BUF_PLANE0_PITCH_EXT , texture->stride,
    EGL_NONE                     , EGL_NONE
  };

  /* create the image backed by the dma buffer */
  EGLImage image = eglCreateImage(
    texture->display,
    EGL_NO_CONTEXT,
    EGL_LINUX_DMA_BUF_EXT,
    (EGLClientBuffer)NULL,
    attribs
  );

  /* bind the texture and initiate the transfer */
  glBindTexture(GL_TEXTURE_2D, texture->tex[t].t);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

  /* wait for completion */
  framebuffer_wait(frame, texture->height * texture->stride);

  /* destroy the image to prevent future writes corrupting the display image */
  eglDestroyImage(texture->display, image);

  atomic_fetch_add_explicit(&texture->state.w, 1, memory_order_release);
  return true;
}

enum EGL_TexStatus egl_texture_process(EGL_Texture * texture)
{
  if (!texture->streaming)
    return EGL_TEX_STATUS_OK;

  const uint8_t su =
    atomic_load_explicit(&texture->state.u, memory_order_acquire);

  const uint8_t nextu = su + 1;
  if (
      su    == atomic_load_explicit(&texture->state.w, memory_order_acquire) ||
      nextu == atomic_load_explicit(&texture->state.s, memory_order_acquire) ||
      nextu == atomic_load_explicit(&texture->state.d, memory_order_acquire))
    return texture->ready ? EGL_TEX_STATUS_OK : EGL_TEX_STATUS_NOTREADY;

  const uint8_t t = su % TEXTURE_COUNT;

  /* update the texture */
  if (!texture->dma)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[t].pbo);
    glBindTexture(GL_TEXTURE_2D, texture->tex[t].t);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->pitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height,
        texture->format, texture->dataType, (const void *)0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    /* create a fence to prevent usage before the update is complete */
    texture->tex[t].sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    /* we must flush to ensure the sync is in the command buffer */
    glFlush();
  }

  texture->ready = true;
  atomic_fetch_add_explicit(&texture->state.u, 1, memory_order_release);

  return EGL_TEX_STATUS_OK;
}

enum EGL_TexStatus egl_texture_bind(EGL_Texture * texture)
{
  uint8_t ss = atomic_load_explicit(&texture->state.s, memory_order_acquire);
  uint8_t sd = atomic_load_explicit(&texture->state.d, memory_order_acquire);

  if (texture->streaming)
  {
    if (!texture->ready)
      return EGL_TEX_STATUS_NOTREADY;

    const uint8_t t = ss % TEXTURE_COUNT;
    if (texture->dma)
    {
      ss = atomic_fetch_add_explicit(&texture->state.s, 1,
        memory_order_release) + 1;
    }
    else if (texture->tex[t].sync != 0)
    {
      switch(glClientWaitSync(texture->tex[t].sync, 0, 20000000)) // 20ms
      {
        case GL_ALREADY_SIGNALED:
        case GL_CONDITION_SATISFIED:
          glDeleteSync(texture->tex[t].sync);
          texture->tex[t].sync = 0;

          ss = atomic_fetch_add_explicit(&texture->state.s, 1,
              memory_order_release) + 1;
          break;

        case GL_TIMEOUT_EXPIRED:
          break;

        case GL_WAIT_FAILED:
        case GL_INVALID_VALUE:
          glDeleteSync(texture->tex[t].sync);
          texture->tex[t].sync = 0;
          EGL_ERROR("glClientWaitSync failed");
          return EGL_TEX_STATUS_ERROR;
      }
    }

    if (ss != sd && ss != (uint8_t)(sd + 1))
      sd = atomic_fetch_add_explicit(&texture->state.d, 1,
          memory_order_release) + 1;
  }

  const uint8_t t = sd % TEXTURE_COUNT;
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture->tex[t].t);
  glBindSampler(0, texture->sampler);

  return EGL_TEX_STATUS_OK;
}

int egl_texture_count(EGL_Texture * texture)
{
  return 1;
}
