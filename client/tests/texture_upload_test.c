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

#include "test.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../renderers/EGL/texture_buffer.h"
#include "../renderers/EGL/texture_util.c"

struct Trace
{
  unsigned int nextBuffer;
  unsigned int storage;
  unsigned int map;
  unsigned int unmap;
  unsigned int deleted;
  unsigned int bufferBinds;
  unsigned int textureBinds;
  unsigned int uploads;
  unsigned int fences;
  unsigned int finishes;
  unsigned int flushes;
  GLuint       boundPBO;
  GLuint       boundTexture;
  GLint        unpackAlignment;
  GLint        unpackRowLength;
  GLint        uploadX;
  GLint        uploadY;
  GLsizei      uploadWidth;
  GLsizei      uploadHeight;
  const void * uploadData;
  bool         failMap;
};

static struct Trace trace;
static uint8_t      mappedStorage[4096];

struct EGLDynProcs              g_egl_dynProcs;
_Thread_local EGL_StateCache    g_eglState;

void debug_level(enum DebugLevel level, const char * file, unsigned int line,
    const char * function, const char * format, ...)
{
}

static void fakeBufferStorage(GLenum target, GLsizeiptr size,
    const void * data, GLbitfield flags)
{
  CHECK(target == GL_PIXEL_UNPACK_BUFFER);
  CHECK(size > 0);
  CHECK(!data);
  CHECK(flags & GL_MAP_PERSISTENT_BIT_EXT);
  ++trace.storage;
}

void glGenBuffers(GLsizei count, GLuint * buffers)
{
  for (GLsizei i = 0; i < count; ++i)
    buffers[i] = ++trace.nextBuffer;
}

void glBindBuffer(GLenum target, GLuint buffer)
{
  if (target == GL_PIXEL_UNPACK_BUFFER)
  {
    trace.boundPBO = buffer;
    ++trace.bufferBinds;
  }
}

void glActiveTexture(GLenum texture)
{
  CHECK(texture == GL_TEXTURE0);
}

void glBindTexture(GLenum target, GLuint texture)
{
  CHECK(target == GL_TEXTURE_2D);
  trace.boundTexture = texture;
  ++trace.textureBinds;
}

void glPixelStorei(GLenum pname, GLint param)
{
  switch (pname)
  {
    case GL_UNPACK_ALIGNMENT:
      trace.unpackAlignment = param;
      break;

    case GL_UNPACK_ROW_LENGTH:
      trace.unpackRowLength = param;
      break;

    default:
      CHECK(false);
  }
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset,
    GLint yoffset, GLsizei width, GLsizei height, GLenum format,
    GLenum type, const void * pixels)
{
  CHECK(target == GL_TEXTURE_2D);
  CHECK(level == 0);
  CHECK(format == GL_RGBA);
  CHECK(type == GL_UNSIGNED_BYTE);

  trace.uploadX      = xoffset;
  trace.uploadY      = yoffset;
  trace.uploadWidth  = width;
  trace.uploadHeight = height;
  trace.uploadData   = pixels;
  ++trace.uploads;
}

GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
  CHECK(condition == GL_SYNC_GPU_COMMANDS_COMPLETE);
  CHECK(flags == 0);
  ++trace.fences;
  return (GLsync)(uintptr_t)1;
}

void glFinish(void)
{
  ++trace.finishes;
}

void glFlush(void)
{
  ++trace.flushes;
}

void * glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
    GLbitfield access)
{
  CHECK(target == GL_PIXEL_UNPACK_BUFFER);
  CHECK(offset == 0);
  CHECK(length <= (GLsizeiptr)sizeof(mappedStorage));
  CHECK(access & GL_MAP_PERSISTENT_BIT_EXT);
  ++trace.map;
  return trace.failMap ? NULL : mappedStorage;
}

GLboolean glUnmapBuffer(GLenum target)
{
  CHECK(target == GL_PIXEL_UNPACK_BUFFER);
  ++trace.unmap;
  return GL_TRUE;
}

void glDeleteBuffers(GLsizei count, const GLuint * buffers)
{
  trace.deleted += count;
}

void egl_stateInvalidateShared(void)
{
  memset(&g_eglState, 0, sizeof(g_eglState));
}

const char * gl_getErrorStr(void)
{
  return "fake";
}

static void reset(void)
{
  memset(&trace, 0, sizeof(trace));
  memset(&g_eglState, 0, sizeof(g_eglState));
  memset(&g_egl_dynProcs, 0, sizeof(g_egl_dynProcs));
  g_egl_dynProcs.glBufferStorageEXT = fakeBufferStorage;
}

static EGL_TexFormat testFormat(void)
{
  return (EGL_TexFormat)
  {
    .dataSize = 1024,
  };
}

static void initTexture(TextureBuffer * texture, EGL_TexBufferMode mode)
{
  memset(texture, 0, sizeof(*texture));

  texture->base.format.width    = 8;
  texture->base.format.height   = 4;
  texture->base.format.stride   = 10;
  texture->base.format.pitch    = 40;
  texture->base.format.bpp      = 4;
  texture->base.format.format   = GL_RGBA;
  texture->base.format.dataType = GL_UNSIGNED_BYTE;
  texture->base.frameToken      = LG_RENDERER_FRAME_TOKEN_NONE;
  texture->texCount             = 1;
  texture->tex[0]               = 17;
  texture->buf[0].mode          = mode;
  texture->buf[0].pbo           =
    mode == EGL_TEX_BUFFER_MODE_PERSISTENT ? 23 : 0;
  texture->buf[0].map           = mappedStorage;
  texture->buf[0].size          = sizeof(mappedStorage);
  texture->buf[0].updated       = true;
  texture->slotToken[0]         = 42;
  texture->rIndex               = -1;
  LG_LOCK_INIT(texture->copyLock);
}

static void checkLockReleased(TextureBuffer * texture)
{
  const bool locked = atomic_flag_test_and_set_explicit(
      &texture->copyLock, memory_order_acquire);
  CHECK(!locked);
  atomic_flag_clear_explicit(&texture->copyLock, memory_order_release);
}

static void testDirect(void)
{
  reset();
  egl_texUtilSetBufferMode(EGL_TEX_BUFFER_MODE_DIRECT);

  EGL_TexBuffer       buffers[2] = { 0 };
  const EGL_TexFormat format     = testFormat();
  CHECK(egl_texUtilGenBuffers(&format, buffers, 2));
  CHECK(buffers[0].mode == EGL_TEX_BUFFER_MODE_DIRECT);
  CHECK(buffers[1].mode == EGL_TEX_BUFFER_MODE_DIRECT);
  CHECK(buffers[0].map);
  CHECK(buffers[1].map);
  CHECK(!buffers[0].pbo);
  CHECK(!buffers[1].pbo);
  CHECK(trace.storage == 0);
  CHECK(trace.map == 0);

  egl_texUtilFreeBuffers(buffers, 2);
  CHECK(!buffers[0].map);
  CHECK(!buffers[1].map);
  CHECK(trace.deleted == 0);
}

static void testPersistent(void)
{
  reset();
  egl_texUtilSetBufferMode(EGL_TEX_BUFFER_MODE_PERSISTENT);

  EGL_TexBuffer       buffer = { 0 };
  const EGL_TexFormat format = testFormat();
  CHECK(egl_texUtilGenBuffers(&format, &buffer, 1));
  CHECK(buffer.mode == EGL_TEX_BUFFER_MODE_PERSISTENT);
  CHECK(buffer.map == mappedStorage);
  CHECK(buffer.pbo == 1);
  CHECK(trace.storage == 1);
  CHECK(trace.map == 1);

  egl_texUtilFreeBuffers(&buffer, 1);
  CHECK(!buffer.map);
  CHECK(!buffer.pbo);
  CHECK(trace.unmap == 1);
  CHECK(trace.deleted == 1);
}

static void testFallback(void)
{
  reset();
  trace.failMap = true;
  egl_texUtilSetBufferMode(EGL_TEX_BUFFER_MODE_PERSISTENT);

  EGL_TexBuffer       buffer = { 0 };
  const EGL_TexFormat format = testFormat();
  CHECK(egl_texUtilGenBuffers(&format, &buffer, 1));
  CHECK(buffer.mode == EGL_TEX_BUFFER_MODE_DIRECT);
  CHECK(buffer.map);
  CHECK(!buffer.pbo);
  CHECK(trace.storage == 1);
  CHECK(trace.map == 1);
  CHECK(trace.unmap == 0);
  CHECK(trace.deleted == 1);

  egl_texUtilFreeBuffers(&buffer, 1);
  CHECK(!buffer.map);

  reset();
  EGL_TexBuffer next = { 0 };
  CHECK(egl_texUtilGenBuffers(&format, &next, 1));
  CHECK(next.mode == EGL_TEX_BUFFER_MODE_PERSISTENT);
  CHECK(next.map == mappedStorage);
  CHECK(next.pbo == 1);
  egl_texUtilFreeBuffers(&next, 1);
}

static void testDirectFull(void)
{
  reset();

  TextureBuffer texture;
  initTexture(&texture, EGL_TEX_BUFFER_MODE_DIRECT);
  texture.damage[0].full = true;

  CHECK(egl_texBufferStreamProcess(&texture.base, 42) ==
      EGL_TEX_STATUS_UPDATED);
  CHECK(trace.bufferBinds == 1);
  CHECK(trace.boundPBO == 0);
  CHECK(trace.textureBinds == 1);
  CHECK(trace.boundTexture == texture.tex[0]);
  CHECK(trace.unpackAlignment == 1);
  CHECK(trace.unpackRowLength == (GLint)texture.base.format.stride);
  CHECK(trace.uploads == 1);
  CHECK(trace.uploadX == 0);
  CHECK(trace.uploadY == 0);
  CHECK(trace.uploadWidth == (GLsizei)texture.base.format.width);
  CHECK(trace.uploadHeight == (GLsizei)texture.base.format.height);
  CHECK(trace.uploadData == mappedStorage);
  CHECK(trace.fences == 0);
  CHECK(trace.finishes == 0);
  CHECK(trace.flushes == 0);
  CHECK(!texture.buf[0].updated);
  CHECK(!texture.damage[0].full);
  CHECK(texture.damage[0].count == 0);
  CHECK(texture.rIndex == 0);
  CHECK(texture.bufIndex == 0);
  CHECK(texture.base.frameToken == 42);
  checkLockReleased(&texture);
}

static void testDirectRect(void)
{
  reset();

  TextureBuffer texture;
  initTexture(&texture, EGL_TEX_BUFFER_MODE_DIRECT);
  texture.damage[0].count           = 1;
  texture.damage[0].rects[0].x      = 2;
  texture.damage[0].rects[0].y      = 1;
  texture.damage[0].rects[0].width  = 3;
  texture.damage[0].rects[0].height = 2;

  const size_t offset =
    texture.damage[0].rects[0].y * texture.base.format.pitch +
    texture.damage[0].rects[0].x * texture.base.format.bpp;

  CHECK(egl_texBufferStreamProcess(&texture.base, 42) ==
      EGL_TEX_STATUS_UPDATED);
  CHECK(trace.bufferBinds == 1);
  CHECK(trace.boundPBO == 0);
  CHECK(trace.textureBinds == 1);
  CHECK(trace.boundTexture == texture.tex[0]);
  CHECK(trace.unpackAlignment == 1);
  CHECK(trace.unpackRowLength == (GLint)texture.base.format.stride);
  CHECK(trace.uploads == 1);
  CHECK(trace.uploadX == 2);
  CHECK(trace.uploadY == 1);
  CHECK(trace.uploadWidth == 3);
  CHECK(trace.uploadHeight == 2);
  CHECK(trace.uploadData == mappedStorage + offset);
  CHECK(trace.fences == 0);
  CHECK(trace.finishes == 0);
  CHECK(trace.flushes == 0);
  CHECK(!texture.buf[0].updated);
  CHECK(!texture.damage[0].full);
  CHECK(texture.damage[0].count == 0);
  CHECK(texture.rIndex == 0);
  CHECK(texture.bufIndex == 0);
  CHECK(texture.base.frameToken == 42);
  checkLockReleased(&texture);
}

static void testPersistentFull(void)
{
  reset();

  TextureBuffer texture;
  initTexture(&texture, EGL_TEX_BUFFER_MODE_PERSISTENT);
  texture.damage[0].full = true;

  CHECK(egl_texBufferStreamProcess(&texture.base, 42) ==
      EGL_TEX_STATUS_UPDATED);
  CHECK(trace.bufferBinds == 1);
  CHECK(trace.boundPBO == texture.buf[0].pbo);
  CHECK(trace.textureBinds == 1);
  CHECK(trace.boundTexture == texture.tex[0]);
  CHECK(trace.unpackAlignment == 1);
  CHECK(trace.unpackRowLength == (GLint)texture.base.format.stride);
  CHECK(trace.uploads == 1);
  CHECK(trace.uploadX == 0);
  CHECK(trace.uploadY == 0);
  CHECK(trace.uploadWidth == (GLsizei)texture.base.format.width);
  CHECK(trace.uploadHeight == (GLsizei)texture.base.format.height);
  CHECK(!trace.uploadData);
  CHECK(trace.fences == 1);
  CHECK(trace.finishes == 0);
  CHECK(trace.flushes == 1);
  CHECK(texture.sync[0] == (GLsync)(uintptr_t)1);
  CHECK(!texture.buf[0].updated);
  CHECK(!texture.damage[0].full);
  CHECK(texture.damage[0].count == 0);
  CHECK(texture.rIndex == 0);
  CHECK(texture.bufIndex == 0);
  CHECK(texture.base.frameToken == 42);
  checkLockReleased(&texture);
}

static void testPersistentRect(void)
{
  reset();

  TextureBuffer texture;
  initTexture(&texture, EGL_TEX_BUFFER_MODE_PERSISTENT);
  texture.damage[0].count           = 1;
  texture.damage[0].rects[0].x      = 2;
  texture.damage[0].rects[0].y      = 1;
  texture.damage[0].rects[0].width  = 3;
  texture.damage[0].rects[0].height = 2;

  const uintptr_t offset =
    texture.damage[0].rects[0].y * texture.base.format.pitch +
    texture.damage[0].rects[0].x * texture.base.format.bpp;

  CHECK(egl_texBufferStreamProcess(&texture.base, 42) ==
      EGL_TEX_STATUS_UPDATED);
  CHECK(trace.bufferBinds == 1);
  CHECK(trace.boundPBO == texture.buf[0].pbo);
  CHECK(trace.textureBinds == 1);
  CHECK(trace.boundTexture == texture.tex[0]);
  CHECK(trace.unpackAlignment == 1);
  CHECK(trace.unpackRowLength == (GLint)texture.base.format.stride);
  CHECK(trace.uploads == 1);
  CHECK(trace.uploadX == 2);
  CHECK(trace.uploadY == 1);
  CHECK(trace.uploadWidth == 3);
  CHECK(trace.uploadHeight == 2);
  CHECK(trace.uploadData == (const void *)offset);
  CHECK(trace.fences == 1);
  CHECK(trace.finishes == 0);
  CHECK(trace.flushes == 1);
  CHECK(texture.sync[0] == (GLsync)(uintptr_t)1);
  CHECK(!texture.buf[0].updated);
  CHECK(!texture.damage[0].full);
  CHECK(texture.damage[0].count == 0);
  CHECK(texture.rIndex == 0);
  CHECK(texture.bufIndex == 0);
  CHECK(texture.base.frameToken == 42);
  checkLockReleased(&texture);
}

struct Test
{
  const char * name;
  void       (*run)(void);
};

static const struct Test tests[] =
{
  { "direct"          , testDirect         },
  { "persistent"      , testPersistent     },
  { "fallback"        , testFallback       },
  { "direct-full"     , testDirectFull     },
  { "direct-rect"     , testDirectRect     },
  { "persistent-full" , testPersistentFull },
  { "persistent-rect" , testPersistentRect },
};

int main(int argc, char ** argv)
{
  CHECK(argc == 2);
  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (!strcmp(argv[1], tests[i].name))
    {
      tests[i].run();
      return EXIT_SUCCESS;
    }

  return EXIT_FAILURE;
}
