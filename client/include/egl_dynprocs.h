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

#ifndef _H_LG_EGL_DYNPROCS_
#define _H_LG_EGL_DYNPROCS_
#ifdef ENABLE_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#undef GL_KHR_debug
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

struct EGLDynProcs
{
  PFNEGLGETPLATFORMDISPLAYPROC        eglGetPlatformDisplay;
  PFNEGLGETPLATFORMDISPLAYPROC        eglGetPlatformDisplayEXT;
  PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC  eglSwapBuffersWithDamageKHR;
  PFNEGLSWAPBUFFERSWITHDAMAGEEXTPROC  eglSwapBuffersWithDamageEXT;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
  PFNGLDEBUGMESSAGECALLBACKKHRPROC    glDebugMessageCallback;
  PFNGLDEBUGMESSAGECALLBACKKHRPROC    glDebugMessageCallbackKHR;
  PFNGLBUFFERSTORAGEEXTPROC           glBufferStorageEXT;
  PFNEGLCREATEIMAGEPROC               eglCreateImage;
  PFNEGLDESTROYIMAGEPROC              eglDestroyImage;
};

extern struct EGLDynProcs g_egl_dynProcs;

void egl_dynProcsInit(void);

#else
  #define egl_dynProcsInit(...)
#endif

#endif // _H_LG_EGL_DYNPROCS_
