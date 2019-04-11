/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "shader.h"
#include "common/debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL2/SDL_egl.h>

struct EGL_Shader
{
  bool   hasShader;
  GLuint shader;
};

bool egl_shader_init(EGL_Shader ** this)
{
  *this = (EGL_Shader *)malloc(sizeof(EGL_Shader));
  if (!*this)
  {
    DEBUG_ERROR("Failed to malloc EGL_Shader");
    return false;
  }

  memset(*this, 0, sizeof(EGL_Shader));
  return true;
}

void egl_shader_free(EGL_Shader ** this)
{
  if (!*this)
    return;

  if ((*this)->hasShader)
    glDeleteProgram((*this)->shader);

  free(*this);
  *this = NULL;
}

bool egl_shader_load(EGL_Shader * this, const char * vertex_file, const char * fragment_file)
{
  char   * vertex_code, * fragment_code;
  size_t   vertex_size,   fragment_size;

  if (!file_get_contents(vertex_file, &vertex_code, &vertex_size))
  {
    DEBUG_ERROR("Failed to read vertex shader");
    return false;
  }

  DEBUG_INFO("Loaded vertex shader: %s", vertex_file);

  if (!file_get_contents(fragment_file, &fragment_code, &fragment_size))
  {
    DEBUG_ERROR("Failed to read fragment shader");
    free(vertex_code);
    return false;
  }

  DEBUG_INFO("Loaded fragment shader: %s", fragment_file);

  bool ret = egl_shader_compile(this, vertex_code, vertex_size, fragment_code, fragment_size);
  free(vertex_code);
  free(fragment_code);
  return ret;
}

bool egl_shader_compile(EGL_Shader * this, const char * vertex_code, size_t vertex_size, const char * fragment_code, size_t fragment_size)
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
      glGetShaderInfoLog(vertexShader, logLength, NULL, log);
      log[logLength] = 0;
      DEBUG_ERROR("%s", log);
      free(log);
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
      glGetShaderInfoLog(fragmentShader, logLength, NULL, log);
      log[logLength] = 0;
      DEBUG_ERROR("%s", log);
      free(log);
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

void egl_shader_use(EGL_Shader * this)
{
  if (this->hasShader)
    glUseProgram(this->shader);
  else
    DEBUG_ERROR("Shader program has not been compiled");
}

void egl_shader_associate_textures(EGL_Shader * this, const int count)
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

GLint egl_shader_get_uniform_location(EGL_Shader * this, const char * name)
{
  if (!this->shader)
  {
    DEBUG_ERROR("Shader program has not been compiled");
    return 0;
  }

  return glGetUniformLocation(this->shader, name);
}