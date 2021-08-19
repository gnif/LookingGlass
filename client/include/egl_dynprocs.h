/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

typedef EGLDisplay (*eglGetPlatformDisplayEXT_t)(EGLenum platform,
    void *native_display, const EGLint *attrib_list);
typedef EGLBoolean (*eglSwapBuffersWithDamageKHR_t)(EGLDisplay dpy,
    EGLSurface surface, const EGLint *rects, EGLint n_rects);
typedef void (*glEGLImageTargetTexture2DOES_t)(GLenum target,
    GLeglImageOES image);
typedef void (*DEBUGPROC_t)(GLenum source,
    GLenum type, GLuint id, GLenum severity, GLsizei length,
    const GLchar *message, const void *userParam);
typedef void (*glDebugMessageCallback_t)(DEBUGPROC_t callback,
    const void * userParam);
typedef void (*glBufferStorageEXT_t)(GLenum target, GLsizeiptr size,
    const void * data, GLbitfield flags);

struct EGLDynProcs
{
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplay;
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplayEXT;
  eglSwapBuffersWithDamageKHR_t  eglSwapBuffersWithDamageKHR;
  eglSwapBuffersWithDamageKHR_t  eglSwapBuffersWithDamageEXT;
  glEGLImageTargetTexture2DOES_t glEGLImageTargetTexture2DOES;
  glDebugMessageCallback_t       glDebugMessageCallback;
  glDebugMessageCallback_t       glDebugMessageCallbackKHR;
  glBufferStorageEXT_t           glBufferStorageEXT;
};

extern struct EGLDynProcs g_egl_dynProcs;

void egl_dynProcsInit(void);

#else
  #define egl_dynProcsInit(...)
#endif

#endif // _H_LG_EGL_DYNPROCS_
