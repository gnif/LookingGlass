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

#include "common/types.h"

#include <stdbool.h>

typedef struct EGL_Framebuffer EGL_Framebuffer;
typedef struct EGL_HDRCompose  EGL_HDRCompose;

bool egl_hdrComposeInit(EGL_HDRCompose ** compose);
void egl_hdrComposeFree(EGL_HDRCompose ** compose);
bool egl_hdrComposeResize(EGL_HDRCompose * compose,
    unsigned int width, unsigned int height);
void egl_hdrComposeSetActive(EGL_HDRCompose * compose, bool active);
bool egl_hdrComposeIsConfigured(EGL_HDRCompose * compose);
bool egl_hdrComposeBegin(EGL_HDRCompose * compose);
void egl_hdrComposeEnd(EGL_HDRCompose * compose,
    const struct Rect * damage, int damageCount);
EGL_Framebuffer * egl_hdrComposeGetFramebuffer(EGL_HDRCompose * compose);
