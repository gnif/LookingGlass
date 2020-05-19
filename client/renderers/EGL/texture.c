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
#include "common/locking.h"
#include "common/framebuffer.h"
#include "debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include <SDL2/SDL_egl.h>

#define TEXTURE_COUNT 3

struct Tex
{
  GLuint t[3];
  bool   hasPBO;
  GLuint pbo;
  void * map;
  GLsync sync;
};

union TexState
{
  uint32_t v;
  struct
  {
    /*
     * w = write
     * u = upload
     * s = schedule
     * d = display
     */
    int8_t w, u, s, d;
  };
};

struct EGL_Texture
{
  enum   EGL_PixelFormat pixFmt;
  size_t width, height, stride;
  size_t bpp;
  bool   streaming;
  bool   ready;

  int      planeCount;
  GLuint   samplers[3];
  size_t   planes  [3][3];
  GLintptr offsets [3];
  GLenum   intFormat;
  GLenum   format;
  GLenum   dataType;
  size_t   pboBufferSize;

  LG_Lock        lock;
  union TexState state;
  struct Tex     tex[TEXTURE_COUNT];
};

bool egl_texture_init(EGL_Texture ** texture)
{
  *texture = (EGL_Texture *)malloc(sizeof(EGL_Texture));
  if (!*texture)
  {
    DEBUG_ERROR("Failed to malloc EGL_Texture");
    return false;
  }

  memset(*texture, 0, sizeof(EGL_Texture));
  LG_LOCK_INIT((*texture)->lock);
  return true;
}

void egl_texture_free(EGL_Texture ** texture)
{
  if (!*texture)
    return;

  if ((*texture)->planeCount > 0)
    glDeleteSamplers((*texture)->planeCount, (*texture)->samplers);

  for(int i = 0; i < ((*texture)->streaming ? TEXTURE_COUNT : 1); ++i)
  {
    struct Tex * t = &(*texture)->tex[i];
    if (t->hasPBO)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, t->pbo);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      glDeleteBuffers(1, &t->pbo);
      if (t->sync)
        glDeleteSync(t->sync);
    }

   if ((*texture)->planeCount > 0)
     glDeleteTextures((*texture)->planeCount, t->t);
  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  LG_LOCK_FREE((*texture)->lock);
  free(*texture);
  *texture = NULL;
}

bool egl_texture_setup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt, size_t width, size_t height, size_t stride, bool streaming)
{
  int planeCount;

  texture->pixFmt    = pixFmt;
  texture->width     = width;
  texture->height    = height;
  texture->stride    = stride;
  texture->streaming = streaming;
  texture->ready     = false;
  texture->state.v   = 0;

  switch(pixFmt)
  {
    case EGL_PF_BGRA:
      planeCount             = 1;
      texture->bpp           = 4;
      texture->format        = GL_BGRA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA:
      planeCount             = 1;
      texture->bpp           = 4;
      texture->format        = GL_RGBA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA10:
      planeCount             = 1;
      texture->bpp           = 4;
      texture->format        = GL_RGBA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_RGB10_A2;
      texture->dataType      = GL_UNSIGNED_INT_2_10_10_10_REV;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_YUV420:
      planeCount             = 3;
      texture->bpp           = 4;
      texture->format        = GL_RED;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride;
      texture->planes[1][0]  = width  / 2;
      texture->planes[1][1]  = height / 2;
      texture->planes[1][2]  = stride / 2;
      texture->planes[2][0]  = width  / 2;
      texture->planes[2][1]  = height / 2;
      texture->planes[2][2]  = stride / 2;
      texture->offsets[0]    = 0;
      texture->offsets[1]    = stride * height;
      texture->offsets[2]    = texture->offsets[1] + (texture->offsets[1] / 4);
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = texture->offsets[2] + (texture->offsets[1] / 4);
      break;

    default:
      DEBUG_ERROR("Unsupported pixel format");
      return false;
  }

  if (planeCount > texture->planeCount)
  {
    if (texture->planeCount > 0)
      glDeleteSamplers(texture->planeCount, texture->samplers);

    for(int i = 0; i < TEXTURE_COUNT; ++i)
    {
      if (texture->planeCount > 0)
        glDeleteTextures(texture->planeCount, texture->tex[i].t);
      glGenTextures(planeCount, texture->tex[i].t);
    }

    glGenSamplers(planeCount, texture->samplers);
    for(int p = 0; p < planeCount; ++p)
    {
      glSamplerParameteri(texture->samplers[p], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glSamplerParameteri(texture->samplers[p], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glSamplerParameteri(texture->samplers[p], GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
      glSamplerParameteri(texture->samplers[p], GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
    }

    texture->planeCount = planeCount;
  }

  for(int i = 0; i < (streaming ? TEXTURE_COUNT : 1); ++i)
  {
    for(int p = 0; p < planeCount; ++p)
    {
      glBindTexture(GL_TEXTURE_2D, texture->tex[i].t[p]);
      glTexImage2D(GL_TEXTURE_2D, 0, texture->intFormat, texture->planes[p][0],
        texture->planes[p][1], 0, texture->format, texture->dataType, NULL);
    }
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (streaming)
  {
    // release old PBOs and delete and re-create the buffers
    for(int i = 0; i < TEXTURE_COUNT; ++i)
    {
      if (texture->tex[i].hasPBO)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[i].pbo);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
        glDeleteBuffers(1, &texture->tex[i].pbo);
      }

      glGenBuffers(1, &texture->tex[i].pbo);
      texture->tex[i].hasPBO = true;

      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[i].pbo);
      glBufferStorage(
        GL_PIXEL_UNPACK_BUFFER,
        texture->pboBufferSize,
        NULL,
        GL_MAP_PERSISTENT_BIT |
        GL_MAP_WRITE_BIT
      );

      texture->tex[i].map = glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        texture->pboBufferSize,
        GL_MAP_PERSISTENT_BIT        |
        GL_MAP_WRITE_BIT             |
        GL_MAP_UNSYNCHRONIZED_BIT    |
        GL_MAP_INVALIDATE_BUFFER_BIT
      );

      if (!texture->tex[i].map)
      {
        EGL_ERROR("glMapBufferRange failed for %d of %lu bytes", i, texture->pboBufferSize);
        return false;
      }
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
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
    DEBUG_WARN("The guest is providing updates faster then we are consuming them");
    DEBUG_WARN("Please increase the client FPS to avoid frame skipping");
    DEBUG_BREAK();
  }
}

bool egl_texture_update(EGL_Texture * texture, const uint8_t * buffer)
{
  if (texture->streaming)
  {
    /* NOTE: DO NOT use any gl commands here as streaming must be thread safe */

    union TexState s;
    LG_LOCK(texture->lock);
    s.v = texture->state.v;
    LG_UNLOCK(texture->lock);

    const uint8_t next = (s.w + 1) % TEXTURE_COUNT;
    if (next == s.u)
    {
      egl_warn_slow();
      return true;
    }

    memcpy(texture->tex[s.w].map, buffer, texture->pboBufferSize);

    LG_LOCK(texture->lock);
    texture->state.w = next;
    LG_UNLOCK(texture->lock);
  }
  else
  {
    /* Non streaming, this is NOT thread safe */

    for(int p = 0; p < texture->planeCount; ++p)
    {
      glBindTexture(GL_TEXTURE_2D, texture->tex[0].t[p]);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->planes[p][0]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->planes[p][0], texture->planes[p][1],
          texture->format, texture->dataType, buffer + texture->offsets[p]);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  return true;
}

bool egl_texture_update_from_frame(EGL_Texture * texture, const FrameBuffer * frame)
{
  if (!texture->streaming)
    return false;

  union TexState s;
  LG_LOCK(texture->lock);
  s.v = texture->state.v;
  LG_UNLOCK(texture->lock);

  const uint8_t next = (s.w + 1) % TEXTURE_COUNT;
  if (next == s.u)
  {
    egl_warn_slow();
    return true;
  }

  framebuffer_read(
    frame,
    texture->tex[s.w].map,
    texture->stride,
    texture->height,
    texture->width,
    texture->bpp,
    texture->stride
  );

  LG_LOCK(texture->lock);
  texture->state.w = next;
  LG_UNLOCK(texture->lock);

  return true;
}

enum EGL_TexStatus egl_texture_process(EGL_Texture * texture)
{
  if (!texture->streaming)
    return EGL_TEX_STATUS_OK;

  union TexState s;
  LG_LOCK(texture->lock);
  s.v = texture->state.v;
  LG_UNLOCK(texture->lock);

  const uint8_t nextu = (s.u + 1) % TEXTURE_COUNT;
  if (s.u == s.w || nextu == s.s || nextu == s.d)
    return texture->ready ? EGL_TEX_STATUS_OK : EGL_TEX_STATUS_NOTREADY;

  /* update the texture */
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->tex[s.u].pbo);
  for(int p = 0; p < texture->planeCount; ++p)
  {
    glBindTexture(GL_TEXTURE_2D, texture->tex[s.u].t[p]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->planes[p][2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->planes[p][0], texture->planes[p][1],
        texture->format, texture->dataType, (const void *)texture->offsets[p]);

  }
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  /* create a fence to prevent usage before the update is complete */
  texture->tex[s.u].sync =
    glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

  LG_LOCK(texture->lock);
  texture->state.u = nextu;
  LG_UNLOCK(texture->lock);

  texture->ready = true;

  return EGL_TEX_STATUS_OK;
}

enum EGL_TexStatus egl_texture_bind(EGL_Texture * texture)
{
  union TexState s;
  LG_LOCK(texture->lock);
  s.v = texture->state.v;
  LG_UNLOCK(texture->lock);

  if (texture->streaming)
  {
    if (!texture->ready)
      return EGL_TEX_STATUS_NOTREADY;

    if (texture->tex[s.s].sync != 0)
    {
      switch(glClientWaitSync(texture->tex[s.s].sync, 0, 0))
      {
        case GL_ALREADY_SIGNALED:
        case GL_CONDITION_SATISFIED:
          glDeleteSync(texture->tex[s.s].sync);
          texture->tex[s.s].sync = 0;

          LG_LOCK(texture->lock);
          texture->state.s = (s.s + 1) % TEXTURE_COUNT;
          LG_UNLOCK(texture->lock);
          break;

        case GL_TIMEOUT_EXPIRED:
          break;

        case GL_WAIT_FAILED:
          glDeleteSync(texture->tex[s.s].sync);
          texture->tex[s.s].sync = 0;
          EGL_ERROR("glClientWaitSync failed");
          return EGL_TEX_STATUS_ERROR;
      }
    }

    LG_LOCK(texture->lock);
    const int8_t nextd = (texture->state.d + 1) % TEXTURE_COUNT;
    if (texture->state.d != texture->state.s && nextd != texture->state.s)
      texture->state.d = nextd;
    LG_UNLOCK(texture->lock);
  }

  for(int i = 0; i < texture->planeCount; ++i)
  {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture->tex[s.d].t[i]);
    glBindSampler(i, texture->samplers[i]);
  }

  return EGL_TEX_STATUS_OK;
}

int egl_texture_count(EGL_Texture * texture)
{
  return texture->planeCount;
}
