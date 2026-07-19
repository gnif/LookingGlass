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

#include "hdr_compose.h"

#include "framebuffer.h"
#include "model.h"
#include "shader.h"
#include "state.h"

#include "common/debug.h"

#include <stdlib.h>

#include "hdr_compose.vert.h"
#include "hdr_compose.frag.h"

struct EGL_HDRCompose
{
  EGL_Framebuffer * framebuffer;
  EGL_Shader      * shader;
  EGL_Model       * model;
  unsigned int      width;
  unsigned int      height;
  bool              configured;
  bool              active;
};

bool egl_hdrComposeInit(EGL_HDRCompose ** compose)
{
  *compose = calloc(1, sizeof(**compose));
  if (!*compose)
    return false;

  EGL_HDRCompose * this = *compose;
  if (!egl_framebufferInit(&this->framebuffer) ||
      !egl_shaderInit(&this->shader) ||
      !egl_shaderCompile(this->shader,
        b_shader_hdr_compose_vert, b_shader_hdr_compose_vert_size,
        b_shader_hdr_compose_frag, b_shader_hdr_compose_frag_size,
        false, NULL) ||
      !egl_modelInit(&this->model))
  {
    DEBUG_ERROR("Failed to initialize linear HDR composition");
    egl_hdrComposeFree(compose);
    return false;
  }

  egl_modelSetDefault(this->model, false);
  egl_modelSetShader(this->model, this->shader);
  egl_modelSetTexture(this->model,
      egl_framebufferGetTexture(this->framebuffer));

  /* Prove that an FP16 render target can actually be created before native
   * PQ support is advertised.  The real size is installed on the first
   * resize. */
  if (!egl_framebufferSetup(this->framebuffer, EGL_PF_RGBA16F, 1, 1))
  {
    DEBUG_ERROR("Failed to create the linear HDR composition target");
    egl_hdrComposeFree(compose);
    return false;
  }
  this->width      = 1;
  this->height     = 1;
  this->configured = true;

  EGL_Texture * texture = egl_framebufferGetTexture(this->framebuffer);
  glSamplerParameteri(texture->sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(texture->sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  return true;
}

void egl_hdrComposeFree(EGL_HDRCompose ** compose)
{
  if (!*compose)
    return;

  EGL_HDRCompose * this = *compose;
  egl_modelFree(&this->model);
  egl_shaderFree(&this->shader);
  egl_framebufferFree(&this->framebuffer);
  free(this);
  *compose = NULL;
}

bool egl_hdrComposeResize(EGL_HDRCompose * this,
    unsigned int width, unsigned int height)
{
  if (!this)
    return false;

  const bool wasActive = this->active;
  this->width      = width;
  this->height     = height;
  this->configured = false;
  this->active     = false;

  if (!width || !height)
    return true;

  this->configured = egl_framebufferSetup(this->framebuffer,
      EGL_PF_RGBA16F, width, height);
  this->active     = wasActive && this->configured;
  return this->configured;
}

void egl_hdrComposeSetActive(EGL_HDRCompose * this, bool active)
{
  if (!this)
    return;

  this->active = active && this->configured;
}

bool egl_hdrComposeIsConfigured(EGL_HDRCompose * this)
{
  return this && this->configured;
}

bool egl_hdrComposeBegin(EGL_HDRCompose * this)
{
  if (!this || !this->active)
    return false;

  egl_framebufferBind(this->framebuffer);
  return true;
}

void egl_hdrComposeEnd(EGL_HDRCompose * this,
    const struct Rect * damage, int damageCount)
{
  if (!this || !this->active)
    return;

  egl_stateBindFramebuffer(0);
  egl_stateViewport(0, 0, this->width, this->height);
  egl_stateBlend(false);

  if (damageCount <= 0)
  {
    egl_stateScissor(false);
    egl_modelRender(this->model);
    return;
  }

  egl_stateScissor(true);
  for (int i = 0; i < damageCount; ++i)
  {
    glScissor(damage[i].x, damage[i].y,
        damage[i].w, damage[i].h);
    egl_modelRender(this->model);
  }
  egl_stateScissor(false);
}

EGL_Framebuffer * egl_hdrComposeGetFramebuffer(EGL_HDRCompose * this)
{
  return this && this->active ? this->framebuffer : NULL;
}
