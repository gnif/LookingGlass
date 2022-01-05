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

#pragma once

#include <stdbool.h>
#include "shader.h"
#include "texture.h"

#include <GLES3/gl3.h>

typedef struct EGL_Model EGL_Model;
typedef struct EGL_Texture EGL_Texture;

bool egl_modelInit(EGL_Model ** model);
void egl_modelFree(EGL_Model ** model);

void egl_modelSetDefault  (EGL_Model * model, bool flipped);
void egl_modelAddVerts(EGL_Model * model, const GLfloat * verticies, const GLfloat * uvs, const size_t count);
void egl_modelSetShader   (EGL_Model * model, EGL_Shader  * shader);
void egl_modelSetTexture  (EGL_Model * model, EGL_Texture * texture);

void egl_modelRender(EGL_Model * model);
