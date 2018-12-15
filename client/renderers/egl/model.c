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

#include "model.h"
#include "shader.h"
#include "texture.h"

#include "debug.h"
#include "utils.h"
#include "ll.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <SDL2/SDL_egl.h>

struct EGL_Model
{
  bool        rebuild;
  struct ll * verticies;
  size_t      vertexCount;
  bool        finish;

  bool    hasBuffer;
  GLuint  buffer;

  EGL_Shader  * shader;
  EGL_Texture * texture;
};

struct FloatList
{
  GLfloat * v;
  GLfloat * u;
  size_t    count;
};

void update_uniform_bindings(EGL_Model * model);

bool egl_model_init(EGL_Model ** model)
{
  *model = (EGL_Model *)malloc(sizeof(EGL_Model));
  if (!*model)
  {
    DEBUG_ERROR("Failed to malloc EGL_Model");
    return false;
  }

  memset(*model, 0, sizeof(EGL_Model));

  (*model)->verticies = ll_new();

  return true;
}

void egl_model_free(EGL_Model ** model)
{
  if (!*model)
    return;

  struct FloatList * fl;
  while(ll_shift((*model)->verticies, (void **)&fl))
  {
    free(fl->u);
    free(fl->v);
    free(fl);
  }
  ll_free((*model)->verticies);

  if ((*model)->hasBuffer)
    glDeleteBuffers(1, &(*model)->buffer);

  free(*model);
  *model = NULL;
}

void egl_model_set_default(EGL_Model * model)
{
  static const GLfloat square[] =
  {
    -1.0f, -1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f, 0.0f
  };

  static const GLfloat uvs[] =
  {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
  };

  egl_model_add_verticies(model, square, uvs, 4);
}

void egl_model_add_verticies(EGL_Model * model, const GLfloat * verticies, const GLfloat * uvs, const size_t count)
{
  struct FloatList * fl = (struct FloatList *)malloc(sizeof(struct FloatList));

  fl->count = count;
  fl->v     = (GLfloat *)malloc(sizeof(GLfloat) * count * 3);
  fl->u     = (GLfloat *)malloc(sizeof(GLfloat) * count * 2);
  memcpy(fl->v, verticies, sizeof(GLfloat) * count * 3);

  if (uvs)
    memcpy(fl->u, uvs, sizeof(GLfloat) * count * 2);
  else
    memset(fl->u, 0  , sizeof(GLfloat) * count * 2);

  ll_push(model->verticies, fl);
  model->rebuild      = true;
  model->vertexCount += count;
}

void egl_model_render(EGL_Model * model)
{
  if (!model->vertexCount)
    return;

  if (model->rebuild)
  {
    if (model->hasBuffer)
      glDeleteBuffers(1, &model->buffer);

    /* create a buffer large enough */
    glGenBuffers(1, &model->buffer);
    glBindBuffer(GL_ARRAY_BUFFER, model->buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * (model->vertexCount * 5), NULL, GL_STATIC_DRAW);

    GLintptr offset = 0;

    /* buffer the verticies */
    struct FloatList * fl;
    for(ll_reset(model->verticies); ll_walk(model->verticies, (void **)&fl);)
    {
      glBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(GLfloat) * fl->count * 3, fl->v);
      offset += sizeof(GLfloat) * fl->count * 3;
    }

    /* buffer the uvs */
    for(ll_reset(model->verticies); ll_walk(model->verticies, (void **)&fl);)
    {
      glBufferSubData(GL_ARRAY_BUFFER, offset, sizeof(GLfloat) * fl->count * 2, fl->u);
      offset += sizeof(GLfloat) * fl->count * 2;
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    model->rebuild = false;
  }

  /* bind the model buffer and setup the pointers */
  glBindBuffer(GL_ARRAY_BUFFER, model->buffer);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void*)(sizeof(GLfloat) * model->vertexCount * 3));

  if (model->shader)
    egl_shader_use(model->shader);

  if (model->texture)
    egl_texture_bind(model->texture);

  /* draw the arrays */
  GLint offset = 0;
  struct FloatList * fl;
  for(ll_reset(model->verticies); ll_walk(model->verticies, (void **)&fl);)
  {
    glDrawArrays(GL_TRIANGLE_STRIP, offset, fl->count);
    offset += fl->count;
  }

  /* unbind and cleanup */
  glBindTexture(GL_TEXTURE_2D, 0);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);
  glUseProgram(0);
}

void egl_model_set_shader(EGL_Model * model, EGL_Shader * shader)
{
  model->shader = shader;
  update_uniform_bindings(model);
}

void egl_model_set_texture(EGL_Model * model, EGL_Texture * texture)
{
  model->texture = texture;
  update_uniform_bindings(model);
}

void update_uniform_bindings(EGL_Model * model)
{
  if (!model->shader || !model->texture)
    return;

  const int count = egl_texture_count(model->texture);
  egl_shader_associate_textures(model->shader, count);
}