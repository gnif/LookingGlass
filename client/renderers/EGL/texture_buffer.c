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

#include "texture_buffer.h"

#include "egldebug.h"

#include <string.h>

// forwards
extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;

// internal functions

static void egl_texBuffer_cleanup(TextureBuffer * this)
{
  egl_texUtilFreeBuffers(this->buf, this->texCount);

  if (this->tex[0])
    glDeleteTextures(this->texCount, this->tex);

  if (this->sync)
  {
    glDeleteSync(this->sync);
    this->sync = 0;
  }
}

// common functions

bool egl_texBufferInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TextureBuffer * this;
  if (!*texture)
  {
    this = calloc(1, sizeof(*this));
    if (!this)
    {
      DEBUG_ERROR("Failed to malloc TexB");
      return false;
    }
    *texture = &this->base;
    this->free = true;
  }
  else
    this = UPCAST(TextureBuffer, *texture);

  this->texCount = 1;
  return true;
}

void egl_texBufferFree(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  egl_texBuffer_cleanup(this);
  LG_LOCK_FREE(this->copyLock);

  if (this->free)
    free(this);
}

bool egl_texBufferSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  egl_texBuffer_cleanup(this);

  glGenTextures(this->texCount, this->tex);
  for(int i = 0; i < this->texCount; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, this->tex[i]);
    glTexImage2D(GL_TEXTURE_2D,
        0,
        texture->format.intFormat,
        texture->format.width,
        texture->format.height,
        0,
        texture->format.format,
        texture->format.dataType,
        NULL);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  this->rIndex = -1;

  return true;
}

static bool egl_texBufferUpdate(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  DEBUG_ASSERT(update->type == EGL_TEXTYPE_BUFFER);

  glBindTexture(GL_TEXTURE_2D, this->tex[0]);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, update->pitch);
  glTexSubImage2D(GL_TEXTURE_2D,
      0,
      update->x,
      update->y,
      update->width,
      update->height,
      texture->format.format,
      texture->format.dataType,
      update->buffer);
  glBindTexture(GL_TEXTURE_2D, 0);

  return true;
}

EGL_TexStatus egl_texBufferProcess(EGL_Texture * texture)
{
  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus egl_texBufferGet(EGL_Texture * texture, GLuint * tex)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  *tex = this->tex[0];
  return EGL_TEX_STATUS_OK;
}

// streaming functions

bool egl_texBufferStreamInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  if (!egl_texBufferInit(texture, type, display))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, *texture);

  switch(type)
  {
    case EGL_TEXTYPE_BUFFER_STREAM:
    case EGL_TEXTYPE_FRAMEBUFFER:
    case EGL_TEXTYPE_DMABUF:
      this->texCount = 2;
      break;

    case EGL_TEXTYPE_BUFFER_MAP:
      this->texCount = 1;
      break;

    default:
      DEBUG_UNREACHABLE();
  }

  LG_LOCK_INIT(this->copyLock);
  return true;
}

bool egl_texBufferStreamSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  if (!egl_texBufferSetup(texture, setup))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  return egl_texUtilGenBuffers(&texture->format, this->buf, this->texCount);
}

static bool egl_texBufferStreamUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  DEBUG_ASSERT(update->type == EGL_TEXTYPE_BUFFER);

  LG_LOCK(this->copyLock);

  uint8_t * dst = this->buf[this->bufIndex].map +
    texture->format.stride * update->y +
    update->x * texture->format.bpp;

  if (update->topDown)
  {
    const uint8_t * src = update->buffer;
    for(int y = 0; y < update->height; ++y)
    {
      memcpy(dst, src, update->stride);
      dst += texture->format.stride;
      src += update->stride;
    }
  }
  else
  {
    const uint8_t * src = update->buffer + update->stride * update->height;
    for(int y = 0; y < update->height; ++y)
    {
      src -= update->stride;
      memcpy(dst, src, update->stride);
      dst += texture->format.stride;
    }
  }

  this->buf[this->bufIndex].updated = true;
  LG_UNLOCK(this->copyLock);

  return true;
}

EGL_TexStatus egl_texBufferStreamProcess(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  LG_LOCK(this->copyLock);

  GLuint          tex    = this->tex[this->bufIndex];
  EGL_TexBuffer * buffer = &this->buf[this->bufIndex];

  if (buffer->updated && this->sync == 0)
  {
    this->rIndex = this->bufIndex;
    if (++this->bufIndex == this->texCount)
      this->bufIndex = 0;
  }

  LG_UNLOCK(this->copyLock);

  if (buffer->updated)
  {
    buffer->updated = false;

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->format.pitch);
    glTexSubImage2D(GL_TEXTURE_2D,
        0, 0, 0,
        texture->format.width,
        texture->format.height,
        texture->format.format,
        texture->format.dataType,
        (const void *)0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    this->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
  }

  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus egl_texBufferStreamGet(EGL_Texture * texture, GLuint * tex)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  if (this->rIndex == -1)
    return EGL_TEX_STATUS_NOTREADY;

  if (this->sync)
  {
    switch(glClientWaitSync(this->sync, 0, 40000000)) // 40ms
    {
      case GL_ALREADY_SIGNALED:
      case GL_CONDITION_SATISFIED:
        glDeleteSync(this->sync);
        this->sync = 0;
        break;

      case GL_TIMEOUT_EXPIRED:
        return EGL_TEX_STATUS_NOTREADY;

      case GL_WAIT_FAILED:
      case GL_INVALID_VALUE:
        glDeleteSync(this->sync);
        this->sync = 0;
        DEBUG_GL_ERROR("glClientWaitSync failed");
        return EGL_TEX_STATUS_ERROR;
    }
  }

  *tex = this->tex[this->rIndex];
  return EGL_TEX_STATUS_OK;
}

const EGL_TextureOps EGL_TextureBuffer =
{
  .init        = egl_texBufferInit,
  .free        = egl_texBufferFree,
  .setup       = egl_texBufferSetup,
  .update      = egl_texBufferUpdate,
  .process     = egl_texBufferProcess,
  .get         = egl_texBufferGet
};

const EGL_TextureOps EGL_TextureBufferStream =
{
  .init        = egl_texBufferStreamInit,
  .free        = egl_texBufferFree,
  .setup       = egl_texBufferStreamSetup,
  .update      = egl_texBufferStreamUpdate,
  .process     = egl_texBufferStreamProcess,
  .get         = egl_texBufferStreamGet
};
