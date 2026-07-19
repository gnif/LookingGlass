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

#include "filter.h"
#include "effect.h"

#include "common/debug.h"

#include "basic.vert.h"
#include "hdr_decode.frag.h"

#include <stdlib.h>

typedef struct EGL_FilterHDR
{
  EGL_Filter base;

  int          useDMA;
  unsigned int width;
  unsigned int height;
  bool         prepared;

  EGL_Shader     * shader;
  EGL_Effect     * effect;
  EGL_EffectPass * pass;
}
EGL_FilterHDR;

static bool egl_filterHDRInit(EGL_Filter ** filter)
{
  EGL_FilterHDR * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  this->useDMA = -1;

  if (!egl_shaderInit(&this->shader))
  {
    DEBUG_ERROR("Failed to initialize the shader");
    goto error_this;
  }

  if (!egl_effectInit(&this->effect))
  {
    DEBUG_ERROR("Failed to initialize the effect");
    goto error_shader;
  }

  if (!egl_effectAddPass(this->effect, this->shader, &this->pass))
  {
    DEBUG_ERROR("Failed to initialize the effect pass");
    goto error_effect;
  }

  egl_effectPassSetFilter(this->pass, GL_NEAREST, GL_NEAREST);
  *filter = &this->base;
  return true;

error_effect:
  egl_effectFree(&this->effect);

error_shader:
  egl_shaderFree(&this->shader);

error_this:
  free(this);
  return false;
}

static void egl_filterHDRFree(EGL_Filter * filter)
{
  EGL_FilterHDR * this = UPCAST(EGL_FilterHDR, filter);

  egl_effectFree(&this->effect);
  egl_shaderFree(&this->shader);
  free(this);
}

static bool egl_filterHDRSetup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height,
    unsigned int desktopWidth, unsigned int desktopHeight, bool useDMA)
{
  EGL_FilterHDR * this = UPCAST(EGL_FilterHDR, filter);
  (void)desktopWidth;
  (void)desktopHeight;

  if (pixFmt != EGL_PF_RGBA10)
    return false;

  if (this->useDMA != useDMA)
  {
    if (!egl_shaderCompile(this->shader,
          b_shader_basic_vert     , b_shader_basic_vert_size,
          b_shader_hdr_decode_frag, b_shader_hdr_decode_frag_size,
          useDMA, NULL))
    {
      DEBUG_ERROR("Failed to compile the HDR decode shader");
      return false;
    }

    this->useDMA   = useDMA;
    this->prepared = false;
  }

  if (this->prepared && this->width == width && this->height == height)
    return true;

  if (!egl_effectPassSetup(this->pass, EGL_PF_RGBA16F, width, height))
    return false;

  this->width    = width;
  this->height   = height;
  this->prepared = false;
  return true;
}

static void egl_filterHDRGetOutputRes(EGL_Filter * filter,
    unsigned int * width, unsigned int * height, EGL_PixelFormat * pixFmt)
{
  EGL_FilterHDR * this = UPCAST(EGL_FilterHDR, filter);
  *width  = this->width;
  *height = this->height;
  *pixFmt = EGL_PF_RGBA16F;
}

static bool egl_filterHDRPrepare(EGL_Filter * filter)
{
  EGL_FilterHDR * this = UPCAST(EGL_FilterHDR, filter);
  this->prepared = true;
  return true;
}

static EGL_Texture * egl_filterHDRRun(EGL_Filter * filter,
    EGL_FilterRects * rects, EGL_Texture * texture)
{
  EGL_FilterHDR * this = UPCAST(EGL_FilterHDR, filter);
  return egl_effectRun(this->effect, rects, texture);
}

EGL_FilterOps egl_filterHDRDecodeOps =
{
  .id           = "hdrDecode",
  .name         = "HDR Decode",
  .type         = EGL_FILTER_TYPE_INTERNAL,
  .init         = egl_filterHDRInit,
  .free         = egl_filterHDRFree,
  .setup        = egl_filterHDRSetup,
  .getOutputRes = egl_filterHDRGetOutputRes,
  .prepare      = egl_filterHDRPrepare,
  .run          = egl_filterHDRRun
};
