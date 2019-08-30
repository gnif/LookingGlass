/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include <GL/gl.h>
#include <stdarg.h>
#include <stdio.h>

void egl_debug_printf(char * format, ...)
{
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);

  GLenum error = glGetError();
  switch(error)
  {
    case GL_NO_ERROR:
      fprintf(stderr, " (GL_NO_ERROR)\n");
      break;

    case GL_INVALID_ENUM:
      fprintf(stderr, " (GL_INVALID_ENUM)\n");
      break;

    case GL_INVALID_VALUE:
      fprintf(stderr, " (GL_INVALID_VALUE)\n");
      break;

    case GL_INVALID_OPERATION:
      fprintf(stderr, " (GL_INVALID_OPERATION)\n");
      break;

    case GL_INVALID_FRAMEBUFFER_OPERATION:
      fprintf(stderr, " (GL_INVALID_FRAMEBUFFER_OPERATION)\n");
      break;

    case GL_OUT_OF_MEMORY:
      fprintf(stderr, " (GL_OUT_OF_MEMORY)\n");
      break;
  }
}