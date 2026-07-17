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

#include "effect.h"

#include "filter.h"
#include "framebuffer.h"
#include "texture.h"

#include "common/debug.h"

#include <stdlib.h>

struct EGL_EffectPass
{
  EGL_Shader * shader;
  EGL_Framebuffer * framebuffer;
  GLuint sampler;

  EGL_Uniform * uTransform;
  EGL_Uniform * uDesktopSize;

  GLenum minFilter;
  GLenum magFilter;
  GLenum wrapS;
  GLenum wrapT;

  bool enabled;
  bool configured;
  enum EGL_PixelFormat pixFmt;
  unsigned int width;
  unsigned int height;

  struct EGL_EffectPass * next;
};

struct EGL_Effect
{
  EGL_EffectPass * passes;
  EGL_EffectPass * passesTail;
};

bool egl_effectInit(EGL_Effect ** effect)
{
  *effect = calloc(1, sizeof(**effect));
  if (!*effect)
  {
    DEBUG_ERROR("Failed to allocate EGL effect");
    return false;
  }

  return true;
}

void egl_effectFree(EGL_Effect ** effect)
{
  if (!*effect)
    return;

  EGL_EffectPass * pass = (*effect)->passes;
  while(pass)
  {
    EGL_EffectPass * next = pass->next;
    glDeleteSamplers(1, &pass->sampler);
    egl_framebufferFree(&pass->framebuffer);
    free(pass);
    pass = next;
  }

  free(*effect);
  *effect = NULL;
}

void egl_effectPassSetShader(EGL_EffectPass * pass, EGL_Shader * shader)
{
  if (pass->shader == shader)
    return;

  pass->shader = shader;
  if (!shader)
  {
    pass->uTransform = NULL;
    pass->uDesktopSize = NULL;
    return;
  }

  pass->uTransform   = egl_shaderGetUniform(shader, "transform");
  pass->uDesktopSize = egl_shaderGetUniform(shader, "desktopSize");
  egl_shaderAssocTextures(shader, 1);
}

bool egl_effectAddPass(EGL_Effect * effect, EGL_Shader * shader,
    EGL_EffectPass ** result)
{
  EGL_EffectPass * pass = calloc(1, sizeof(*pass));
  if (!pass)
  {
    DEBUG_ERROR("Failed to allocate EGL effect pass");
    return false;
  }

  if (!egl_framebufferInit(&pass->framebuffer))
  {
    free(pass);
    return false;
  }

  glGenSamplers(1, &pass->sampler);
  glSamplerParameteri(pass->sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glSamplerParameteri(pass->sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glSamplerParameteri(pass->sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(pass->sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  pass->minFilter = GL_LINEAR;
  pass->magFilter = GL_LINEAR;
  pass->wrapS = GL_CLAMP_TO_EDGE;
  pass->wrapT = GL_CLAMP_TO_EDGE;

  pass->enabled = true;
  egl_effectPassSetShader(pass, shader);

  if (effect->passesTail)
    effect->passesTail->next = pass;
  else
    effect->passes = pass;
  effect->passesTail = pass;

  if (result)
    *result = pass;
  return true;
}

void egl_effectPassSetEnabled(EGL_EffectPass * pass, bool enabled)
{
  pass->enabled = enabled;
}

void egl_effectPassSetFilter(EGL_EffectPass * pass, GLenum minFilter,
    GLenum magFilter)
{
  if (pass->minFilter != minFilter)
  {
    glSamplerParameteri(pass->sampler, GL_TEXTURE_MIN_FILTER, minFilter);
    pass->minFilter = minFilter;
  }

  if (pass->magFilter != magFilter)
  {
    glSamplerParameteri(pass->sampler, GL_TEXTURE_MAG_FILTER, magFilter);
    pass->magFilter = magFilter;
  }
}

void egl_effectPassSetWrap(EGL_EffectPass * pass, GLenum wrapS, GLenum wrapT)
{
  if (pass->wrapS != wrapS)
  {
    glSamplerParameteri(pass->sampler, GL_TEXTURE_WRAP_S, wrapS);
    pass->wrapS = wrapS;
  }

  if (pass->wrapT != wrapT)
  {
    glSamplerParameteri(pass->sampler, GL_TEXTURE_WRAP_T, wrapT);
    pass->wrapT = wrapT;
  }
}

bool egl_effectPassSetup(EGL_EffectPass * pass, enum EGL_PixelFormat pixFmt,
    unsigned int width, unsigned int height)
{
  if (pass->configured && pass->pixFmt == pixFmt &&
      pass->width == width && pass->height == height)
    return true;

  if (!egl_framebufferSetup(pass->framebuffer, pixFmt, width, height))
    return false;

  pass->pixFmt = pixFmt;
  pass->width = width;
  pass->height = height;
  pass->configured = true;
  return true;
}

EGL_Texture * egl_effectRun(EGL_Effect * effect, EGL_FilterRects * rects,
    EGL_Texture * texture)
{
  for(EGL_EffectPass * pass = effect->passes; pass; pass = pass->next)
  {
    if (!pass->enabled)
      continue;

    if (!pass->configured || !pass->shader)
    {
      DEBUG_ERROR("Attempted to run an unconfigured EGL effect pass");
      return NULL;
    }

    egl_framebufferBind(pass->framebuffer);

    glActiveTexture(GL_TEXTURE0);
    egl_textureBind(texture);
    glBindSampler(0, pass->sampler);

    egl_uniformMatrix3x2fv(pass->uTransform, 1, GL_FALSE, rects->matrix);
    egl_uniform2f(pass->uDesktopSize, rects->width, rects->height);
    egl_shaderUse(pass->shader);
    egl_desktopRectsRender(rects->rects);

    texture = egl_framebufferGetTexture(pass->framebuffer);
  }

  return texture;
}
