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

#include "model.h"
#include "shader.h"
#include "texture.h"

#include "common/debug.h"
#include "ll.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct EGL_Model
{
  bool        rebuild;
  struct ll * verticies;
  size_t      vertexCount;
  bool        finish;

  GLuint  buffer;
  GLuint  vao;

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

bool egl_modelInit(EGL_Model ** model)
{
  *model = malloc(sizeof(**model));
  if (!*model)
  {
    DEBUG_ERROR("Failed to malloc EGL_Model");
    return false;
  }

  memset(*model, 0, sizeof(**model));

  (*model)->verticies = ll_new();

  return true;
}

void egl_modelFree(EGL_Model ** model)
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

  if ((*model)->buffer)
    glDeleteBuffers(1, &(*model)->buffer);

  if ((*model)->vao)
    glDeleteVertexArrays(1, &(*model)->vao);

  free(*model);
  *model = NULL;
}

void egl_modelSetDefault(EGL_Model * model, bool flipped)
{
  static const GLfloat square[] =
  {
    -1.0f, -1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f, 0.0f
  };

  static const GLfloat uvsNormal[] =
  {
    0.0f, 0.0f,
    1.0f, 0.0f,
    0.0f, 1.0f,
    1.0f, 1.0f
  };

  static const GLfloat uvsFlipped[] =
  {
    0.0f, 1.0f,
    1.0f, 1.0f,
    0.0f, 0.0f,
    1.0f, 0.0f
  };

  egl_modelAddVerts(model, square, flipped ? uvsFlipped : uvsNormal, 4);
}

void egl_modelAddVerts(EGL_Model * model, const GLfloat * verticies, const GLfloat * uvs, const size_t count)
{
  struct FloatList * fl = malloc(sizeof(*fl));

  fl->count = count;
  fl->v     = malloc(sizeof(GLfloat) * count * 3);
  fl->u     = malloc(sizeof(GLfloat) * count * 2);
  memcpy(fl->v, verticies, sizeof(GLfloat) * count * 3);

  if (uvs)
    memcpy(fl->u, uvs, sizeof(GLfloat) * count * 2);
  else
    memset(fl->u, 0  , sizeof(GLfloat) * count * 2);

  ll_push(model->verticies, fl);
  model->rebuild      = true;
  model->vertexCount += count;
}

void egl_modelRender(EGL_Model * model)
{
  if (!model->vertexCount)
    return;

  if (model->rebuild)
  {
    if (model->buffer)
      glDeleteBuffers(1, &model->buffer);

    if (!model->vao)
      glGenVertexArrays(1, &model->vao);

    glBindVertexArray(model->vao);

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

    /* set up vertex arrays in the VAO */
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, (void*)(sizeof(GLfloat) * model->vertexCount * 3));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    model->rebuild = false;
  }

  glBindVertexArray(model->vao);

  if (model->shader)
    egl_shaderUse(model->shader);

  if (model->texture)
    egl_textureBind(model->texture);

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
  glBindVertexArray(0);
  glUseProgram(0);
}

void egl_modelSetShader(EGL_Model * model, EGL_Shader * shader)
{
  model->shader = shader;
  update_uniform_bindings(model);
}

void egl_modelSetTexture(EGL_Model * model, EGL_Texture * texture)
{
  model->texture = texture;
  update_uniform_bindings(model);
}

void update_uniform_bindings(EGL_Model * model)
{
  if (!model->shader || !model->texture)
    return;

  egl_shaderAssocTextures(model->shader, 1);
}
