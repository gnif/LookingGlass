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

#pragma once

#include <stdbool.h>

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#define EGL_STATE_TEXTURE_UNITS 16

/*
 * GL bindings are context-local and the renderer keeps each context on its
 * own thread, so the cache is thread-local too. Code that changes tracked GL
 * state without these helpers must invalidate the cache afterwards.
 */

typedef struct EGL_StateBinding
{
  GLuint value;
  bool valid;
}
EGL_StateBinding;

typedef struct EGL_StateTextureUnit
{
  EGL_StateBinding texture2D;
  EGL_StateBinding textureExternal;
  EGL_StateBinding sampler;
}
EGL_StateTextureUnit;

typedef struct EGL_StateCache
{
  EGL_StateBinding program;
  EGL_StateBinding framebuffer;
  EGL_StateBinding vertexArray;
  EGL_StateBinding arrayBuffer;
  EGL_StateBinding pixelUnpackBuffer;
  EGL_StateBinding activeTexture;
  EGL_StateTextureUnit textureUnit[EGL_STATE_TEXTURE_UNITS];

  bool viewportValid;
  GLint viewportX;
  GLint viewportY;
  GLsizei viewportWidth;
  GLsizei viewportHeight;

  bool blendValid;
  bool blend;
  bool blendFuncValid;
  GLenum blendSrc;
  GLenum blendDst;

  bool scissorValid;
  bool scissor;
}
EGL_StateCache;

extern _Thread_local EGL_StateCache g_eglState;

/* Invalidates only the current context/thread cache. */
void egl_stateInvalidate(void);

/* Invalidates all context caches at their next synchronization point. */
void egl_stateInvalidateShared(void);
void egl_stateCheckShared(void);

static inline void egl_stateUseProgram(GLuint program)
{
  if (g_eglState.program.valid && g_eglState.program.value == program)
    return;

  glUseProgram(program);
  g_eglState.program.value = program;
  g_eglState.program.valid = true;
}

static inline void egl_stateBindFramebuffer(GLuint framebuffer)
{
  if (g_eglState.framebuffer.valid &&
      g_eglState.framebuffer.value == framebuffer)
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  g_eglState.framebuffer.value = framebuffer;
  g_eglState.framebuffer.valid = true;
}

static inline void egl_stateBindVertexArray(GLuint vertexArray)
{
  if (g_eglState.vertexArray.valid &&
      g_eglState.vertexArray.value == vertexArray)
    return;

  glBindVertexArray(vertexArray);
  g_eglState.vertexArray.value = vertexArray;
  g_eglState.vertexArray.valid = true;
}

static inline void egl_stateBindBuffer(GLenum target, GLuint buffer)
{
  EGL_StateBinding * binding;
  switch(target)
  {
    case GL_ARRAY_BUFFER:
      binding = &g_eglState.arrayBuffer;
      break;

    case GL_PIXEL_UNPACK_BUFFER:
      binding = &g_eglState.pixelUnpackBuffer;
      break;

    /* GL_ELEMENT_ARRAY_BUFFER belongs to the currently bound VAO. */
    default:
      glBindBuffer(target, buffer);
      return;
  }

  if (binding->valid && binding->value == buffer)
    return;

  glBindBuffer(target, buffer);
  binding->value = buffer;
  binding->valid = true;
}

static inline bool egl_stateActiveTextureUnit(GLuint unit)
{
  const GLenum texture = GL_TEXTURE0 + unit;
  if (!g_eglState.activeTexture.valid ||
      g_eglState.activeTexture.value != texture)
  {
    glActiveTexture(texture);
    g_eglState.activeTexture.value = texture;
    g_eglState.activeTexture.valid = true;
  }

  return unit < EGL_STATE_TEXTURE_UNITS;
}

static inline void egl_stateBindTexture(GLuint unit, GLenum target,
    GLuint texture)
{
  if (!egl_stateActiveTextureUnit(unit))
  {
    glBindTexture(target, texture);
    return;
  }

  EGL_StateBinding * binding;
  switch(target)
  {
    case GL_TEXTURE_2D:
      binding = &g_eglState.textureUnit[unit].texture2D;
      break;

    case GL_TEXTURE_EXTERNAL_OES:
      binding = &g_eglState.textureUnit[unit].textureExternal;
      break;

    default:
      glBindTexture(target, texture);
      return;
  }

  if (binding->valid && binding->value == texture)
    return;

  glBindTexture(target, texture);
  binding->value = texture;
  binding->valid = true;
}

static inline void egl_stateBindSampler(GLuint unit, GLuint sampler)
{
  if (unit >= EGL_STATE_TEXTURE_UNITS)
  {
    glBindSampler(unit, sampler);
    return;
  }

  EGL_StateBinding * binding = &g_eglState.textureUnit[unit].sampler;
  if (binding->valid && binding->value == sampler)
    return;

  glBindSampler(unit, sampler);
  binding->value = sampler;
  binding->valid = true;
}

static inline void egl_stateViewport(GLint x, GLint y, GLsizei width,
    GLsizei height)
{
  if (g_eglState.viewportValid &&
      g_eglState.viewportX      == x      &&
      g_eglState.viewportY      == y      &&
      g_eglState.viewportWidth  == width  &&
      g_eglState.viewportHeight == height)
    return;

  glViewport(x, y, width, height);
  g_eglState.viewportX = x;
  g_eglState.viewportY = y;
  g_eglState.viewportWidth = width;
  g_eglState.viewportHeight = height;
  g_eglState.viewportValid = true;
}

static inline void egl_stateBlend(bool enabled)
{
  if (g_eglState.blendValid && g_eglState.blend == enabled)
    return;

  if (enabled)
    glEnable(GL_BLEND);
  else
    glDisable(GL_BLEND);

  g_eglState.blend = enabled;
  g_eglState.blendValid = true;
}

static inline void egl_stateBlendFunc(GLenum src, GLenum dst)
{
  if (g_eglState.blendFuncValid &&
      g_eglState.blendSrc == src && g_eglState.blendDst == dst)
    return;

  glBlendFunc(src, dst);
  g_eglState.blendSrc = src;
  g_eglState.blendDst = dst;
  g_eglState.blendFuncValid = true;
}

static inline void egl_stateScissor(bool enabled)
{
  if (g_eglState.scissorValid && g_eglState.scissor == enabled)
    return;

  if (enabled)
    glEnable(GL_SCISSOR_TEST);
  else
    glDisable(GL_SCISSOR_TEST);

  g_eglState.scissor = enabled;
  g_eglState.scissorValid = true;
}
