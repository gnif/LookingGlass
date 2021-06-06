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

#include "interface/renderer.h"

typedef struct EGL_Desktop EGL_Desktop;

enum EGL_DesktopScaleType
{
  EGL_DESKTOP_NOSCALE,
  EGL_DESKTOP_UPSCALE,
  EGL_DESKTOP_DOWNSCALE,
};

struct Option;
bool egl_desktop_scale_validate(struct Option * opt, const char ** error);

bool egl_desktop_init(EGL_Desktop ** desktop, EGLDisplay * display);
void egl_desktop_free(EGL_Desktop ** desktop);

bool egl_desktop_setup (EGL_Desktop * desktop, const LG_RendererFormat format, bool useDMA);
bool egl_desktop_update(EGL_Desktop * desktop, const FrameBuffer * frame, int dmaFd);
bool egl_desktop_render(EGL_Desktop * desktop, const float x, const float y,
    const float scaleX, const float scaleY, enum EGL_DesktopScaleType scaleType,
    LG_RendererRotate rotate);
