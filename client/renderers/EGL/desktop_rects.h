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
#include "common/types.h"
#include "interface/renderer.h"

struct DamageRects
{
  int count;
  FrameDamageRect rects[];
};

typedef struct EGL_DesktopRects EGL_DesktopRects;

bool egl_desktopRectsInit(EGL_DesktopRects ** rects, int maxCount);
void egl_desktopRectsFree(EGL_DesktopRects ** rects);

void egl_desktopRectsMatrix(float matrix[6], int width, int height, float translateX,
    float translateY, float scaleX, float scaleY, LG_RendererRotate rotate);
void egl_desktopToScreenMatrix(double matrix[6], int frameWidth, int frameHeight,
    double translateX, double translateY, double scaleX, double scaleY, LG_RendererRotate rotate,
    double windowWidth, double windowHeight);
struct Rect egl_desktopToScreen(const double matrix[6], const struct FrameDamageRect * rect);

void egl_screenToDesktopMatrix(double matrix[6], int frameWidth, int frameHeight,
    double translateX, double translateY, double scaleX, double scaleY, LG_RendererRotate rotate,
    double windowWidth, double windowHeight);
bool egl_screenToDesktop(struct FrameDamageRect * output, const double matrix[6],
    const struct Rect * rect, int width, int height);

void egl_desktopRectsUpdate(EGL_DesktopRects * rects, const struct DamageRects * data,
    int width, int height);
void egl_desktopRectsRender(EGL_DesktopRects * rects);