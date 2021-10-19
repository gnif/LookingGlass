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

#include "common/countedbuffer.h"
#include "common/debug.h"
#include "common/option.h"
#include "cimgui.h"
#include "ffx.h"

#include "basic.vert.h"
#include "ffx_cas.frag.h"

typedef struct EGL_FilterFFXCAS
{
  EGL_Filter base;

  EGL_Shader * shader;
  bool         enable;

  enum EGL_PixelFormat pixFmt;
  unsigned int width, height;
  float sharpness;
  CountedBuffer * consts;
  bool prepared;

  EGL_Framebuffer * fb;
  GLuint            sampler;
}
EGL_FilterFFXCAS;

static void egl_filterFFXCASEarlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module        = "eglFilter",
      .name          = "ffxCAS",
      .description   = "AMD FidelityFX CAS",
      .preset        = true,
      .type          = OPTION_TYPE_BOOL,
      .value.x_bool  = false
    },
    {
      .module        = "eglFilter",
      .name          = "ffxCASSharpness",
      .description   = "AMD FidelityFX CAS Sharpness",
      .preset        = true,
      .type          = OPTION_TYPE_FLOAT,
      .value.x_float = 0.0f
    },
    { 0 }
  };

  option_register(options);
}

static void casUpdateConsts(EGL_FilterFFXCAS * this)
{
  ffxCasConst((uint32_t *) this->consts->data, this->sharpness,
      this->width, this->height,
      this->width, this->height);
}

static void egl_filterFFXCASSaveState(EGL_Filter * filter)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  option_set_bool ("eglFilter", "ffxCAS", this->enable);
  option_set_float("eglFilter", "ffxCASSharpness", this->sharpness);
}

static void egl_filterFFXCASLoadState(EGL_Filter * filter)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  this->enable    = option_get_bool ("eglFilter", "ffxCAS");
  this->sharpness = option_get_float("eglFilter", "ffxCASSharpness");
}

static bool egl_filterFFXCASInit(EGL_Filter ** filter)
{
  EGL_FilterFFXCAS * this = calloc(1, sizeof(*this));
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
        b_shader_basic_vert  , b_shader_basic_vert_size,
        b_shader_ffx_cas_frag, b_shader_ffx_cas_frag_size)
     )
  {
    DEBUG_ERROR("Failed to compile the shader");
    goto error_shader;
  }

  this->consts = countedBufferNew(8 * sizeof(GLuint));
  if (!this->consts)
  {
    DEBUG_ERROR("Failed to allocate consts buffer");
    goto error_shader;
  }

  egl_shaderSetUniforms(this->shader, &(EGL_Uniform) {
    .type     = EGL_UNIFORM_TYPE_4UIV,
    .location = egl_shaderGetUniform(this->shader, "uConsts"),
    .v        = this->consts,
  }, 1);

  egl_filterFFXCASLoadState(&this->base);

  if (!egl_framebufferInit(&this->fb))
  {
    DEBUG_ERROR("Failed to initialize the framebuffer");
    goto error_consts;
  }

  glGenSamplers(1, &this->sampler);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->sampler, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

  *filter = &this->base;
  return true;

error_consts:
   countedBufferRelease(&this->consts);

error_shader:
  egl_shaderFree(&this->shader);

error_this:
  free(this);
  return false;
}

static void egl_filterFFXCASFree(EGL_Filter * filter)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  egl_shaderFree(&this->shader);
  countedBufferRelease(&this->consts);
  egl_framebufferFree(&this->fb);
  glDeleteSamplers(1, &this->sampler);
  free(this);
}

static bool egl_filterFFXCASImguiConfig(EGL_Filter * filter)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  bool  redraw       = false;
  bool  cas          = this->enable;
  float casSharpness = this->sharpness;

  igCheckbox("Enabled", &cas);
  if (cas != this->enable)
  {
    this->enable = cas;
    redraw = true;
  }

  igText("Sharpness:");
  igSameLine(0.0f, -1.0f);
  igPushItemWidth(igGetWindowWidth() - igGetCursorPosX() -
      igGetStyle()->WindowPadding.x);

  igSliderFloat("##casSharpness", &casSharpness, 0.0f, 1.0f, NULL, 0);
  casSharpness = util_clamp(casSharpness, 0.0f, 1.0f);
  if (igIsItemHovered(ImGuiHoveredFlags_None))
    igSetTooltip("Ctrl+Click to enter a value");
  igPopItemWidth();

  if (casSharpness != this->sharpness)
  {
    // enable CAS if the sharpness was changed
    if (!cas)
    {
      cas = true;
      this->enable = true;
    }

    this->sharpness = casSharpness;
    casUpdateConsts(this);
    redraw = true;
  }

  if (redraw)
    this->prepared = false;

  return redraw;
}

static bool egl_filterFFXCASSetup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  if (!this->enable)
    return false;

  if (pixFmt == this->pixFmt && this->width == width && this->height == height)
    return true;

  if (!egl_framebufferSetup(this->fb, pixFmt, width, height))
    return false;

  this->pixFmt   = pixFmt;
  this->width    = width;
  this->height   = height;
  this->prepared = false;
  casUpdateConsts(this);

  return true;
}

static void egl_filterFFXCASGetOutputRes(EGL_Filter * filter,
    unsigned int *width, unsigned int *height)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);
  *width  = this->width;
  *height = this->height;
}

static bool egl_filterFFXCASPrepare(EGL_Filter * filter)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  if (this->prepared)
    return true;

  this->prepared = true;

  return true;
}

static GLuint egl_filterFFXCASRun(EGL_Filter * filter,
    EGL_FilterRects * rects, GLuint texture)
{
  EGL_FilterFFXCAS * this = UPCAST(EGL_FilterFFXCAS, filter);

  egl_framebufferBind(this->fb);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glBindSampler(0, this->sampler);

  egl_shaderUse(this->shader);
  egl_filterRectsRender(this->shader, rects);

  return egl_framebufferGetTexture(this->fb);
}

EGL_FilterOps egl_filterFFXCASOps =
{
  .id           = "ffxCAS",
  .name         = "AMD FidelityFX CAS",
  .type         = EGL_FILTER_TYPE_EFFECT,
  .earlyInit    = egl_filterFFXCASEarlyInit,
  .init         = egl_filterFFXCASInit,
  .free         = egl_filterFFXCASFree,
  .imguiConfig  = egl_filterFFXCASImguiConfig,
  .saveState    = egl_filterFFXCASSaveState,
  .loadState    = egl_filterFFXCASLoadState,
  .setup        = egl_filterFFXCASSetup,
  .getOutputRes = egl_filterFFXCASGetOutputRes,
  .prepare      = egl_filterFFXCASPrepare,
  .run          = egl_filterFFXCASRun
};
