/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include "convert_24bit.frag.h"


typedef struct EGL_Filter24bit
{
  EGL_Filter base;

  bool enable;
  EGL_PixelFormat format;
  int  useDMA;
  unsigned int width, height;
  unsigned int desktopWidth, desktopHeight;
  bool prepared;

  EGL_Uniform       uOutputSize;

  EGL_Shader      * shader;
  EGL_Framebuffer * fb;
  GLuint            sampler[2];
}
EGL_Filter24bit;

static bool egl_filter24bitInit(EGL_Filter ** filter)
{
  EGL_Filter24bit * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate ram");
    return false;
  }

  this->useDMA = -1;

  if (!egl_shaderInit(&this->shader))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

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

  *filter = &this->base;
  return true;

error_shader:
  egl_shaderFree(&this->shader);

error_this:
  free(this);
  return false;
}

static void egl_filter24bitFree(EGL_Filter * filter)
{
  EGL_Filter24bit * this = UPCAST(EGL_Filter24bit, filter);

  egl_shaderFree(&this->shader);
  egl_framebufferFree(&this->fb);
  glDeleteSamplers(ARRAY_LENGTH(this->sampler), this->sampler);
  free(this);
}

static bool egl_filter24bitSetup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height,
    unsigned int desktopWidth, unsigned int desktopHeight,
    bool useDMA)
{
  EGL_Filter24bit * this = UPCAST(EGL_Filter24bit, filter);

  if (pixFmt != EGL_PF_BGR_32 && pixFmt != EGL_PF_RGB_24_32)
    return false;

  if (this->useDMA != useDMA || this->format != pixFmt)
  {
    EGL_ShaderDefine defines[] =
    {
      {"OUTPUT", pixFmt == EGL_PF_BGR_32 ? "fragColor.bgra" : "fragColor.rgba" },
      {0}
    };

    if (!egl_shaderCompile(this->shader,
          b_shader_basic_vert        , b_shader_basic_vert_size,
          b_shader_convert_24bit_frag, b_shader_convert_24bit_frag_size,
          useDMA, defines)
       )
    {
      DEBUG_ERROR("Failed to compile the shader");
      return false;
    }

    this->uOutputSize.type     = EGL_UNIFORM_TYPE_2F;
    this->uOutputSize.location =
      egl_shaderGetUniform(this->shader, "outputSize");

    this->useDMA   = useDMA;
    this->prepared = false;
  }

  if (this->prepared &&
      this->width         == width        &&
      this->height        == height       &&
      this->desktopWidth  == desktopWidth &&
      this->desktopHeight == desktopHeight)
    return true;

  if (!egl_framebufferSetup(this->fb, EGL_PF_BGRA, desktopWidth, desktopHeight))
    return false;

  this->format        = pixFmt;
  this->width         = width;
  this->height        = height;
  this->desktopWidth  = desktopWidth;
  this->desktopHeight = desktopHeight;
  this->prepared      = false;

  return true;
}

static void egl_filter24bitGetOutputRes(EGL_Filter * filter,
    unsigned int *width, unsigned int *height, enum EGL_PixelFormat *pixFmt)
{
  EGL_Filter24bit * this = UPCAST(EGL_Filter24bit, filter);
  *width  = this->desktopWidth;
  *height = this->desktopHeight;
  *pixFmt = EGL_PF_BGRA;
}

static bool egl_filter24bitPrepare(EGL_Filter * filter)
{
  EGL_Filter24bit * this = UPCAST(EGL_Filter24bit, filter);

  if (this->prepared)
    return true;

  this->uOutputSize.f[0] = this->desktopWidth;
  this->uOutputSize.f[1] = this->desktopHeight;
  egl_shaderSetUniforms(this->shader, &this->uOutputSize, 1);

  this->prepared = true;
  return true;
}

static EGL_Texture * egl_filter24bitRun(EGL_Filter * filter,
    EGL_FilterRects * rects, EGL_Texture * texture)
{
  EGL_Filter24bit * this = UPCAST(EGL_Filter24bit, filter);

  egl_framebufferBind(this->fb);

  glActiveTexture(GL_TEXTURE0);
  egl_textureBind(texture);

  glBindSampler(0, this->sampler[0]);

  egl_shaderUse(this->shader);
  egl_filterRectsRender(this->shader, rects);

  return egl_framebufferGetTexture(this->fb);
}

EGL_FilterOps egl_filter24bitOps =
{
  .id           = "24bit",
  .name         = "24bit",
  .type         = EGL_FILTER_TYPE_INTERNAL,
  .earlyInit    = NULL,
  .init         = egl_filter24bitInit,
  .free         = egl_filter24bitFree,
  .imguiConfig  = NULL,
  .saveState    = NULL,
  .loadState    = NULL,
  .setup        = egl_filter24bitSetup,
  .getOutputRes = egl_filter24bitGetOutputRes,
  .prepare      = egl_filter24bitPrepare,
  .run          = egl_filter24bitRun
};
