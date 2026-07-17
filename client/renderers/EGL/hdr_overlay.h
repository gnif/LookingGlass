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

typedef struct EGL_HDROverlay EGL_HDROverlay;

bool egl_hdrOverlayInit(EGL_HDROverlay ** overlay);
void egl_hdrOverlayFree(EGL_HDROverlay ** overlay);

bool egl_hdrOverlayResize(EGL_HDROverlay * overlay, unsigned int width,
    unsigned int height);
void egl_hdrOverlaySetState(EGL_HDROverlay * overlay, bool active, bool pq,
    float referenceWhiteLevel);

/*
 * Begin returns true when ImGui should be rendered into the HDR overlay
 * framebuffer. The caller must then call egl_hdrOverlayEnd after ImGui has
 * finished rendering and the EGL state cache has been invalidated.
 */
bool egl_hdrOverlayBegin(EGL_HDROverlay * overlay,
    const struct Rect * damage, int damageCount);
void egl_hdrOverlayEnd(EGL_HDROverlay * overlay,
    const struct Rect * damage, int damageCount);
