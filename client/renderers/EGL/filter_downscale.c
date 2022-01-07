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

#include "filter.h"
#include "framebuffer.h"

#include <math.h>

#include "common/array.h"
#include "common/debug.h"
#include "common/option.h"
#include "cimgui.h"

#include "basic.vert.h"
#include "downscale.frag.h"
#include "downscale_lanczos2.frag.h"
#include "downscale_linear.frag.h"

typedef enum
{
  DOWNSCALE_NEAREST = 0,
  DOWNSCALE_LINEAR,
  DOWNSCALE_LANCZOS2,
}
DownscaleFilter;

#define DOWNSCALE_COUNT (DOWNSCALE_LANCZOS2 + 1)

const char *filterNames[DOWNSCALE_COUNT] = {
  "Nearest pixel",
  "Linear",
  "Lanczos",
};

typedef struct EGL_FilterDownscale
{
  EGL_Filter base;

  bool         enable;
  EGL_Shader * nearest;
  EGL_Uniform  uNearest;
  EGL_Shader * linear;
  EGL_Shader * lanczos2;

  DownscaleFilter filter;
  enum EGL_PixelFormat pixFmt;
  unsigned int width, height;
  float pixelSize;
  float vOffset, hOffset;
  bool prepared;

  EGL_Framebuffer * fb;
  GLuint            sampler[2];
}
EGL_FilterDownscale;

static void egl_filterDownscaleEarlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module        = "eglFilter",
      .name          = "downscale",
      .description   = "Enable downscaling",
      .preset        = true,
      .type          = OPTION_TYPE_BOOL,
      .value.x_bool  = false
    },
    {
      .module        = "eglFilter",
      .name          = "downscalePixelSize",
      .description   = "Downscale filter pixel size",
      .preset        = true,
      .type          = OPTION_TYPE_FLOAT,
      .value.x_float = 2.0f
    },
    {
      .module        = "eglFilter",
      .name          = "downscaleHOffset",
      .description   = "Downscale filter horizontal offset",
      .preset        = true,
      .type          = OPTION_TYPE_FLOAT,
      .value.x_float = 0.0f
    },
    {
      .module        = "eglFilter",
      .name          = "downscaleVOffset",
      .description   = "Downscale filter vertical offset",
      .preset        = true,
      .type          = OPTION_TYPE_FLOAT,
      .value.x_float = 0.0f
    },
    {
      .module        = "eglFilter",
      .name          = "downscaleFilter",
      .description   = "Downscale filter type",
      .preset        = true,
      .type          = OPTION_TYPE_INT,
      .value.x_int   = 0
    },
    { 0 }
  };

  option_register(options);
}

static void egl_filterDownscaleSaveState(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  option_set_bool ("eglFilter", "downscale",          this->enable);
  option_set_float("eglFilter", "downscalePixelSize", this->pixelSize);
  option_set_float("eglFilter", "downscaleHOffset",   this->vOffset);
  option_set_float("eglFilter", "downscaleVOffset",   this->hOffset);
  option_set_int  ("eglFilter", "downscaleFilter",    this->filter);
}

static void egl_filterDownscaleLoadState(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  this->enable    = option_get_bool ("eglFilter", "downscale");
  this->pixelSize = option_get_float("eglFilter", "downscalePixelSize");
  this->vOffset   = option_get_float("eglFilter", "downscaleHOffset");
  this->hOffset   = option_get_float("eglFilter", "downscaleVOffset");
  this->filter    = option_get_int  ("eglFilter", "downscaleFilter");

  if (this->filter < 0 || this->filter >= DOWNSCALE_COUNT)
    this->filter = 0;

  this->prepared = false;
}

static bool egl_filterDownscaleInit(EGL_Filter ** filter)
{
  EGL_FilterDownscale * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  if (!egl_shaderInit(&this->nearest))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

  if (!egl_shaderCompile(this->nearest,
        b_shader_basic_vert    , b_shader_basic_vert_size,
        b_shader_downscale_frag, b_shader_downscale_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the shader");
    goto error_shader;
  }

  if (!egl_shaderInit(&this->linear))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

  if (!egl_shaderCompile(this->linear,
        b_shader_basic_vert, b_shader_basic_vert_size,
        b_shader_downscale_linear_frag, b_shader_downscale_linear_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the shader");
    goto error_shader;
  }

  if (!egl_shaderInit(&this->lanczos2))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

  if (!egl_shaderCompile(this->lanczos2,
        b_shader_basic_vert, b_shader_basic_vert_size,
        b_shader_downscale_lanczos2_frag, b_shader_downscale_lanczos2_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the shader");
    goto error_shader;
  }

  this->uNearest.type = EGL_UNIFORM_TYPE_3F;
  this->uNearest.location =
    egl_shaderGetUniform(this->nearest, "uConfig");

  if (!egl_framebufferInit(&this->fb))
  {
    DEBUG_ERROR("Failed to initialize the framebuffer");
    goto error_shader;
  }

  glGenSamplers(ARRAY_LENGTH(this->sampler), this->sampler);
  glSamplerParameteri(this->sampler[0], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(this->sampler[0], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(this->sampler[0], GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler[0], GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler[1], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler[1], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler[1], GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler[1], GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  egl_filterDownscaleLoadState(&this->base);

  *filter = &this->base;
  return true;

error_shader:
  egl_shaderFree(&this->nearest);
  egl_shaderFree(&this->linear);
  egl_shaderFree(&this->lanczos2);

error_this:
  free(this);
  return false;
}

static void egl_filterDownscaleFree(EGL_Filter * filter)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  egl_shaderFree(&this->nearest);
  egl_shaderFree(&this->linear);
  egl_shaderFree(&this->lanczos2);
  egl_framebufferFree(&this->fb);
  glDeleteSamplers(ARRAY_LENGTH(this->sampler), this->sampler);
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

  if (igBeginCombo("Filter", filterNames[this->filter], 0))
  {
    for (int i = 0; i < DOWNSCALE_COUNT; ++i)
    {
      bool selected = i == this->filter;
      if (igSelectable_Bool(filterNames[i], selected, 0, (ImVec2) { 0.0f, 0.0f }))
      {
        redraw = true;
        this->filter = i;
      }
      if (selected)
        igSetItemDefaultFocus();
    }
    igEndCombo();
  }

  float pixelSize = this->pixelSize;
  igInputFloat("Pixel size", &pixelSize, 0.1f, 1.0f, "%.2f", ImGuiInputTextFlags_CharsDecimal);
  pixelSize = util_clamp(pixelSize, 1.0f, 10.0f);
  igSliderFloat("##pixelsize", &pixelSize, 1.0f, 10.0f, "%.2f",
      ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_NoInput);

  igText("Resolution: %dx%d", this->width, this->height);

  if (pixelSize != this->pixelSize)
  {
    this->pixelSize = pixelSize;
    redraw = true;
  }

  switch (this->filter)
  {
    case DOWNSCALE_NEAREST:
    {
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
      break;
    }

    default:
      break;
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

  if (!this->enable)
    return false;

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

  if (this->prepared)
    return true;

  switch (this->filter)
  {
    case DOWNSCALE_NEAREST:
      this->uNearest.f[0] = this->pixelSize;
      this->uNearest.f[1] = this->vOffset;
      this->uNearest.f[2] = this->hOffset;
      egl_shaderSetUniforms(this->nearest, &this->uNearest, 1);
      break;

    default:
      break;
  }
  this->prepared = true;

  return true;
}

static GLuint egl_filterDownscaleRun(EGL_Filter * filter,
    EGL_FilterRects * rects, GLuint texture)
{
  EGL_FilterDownscale * this = UPCAST(EGL_FilterDownscale, filter);

  egl_framebufferBind(this->fb);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);

  EGL_Shader * shader;

  switch (this->filter)
  {
    case DOWNSCALE_NEAREST:
      glBindSampler(0, this->sampler[0]);
      shader = this->nearest;
      break;

    case DOWNSCALE_LINEAR:
      glBindSampler(0, this->sampler[1]);
      shader = this->linear;
      break;

    case DOWNSCALE_LANCZOS2:
      glBindSampler(0, this->sampler[0]);
      shader = this->lanczos2;
      break;

    default:
      DEBUG_UNREACHABLE();
  }

  egl_shaderUse(shader);
  egl_filterRectsRender(shader, rects);

  return egl_framebufferGetTexture(this->fb);
}

EGL_FilterOps egl_filterDownscaleOps =
{
  .id           = "downscale",
  .name         = "Downscaler",
  .type         = EGL_FILTER_TYPE_DOWNSCALE,
  .earlyInit    = egl_filterDownscaleEarlyInit,
  .init         = egl_filterDownscaleInit,
  .free         = egl_filterDownscaleFree,
  .imguiConfig  = egl_filterDownscaleImguiConfig,
  .saveState    = egl_filterDownscaleSaveState,
  .loadState    = egl_filterDownscaleLoadState,
  .setup        = egl_filterDownscaleSetup,
  .getOutputRes = egl_filterDownscaleGetOutputRes,
  .prepare      = egl_filterDownscalePrepare,
  .run          = egl_filterDownscaleRun
};
