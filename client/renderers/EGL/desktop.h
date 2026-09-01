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

#include "egl.h"
#include "desktop_rects.h"

typedef struct EGL_Desktop     EGL_Desktop;
typedef struct EGL_Framebuffer EGL_Framebuffer;

enum EGL_DesktopScaleType
{
  EGL_DESKTOP_NOSCALE,
  EGL_DESKTOP_UPSCALE,
  EGL_DESKTOP_DOWNSCALE,
};

struct Option;
bool egl_desktopScaleValidate(struct Option * opt, const char ** error);

bool egl_desktopInit(EGL * egl, EGL_Desktop ** desktop, EGLDisplay * display,
    bool useDMA, int maxRects);
void egl_desktopFree(EGL_Desktop ** desktop);

void egl_desktopConfigUI(EGL_Desktop * desktop);
void egl_desktopSetNativeHDR(EGL_Desktop * desktop, bool nativeHDR,
    bool linearComposition);
void egl_desktopGetHDRMapping(EGL_Desktop * desktop, bool * enabled,
    float * gain, float * contentPeak);
bool egl_desktopSetup (EGL_Desktop * desktop, const LG_RendererFormat format);
bool egl_desktopUpdate(EGL_Desktop * desktop, const KVMFRFrameBuffer * frame,
    LG_RendererFrameToken frameToken, int dmaFd,
    const KVMFRFrameDamageRect * damageRects, int damageRectsCount,
    uint64_t * waitTimeNs, LG_FrameReleaseFn releaseFn,
    void * releaseOpaque, uint64_t releaseHandle);
void egl_desktopRestart(EGL_Desktop * desktop);
void egl_desktopPoll(EGL_Desktop * desktop);
void egl_desktopResize(EGL_Desktop * desktop, int width, int height);
bool egl_desktopRender(EGL_Desktop * desktop, unsigned int outputWidth,
    unsigned int outputHeight, const float x, const float y,
    const float scaleX, const float scaleY, enum EGL_DesktopScaleType scaleType,
    LG_RendererRotate rotate, const struct DamageRects * rects,
    LG_RendererFrameToken damageFrameToken,
    LG_RendererFrameToken frameTokenLimit, bool * fullFrame,
    LG_RendererFrameToken * consumedFrameToken,
    LG_RendererFrameToken * renderedFrameToken,
    uint64_t * effectsTime, EGL_Framebuffer * target);

void egl_desktopSwSurfaceConfigure(EGL_Desktop * desktop,
    int width, int height);
void egl_desktopSwSurfaceDrawFill(EGL_Desktop * desktop,
    int x, int y, int width, int height, uint32_t color);
void egl_desktopSwSurfaceDrawBitmap(EGL_Desktop * desktop,
    int x, int y, int width, int height, int stride, uint8_t * data,
    bool topDown);
void egl_desktopSwSurfaceShow(EGL_Desktop * desktop, bool show);
