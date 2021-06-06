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

#ifdef ENABLE_EGL

#include "egl_dynprocs.h"

struct EGLDynProcs g_egl_dynProcs = {0};

void egl_dynProcsInit(void)
{
  g_egl_dynProcs.eglGetPlatformDisplay = (eglGetPlatformDisplayEXT_t)
    eglGetProcAddress("eglGetPlatformDisplay");
  g_egl_dynProcs.eglGetPlatformDisplayEXT = (eglGetPlatformDisplayEXT_t)
    eglGetProcAddress("eglGetPlatformDisplayEXT");
  g_egl_dynProcs.glEGLImageTargetTexture2DOES = (glEGLImageTargetTexture2DOES_t)
    eglGetProcAddress("glEGLImageTargetTexture2DOES");
  g_egl_dynProcs.eglSwapBuffersWithDamageKHR = (eglSwapBuffersWithDamageKHR_t)
    eglGetProcAddress("eglSwapBuffersWithDamageKHR");
  g_egl_dynProcs.eglSwapBuffersWithDamageEXT = (eglSwapBuffersWithDamageKHR_t)
    eglGetProcAddress("eglSwapBuffersWithDamageEXT");
};

#endif
