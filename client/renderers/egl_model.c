/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "egl_model.h"
#include "egl_shader.h"
#include "debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL2/SDL_egl.h>

struct EGL_Model
{
  bool    hasVertexBuffer;
  GLuint  vertexBuffer;
  GLsizei vertexCount;

  bool   hasUVBuffer;
  GLuint uvBuffer;

  EGL_Shader * shader;

  bool   hasTexture;
  GLuint texture;

  bool   hasPBO, pboUpdate;
  GLuint pbo[2];
  int    pboIndex;
  size_t pboWidth, pboHeight;
};

bool egl_model_init(EGL_Model ** model)
{
  *model = (EGL_Model *)malloc(sizeof(EGL_Model));
  if (!*model)
  {
    DEBUG_ERROR("Failed to malloc EGL_Model");
    return false;
  }

  memset(*model, 0, sizeof(EGL_Model));
  return true;
}

void egl_model_free(EGL_Model ** model)
{
  if (!*model)
    return;

  if ((*model)->hasVertexBuffer)
    glDeleteBuffers(1, &(*model)->vertexBuffer);

  if ((*model)->hasUVBuffer)
    glDeleteBuffers(1, &(*model)->uvBuffer);

  if ((*model)->hasTexture)
    glDeleteTextures(1, &(*model)->texture);

  if ((*model)->hasPBO)
    glDeleteBuffers(2, (*model)->pbo);

  free(*model);
  *model = NULL;
}

bool egl_model_init_streaming(EGL_Model * model, size_t width, size_t height, size_t bufferSize)
{
  model->pboWidth  = width;
  model->pboHeight = height;

  glBindTexture(GL_TEXTURE_2D, egl_model_get_texture_id(model));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  if (!model->hasPBO)
    glGenBuffers(2, model->pbo);

  for(int i = 0; i < 2; ++i)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, model->pbo[i]);
    glBufferData(
      GL_PIXEL_UNPACK_BUFFER,
      bufferSize,
      NULL,
      GL_STREAM_DRAW
    );
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  model->hasPBO = true;
  return true;
}

bool egl_model_is_streaming(EGL_Model * model)
{
  return model->hasPBO;
}

bool egl_model_stream_buffer(EGL_Model * model, const uint8_t * buffer, size_t bufferSize)
{
  if (++model->pboIndex == 2)
    model->pboIndex = 0;

  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, model->pbo[model->pboIndex]);
  glBufferData(GL_PIXEL_UNPACK_BUFFER, bufferSize, 0, GL_STREAM_DRAW);
  GLubyte * ptr = glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
  if (!ptr)
  {
    DEBUG_ERROR("Failed to map the buffer");
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return false;
  }

  memcpy(ptr, buffer, bufferSize);

  glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
  glBindTexture(GL_TEXTURE_2D, 0);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  model->pboUpdate = true;
  return true;
}

void egl_model_set_verticies(EGL_Model * model, const GLfloat * verticies, const size_t count)
{
  if (model->hasVertexBuffer)
    glDeleteBuffers(1, &model->vertexBuffer);

  glGenBuffers(1, &model->vertexBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, model->vertexBuffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * count, verticies, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  model->hasVertexBuffer = true;
  model->vertexCount     = count / 3;
}

void egl_model_set_uvs(EGL_Model * model, const GLfloat * uvs, const size_t count)
{
  if (model->hasUVBuffer)
    glDeleteBuffers(1, &model->uvBuffer);

  glGenBuffers(1, &model->uvBuffer);
  glBindBuffer(GL_ARRAY_BUFFER, model->uvBuffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * count, uvs, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  model->hasUVBuffer = true;
}

void egl_model_render(EGL_Model * model)
{
  if (!model->hasVertexBuffer)
  {
    DEBUG_ERROR("Model has no verticies");
    return;
  }

  if (model->shader)
    egl_shader_use(model->shader);

  GLuint location = 0;
  glEnableVertexAttribArray(location);
  glBindBuffer(GL_ARRAY_BUFFER, model->vertexBuffer);
  glVertexAttribPointer(location, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);

  if (model->hasUVBuffer)
  {
    ++location;
    glEnableVertexAttribArray(location);
    glBindBuffer(GL_ARRAY_BUFFER, model->uvBuffer);
    glVertexAttribPointer(location, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
  }

  if (model->hasTexture)
    glBindTexture(GL_TEXTURE_2D, model->texture);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, model->vertexCount);

  if (model->hasTexture)
    glBindTexture(GL_TEXTURE_2D, 0);

  while(location > 0)
    glDisableVertexAttribArray(location--);
  glDisableVertexAttribArray(0);

  if (model->shader)
    glUseProgram(0);

  if (model->pboUpdate)
  {
    glBindTexture(GL_TEXTURE_2D, egl_model_get_texture_id(model));
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, model->pbo[model->pboIndex]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, model->pboWidth, model->pboHeight, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    model->pboUpdate = false;
  }
}

void egl_model_set_shader(EGL_Model * model, EGL_Shader * shader)
{
  model->shader = shader;
}

GLuint egl_model_get_texture_id(EGL_Model * model)
{
  if (model->hasTexture)
    return model->texture;

  glGenTextures(1, &model->texture);
  model->hasTexture = true;

  return model->texture;
}