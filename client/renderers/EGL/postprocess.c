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

#include "postprocess.h"
#include "filters.h"
#include "app.h"
#include "cimgui.h"

#include <stdatomic.h>

#include "common/debug.h"
#include "common/array.h"

#include "ll.h"

static const EGL_FilterOps * EGL_Filters[] =
{
  &egl_filterDownscaleOps,
  &egl_filterFFXFSR1Ops,
  &egl_filterFFXCASOps
};

struct EGL_PostProcess
{
  struct ll * filters;
  GLuint output;
  unsigned int outputX, outputY;
  _Atomic(bool) modified;

  EGL_Model * model;
};

void egl_postProcessEarlyInit(void)
{
  for(int i = 0; i < ARRAY_LENGTH(EGL_Filters); ++i)
    EGL_Filters[i]->earlyInit();
}

static void configUI(void * opaque, int * id)
{
  struct EGL_PostProcess * this = opaque;

  bool redraw = false;
  EGL_Filter * filter;
  for(ll_reset(this->filters); ll_walk(this->filters, (void **)&filter); )
  {
    igPushIDInt(++*id);
    if (igCollapsingHeaderBoolPtr(filter->ops.name, NULL, 0))
      redraw |= egl_filterImguiConfig(filter);
    igPopID();
  }

  if (redraw)
  {
    atomic_store(&this->modified, true);
    app_invalidateWindow(false);
  }
}

bool egl_postProcessInit(EGL_PostProcess ** pp)
{
  EGL_PostProcess * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  this->filters = ll_new(sizeof(EGL_Filter *));
  if (!this->filters)
  {
    DEBUG_ERROR("Failed to allocate the filter list");
    goto error_this;
  }

  if (!egl_modelInit(&this->model))
  {
    DEBUG_ERROR("Failed to initialize the model");
    goto error_filters;
  }
  egl_modelSetDefault(this->model, false);

  app_overlayConfigRegisterTab("EGL Filters", configUI, this);

  *pp = this;
  return true;

error_filters:
  ll_free(this->filters);

error_this:
  free(this);
  return false;
}

void egl_postProcessFree(EGL_PostProcess ** pp)
{
  if (!*pp)
    return;

  EGL_PostProcess * this = *pp;

  if (this->filters)
  {
    EGL_Filter * filter;
    while(ll_shift(this->filters, (void **)&filter))
      egl_filterFree(&filter);
    ll_free(this->filters);
  }

  egl_modelFree(&this->model);
  free(this);
  *pp = NULL;
}

bool egl_postProcessAdd(EGL_PostProcess * this, const EGL_FilterOps * ops)
{
  EGL_Filter * filter;
  if (!egl_filterInit(ops, &filter))
    return false;

  ll_push(this->filters, filter);
  return true;
}

bool egl_postProcessConfigModified(EGL_PostProcess * this)
{
  return atomic_load(&this->modified);
}

bool egl_postProcessRun(EGL_PostProcess * this, EGL_Texture * tex,
    unsigned int targetX, unsigned int targetY)
{
  EGL_Filter * lastFilter = NULL, * filter;
  unsigned int sizeX, sizeY;

  GLuint texture;
  if (egl_textureGet(tex, &texture, &sizeX, &sizeY) != EGL_TEX_STATUS_OK)
    return false;

  atomic_store(&this->modified, false);
  for(ll_reset(this->filters); ll_walk(this->filters, (void **)&filter); )
  {
    egl_filterSetOutputResHint(filter, targetX, targetY);
    egl_filterSetup(filter, tex->format.pixFmt, sizeX, sizeY);

    if (!egl_filterPrepare(filter))
      continue;

    texture = egl_filterRun(filter, this->model, texture);
    egl_filterGetOutputRes(filter, &sizeX, &sizeY);

    if (lastFilter)
      egl_filterRelease(lastFilter);

    lastFilter = filter;
  }

  this->output  = texture;
  this->outputX = sizeX;
  this->outputY = sizeY;
  return true;
}

GLuint egl_postProcessGetOutput(EGL_PostProcess * this,
    unsigned int * outputX, unsigned int * outputY)
{
  *outputX = this->outputX;
  *outputY = this->outputY;
  return this->output;
}
