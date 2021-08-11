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

#include "filter.h"
#include "framebuffer.h"

#include "common/debug.h"
#include "common/option.h"
#include "cimgui.h"

#include "basic.vert.h"
#include "ffx_fsr1_easu.frag.h"
#include "ffx_fsr1_rcas.frag.h"

typedef struct EGL_FilterFFXFSR1
{
  EGL_Filter base;

  EGL_Shader * easu, * rcas;
  bool         enable, active;
  float        sharpness;
  EGL_Uniform  easuUniform, rcasUniform;

  enum EGL_PixelFormat pixFmt;
  unsigned int width, height;
  bool sizeChanged;
  bool prepared;

  EGL_Framebuffer * easuFb, * rcasFb;
  GLuint            sampler;
}
EGL_FilterFFXFSR1;

static void egl_filterFFXFSR1EarlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module        = "eglFilter",
      .name          = "ffxFSR",
      .description   = "AMD FidelityFX FSR",
      .type          = OPTION_TYPE_BOOL,
      .value.x_bool  = false
    },
    {
      .module        = "eglFilter",
      .name          = "ffxFSRSharpness",
      .description   = "AMD FidelityFX FSR Sharpness",
      .type          = OPTION_TYPE_FLOAT,
      .value.x_float = 1.0f
    },
    { 0 }
  };

  option_register(options);
}

static bool egl_filterFFXFSR1Init(EGL_Filter ** filter)
{
  EGL_FilterFFXFSR1 * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  if (!egl_shaderInit(&this->easu))
  {
    DEBUG_ERROR("Failed to initialize the Easu shader");
    goto error_this;
  }

  if (!egl_shaderInit(&this->rcas))
  {
    DEBUG_ERROR("Failed to initialize the Rcas shader");
    goto error_esau;
  }

  if (!egl_shaderCompile(this->easu,
        b_shader_basic_vert        , b_shader_basic_vert_size,
        b_shader_ffx_fsr1_easu_frag, b_shader_ffx_fsr1_easu_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the Easu shader");
    goto error_rcas;
  }

  if (!egl_shaderCompile(this->rcas,
        b_shader_basic_vert        , b_shader_basic_vert_size,
        b_shader_ffx_fsr1_rcas_frag, b_shader_ffx_fsr1_rcas_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the Rcas shader");
    goto error_rcas;
  }


  this->enable = option_get_bool("eglFilter", "ffxFSR");

  this->easuUniform.type = EGL_UNIFORM_TYPE_2UI;
  this->easuUniform.location =
    egl_shaderGetUniform(this->easu, "uOutRes");

  this->rcasUniform.type = EGL_UNIFORM_TYPE_1F;
  this->rcasUniform.location =
    egl_shaderGetUniform(this->rcas, "uSharpness");
  this->sharpness = option_get_float("eglFilter", "ffxFSRSharpness");
  this->rcasUniform.f[0] = 2.0f - this->sharpness * 2.0f;

  if (!egl_framebufferInit(&this->easuFb))
  {
    DEBUG_ERROR("Failed to initialize the Easu framebuffer");
    goto error_rcas;
  }

  if (!egl_framebufferInit(&this->rcasFb))
  {
    DEBUG_ERROR("Failed to initialize the Rcas framebuffer");
    goto error_easuFb;
  }

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  *filter = &this->base;
  return true;

error_easuFb:
  egl_framebufferFree(&this->rcasFb);

error_rcas:
  egl_shaderFree(&this->rcas);

error_esau:
  egl_shaderFree(&this->easu);

error_this:
  free(this);
  return false;
}

static void egl_filterFFXFSR1Free(EGL_Filter * filter)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);

  egl_shaderFree(&this->easu);
  egl_shaderFree(&this->rcas);
  egl_framebufferFree(&this->easuFb);
  egl_framebufferFree(&this->rcasFb);
  glDeleteSamplers(1, &this->sampler);
  free(this);
}

static bool egl_filterFFXFSR1ImguiConfig(EGL_Filter * filter)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);

  bool  redraw    = false;
  bool  enable    = this->enable;
  float sharpness = this->sharpness;

  igCheckbox("AMD FidelityFX FSR", &enable);
  if (enable != this->enable)
  {
    this->enable = enable;
    redraw = true;
  }

  igText("Sharpness:");
  igSameLine(0.0f, -1.0f);
  igPushItemWidth(igGetWindowWidth() - igGetCursorPosX() -
      igGetStyle()->WindowPadding.x);
  igSliderFloat("##fsr1Sharpness", &sharpness, 0.0f, 1.0f, NULL, 0);
  igPopItemWidth();

  if (sharpness != this->sharpness)
  {
    // enable FSR1 if the sharpness was changed
    if (!enable)
    {
      enable = true;
      this->enable = true;
    }

    this->sharpness = sharpness;
    this->rcasUniform.f[0] = 2.0f - sharpness * 2.0f;
    redraw = true;
  }

  if (redraw)
    this->prepared = false;

  return redraw;
}

static void egl_filterFFXFSR1SetOutputResHint(EGL_Filter * filter,
    unsigned int width, unsigned int height)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);
  if (this->width == width && this->height == height)
    return;

  this->width       = width;
  this->height      = height;
  this->sizeChanged = true;
  this->prepared    = false;
}

static bool egl_filterFFXFSR1Setup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);

  if (pixFmt == this->pixFmt && !this->sizeChanged)
    return true;

  if (!egl_framebufferSetup(this->easuFb, pixFmt, this->width, this->height))
    return false;

  if (!egl_framebufferSetup(this->rcasFb, pixFmt, this->width, this->height))
    return false;

  this->active      = this->width > width && this->height > height;
  this->sizeChanged = false;
  this->pixFmt      = pixFmt;
  this->prepared    = false;
  return true;
}

static void egl_filterFFXFSR1GetOutputRes(EGL_Filter * filter,
    unsigned int *width, unsigned int *height)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);
  *width  = this->width;
  *height = this->height;
}

static bool egl_filterFFXFSR1Prepare(EGL_Filter * filter)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);

  if (!this->enable || !this->active)
    return false;

  if (this->prepared)
    return true;

  this->easuUniform.ui[0] = this->width;
  this->easuUniform.ui[1] = this->height;

  egl_shaderSetUniforms(this->easu, &this->easuUniform, 1);
  egl_shaderSetUniforms(this->rcas, &this->rcasUniform, 1);
  this->prepared = true;

  return true;
}

static GLuint egl_filterFFXFSR1Run(EGL_Filter * filter, EGL_Model * model,
    GLuint texture)
{
  EGL_FilterFFXFSR1 * this = UPCAST(EGL_FilterFFXFSR1, filter);

  // pass 1, Easu
  egl_framebufferBind(this->easuFb);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, this->sampler);
  egl_shaderUse(this->easu);
  egl_modelRender(model);
  texture = egl_framebufferGetTexture(this->easuFb);

  // pass 2, Rcas
  egl_framebufferBind(this->rcasFb);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, this->sampler);
  egl_shaderUse(this->rcas);
  egl_modelRender(model);
  texture = egl_framebufferGetTexture(this->rcasFb);

  return texture;
}

EGL_FilterOps egl_filterFFXFSR1Ops =
{
  .name             = "AMD FidelityFX FSR",
  .type             = EGL_FILTER_TYPE_UPSCALE,
  .earlyInit        = egl_filterFFXFSR1EarlyInit,
  .init             = egl_filterFFXFSR1Init,
  .free             = egl_filterFFXFSR1Free,
  .imguiConfig      = egl_filterFFXFSR1ImguiConfig,
  .setup            = egl_filterFFXFSR1Setup,
  .setOutputResHint = egl_filterFFXFSR1SetOutputResHint,
  .getOutputRes     = egl_filterFFXFSR1GetOutputRes,
  .prepare          = egl_filterFFXFSR1Prepare,
  .run              = egl_filterFFXFSR1Run
};
