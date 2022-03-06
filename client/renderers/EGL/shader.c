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

#include "shader.h"
#include "common/debug.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct EGL_Shader
{
  bool   hasShader;
  GLuint shader;

  EGL_Uniform * uniforms;
  int           uniformCount;
  int           uniformUsed;
};

bool egl_shaderInit(EGL_Shader ** this)
{
  *this = calloc(1, sizeof(EGL_Shader));
  if (!*this)
  {
    DEBUG_ERROR("Failed to malloc EGL_Shader");
    return false;
  }

  return true;
}

void egl_shaderFree(EGL_Shader ** shader)
{
  EGL_Shader * this = *shader;
  if (!this)
    return;

  if (this->hasShader)
    glDeleteProgram(this->shader);

  egl_shaderFreeUniforms(this);
  free(this->uniforms);

  free(this);
  *shader = NULL;
}

bool egl_shaderLoad(EGL_Shader * this, const char * vertex_file, const char * fragment_file)
{
  char   * vertex_code, * fragment_code;
  size_t   vertex_size,   fragment_size;

  if (!util_fileGetContents(vertex_file, &vertex_code, &vertex_size))
  {
    DEBUG_ERROR("Failed to read vertex shader");
    return false;
  }

  DEBUG_INFO("Loaded vertex shader: %s", vertex_file);

  if (!util_fileGetContents(fragment_file, &fragment_code, &fragment_size))
  {
    DEBUG_ERROR("Failed to read fragment shader");
    free(vertex_code);
    return false;
  }

  DEBUG_INFO("Loaded fragment shader: %s", fragment_file);

  bool ret = egl_shaderCompile(this, vertex_code, vertex_size, fragment_code, fragment_size);
  free(vertex_code);
  free(fragment_code);
  return ret;
}

bool egl_shaderCompile(EGL_Shader * this, const char * vertex_code,
    size_t vertex_size, const char * fragment_code, size_t fragment_size)
{
  if (this->hasShader)
  {
    glDeleteProgram(this->shader);
    this->hasShader = false;
  }

  GLint  length;
  GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);

  length = vertex_size;
  glShaderSource(vertexShader, 1, (const char**)&vertex_code, &length);
  glCompileShader(vertexShader);

  GLint result = GL_FALSE;
  glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &result);
  if (result == GL_FALSE)
  {
    DEBUG_ERROR("Failed to compile vertex shader");

    int logLength;
    glGetShaderiv(vertexShader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0)
    {
      char *log = malloc(logLength + 1);
      if (!log)
        DEBUG_ERROR("out of memory");
      else
      {
        glGetShaderInfoLog(vertexShader, logLength, NULL, log);
        log[logLength] = 0;
        DEBUG_ERROR("%s", log);
        free(log);
      }
    }

    glDeleteShader(vertexShader);
    return false;
  }

  GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

  length = fragment_size;
  glShaderSource(fragmentShader, 1, (const char**)&fragment_code, &length);
  glCompileShader(fragmentShader);

  glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &result);
  if (result == GL_FALSE)
  {
    DEBUG_ERROR("Failed to compile fragment shader");

    int logLength;
    glGetShaderiv(fragmentShader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0)
    {
      char *log = malloc(logLength + 1);
      if (!log)
        DEBUG_ERROR("out of memory");
      else
      {
        glGetShaderInfoLog(fragmentShader, logLength, NULL, log);
        log[logLength] = 0;
        DEBUG_ERROR("%s", log);
        free(log);
      }
    }

    glDeleteShader(fragmentShader);
    glDeleteShader(vertexShader  );
    return false;
  }

  this->shader = glCreateProgram();
  glAttachShader(this->shader, vertexShader  );
  glAttachShader(this->shader, fragmentShader);
  glLinkProgram(this->shader);

  glGetProgramiv(this->shader, GL_LINK_STATUS, &result);
  if (result == GL_FALSE)
  {
    DEBUG_ERROR("Failed to link shader program");

    int logLength;
    glGetProgramiv(this->shader, GL_INFO_LOG_LENGTH, &logLength);
    if (logLength > 0)
    {
      char *log = malloc(logLength + 1);
      glGetProgramInfoLog(this->shader, logLength, NULL, log);
      log[logLength] = 0;
      DEBUG_ERROR("%s", log);
      free(log);
    }

    glDetachShader(this->shader, vertexShader  );
    glDetachShader(this->shader, fragmentShader);
    glDeleteShader(fragmentShader);
    glDeleteShader(vertexShader  );
    glDeleteProgram(this->shader );
    return false;
  }

  glDetachShader(this->shader, vertexShader  );
  glDetachShader(this->shader, fragmentShader);
  glDeleteShader(fragmentShader);
  glDeleteShader(vertexShader  );

  this->hasShader = true;
  return true;
}

void egl_shaderSetUniforms(EGL_Shader * this, EGL_Uniform * uniforms, int count)
{
  egl_shaderFreeUniforms(this);
  if (count > this->uniformCount)
  {
    free(this->uniforms);
    this->uniforms = malloc(sizeof(*this->uniforms) * count);
    if (!this->uniforms)
    {
      DEBUG_ERROR("out of memory");
      return;
    }

    this->uniformCount = count;
  }

  this->uniformUsed = count;
  memcpy(this->uniforms, uniforms, sizeof(*this->uniforms) * count);

  for(int i = 0; i < this->uniformUsed; ++i)
  {
    switch(this->uniforms[i].type)
    {
      case EGL_UNIFORM_TYPE_1FV:
      case EGL_UNIFORM_TYPE_2FV:
      case EGL_UNIFORM_TYPE_3FV:
      case EGL_UNIFORM_TYPE_4FV:
      case EGL_UNIFORM_TYPE_1IV:
      case EGL_UNIFORM_TYPE_2IV:
      case EGL_UNIFORM_TYPE_3IV:
      case EGL_UNIFORM_TYPE_4IV:
      case EGL_UNIFORM_TYPE_1UIV:
      case EGL_UNIFORM_TYPE_2UIV:
      case EGL_UNIFORM_TYPE_3UIV:
      case EGL_UNIFORM_TYPE_4UIV:
        countedBufferAddRef(this->uniforms[i].v);
        break;

      case EGL_UNIFORM_TYPE_M2FV:
      case EGL_UNIFORM_TYPE_M3FV:
      case EGL_UNIFORM_TYPE_M4FV:
      case EGL_UNIFORM_TYPE_M2x3FV:
      case EGL_UNIFORM_TYPE_M3x2FV:
      case EGL_UNIFORM_TYPE_M2x4FV:
      case EGL_UNIFORM_TYPE_M4x2FV:
      case EGL_UNIFORM_TYPE_M3x4FV:
      case EGL_UNIFORM_TYPE_M4x3FV:
        countedBufferAddRef(this->uniforms[i].m.v);
        break;

      default:
        break;
    }
  }
};

void egl_shaderFreeUniforms(EGL_Shader * this)
{
  for(int i = 0; i < this->uniformUsed; ++i)
  {
    switch(this->uniforms[i].type)
    {
      case EGL_UNIFORM_TYPE_1FV:
      case EGL_UNIFORM_TYPE_2FV:
      case EGL_UNIFORM_TYPE_3FV:
      case EGL_UNIFORM_TYPE_4FV:
      case EGL_UNIFORM_TYPE_1IV:
      case EGL_UNIFORM_TYPE_2IV:
      case EGL_UNIFORM_TYPE_3IV:
      case EGL_UNIFORM_TYPE_4IV:
      case EGL_UNIFORM_TYPE_1UIV:
      case EGL_UNIFORM_TYPE_2UIV:
      case EGL_UNIFORM_TYPE_3UIV:
      case EGL_UNIFORM_TYPE_4UIV:
        countedBufferRelease(&this->uniforms[i].v);
        break;

      case EGL_UNIFORM_TYPE_M2FV:
      case EGL_UNIFORM_TYPE_M3FV:
      case EGL_UNIFORM_TYPE_M4FV:
      case EGL_UNIFORM_TYPE_M2x3FV:
      case EGL_UNIFORM_TYPE_M3x2FV:
      case EGL_UNIFORM_TYPE_M2x4FV:
      case EGL_UNIFORM_TYPE_M4x2FV:
      case EGL_UNIFORM_TYPE_M3x4FV:
      case EGL_UNIFORM_TYPE_M4x3FV:
        countedBufferRelease(&this->uniforms[i].m.v);
        break;

      default:
        break;
    }
  }
  this->uniformUsed = 0;
}

void egl_shaderUse(EGL_Shader * this)
{
  if (this->hasShader)
    glUseProgram(this->shader);
  else
    DEBUG_ERROR("Shader program has not been compiled");

  for(int i = 0; i < this->uniformUsed; ++i)
  {
    EGL_Uniform * uniform = &this->uniforms[i];
    switch(uniform->type)
    {
      case EGL_UNIFORM_TYPE_1F:
        glUniform1f(uniform->location, uniform->f[0]);
        break;

      case EGL_UNIFORM_TYPE_2F:
        glUniform2f(uniform->location, uniform->f[0], uniform->f[1]);
        break;

      case EGL_UNIFORM_TYPE_3F:
        glUniform3f(uniform->location, uniform->f[0], uniform->f[1],
            uniform->f[2]);
        break;

      case EGL_UNIFORM_TYPE_4F:
        glUniform4f(uniform->location, uniform->f[0], uniform->f[1],
            uniform->f[2], uniform->f[3]);
        break;

      case EGL_UNIFORM_TYPE_1I:
        glUniform1i(uniform->location, uniform->i[0]);
        break;

      case EGL_UNIFORM_TYPE_2I:
        glUniform2i(uniform->location, uniform->i[0], uniform->i[1]);
        break;

      case EGL_UNIFORM_TYPE_3I:
        glUniform3i(uniform->location, uniform->i[0], uniform->i[1],
            uniform->i[2]);
        break;

      case EGL_UNIFORM_TYPE_4I:
        glUniform4i(uniform->location, uniform->i[0], uniform->i[1],
            uniform->i[2], uniform->i[3]);
        break;

      case EGL_UNIFORM_TYPE_1UI:
        glUniform1ui(uniform->location, uniform->ui[0]);
        break;

      case EGL_UNIFORM_TYPE_2UI:
        glUniform2ui(uniform->location, uniform->ui[0], uniform->ui[1]);
        break;

      case EGL_UNIFORM_TYPE_3UI:
        glUniform3ui(uniform->location, uniform->ui[0], uniform->ui[1],
            uniform->ui[2]);
        break;

      case EGL_UNIFORM_TYPE_4UI:
        glUniform4ui(uniform->location, uniform->ui[0], uniform->ui[1],
            uniform->ui[2], uniform->ui[3]);
        break;

      case EGL_UNIFORM_TYPE_1FV:
        glUniform1fv(uniform->location, uniform->v->size / sizeof(GLfloat),
            (const GLfloat *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_2FV:
        glUniform2fv(uniform->location, uniform->v->size / sizeof(GLfloat) / 2,
            (const GLfloat *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_3FV:
        glUniform3fv(uniform->location, uniform->v->size / sizeof(GLfloat) / 3,
            (const GLfloat *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_4FV:
        glUniform4fv(uniform->location, uniform->v->size / sizeof(GLfloat) / 4,
            (const GLfloat *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_1IV:
        glUniform1iv(uniform->location, uniform->v->size / sizeof(GLint),
            (const GLint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_2IV:
        glUniform2iv(uniform->location, uniform->v->size / sizeof(GLint) / 2,
            (const GLint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_3IV:
        glUniform3iv(uniform->location, uniform->v->size / sizeof(GLint) / 3,
            (const GLint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_4IV:
        glUniform4iv(uniform->location, uniform->v->size / sizeof(GLint) / 4,
            (const GLint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_1UIV:
        glUniform1uiv(uniform->location, uniform->v->size / sizeof(GLuint),
            (const GLuint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_2UIV:
        glUniform2uiv(uniform->location, uniform->v->size / sizeof(GLuint) / 2,
            (const GLuint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_3UIV:
        glUniform3uiv(uniform->location, uniform->v->size / sizeof(GLuint) / 3,
            (const GLuint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_4UIV:
        glUniform4uiv(uniform->location, uniform->v->size / sizeof(GLuint) / 4,
            (const GLuint *)uniform->v->data);
        break;

      case EGL_UNIFORM_TYPE_M2FV:
        glUniformMatrix2fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 2,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M3FV:
        glUniformMatrix3fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 3,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M4FV:
        glUniformMatrix4fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 4,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M2x3FV:
        glUniformMatrix2x3fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 6,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M3x2FV:
        glUniformMatrix3x2fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 6,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M2x4FV:
        glUniformMatrix2x4fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 8,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M4x2FV:
        glUniformMatrix4x2fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 8,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M3x4FV:
        glUniformMatrix3x4fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 12,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;

      case EGL_UNIFORM_TYPE_M4x3FV:
        glUniformMatrix4x3fv(uniform->location,
            uniform->v->size / sizeof(GLfloat) / 12,
            uniform->m.transpose, (const GLfloat *)uniform->m.v->data);
        break;
    }
  }
}

void egl_shaderAssocTextures(EGL_Shader * this, const int count)
{
  char name[] = "sampler1";
  glUseProgram(this->shader);
  for(int i = 0; i < count; ++i, name[7]++)
  {
    GLint loc = glGetUniformLocation(this->shader, name);
    if (loc == -1)
    {
      DEBUG_WARN("Shader uniform location `%s` not found", name);
      continue;
    }

    glUniform1i(loc, i);
  }
  glUseProgram(0);
}

GLint egl_shaderGetUniform(EGL_Shader * this, const char * name)
{
  if (!this->shader)
  {
    DEBUG_ERROR("Shader program has not been compiled");
    return 0;
  }

  return glGetUniformLocation(this->shader, name);
}
