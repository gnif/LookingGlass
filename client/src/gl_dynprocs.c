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

#ifdef ENABLE_OPENGL

#include "gl_dynprocs.h"
#include <GL/glx.h>

struct GLDynProcs g_gl_dynProcs = {0};


static void * getProcAddressGL(const char * name)
{
  return (void *) glXGetProcAddressARB((const GLubyte *) name);
}

static void * getProcAddressGL2(const char * name, const char * backup)
{
  void * func = getProcAddressGL(name);
  return func ? func : getProcAddressGL(backup);
}

void gl_dynProcsInit(void)
{
  g_gl_dynProcs.glGenBuffers    = getProcAddressGL2("glGenBuffers", "glGenBuffersARB");
  g_gl_dynProcs.glBindBuffer    = getProcAddressGL2("glBindBuffer", "glBindBufferARB");
  g_gl_dynProcs.glBufferData    = getProcAddressGL2("glBufferData", "glBufferDataARB");
  g_gl_dynProcs.glBufferSubData = getProcAddressGL2("glBufferSubData", "glBufferSubDataARB");
  g_gl_dynProcs.glDeleteBuffers = getProcAddressGL2("glDeleteBuffers", "glDeleteBuffersARB");

  g_gl_dynProcs.glIsSync         = getProcAddressGL("glIsSync");
  g_gl_dynProcs.glFenceSync      = getProcAddressGL("glFenceSync");
  g_gl_dynProcs.glClientWaitSync = getProcAddressGL("glClientWaitSync");
  g_gl_dynProcs.glDeleteSync     = getProcAddressGL("glDeleteSync");

  g_gl_dynProcs.glGenerateMipmap = getProcAddressGL("glGenerateMipmap");
  if (!g_gl_dynProcs.glGenerateMipmap)
    g_gl_dynProcs.glGenerateMipmap = getProcAddressGL("glGenerateMipmapEXT");
};

#endif
