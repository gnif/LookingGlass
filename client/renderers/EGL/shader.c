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

#include "shader.h"
#include "common/debug.h"
#include "common/stringutils.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct EGL_Uniform
{
  char  * name;
  GLint   location;

  enum EGL_UniformType type;
  GLsizei              count;
  GLboolean            transpose;
  bool                 initialized;
  bool                 dirty;

  void * data;
  size_t size;
  size_t capacity;

  EGL_Shader         * shader;
  struct EGL_Uniform * next;
  struct EGL_Uniform * dirtyNext;
};

struct EGL_Shader
{
  bool   hasShader;
  GLuint shader;

  EGL_Uniform * uniforms;
  EGL_Uniform * uniformsTail;
  EGL_Uniform * dirtyUniforms;
  EGL_Uniform * dirtyUniformsTail;
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

  EGL_Uniform * uniform = this->uniforms;
  while (uniform)
  {
    EGL_Uniform * next = uniform->next;
    free(uniform->name);
    free(uniform->data);
    free(uniform);
    uniform = next;
  }

  free(this);
  *shader = NULL;
}

bool egl_shaderLoad(EGL_Shader * this,
    const char * vertex_file, const char * fragment_file, bool useDMA,
    const EGL_ShaderDefine * defines)
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

  bool ret = egl_shaderCompile(this,
      vertex_code, vertex_size, fragment_code, fragment_size,
      useDMA, defines);

  free(vertex_code);
  free(fragment_code);
  return ret;
}

static void uniformMarkDirty(EGL_Uniform * uniform)
{
  if (uniform->dirty)
    return;

  uniform->dirty = true;
  if (uniform->shader->dirtyUniformsTail)
    uniform->shader->dirtyUniformsTail->dirtyNext = uniform;
  else
    uniform->shader->dirtyUniforms = uniform;
  uniform->shader->dirtyUniformsTail = uniform;
}

static void shaderResolveUniforms(EGL_Shader * this)
{
  this->dirtyUniforms     = NULL;
  this->dirtyUniformsTail = NULL;

  for(EGL_Uniform * uniform = this->uniforms; uniform; uniform = uniform->next)
  {
    uniform->location  = glGetUniformLocation(this->shader, uniform->name);
    uniform->dirty     = false;
    uniform->dirtyNext = NULL;
    if (uniform->initialized)
      uniformMarkDirty(uniform);
  }
}

static bool shaderCompile(EGL_Shader * this, const char * vertex_code,
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
      if (!log)
        DEBUG_ERROR("out of memory");
      else
      {
        glGetProgramInfoLog(this->shader, logLength, NULL, log);
        log[logLength] = 0;
        DEBUG_ERROR("%s", log);
        free(log);
      }
    }

    glDetachShader(this->shader, vertexShader  );
    glDetachShader(this->shader, fragmentShader);
    glDeleteShader(fragmentShader);
    glDeleteShader(vertexShader  );
    glDeleteProgram(this->shader );
    this->shader = 0;
    return false;
  }

  glDetachShader(this->shader, vertexShader  );
  glDetachShader(this->shader, fragmentShader);
  glDeleteShader(fragmentShader);
  glDeleteShader(vertexShader  );

  this->hasShader = true;
  shaderResolveUniforms(this);
  return true;
}

bool egl_shaderCompile(EGL_Shader * this, const char * vertex_code,
    size_t vertex_size, const char * fragment_code, size_t fragment_size,
    bool useDMA, const EGL_ShaderDefine * defines)
{
  bool result      = false;
  char * processed = NULL;
  char * newCode   = NULL;

  if (useDMA)
  {
    const char search[]  = "sampler2D";
    const char replace[] = "samplerExternalOES";

    const char * offset = NULL;
    int instances = 0;

    while((offset = memsearch(
      fragment_code, fragment_size,
      search       , sizeof(search)-1,
      offset)))
    {
      ++instances;
      offset += sizeof(search)-1;
    }

    const int diff   = (sizeof(replace) - sizeof(search)) * instances;
    const int newLen = fragment_size + diff;
    newCode = malloc(newLen + 1);
    if (!newCode)
    {
      DEBUG_ERROR("Out of memory");
      goto exit;
    }

    const char * src = fragment_code;
    char * dst = newCode;
    for(int i = 0; i < instances; ++i)
    {
      const char * pos = strstr(src, search);
      const int offset = pos - src;

      memcpy(dst, src, offset);
      dst += offset;
      src  = pos + sizeof(search)-1;

      memcpy(dst, replace, sizeof(replace)-1);
      dst += sizeof(replace)-1;
    }

    const int final = fragment_size - (src - fragment_code);
    memcpy(dst, src, final);
    dst[final] = '\0';

    fragment_code = newCode;
    fragment_size = newLen;
  }

  if (defines)
  {
    // find the end of any existing lines starting with #
    bool newLine   = true;
    bool skip      = false;
    int  insertPos = 0;
    for(int i = 0; i < fragment_size; ++i)
    {
      if (skip)
      {
        if (fragment_code[i] == '\n')
          skip = false;
        continue;
      }

      switch(fragment_code[i])
      {
        case '\n':
          newLine = true;
          continue;

        case ' ':
        case '\t':
        case '\r':
          continue;

        case '#':
          if (newLine)
          {
            skip = true;
            continue;
          }
          //fallthrough

        default:
          newLine = false;
          break;
      }

      if (!newLine)
      {
        insertPos = i;
        if (insertPos > 0)
          --insertPos;
        break;
      }
    }

    int processedLen = fragment_size;
    const char * defineFormat = "#define %s %s\n";
    for(const EGL_ShaderDefine * define = defines; define->name; ++define)
      processedLen += snprintf(NULL, 0, defineFormat, define->name, define->value);

    processed = malloc(processedLen);
    if (!processed)
    {
      DEBUG_ERROR("Out of memory");
      goto exit;
    }

    memcpy(processed, fragment_code, insertPos);

    int offset = insertPos;
    for(const EGL_ShaderDefine * define = defines; define->name; ++define)
      offset += sprintf(processed + offset, defineFormat,
          define->name, define->value);

    memcpy(
        processed       + offset,
        fragment_code + insertPos,
        fragment_size - insertPos);

    fragment_code = processed;
    fragment_size = processedLen;
  }

  result = shaderCompile(this,
      vertex_code  , vertex_size,
      fragment_code, fragment_size);

exit:
  free(processed);
  free(newCode);
  return result;
}

static bool uniformLayout(enum EGL_UniformType type, size_t * scalarSize,
    size_t * components)
{
  switch(type)
  {
    case EGL_UNIFORM_TYPE_1F:
    case EGL_UNIFORM_TYPE_1FV:
      *scalarSize = sizeof(GLfloat); *components = 1; break;
    case EGL_UNIFORM_TYPE_2F:
    case EGL_UNIFORM_TYPE_2FV:
      *scalarSize = sizeof(GLfloat); *components = 2; break;
    case EGL_UNIFORM_TYPE_3F:
    case EGL_UNIFORM_TYPE_3FV:
      *scalarSize = sizeof(GLfloat); *components = 3; break;
    case EGL_UNIFORM_TYPE_4F:
    case EGL_UNIFORM_TYPE_4FV:
      *scalarSize = sizeof(GLfloat); *components = 4; break;

    case EGL_UNIFORM_TYPE_1I:
    case EGL_UNIFORM_TYPE_1IV:
      *scalarSize = sizeof(GLint); *components = 1; break;
    case EGL_UNIFORM_TYPE_2I:
    case EGL_UNIFORM_TYPE_2IV:
      *scalarSize = sizeof(GLint); *components = 2; break;
    case EGL_UNIFORM_TYPE_3I:
    case EGL_UNIFORM_TYPE_3IV:
      *scalarSize = sizeof(GLint); *components = 3; break;
    case EGL_UNIFORM_TYPE_4I:
    case EGL_UNIFORM_TYPE_4IV:
      *scalarSize = sizeof(GLint); *components = 4; break;

    case EGL_UNIFORM_TYPE_1UI:
    case EGL_UNIFORM_TYPE_1UIV:
      *scalarSize = sizeof(GLuint); *components = 1; break;
    case EGL_UNIFORM_TYPE_2UI:
    case EGL_UNIFORM_TYPE_2UIV:
      *scalarSize = sizeof(GLuint); *components = 2; break;
    case EGL_UNIFORM_TYPE_3UI:
    case EGL_UNIFORM_TYPE_3UIV:
      *scalarSize = sizeof(GLuint); *components = 3; break;
    case EGL_UNIFORM_TYPE_4UI:
    case EGL_UNIFORM_TYPE_4UIV:
      *scalarSize = sizeof(GLuint); *components = 4; break;

    case EGL_UNIFORM_TYPE_M2FV:
      *scalarSize = sizeof(GLfloat); *components = 4; break;
    case EGL_UNIFORM_TYPE_M3FV:
      *scalarSize = sizeof(GLfloat); *components = 9; break;
    case EGL_UNIFORM_TYPE_M4FV:
      *scalarSize = sizeof(GLfloat); *components = 16; break;
    case EGL_UNIFORM_TYPE_M2x3FV:
    case EGL_UNIFORM_TYPE_M3x2FV:
      *scalarSize = sizeof(GLfloat); *components = 6; break;
    case EGL_UNIFORM_TYPE_M2x4FV:
    case EGL_UNIFORM_TYPE_M4x2FV:
      *scalarSize = sizeof(GLfloat); *components = 8; break;
    case EGL_UNIFORM_TYPE_M3x4FV:
    case EGL_UNIFORM_TYPE_M4x3FV:
      *scalarSize = sizeof(GLfloat); *components = 12; break;

    default:
      return false;
  }
  return true;

  return false;
}

EGL_Uniform * egl_shaderGetUniform(EGL_Shader * this, const char * name)
{
  for(EGL_Uniform * uniform = this->uniforms; uniform; uniform = uniform->next)
    if (!strcmp(uniform->name, name))
      return uniform;

  EGL_Uniform * uniform = calloc(1, sizeof(*uniform));
  if (!uniform)
  {
    DEBUG_ERROR("Failed to allocate uniform `%s`", name);
    return NULL;
  }

  const size_t nameSize = strlen(name) + 1;
  uniform->name = malloc(nameSize);
  if (!uniform->name)
  {
    DEBUG_ERROR("Failed to allocate uniform name `%s`", name);
    free(uniform);
    return NULL;
  }

  memcpy(uniform->name, name, nameSize);
  uniform->shader = this;
  uniform->location = this->hasShader ?
    glGetUniformLocation(this->shader, name) : -1;

  if (this->uniformsTail)
    this->uniformsTail->next = uniform;
  else
    this->uniforms = uniform;
  this->uniformsTail = uniform;

  return uniform;
}

bool egl_uniformSet(EGL_Uniform * uniform, enum EGL_UniformType type,
    GLsizei count, GLboolean transpose, const void * data)
{
  if (!uniform || !data || count < 1)
    return false;

  size_t scalarSize, components;
  if (!uniformLayout(type, &scalarSize, &components) ||
      components > SIZE_MAX / scalarSize ||
      (size_t)count > SIZE_MAX / (scalarSize * components))
    return false;

  const size_t size = scalarSize * components * count;
  const bool changed = !uniform->initialized ||
    uniform->type      != type      ||
    uniform->count     != count     ||
    uniform->transpose != transpose ||
    uniform->size      != size      ||
    memcmp(uniform->data, data, size);

  if (!changed)
    return true;

  if (size > uniform->capacity)
  {
    void * newData = realloc(uniform->data, size);
    if (!newData)
    {
      DEBUG_ERROR("Failed to allocate data for uniform `%s`", uniform->name);
      return false;
    }

    uniform->data = newData;
    uniform->capacity = size;
  }

  memcpy(uniform->data, data, size);
  uniform->type      = type;
  uniform->count     = count;
  uniform->transpose = transpose;
  uniform->size      = size;
  uniform->initialized = true;
  uniformMarkDirty(uniform);
  return true;
}

bool egl_uniform1f(EGL_Uniform * uniform, GLfloat x)
{
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_1F, 1, GL_FALSE, &x);
}

bool egl_uniform2f(EGL_Uniform * uniform, GLfloat x, GLfloat y)
{
  const GLfloat value[] = { x, y };
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_2F, 1, GL_FALSE, value);
}

bool egl_uniform3f(EGL_Uniform * uniform, GLfloat x, GLfloat y, GLfloat z)
{
  const GLfloat value[] = { x, y, z };
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_3F, 1, GL_FALSE, value);
}

bool egl_uniform4f(EGL_Uniform * uniform, GLfloat x, GLfloat y, GLfloat z,
    GLfloat w)
{
  const GLfloat value[] = { x, y, z, w };
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_4F, 1, GL_FALSE, value);
}

bool egl_uniform1i(EGL_Uniform * uniform, GLint x)
{
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_1I, 1, GL_FALSE, &x);
}

bool egl_uniform1ui(EGL_Uniform * uniform, GLuint x)
{
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_1UI, 1, GL_FALSE, &x);
}

bool egl_uniform4ui(EGL_Uniform * uniform, GLuint x, GLuint y, GLuint z,
    GLuint w)
{
  const GLuint value[] = { x, y, z, w };
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_4UI, 1, GL_FALSE, value);
}

bool egl_uniform4uiv(EGL_Uniform * uniform, GLsizei count,
    const GLuint * value)
{
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_4UIV, count, GL_FALSE,
      value);
}

bool egl_uniformMatrix3x2fv(EGL_Uniform * uniform, GLsizei count,
    GLboolean transpose, const GLfloat * value)
{
  return egl_uniformSet(uniform, EGL_UNIFORM_TYPE_M3x2FV, count, transpose,
      value);
}

static void uniformUpload(EGL_Uniform * uniform)
{
  switch(uniform->type)
  {
    case EGL_UNIFORM_TYPE_1F:
      glUniform1fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2F:
      glUniform2fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3F:
      glUniform3fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4F:
      glUniform4fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_1I:
      glUniform1iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2I:
      glUniform2iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3I:
      glUniform3iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4I:
      glUniform4iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_1UI:
      glUniform1uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2UI:
      glUniform2uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3UI:
      glUniform3uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4UI:
      glUniform4uiv(uniform->location, uniform->count, uniform->data); break;

    case EGL_UNIFORM_TYPE_1FV:
      glUniform1fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2FV:
      glUniform2fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3FV:
      glUniform3fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4FV:
      glUniform4fv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_1IV:
      glUniform1iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2IV:
      glUniform2iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3IV:
      glUniform3iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4IV:
      glUniform4iv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_1UIV:
      glUniform1uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_2UIV:
      glUniform2uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_3UIV:
      glUniform3uiv(uniform->location, uniform->count, uniform->data); break;
    case EGL_UNIFORM_TYPE_4UIV:
      glUniform4uiv(uniform->location, uniform->count, uniform->data); break;

    case EGL_UNIFORM_TYPE_M2FV:
      glUniformMatrix2fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M3FV:
      glUniformMatrix3fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M4FV:
      glUniformMatrix4fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M2x3FV:
      glUniformMatrix2x3fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M3x2FV:
      glUniformMatrix3x2fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M2x4FV:
      glUniformMatrix2x4fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M4x2FV:
      glUniformMatrix4x2fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M3x4FV:
      glUniformMatrix3x4fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
    case EGL_UNIFORM_TYPE_M4x3FV:
      glUniformMatrix4x3fv(uniform->location, uniform->count,
          uniform->transpose, uniform->data); break;
  }
}

void egl_shaderUse(EGL_Shader * this)
{
  if (!this->hasShader)
  {
    DEBUG_ERROR("Shader program has not been compiled");
    return;
  }

  glUseProgram(this->shader);

  while(this->dirtyUniforms)
  {
    EGL_Uniform * uniform = this->dirtyUniforms;
    this->dirtyUniforms = uniform->dirtyNext;
    uniform->dirtyNext = NULL;
    uniform->dirty = false;

    if (uniform->location != -1)
      uniformUpload(uniform);
  }

  this->dirtyUniformsTail = NULL;
}

void egl_shaderAssocTextures(EGL_Shader * this, int count)
{
  char name[32];
  for(int i = 0; i < count; ++i)
  {
    snprintf(name, sizeof(name), "sampler%d", i + 1);
    egl_uniform1i(egl_shaderGetUniform(this, name), i);
  }
}
