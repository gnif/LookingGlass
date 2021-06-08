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
#include "common/debug.h"
#include "common/framebuffer.h"
#include "egl_dynprocs.h"
#include "egldebug.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

/**
 * the following comes from drm_fourcc.h and is included here to avoid the
 * external dependency for the few simple defines we need
 */
#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
         ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ARGB8888      fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888      fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_BGRA1010102   fourcc_code('B', 'A', '3', '0')
#define DRM_FORMAT_ABGR16161616F fourcc_code('A', 'B', '4', 'H')

/* this must be a multiple of 2 */
#define BUFFER_COUNT 4

struct Buffer
{
  bool     hasPBO;
  GLuint   pbo;
  void *   map;
  GLsync   sync;
};

struct BufferState
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

  struct BufferState state;
  int             bufferCount;
  GLuint          tex[BUFFER_COUNT];
  struct Buffer   buf[BUFFER_COUNT];

  size_t dmaImageCount;
  size_t dmaImageUsed;
  struct
  {
    int fd;
    EGLImage image;
  }
  * dmaImages;

  GLuint dmaFBO;
  GLuint dmaTex;
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

  for(int i = 0; i < (*texture)->bufferCount; ++i)
  {
    struct Buffer * b = &(*texture)->buf[i];
    if (b->hasPBO)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, b->pbo);
      if ((*texture)->buf[i].map)
      {
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        (*texture)->buf[i].map = NULL;
      }
      glDeleteBuffers(1, &b->pbo);
      if (b->sync)
        glDeleteSync(b->sync);
    }
  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glDeleteTextures((*texture)->bufferCount, (*texture)->tex);

  for (size_t i = 0; i < (*texture)->dmaImageUsed; ++i)
    eglDestroyImage((*texture)->display, (*texture)->dmaImages[i].image);
  free((*texture)->dmaImages);

  free(*texture);
  *texture = NULL;
}

static bool egl_texture_map(EGL_Texture * texture, uint8_t i)
{
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->buf[i].pbo);
  texture->buf[i].map = glMapBufferRange(
    GL_PIXEL_UNPACK_BUFFER,
    0,
    texture->pboBufferSize,
    GL_MAP_WRITE_BIT             |
    GL_MAP_UNSYNCHRONIZED_BIT    |
    GL_MAP_INVALIDATE_BUFFER_BIT |
    GL_MAP_PERSISTENT_BIT        |
    GL_MAP_COHERENT_BIT
  );

  if (!texture->buf[i].map)
  {
    DEBUG_EGL_ERROR("glMapBufferRange failed for %d of %lu bytes", i,
        texture->pboBufferSize);
  }

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  return texture->buf[i].map;
}

static void egl_texture_unmap(EGL_Texture * texture, uint8_t i)
{
  if (!texture->buf[i].map)
    return;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->buf[i].pbo);
  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  texture->buf[i].map = NULL;
}

bool egl_texture_setup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt, size_t width, size_t height, size_t stride, bool streaming, bool useDMA)
{
  if (texture->streaming && !useDMA)
  {
    for(int i = 0; i < texture->bufferCount; ++i)
    {
      egl_texture_unmap(texture, i);
      if (texture->buf[i].hasPBO)
      {
        glDeleteBuffers(1, &texture->buf[i].pbo);
        texture->buf[i].hasPBO = false;
      }
    }
  }

  texture->pixFmt      = pixFmt;
  texture->width       = width;
  texture->height      = height;
  texture->stride      = stride;
  texture->streaming   = streaming;
  texture->bufferCount = streaming ? BUFFER_COUNT : 1;
  texture->dma         = useDMA;
  texture->ready       = false;

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

  if (texture->tex[0])
    glDeleteTextures(texture->bufferCount, texture->tex);
  glGenTextures(texture->bufferCount, texture->tex);

  if (!texture->sampler)
  {
    glGenSamplers(1, &texture->sampler);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glSamplerParameteri(texture->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
  }

  if (useDMA)
  {
    if (texture->dmaFBO)
      glDeleteFramebuffers(1, &texture->dmaFBO);
    if (texture->dmaTex)
      glDeleteTextures(1, &texture->dmaTex);
    glGenFramebuffers(1, &texture->dmaFBO);
    glGenTextures(1, &texture->dmaTex);

    for (size_t i = 0; i < texture->dmaImageUsed; ++i)
      eglDestroyImage(texture->display, texture->dmaImages[i].image);
    texture->dmaImageUsed = 0;
    return true;
  }

  for(int i = 0; i < texture->bufferCount; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, texture->tex[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, texture->intFormat, texture->width,
      texture->height, 0, texture->format, texture->dataType, NULL);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!streaming)
    return true;

  for(int i = 0; i < texture->bufferCount; ++i)
  {
    glGenBuffers(1, &texture->buf[i].pbo);
    texture->buf[i].hasPBO = true;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->buf[i].pbo);
    glBufferStorage(
      GL_PIXEL_UNPACK_BUFFER,
      texture->pboBufferSize,
      NULL,
      GL_MAP_WRITE_BIT      |
      GL_MAP_PERSISTENT_BIT |
      GL_MAP_COHERENT_BIT
    );

    if (!egl_texture_map(texture, i))
      return false;
  }

  return true;
}

static void egl_warn_slow(void)
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

    const uint8_t b = sw % BUFFER_COUNT;
    memcpy(texture->buf[b].map, buffer, texture->pboBufferSize);
    atomic_fetch_add_explicit(&texture->state.w, 1, memory_order_release);
  }
  else
  {
    glBindTexture(GL_TEXTURE_2D, texture->tex[0]);
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

  const uint8_t b = sw % BUFFER_COUNT;

  framebuffer_read(
    frame,
    texture->buf[b].map,
    texture->stride,
    texture->height,
    texture->width,
    texture->bpp,
    texture->stride
  );

  atomic_fetch_add_explicit(&texture->state.w, 1, memory_order_release);

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

  EGLImage image = EGL_NO_IMAGE;

  for (int i = 0; i < texture->dmaImageUsed; ++i)
  {
    if (texture->dmaImages[i].fd == dmaFd)
    {
      image = texture->dmaImages[i].image;
      break;
    }
  }

  if (image == EGL_NO_IMAGE)
  {
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
    image = eglCreateImage(
      texture->display,
      EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer)NULL,
      attribs
    );

    if (image == EGL_NO_IMAGE)
    {
      DEBUG_EGL_ERROR("Failed to create ELGImage for DMA transfer");
      return false;
    }

    if (texture->dmaImageUsed == texture->dmaImageCount)
    {
      size_t newCount = texture->dmaImageCount * 2 + 2;
      void * new = realloc(texture->dmaImages, newCount * sizeof *texture->dmaImages);
      if (!new)
      {
        DEBUG_EGL_ERROR("Failed to allocate memory");
        eglDestroyImage(texture->display, image);
        return false;
      }
      texture->dmaImageCount = newCount;
      texture->dmaImages     = new;
    }

    const size_t index = texture->dmaImageUsed++;
    texture->dmaImages[index].fd    = dmaFd;
    texture->dmaImages[index].image = image;
  }

  /* wait for completion */
  framebuffer_wait(frame, texture->height * texture->stride);

  glBindTexture(GL_TEXTURE_2D, texture->dmaTex);
  g_egl_dynProcs.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

  glBindFramebuffer(GL_FRAMEBUFFER, texture->dmaFBO);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture->dmaTex, 0);

  glBindTexture(GL_TEXTURE_2D, texture->tex[0]);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, texture->intFormat, 0, 0, texture->width, texture->height, 0);

  GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();

  switch (glClientWaitSync(fence, 0, 10000000)) // 10ms
  {
    case GL_ALREADY_SIGNALED:
    case GL_CONDITION_SATISFIED:
      break;

    case GL_TIMEOUT_EXPIRED:
      egl_warn_slow();
      break;

    case GL_WAIT_FAILED:
    case GL_INVALID_VALUE:
      DEBUG_EGL_ERROR("glClientWaitSync failed");
  }

  glDeleteSync(fence);
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

  const uint8_t b = su % BUFFER_COUNT;

  /* update the texture */
  if (!texture->dma)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->buf[b].pbo);
    glBindTexture(GL_TEXTURE_2D, texture->tex[b]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->pitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->width, texture->height,
        texture->format, texture->dataType, (const void *)0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    /* create a fence to prevent usage before the update is complete */
    texture->buf[b].sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

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
  GLuint tex = texture->tex[0];

  if (texture->streaming)
  {
    if (!texture->ready)
      return EGL_TEX_STATUS_NOTREADY;

    const uint8_t b = ss % BUFFER_COUNT;
    if (texture->dma)
    {
      ss = atomic_fetch_add_explicit(&texture->state.s, 1,
        memory_order_release) + 1;
    }
    else if (texture->buf[b].sync != 0)
    {
      switch(glClientWaitSync(texture->buf[b].sync, 0, 20000000)) // 20ms
      {
        case GL_ALREADY_SIGNALED:
        case GL_CONDITION_SATISFIED:
          glDeleteSync(texture->buf[b].sync);
          texture->buf[b].sync = 0;

          ss = atomic_fetch_add_explicit(&texture->state.s, 1,
              memory_order_release) + 1;
          break;

        case GL_TIMEOUT_EXPIRED:
          break;

        case GL_WAIT_FAILED:
        case GL_INVALID_VALUE:
          glDeleteSync(texture->buf[b].sync);
          texture->buf[b].sync = 0;
          DEBUG_EGL_ERROR("glClientWaitSync failed");
          return EGL_TEX_STATUS_ERROR;
      }
    }

    if (ss != sd && ss != (uint8_t)(sd + 1))
      sd = atomic_fetch_add_explicit(&texture->state.d, 1,
          memory_order_release) + 1;

    if (!texture->dma)
      tex = texture->tex[sd % BUFFER_COUNT];
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glBindSampler(0, texture->sampler);

  return EGL_TEX_STATUS_OK;
}

int egl_texture_count(EGL_Texture * texture)
{
  return 1;
}
