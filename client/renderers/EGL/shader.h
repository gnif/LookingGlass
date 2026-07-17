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
#include <stddef.h>

#include <GLES3/gl3.h>

typedef struct EGL_Shader  EGL_Shader;
typedef struct EGL_Uniform EGL_Uniform;

enum EGL_UniformType
{
  EGL_UNIFORM_TYPE_1F,
  EGL_UNIFORM_TYPE_2F,
  EGL_UNIFORM_TYPE_3F,
  EGL_UNIFORM_TYPE_4F,
  EGL_UNIFORM_TYPE_1I,
  EGL_UNIFORM_TYPE_2I,
  EGL_UNIFORM_TYPE_3I,
  EGL_UNIFORM_TYPE_4I,
  EGL_UNIFORM_TYPE_1UI,
  EGL_UNIFORM_TYPE_2UI,
  EGL_UNIFORM_TYPE_3UI,
  EGL_UNIFORM_TYPE_4UI,

  // vectors
  EGL_UNIFORM_TYPE_1FV,
  EGL_UNIFORM_TYPE_2FV,
  EGL_UNIFORM_TYPE_3FV,
  EGL_UNIFORM_TYPE_4FV,
  EGL_UNIFORM_TYPE_1IV,
  EGL_UNIFORM_TYPE_2IV,
  EGL_UNIFORM_TYPE_3IV,
  EGL_UNIFORM_TYPE_4IV,
  EGL_UNIFORM_TYPE_1UIV,
  EGL_UNIFORM_TYPE_2UIV,
  EGL_UNIFORM_TYPE_3UIV,
  EGL_UNIFORM_TYPE_4UIV,

  // matrices
  EGL_UNIFORM_TYPE_M2FV,
  EGL_UNIFORM_TYPE_M3FV,
  EGL_UNIFORM_TYPE_M4FV,
  EGL_UNIFORM_TYPE_M2x3FV,
  EGL_UNIFORM_TYPE_M3x2FV,
  EGL_UNIFORM_TYPE_M2x4FV,
  EGL_UNIFORM_TYPE_M4x2FV,
  EGL_UNIFORM_TYPE_M3x4FV,
  EGL_UNIFORM_TYPE_M4x3FV
};

typedef struct EGL_ShaderDefine
{
  const char * name;
  const char * value;
}
EGL_ShaderDefine;

bool egl_shaderInit(EGL_Shader ** shader);
void egl_shaderFree(EGL_Shader ** shader);

bool egl_shaderLoad(EGL_Shader * shader, const char * vertex_file,
    const char * fragment_file, bool useDMA,
    const EGL_ShaderDefine * defines);

bool egl_shaderCompile(EGL_Shader * shader, const char * vertex_code,
    size_t vertex_size, const char * fragment_code, size_t fragment_size,
    bool useDMA, const EGL_ShaderDefine * defines);

/*
 * Uniform handles remain valid when a shader is recompiled. Values are copied
 * into the shader and are only uploaded when they have changed or the program
 * has been relinked. count is the number of vectors or matrices in data.
 */
EGL_Uniform * egl_shaderGetUniform(EGL_Shader * shader, const char * name);
bool egl_uniformSet(EGL_Uniform * uniform, enum EGL_UniformType type,
    GLsizei count, GLboolean transpose, const void * data);

bool egl_uniform1f (EGL_Uniform * uniform, GLfloat x);
bool egl_uniform2f (EGL_Uniform * uniform, GLfloat x, GLfloat y);
bool egl_uniform3f (EGL_Uniform * uniform, GLfloat x, GLfloat y, GLfloat z);
bool egl_uniform4f (EGL_Uniform * uniform, GLfloat x, GLfloat y, GLfloat z,
    GLfloat w);
bool egl_uniform1i (EGL_Uniform * uniform, GLint x);
bool egl_uniform1ui(EGL_Uniform * uniform, GLuint x);
bool egl_uniform4ui(EGL_Uniform * uniform, GLuint x, GLuint y, GLuint z,
    GLuint w);

bool egl_uniform4uiv(EGL_Uniform * uniform, GLsizei count,
    const GLuint * value);
bool egl_uniformMatrix3x2fv(EGL_Uniform * uniform, GLsizei count,
    GLboolean transpose, const GLfloat * value);

void egl_shaderUse(EGL_Shader * shader);
void egl_shaderAssocTextures(EGL_Shader * shader, int count);
