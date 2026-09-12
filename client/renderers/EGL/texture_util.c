/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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
#include "state.h"
#include "texture_util.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "egldebug.h"
#include "egl_dynprocs.h"

#include <inttypes.h>
#include <stdlib.h>

static EGL_TexBufferMode bufferMode = EGL_TEX_BUFFER_MODE_DIRECT;

bool egl_texUtilGetFormat(const EGL_TexSetup * setup, EGL_TexFormat * fmt)
{
  fmt->pixFmt = setup->pixFmt;
  fmt->width  = setup->width;
  fmt->height = setup->height;
  fmt->stride = setup->stride;
  fmt->pitch  = setup->pitch;

  switch (setup->pixFmt)
  {
    case EGL_PF_BGRA:
      fmt->bpp        = 4;
      fmt->format     = GL_BGRA_EXT;
      fmt->intFormat  = GL_BGRA_EXT;
      fmt->dataType   = GL_UNSIGNED_BYTE;
      fmt->fourcc     = DRM_FORMAT_XRGB8888;
      break;

    case EGL_PF_RGBA:
      fmt->bpp        = 4;
      fmt->format     = GL_RGBA;
      fmt->intFormat  = GL_RGBA;
      fmt->dataType   = GL_UNSIGNED_BYTE;
      fmt->fourcc     = DRM_FORMAT_XBGR8888;
      break;

    case EGL_PF_RGBA10:
      fmt->bpp        = 4;
      fmt->format     = GL_RGBA;
      fmt->intFormat  = GL_RGB10_A2;
      fmt->dataType   = GL_UNSIGNED_INT_2_10_10_10_REV;
      fmt->fourcc     = DRM_FORMAT_XBGR2101010;
      break;

    case EGL_PF_RGBA16F:
      fmt->bpp        = 8;
      fmt->format     = GL_RGBA;
      fmt->intFormat  = GL_RGBA16F;
      fmt->dataType   = GL_HALF_FLOAT;
      fmt->fourcc     = DRM_FORMAT_XBGR16161616F;
      break;

    //EGL has no support for 24-bit formats, so we stuff it into a 32-bit
    //texture to unpack with a shader later
    case EGL_PF_BGR_32:
      fmt->bpp        = 4;
      fmt->format     = GL_BGRA_EXT;
      fmt->intFormat  = GL_BGRA_EXT;
      fmt->dataType   = GL_UNSIGNED_BYTE;
      fmt->fourcc     = DRM_FORMAT_ARGB8888;
      break;

    case EGL_PF_RGB_24:
      fmt->bpp        = 3;
      fmt->format     = GL_RGB;
      fmt->intFormat  = GL_BGRA_EXT;
      fmt->dataType   = GL_UNSIGNED_BYTE;
      fmt->fourcc     = DRM_FORMAT_RGB888;
      break;

    default:
      DEBUG_ERROR("Unsupported pixel format");
      return false;
  }

  if (!fmt->stride)
    fmt->stride = setup->width;

  if (!fmt->pitch)
    fmt->pitch = fmt->stride * fmt->bpp;

  fmt->dataSize = fmt->height * fmt->pitch;
  return true;
}

void egl_texUtilSetBufferMode(EGL_TexBufferMode mode)
{
  bufferMode = mode;
}

static bool egl_texUtilGenBuffersMode(const EGL_TexFormat * fmt,
    EGL_TexBuffer * buffers, int count, EGL_TexBufferMode mode)
{
  for (int i = 0; i < count; ++i)
  {
    EGL_TexBuffer * buffer = &buffers[i];

    buffer->size = fmt->dataSize;
    buffer->mode = mode;
    if (mode == EGL_TEX_BUFFER_MODE_DIRECT)
    {
      buffer->map = malloc(buffer->size);
      if (!buffer->map)
      {
        DEBUG_ERROR("Failed to allocate %" PRIu64
            "-byte texture upload buffer", (uint64_t)buffer->size);
        return false;
      }

      continue;
    }

    glGenBuffers(1, &buffer->pbo);
    egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
    g_egl_dynProcs.glBufferStorageEXT(
      GL_PIXEL_UNPACK_BUFFER,
      fmt->dataSize,
      NULL,
      GL_MAP_WRITE_BIT          |
      GL_MAP_PERSISTENT_BIT_EXT |
      GL_MAP_COHERENT_BIT_EXT
    );

    if (!egl_texUtilMapBuffer(buffer))
      return false;
  }

  return true;
}

bool egl_texUtilGenBuffers(const EGL_TexFormat * fmt, EGL_TexBuffer * buffers,
    int count)
{
  EGL_TexBufferMode mode = bufferMode;
  if (mode == EGL_TEX_BUFFER_MODE_PERSISTENT &&
      !g_egl_dynProcs.glBufferStorageEXT)
  {
    DEBUG_WARN("Persistent texture uploads unavailable, using direct uploads");
    mode = EGL_TEX_BUFFER_MODE_DIRECT;
  }

  if (egl_texUtilGenBuffersMode(fmt, buffers, count, mode))
    return true;

  egl_texUtilFreeBuffers(buffers, count);
  if (mode != EGL_TEX_BUFFER_MODE_PERSISTENT)
    return false;

  DEBUG_WARN("Persistent texture upload buffers failed, using direct uploads");
  mode = EGL_TEX_BUFFER_MODE_DIRECT;
  if (egl_texUtilGenBuffersMode(fmt, buffers, count, mode))
    return true;

  egl_texUtilFreeBuffers(buffers, count);
  return false;
}

void egl_texUtilFreeBuffers(EGL_TexBuffer * buffers, int count)
{
  bool deleted = false;
  for (int i = 0; i < count; ++i)
  {
    EGL_TexBuffer * buffer = &buffers[i];

    if (buffer->mode == EGL_TEX_BUFFER_MODE_DIRECT)
    {
      free(buffer->map);
      buffer->map  = NULL;
      buffer->size = 0;
      continue;
    }

    if (!buffer->pbo)
      continue;

    egl_texUtilUnmapBuffer(buffer);
    glDeleteBuffers(1, &buffer->pbo);
    deleted = true;
    buffer->pbo  = 0;
    buffer->size = 0;
  }

  if (deleted)
    egl_stateInvalidateShared();
}

bool egl_texUtilMapBuffer(EGL_TexBuffer * buffer)
{
  DEBUG_ASSERT(buffer->mode == EGL_TEX_BUFFER_MODE_PERSISTENT);
  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
  buffer->map = glMapBufferRange(
      GL_PIXEL_UNPACK_BUFFER,
      0,
      buffer->size,
      GL_MAP_WRITE_BIT             |
      GL_MAP_UNSYNCHRONIZED_BIT    |
      GL_MAP_INVALIDATE_BUFFER_BIT |
      GL_MAP_PERSISTENT_BIT_EXT    |
      GL_MAP_COHERENT_BIT_EXT);

  if (!buffer->map)
    DEBUG_GL_ERROR("glMapBufferRange failed of %" PRIu64 " bytes",
        (uint64_t)buffer->size);

  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  return buffer->map;
}

void egl_texUtilUnmapBuffer(EGL_TexBuffer * buffer)
{
  if (!buffer->map)
    return;

  DEBUG_ASSERT(buffer->mode == EGL_TEX_BUFFER_MODE_PERSISTENT);
  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  buffer->map = NULL;
}
