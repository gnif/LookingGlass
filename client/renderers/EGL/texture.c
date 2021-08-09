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

#include <stdbool.h>
#include <assert.h>
#include <string.h>
#include "shader.h"
#include "common/framebuffer.h"
#include "common/debug.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "texture_buffer.h"

extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;
extern const EGL_TextureOps EGL_TextureFrameBuffer;
extern const EGL_TextureOps EGL_TextureDMABUF;

typedef struct RenderStep
{
  GLuint fb;
  GLuint tex;
  EGL_Shader * shader;
}
RenderStep;

bool egl_textureInit(EGL * egl, EGL_Texture ** texture_,
    EGLDisplay * display, EGL_TexType type, bool streaming)
{
  const EGL_TextureOps * ops;

  switch(type)
  {
    case EGL_TEXTYPE_BUFFER:
      ops = streaming ? &EGL_TextureBufferStream : &EGL_TextureBuffer;
      break;

    case EGL_TEXTYPE_FRAMEBUFFER:
      assert(streaming);
      ops = &EGL_TextureFrameBuffer;
      break;

    case EGL_TEXTYPE_DMABUF:
      assert(streaming);
      ops = &EGL_TextureDMABUF;
      break;

    default:
      return false;
  }

  *texture_ = NULL;
  if (!ops->init(texture_, display))
    return false;

  EGL_Texture * this = *texture_;
  memcpy(&this->ops, ops, sizeof(*ops));
  this->egl = egl;

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  this->textures = ringbuffer_new(8, sizeof(GLuint));

  return true;
}

void egl_textureFree(EGL_Texture ** tex)
{
  EGL_Texture * this = *tex;

  if (this->render)
  {
    RenderStep * step;
    while(ll_shift(this->render, (void **)&step))
    {
      if (step->fb)
        glDeleteFramebuffers(1, &step->fb);

      glDeleteTextures(1, &step->tex);
      free(step);
    }
    ll_free(this->render);
    egl_modelFree(&this->model);
  }

  glDeleteSamplers(1, &this->sampler);
  ringbuffer_free(&this->textures);

  this->ops.free(this);
  *tex = NULL;
}

bool setupRenderStep(EGL_Texture * this, RenderStep * step)
{
  glBindTexture(GL_TEXTURE_2D, step->tex);
  glTexImage2D(GL_TEXTURE_2D,
      0,
      this->format.intFormat,
      this->format.width,
      this->format.height,
      0,
      this->format.format,
      this->format.dataType,
      NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, 0);

  return true;
}

bool egl_textureSetup(EGL_Texture * this, enum EGL_PixelFormat pixFmt,
    size_t width, size_t height, size_t stride)
{
  const struct EGL_TexSetup setup =
  {
    .pixFmt = pixFmt,
    .width  = width,
    .height = height,
    .stride = stride
  };
  this->size = height * stride;

  if (!egl_texUtilGetFormat(&setup, &this->format))
    return false;

  this->formatValid = true;

  /* reconfigure any intermediate render steps */
  if (this->render)
  {
    RenderStep * step;
    for(ll_reset(this->render); ll_walk(this->render, (void **)&step); )
      if (!setupRenderStep(this, step))
        return false;
  }

  return this->ops.setup(this, &setup);
}

bool egl_textureUpdate(EGL_Texture * this, const uint8_t * buffer)
{
  const struct EGL_TexUpdate update =
  {
    .type   = EGL_TEXTYPE_BUFFER,
    .buffer = buffer
  };

  if (this->ops.update(this, &update))
  {
    atomic_store(&this->updated, true);
    return true;
  }

  return false;
}

bool egl_textureUpdateFromFrame(EGL_Texture * this,
    const FrameBuffer * frame, const FrameDamageRect * damageRects,
    int damageRectsCount)
{
  const struct EGL_TexUpdate update =
  {
    .type      = EGL_TEXTYPE_FRAMEBUFFER,
    .frame     = frame,
    .rects     = damageRects,
    .rectCount = damageRectsCount,
  };

  if (this->ops.update(this, &update))
  {
    atomic_store(&this->updated, true);
    return true;
  }

  return false;
}

bool egl_textureUpdateFromDMA(EGL_Texture * this,
    const FrameBuffer * frame, const int dmaFd)
{
  const struct EGL_TexUpdate update =
  {
    .type  = EGL_TEXTYPE_DMABUF,
    .dmaFD = dmaFd
  };

  /* wait for completion */
  framebuffer_wait(frame, this->size);

  if (this->ops.update(this, &update))
  {
    atomic_store(&this->updated, true);
    return true;
  }

  return false;
}

enum EGL_TexStatus egl_textureProcess(EGL_Texture * this)
{
  EGL_TexStatus status;
  if ((status = this->ops.process(this)) == EGL_TEX_STATUS_OK)
  {
    if (atomic_exchange(&this->updated, false))
      this->postProcessed = false;
  }
  return status;
}

static bool rbBindTexture(int index, void * value, void * udata)
{
  GLuint      * tex  = (GLuint *)value;
  EGL_Texture * this = (EGL_Texture *)udata;

  glActiveTexture(GL_TEXTURE0 + index);
  glBindTexture(GL_TEXTURE_2D, *tex);
  glBindSampler(0, this->sampler);
  return true;
}

enum EGL_TexStatus egl_textureBind(EGL_Texture * this)
{
  GLuint tex;
  EGL_TexStatus status;

  if (!this->render)
  {
    if ((status = this->ops.get(this, &tex)) != EGL_TEX_STATUS_OK)
      return status;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, this->sampler);
    return EGL_TEX_STATUS_OK;
  }

  RenderStep * step;
  ringbuffer_reset(this->textures);

  /* if the postProcessing has not yet been done */
  if (!this->postProcessed)
  {
    if ((status = this->ops.get(this, &tex)) != EGL_TEX_STATUS_OK)
      return status;

    ringbuffer_push(this->textures, &tex);
    ringbuffer_forEach(this->textures, rbBindTexture, this, true);

    for(ll_reset(this->render); ll_walk(this->render, (void **)&step); )
    {
      /* create the framebuffer here as it must be in the same gl context as
       * it's usage */
      if (!step->fb)
      {
        glGenFramebuffers(1, &step->fb);
        glBindFramebuffer(GL_FRAMEBUFFER, step->fb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, step->tex, 0);
        glDrawBuffers(1, &(GLenum){GL_COLOR_ATTACHMENT0});
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        {
          DEBUG_ERROR("Failed to setup the shader framebuffer");
          return EGL_TEX_STATUS_ERROR;
        }
      }
      else
        glBindFramebuffer(GL_FRAMEBUFFER, step->fb);

      glViewport(0, 0, this->format.width, this->format.height);
      egl_shaderUse(step->shader);
      egl_modelRender(this->model);

      ringbuffer_push(this->textures, &step->tex);
      ringbuffer_forEach(this->textures, rbBindTexture, this, true);
    }

    /* restore the state and the viewport */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    egl_resetViewport(this->egl);

    this->postProcessed = true;
  }
  else
  {
    /* bind the last texture */
    ll_peek_tail(this->render, (void **)&step);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, step->tex);
    glBindSampler(0, this->sampler);
  }

  return EGL_TEX_STATUS_OK;
}

enum EGL_TexStatus egl_textureAddShader(EGL_Texture * this, EGL_Shader * shader)
{
  if (!this->render)
  {
    this->render = ll_new();
    egl_modelInit(&this->model);
    egl_modelSetDefault(this->model, false);
  }

  RenderStep * step = calloc(1, sizeof(*step));
  glGenTextures(1, &step->tex);
  step->shader = shader;

  if (this->formatValid)
    if (!setupRenderStep(this, step))
    {
      glDeleteTextures(1, &step->tex);
      free(step);
      return EGL_TEX_STATUS_ERROR;
    }

  ll_push(this->render, step);
  return EGL_TEX_STATUS_OK;
}
