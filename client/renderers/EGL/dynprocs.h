/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <SDL2/SDL_egl.h>
#include <GL/gl.h>

typedef EGLDisplay (*eglGetPlatformDisplayEXT_t)(EGLenum platform,
    void *native_display, const EGLint *attrib_list);
typedef void (*glEGLImageTargetTexture2DOES_t)(GLenum target,
    GLeglImageOES image);

struct EGLDynProcs
{
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplay;
  eglGetPlatformDisplayEXT_t     eglGetPlatformDisplayEXT;
  glEGLImageTargetTexture2DOES_t glEGLImageTargetTexture2DOES;
};

extern struct EGLDynProcs g_dynprocs;

void egl_dynProcsInit(void);
