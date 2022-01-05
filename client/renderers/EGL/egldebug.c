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

#include "egldebug.h"
#include <GLES3/gl3.h>
#include <EGL/egl.h>

const char * egl_getErrorStr(void)
{
  switch (eglGetError())
  {
    case EGL_SUCCESS            : return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED    : return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS         : return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC          : return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE      : return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT        : return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG         : return "EGL_BAD_CONFIG";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY        : return "EGL_BAD_DISPLAY";
    case EGL_BAD_SURFACE        : return "EGL_BAD_SURFACE";
    case EGL_BAD_MATCH          : return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER      : return "EGL_BAD_PARAMETER";
    case EGL_BAD_NATIVE_PIXMAP  : return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW  : return "EGL_BAD_NATIVE_WINDOW";
    case EGL_CONTEXT_LOST       : return "EGL_CONTEXT_LOST";
    default                     : return "UNKNOWN";
  }
}

const char * gl_getErrorStr(void)
{
  switch (glGetError())
  {
    case GL_NO_ERROR                     : return "GL_NO_ERROR";
    case GL_INVALID_ENUM                 : return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE                : return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION            : return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY                : return "GL_OUT_OF_MEMORY";
    default                              : return "UNKNOWN";
  }
}
