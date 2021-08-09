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
  EGL_Texture *owner;

  bool enabled;
  GLuint fb;
  GLuint tex;
  EGL_Shader * shader;
  float scale;

  unsigned int width, height;

  GLint uInRes;
  GLint uOutRes;
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

  this->scale = 1.0f;
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
    ringbuffer_free(&this->textures);
    free(this->bindData);
  }

  glDeleteSamplers(1, &this->sampler);

  this->ops.free(this);
  *tex = NULL;
}

bool setupRenderStep(EGL_Texture * this, RenderStep * step)
{
  step->width  = this->format.width  * step->scale;
  step->height = this->format.height * step->scale;

  glBindTexture(GL_TEXTURE_2D, step->tex);
  glTexImage2D(GL_TEXTURE_2D,
      0,
      this->format.intFormat,
      step->width,
      step->height,
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

typedef struct BindInfo
{
  GLuint tex;
  unsigned int width;
  unsigned int height;
}
BindInfo;

typedef struct BindData
{
  GLuint sampler;
  GLuint dimensions[];
}
BindData;

static bool rbBindTexture(int index, void * value, void * udata)
{
  BindInfo * bi = (BindInfo *)value;
  BindData * bd = (BindData *)udata;

  glActiveTexture(GL_TEXTURE0 + index);
  glBindTexture(GL_TEXTURE_2D, bi->tex);
  glBindSampler(0, bd->sampler);
  bd->dimensions[index * 2 + 0] = bi->width;
  bd->dimensions[index * 2 + 1] = bi->height;
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

  if (this->bindDataSize < ll_count(this->render))
  {
    free(this->bindData);

    BindData * bd = (BindData *)calloc(1, sizeof(struct BindData) +
        sizeof(bd->dimensions[0]) * (ll_count(this->render)+1) * 2);
    bd->sampler = this->sampler;

    this->bindData     = bd;
    this->bindDataSize = ll_count(this->render);
  }

  BindData * bd = (BindData *)this->bindData;
  RenderStep * step;

  /* if the postProcessing has not yet been done */
  if (!this->postProcessed)
  {
    ringbuffer_reset(this->textures);

    if ((status = this->ops.get(this, &tex)) != EGL_TEX_STATUS_OK)
      return status;

    ringbuffer_push(this->textures, &(BindInfo) {
      .tex    = tex,
      .width  = this->format.width,
      .height = this->format.height
    });

    ringbuffer_forEach(this->textures, rbBindTexture, bd, true);

    bool cleanup = false;
    for(ll_reset(this->render); ll_walk(this->render, (void **)&step); )
    {
      if (!step->enabled)
        continue;

      cleanup = true;

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

      glViewport(0, 0, step->width, step->height);

      /* use the shader (also configures it's set uniforms) */
      egl_shaderUse(step->shader);

      /* set the size uniforms */
      glUniform2uiv(step->uInRes,
          ringbuffer_getCount(this->textures), bd->dimensions);
      glUniform2ui(step->uOutRes, step->width, step->height);

      /* render the scene */
      egl_modelRender(this->model);

      /* push the details into the ringbuffer for the next pass */
      ringbuffer_push(this->textures, &(BindInfo) {
        .tex    = step->tex,
        .width  = step->width,
        .height = step->height
      });

      /* bind the textures for the next pass */
      ringbuffer_forEach(this->textures, rbBindTexture, bd, true);
    }

    /* restore the state and the viewport */
    if (cleanup)
    {
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      glUseProgram(0);
      egl_resetViewport(this->egl);
    }

    this->postProcessed = true;
  }
  else
  {
    /* bind the last texture */
    BindInfo * bi = (BindInfo *)ringBuffer_getLastValue(this->textures);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, bi->tex);
    glBindSampler(0, this->sampler);
  }

  return EGL_TEX_STATUS_OK;
}

PostProcessHandle egl_textureAddFilter(EGL_Texture * this, EGL_Shader * shader,
    float outputScale, bool enabled)
{
  if (!this->render)
  {
    this->render = ll_new();
    egl_modelInit(&this->model);
    egl_modelSetDefault(this->model, false);
    this->textures = ringbuffer_new(8, sizeof(BindInfo));
  }

  RenderStep * step = calloc(1, sizeof(*step));
  glGenTextures(1, &step->tex);
  step->owner   = this;
  step->shader  = shader;
  step->scale   = outputScale;
  step->uInRes  = egl_shaderGetUniform(shader, "uInRes" );
  step->uOutRes = egl_shaderGetUniform(shader, "uOutRes");
  step->enabled = enabled;

  this->scale = outputScale;

  if (this->formatValid)
    if (!setupRenderStep(this, step))
    {
      glDeleteTextures(1, &step->tex);
      free(step);
      return NULL;
    }

  ll_push(this->render, step);
  return (PostProcessHandle)step;
}

void egl_textureEnableFilter(PostProcessHandle * handle, bool enable)
{
  RenderStep * step = (RenderStep *)handle;
  if (step->enabled == enable)
    return;

  step->enabled = enable;
  egl_textureInvalidate(step->owner);
}

void egl_textureInvalidate(EGL_Texture * texture)
{
  texture->postProcessed = false;
}

float egl_textureGetScale(EGL_Texture * this)
{
  return this->scale;
}
