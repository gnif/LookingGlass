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

#include "desktop_rects.h"
#include "filter.h"
#include "texture.h"

typedef struct EGL_PostProcess EGL_PostProcess;

void egl_postProcessEarlyInit(void);

bool egl_postProcessInit(EGL_PostProcess ** pp);
void egl_postProcessFree(EGL_PostProcess ** pp);

/* create and add a filter to this processor */
bool egl_postProcessAdd(EGL_PostProcess * this, const EGL_FilterOps * ops);

/* returns true if the configuration was modified since the last run */
bool egl_postProcessConfigModified(EGL_PostProcess * this);

/* apply the filters to the supplied texture
 * targetX/Y is the final target output dimension hint if scalers are present */
bool egl_postProcessRun(EGL_PostProcess * this, EGL_Texture * tex,
    EGL_DesktopRects * rects, int desktopWidth, int desktopHeight,
    unsigned int targetX, unsigned int targetY);

GLuint egl_postProcessGetOutput(EGL_PostProcess * this,
    unsigned int * outputX, unsigned int * outputY);
