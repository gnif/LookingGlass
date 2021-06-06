/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include <GL/gl.h>

typedef struct EGL_Shader EGL_Shader;

bool egl_shader_init(EGL_Shader ** shader);
void egl_shader_free(EGL_Shader ** shader);

bool egl_shader_load   (EGL_Shader * model, const char * vertex_file, const char * fragment_file);
bool egl_shader_compile(EGL_Shader * model, const char * vertex_code, size_t vertex_size, const char * fragment_code, size_t fragment_size);
void egl_shader_use    (EGL_Shader * shader);

void egl_shader_associate_textures(EGL_Shader * shader, const int count);
GLint egl_shader_get_uniform_location(EGL_Shader * shader, const char * name);