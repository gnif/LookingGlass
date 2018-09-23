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
#include "egl_texture.h"

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

  EGL_Shader  * shader;
  EGL_Texture * texture;
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

  free(*model);
  *model = NULL;
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

  if (model->texture)
    egl_texture_bind(model->texture);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, model->vertexCount);
  glBindTexture(GL_TEXTURE_2D, 0);

  while(location > 0)
    glDisableVertexAttribArray(location--);
  glDisableVertexAttribArray(0);

  glUseProgram(0);
}

void egl_model_set_shader(EGL_Model * model, EGL_Shader * shader)
{
  model->shader = shader;
}

void egl_model_set_texture(EGL_Model * model, EGL_Texture * texture)
{
  model->texture = texture;
}