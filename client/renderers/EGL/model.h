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

#pragma once

#include <stdbool.h>
#include "shader.h"
#include "texture.h"

#include <GL/gl.h>

typedef struct EGL_Model EGL_Model;

bool egl_model_init(EGL_Model ** model);
void egl_model_free(EGL_Model ** model);

void egl_model_set_default  (EGL_Model * model);
void egl_model_add_verticies(EGL_Model * model, const GLfloat * verticies, const GLfloat * uvs, const size_t count);
void egl_model_set_shader   (EGL_Model * model, EGL_Shader  * shader);
void egl_model_set_texture  (EGL_Model * model, EGL_Texture * texture);

void egl_model_render(EGL_Model * model);