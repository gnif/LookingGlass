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

#include "texture_buffer.h"

#include "egldebug.h"

#include <string.h>
#include <assert.h>

// forwards
extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;

// internal functions

static void eglTexBuffer_cleanup(TextureBuffer * this)
{
  eglTexUtilFreeBuffers(this->buf, this->texCount);

  if (this->tex[0])
    glDeleteTextures(this->texCount, this->tex);

  if (this->sampler)
    glDeleteSamplers(1, &this->sampler);

  if (this->sync)
  {
    glDeleteSync(this->sync);
    this->sync = 0;
  }
}

// common functions

bool eglTexBuffer_init(EGL_Texture ** texture, EGLDisplay * display)
{
  TextureBuffer * this;
  if (!*texture)
  {
    this = (TextureBuffer *)calloc(1, sizeof(*this));
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
  atomic_init(&this->sync, 0);
  return true;
}

void eglTexBuffer_free(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  eglTexBuffer_cleanup(this);
  LG_LOCK_FREE(this->copyLock);

  if (this->free)
    free(this);
}

bool eglTexBuffer_setup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  eglTexBuffer_cleanup(this);

  if (!eglTexUtilGetFormat(setup, &this->format))
    return false;

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  glGenTextures(this->texCount, this->tex);
  for(int i = 0; i < this->texCount; ++i)
  {
    glBindTexture(GL_TEXTURE_2D, this->tex[i]);
    glTexImage2D(GL_TEXTURE_2D,
        0,
        this->format.intFormat,
        this->format.width,
        this->format.height,
        0,
        this->format.format,
        this->format.dataType,
        NULL);
  }

  glBindTexture(GL_TEXTURE_2D, 0);
  this->rIndex = -1;

  return true;
}

static bool eglTexBuffer_update(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  assert(update->type == EGL_TEXTYPE_BUFFER);

  glBindTexture(GL_TEXTURE_2D, this->tex[0]);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, this->format.pitch);
  glTexSubImage2D(GL_TEXTURE_2D,
      0, 0, 0,
      this->format.width,
      this->format.height,
      this->format.format,
      this->format.dataType,
      update->buffer);
  glBindTexture(GL_TEXTURE_2D, 0);

  return true;
}

EGL_TexStatus eglTexBuffer_process(EGL_Texture * texture)
{
  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus eglTexBuffer_bind(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, this->tex[0]);
  glBindSampler(0, this->sampler);

  return true;
}

// streaming functions

bool eglTexBuffer_stream_init(EGL_Texture ** texture, EGLDisplay * display)
{
  if (!eglTexBuffer_init(texture, display))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, *texture);

  this->texCount = 2;
  LG_LOCK_INIT(this->copyLock);
  return true;
}

bool eglTexBuffer_stream_setup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  if (!eglTexBuffer_setup(texture, setup))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  return eglTexUtilGenBuffers(&this->format, this->buf, this->texCount);
}

static bool eglTexBuffer_stream_update(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  assert(update->type == EGL_TEXTYPE_BUFFER);

  LG_LOCK(this->copyLock);
  memcpy(this->buf[this->bufIndex].map, update->buffer,
      this->format.bufferSize);
  this->buf[this->bufIndex].updated = true;
  LG_UNLOCK(this->copyLock);

  return true;
}

EGL_TexStatus eglTexBuffer_stream_process(EGL_Texture * texture)
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
    glPixelStorei(GL_UNPACK_ROW_LENGTH, this->format.pitch);
    glTexSubImage2D(GL_TEXTURE_2D,
        0, 0, 0,
        this->format.width,
        this->format.height,
        this->format.format,
        this->format.dataType,
        (const void *)0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glDeleteSync(atomic_exchange(&this->sync, sync));
    glFlush();
  }

  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus eglTexBuffer_stream_bind(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  if (this->rIndex == -1)
    return EGL_TEX_STATUS_NOTREADY;

  GLsync sync = atomic_exchange(&this->sync, 0);
  if (sync)
  {
    switch(glClientWaitSync(sync, 0, 20000000)) // 20ms
    {
      case GL_ALREADY_SIGNALED:
      case GL_CONDITION_SATISFIED:
        glDeleteSync(sync);
        break;

      case GL_TIMEOUT_EXPIRED:
        return EGL_TEX_STATUS_NOTREADY;

      case GL_WAIT_FAILED:
      case GL_INVALID_VALUE:
        glDeleteSync(sync);
        DEBUG_GL_ERROR("glClientWaitSync failed");
        return EGL_TEX_STATUS_ERROR;
    }
  }

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, this->tex[this->rIndex]);
  glBindSampler(0, this->sampler);

  return EGL_TEX_STATUS_OK;
}

const EGL_TextureOps EGL_TextureBuffer =
{
  .init        = eglTexBuffer_init,
  .free        = eglTexBuffer_free,
  .setup       = eglTexBuffer_setup,
  .update      = eglTexBuffer_update,
  .process     = eglTexBuffer_process,
  .bind        = eglTexBuffer_bind
};

const EGL_TextureOps EGL_TextureBufferStream =
{
  .init        = eglTexBuffer_stream_init,
  .free        = eglTexBuffer_free,
  .setup       = eglTexBuffer_stream_setup,
  .update      = eglTexBuffer_stream_update,
  .process     = eglTexBuffer_stream_process,
  .bind        = eglTexBuffer_stream_bind
};
