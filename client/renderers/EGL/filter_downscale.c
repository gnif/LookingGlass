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

#include <math.h>

#include "common/debug.h"
#include "common/option.h"
#include "cimgui.h"

#include "basic.vert.h"
#include "downscale.frag.h"

typedef struct EGL_FilterDownscale
{
  EGL_Filter base;

  EGL_Shader * shader;
  bool         enable;
  EGL_Uniform  uniform;

  enum EGL_PixelFormat pixFmt;
  unsigned int width, height;
  float pixelSize;
  float vOffset, hOffset;
  bool prepared;

  EGL_Framebuffer * fb;
  GLuint            sampler;
}
EGL_FilterDownscale;

static void egl_filterDownscaleEarlyInit(void)
{
  // doesn't really make sense to have any options for this filter
  // as it's per title. We need presets to make this nicer to use.
  static struct Option options[] =
  {
    { 0 }
  };

  option_register(options);
}

static bool egl_filterDownscaleInit(EGL_Filter ** filter)
{
  EGL_FilterDownscale * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  if (!egl_shaderInit(&this->shader))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

  if (!egl_shaderCompile(this->shader,
        b_shader_basic_vert    , b_shader_basic_vert_size,
        b_shader_downscale_frag, b_shader_downscale_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the shader");
    goto error_shader;
  }

  this->uniform.type = EGL_UNIFORM_TYPE_3F;
  this->uniform.location =
    egl_shaderGetUniform(this->shader, "uConfig");

  if (!egl_framebufferInit(&this->fb))
  {
    DEBUG_ERROR("Failed to initialize the framebuffer");
    goto error_shader;
  }

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  this->enable    = false;
  this->pixelSize = 2.0f;
  this->vOffset   = 0.0f;
  this->hOffset   = 0.0f;

  *filter = &this->base;
  return true;

error_shader:
  egl_shaderFree(&this->shader);

error_this:
  free(this);
  return false;
}

static void egl_filterDownscaleFree(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  egl_shaderFree(&this->shader);
  egl_framebufferFree(&this->fb);
  glDeleteSamplers(1, &this->sampler);
  free(this);
}

static bool egl_filterDownscaleImguiConfig(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  bool redraw = false;
  bool enable = this->enable;

  igCheckbox("Enable", &enable);
  if (enable != this->enable)
  {
    this->enable = enable;
    redraw = true;
  }

  int majorPixelSize = floor(this->pixelSize);
  int minorPixelSize = (this->pixelSize - majorPixelSize) * 10.0f;

  igSliderInt("Major Pixel Size", &majorPixelSize, 1, 10, NULL, 0);
  igSliderInt("Minor Pixel Size", &minorPixelSize, 0,  9, NULL, 0);

  float pixelSize = (float)majorPixelSize + (float)minorPixelSize / 10.0f;
  igText("Pixel Size: %.2f", pixelSize);
  igText("Resolution: %dx%d", this->width, this->height);
  if (pixelSize != this->pixelSize)
  {
    this->pixelSize = pixelSize;
    redraw = true;
  }

  float vOffset = this->vOffset;
  igSliderFloat("V-Offset", &vOffset, -2, 2, NULL, 0);
  if (vOffset != this->vOffset)
  {
    this->vOffset = vOffset;
    redraw = true;
  }

  float hOffset = this->hOffset;
  igSliderFloat("H-Offset", &hOffset, -2, 2, NULL, 0);
  if (hOffset != this->hOffset)
  {
    this->hOffset = hOffset;
    redraw = true;
  }

  if (redraw)
    this->prepared = false;

  return redraw;
}

static bool egl_filterDownscaleSetup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  width  = (float)width  / this->pixelSize;
  height = (float)height / this->pixelSize;

  if (this->prepared               &&
      pixFmt       == this->pixFmt &&
      this->width  == width        &&
      this->height == height)
    return this->pixelSize > 1.0f;

  if (!egl_framebufferSetup(this->fb, pixFmt, width, height))
    return false;

  this->pixFmt   = pixFmt;
  this->width    = width;
  this->height   = height;
  this->prepared = false;

  return this->pixelSize > 1.0f;
}

static void egl_filterDownscaleGetOutputRes(EGL_Filter * filter,
    unsigned int *width, unsigned int *height)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);
  *width  = this->width;
  *height = this->height;
}

static bool egl_filterDownscalePrepare(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  if (!this->enable)
    return false;

  if (this->prepared)
    return true;

  this->uniform.f[0] = this->pixelSize;
  this->uniform.f[1] = this->vOffset;
  this->uniform.f[2] = this->hOffset;
  egl_shaderSetUniforms(this->shader, &this->uniform, 1);
  this->prepared = true;

  return true;
}

static GLuint egl_filterDownscaleRun(EGL_Filter * filter, EGL_Model * model,
    GLuint texture)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  egl_framebufferBind(this->fb);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, this->sampler);

  egl_shaderUse(this->shader);
  egl_modelRender(model);

  return egl_framebufferGetTexture(this->fb);
}

EGL_FilterOps egl_filterDownscaleOps =
{
  .name         = "Downscaler",
  .type         = EGL_FILTER_TYPE_DOWNSCALE,
  .earlyInit    = egl_filterDownscaleEarlyInit,
  .init         = egl_filterDownscaleInit,
  .free         = egl_filterDownscaleFree,
  .imguiConfig  = egl_filterDownscaleImguiConfig,
  .setup        = egl_filterDownscaleSetup,
  .getOutputRes = egl_filterDownscaleGetOutputRes,
  .prepare      = egl_filterDownscalePrepare,
  .run          = egl_filterDownscaleRun
};
