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

#ifndef _H_LG_EGL_DYNPROCS_
#define _H_LG_EGL_DYNPROCS_
#ifdef ENABLE_EGL

#include <EGL/egl.h>
#include <GL/gl.h>

typedef EGLDisplay (*eglGetPlatformDisplayEXT_t)(EGLenum platform,
    void *native_display, const EGLint *attrib_list);
typedef void (*eglSwapBuffersWithDamageKHR_t)(EGLDisplay dpy,
    EGLSurface surface, const EGLint *rects, EGLint n_rects);
typedef void (*glEGLImageTargetTexture2DOES_t)(GLenum target,
    GLeglImageOES image);

struct EGLDynProcs
{
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplay;
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplayEXT;
  eglSwapBuffersWithDamageKHR_t  eglSwapBuffersWithDamageKHR;
  eglSwapBuffersWithDamageKHR_t  eglSwapBuffersWithDamageEXT;
  glEGLImageTargetTexture2DOES_t glEGLImageTargetTexture2DOES;
};

extern struct EGLDynProcs g_egl_dynProcs;

void egl_dynProcsInit(void);

#else
  #define egl_dynProcsInit(...)
#endif

#endif // _H_LG_EGL_DYNPROCS_
