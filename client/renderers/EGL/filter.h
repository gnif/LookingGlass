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

#pragma once

#include "util.h"
#include "shader.h"
#include "egltypes.h"
#include "desktop_rects.h"
#include "model.h"

#include <string.h>

typedef struct EGL_FilterRects
{
  EGL_DesktopRects * rects;
  GLfloat * matrix;
  int width, height;
}
EGL_FilterRects;

typedef struct EGL_Filter EGL_Filter;

typedef struct EGL_FilterOps
{
  /* the identifier of this filter */
  const char * id;

  /* the friendly name of this filter */
  const char * name;

  /* the type of this filter */
  EGL_FilterType type;

  /* early initialization for registration of options */
  void (*earlyInit)(void);

  /* initialize the filter */
  bool (*init)(EGL_Filter ** filter);

  /* free the filter */
  void (*free)(EGL_Filter * filter);

  /* render any imgui config
   * Returns true if a redraw is required */
  bool (*imguiConfig)(EGL_Filter * filter);

  /* writes filter state to options */
  void (*saveState)(EGL_Filter * filter);

  /* reads filter state from options */
  void (*loadState)(EGL_Filter * filter);

  /* set the input format of the filter */
  bool (*setup)(EGL_Filter * filter, enum EGL_PixelFormat pixFmt,
      unsigned int width, unsigned int height);

  /* set the output resolution hint for the filter
   * this is optional and only a hint */
  void (*setOutputResHint)(EGL_Filter * filter,
      unsigned int x, unsigned int y);

  /* returns the output resolution of the filter */
  void (*getOutputRes)(EGL_Filter * filter,
      unsigned int *x, unsigned int *y);

  /* prepare the shader for use
   * A filter can return false to bypass it */
  bool (*prepare)(EGL_Filter * filter);

  /* runs the filter on the provided texture
   * returns the processed texture as the output */
  GLuint (*run)(EGL_Filter * filter, EGL_FilterRects * rects,
      GLuint texture);

  /* called when the filter output is no loger needed so it can release memory
   * this is optional */
  void (*release)(EGL_Filter * filter);
}
EGL_FilterOps;

typedef struct EGL_Filter
{
  EGL_FilterOps ops;
}
EGL_Filter;

static inline bool egl_filterInit(const EGL_FilterOps * ops, EGL_Filter ** filter)
{
  if (!ops->init(filter))
    return false;

  memcpy(&(*filter)->ops, ops, sizeof(*ops));
  return true;
}

static inline void egl_filterFree(EGL_Filter ** filter)
{
  (*filter)->ops.free(*filter);
  *filter = NULL;
}

static inline bool egl_filterImguiConfig(EGL_Filter * filter)
{
  return filter->ops.imguiConfig(filter);
}

static inline void egl_filterSaveState(EGL_Filter * filter)
{
  filter->ops.saveState(filter);
}

static inline void egl_filterLoadState(EGL_Filter * filter)
{
  filter->ops.loadState(filter);
}

static inline bool egl_filterSetup(EGL_Filter * filter,
    enum EGL_PixelFormat pixFmt, unsigned int width, unsigned int height)
{
  return filter->ops.setup(filter, pixFmt, width, height);
}

static inline void egl_filterSetOutputResHint(EGL_Filter * filter,
    unsigned int x, unsigned int y)
{
  if (filter->ops.setOutputResHint)
    filter->ops.setOutputResHint(filter, x, y);
}

static inline void egl_filterGetOutputRes(EGL_Filter * filter,
    unsigned int *x, unsigned int *y)
{
  return filter->ops.getOutputRes(filter, x, y);
}

static inline bool egl_filterPrepare(EGL_Filter * filter)
{
  return filter->ops.prepare(filter);
}

static inline GLuint egl_filterRun(EGL_Filter * filter,
    EGL_FilterRects * rects, GLuint texture)
{
  return filter->ops.run(filter, rects, texture);
}

static inline void egl_filterRelease(EGL_Filter * filter)
{
  if (filter->ops.release)
    filter->ops.release(filter);
}

void egl_filterRectsRender(EGL_Shader * shader, EGL_FilterRects * rects);
