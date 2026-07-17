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

#include "egltypes.h"
#include "shader.h"

typedef struct EGL_Effect     EGL_Effect;
typedef struct EGL_EffectPass EGL_EffectPass;
typedef struct EGL_Texture    EGL_Texture;
struct EGL_FilterRects;

/*
 * Effects execute their enabled passes in insertion order. Each pass owns an
 * intermediate render target and sampler, while shaders remain caller-owned
 * so they can be shared or selected at run time.
 */
bool egl_effectInit(EGL_Effect ** effect);
void egl_effectFree(EGL_Effect ** effect);

bool egl_effectAddPass(EGL_Effect * effect, EGL_Shader * shader,
    EGL_EffectPass ** pass);

void egl_effectPassSetShader(EGL_EffectPass * pass, EGL_Shader * shader);
void egl_effectPassSetEnabled(EGL_EffectPass * pass, bool enabled);
void egl_effectPassSetFilter(EGL_EffectPass * pass, GLenum minFilter,
    GLenum magFilter);
void egl_effectPassSetWrap(EGL_EffectPass * pass, GLenum wrapS,
    GLenum wrapT);

bool egl_effectPassSetup(EGL_EffectPass * pass, enum EGL_PixelFormat pixFmt,
    unsigned int width, unsigned int height);
EGL_Texture * egl_effectPassGetTexture(EGL_EffectPass * pass);
EGL_Texture * egl_effectPassRun(EGL_EffectPass * pass,
    struct EGL_FilterRects * rects, EGL_Texture * const * textures,
    unsigned int textureCount);

EGL_Texture * egl_effectRun(EGL_Effect * effect,
    struct EGL_FilterRects * rects, EGL_Texture * texture);
