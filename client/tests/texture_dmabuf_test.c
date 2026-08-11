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

#include "../renderers/EGL/texture_dmabuf.c"

#define MAX_SYNC 64U
#define MAX_REL  32U

struct FakeSync
{
  GLenum state;
  bool   deleted;
};

struct Trace
{
  struct FakeSync sync[MAX_SYNC];
  unsigned int    syncN;
  unsigned int    poll;
  unsigned int    wait;
  unsigned int    serverWait;
  unsigned int    delSync;
  unsigned int    finish;
  unsigned int    flush;
  unsigned int    imageNew;
  unsigned int    imageFree;
  unsigned int    invalidate;
  unsigned int    nextObj;
  unsigned int    failFence;
};

struct Releases
{
  unsigned int count;
  uint64_t     handle[MAX_REL];
};

static struct Trace t;

struct EGLDynProcs g_egl_dynProcs;
_Thread_local EGL_StateCache g_eglState;

const char b_shader_hdr_compose_vert[]          = "";
const char b_shader_hdr_compose_vert_end[]      = "";
const char b_shader_downscale_linear_frag[]     = "";
const char b_shader_downscale_linear_frag_end[] = "";

static struct FakeSync * syncPtr(GLsync sync)
{
  CHECK(sync);
  return (struct FakeSync *)sync;
}

static GLsync newSync(GLenum state)
{
  CHECK(t.syncN < MAX_SYNC);
  struct FakeSync * sync = &t.sync[t.syncN++];
  sync->state             = state;
  sync->deleted           = false;
  return (GLsync)sync;
}

static void signalSync(GLsync sync)
{
  struct FakeSync * fake = syncPtr(sync);
  CHECK(!fake->deleted);
  fake->state = GL_ALREADY_SIGNALED;
}

GLsync glFenceSync(GLenum condition, GLbitfield flags)
{
  CHECK(condition == GL_SYNC_GPU_COMMANDS_COMPLETE);
  if (t.failFence)
  {
    --t.failFence;
    return NULL;
  }
  return newSync(GL_TIMEOUT_EXPIRED);
}

GLenum glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
  struct FakeSync * fake = syncPtr(sync);
  CHECK(!fake->deleted);
  if (timeout == 0)
  {
    ++t.poll;
    return fake->state;
  }

  CHECK(timeout == GL_TIMEOUT_IGNORED);
  ++t.wait;
  return fake->state == GL_WAIT_FAILED ?
    GL_WAIT_FAILED : GL_CONDITION_SATISFIED;
}

void glWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout)
{
  struct FakeSync * fake = syncPtr(sync);
  CHECK(!fake->deleted);
  CHECK(timeout == GL_TIMEOUT_IGNORED);
  ++t.serverWait;
}

void glDeleteSync(GLsync sync)
{
  struct FakeSync * fake = syncPtr(sync);
  CHECK(!fake->deleted);
  fake->deleted = true;
  ++t.delSync;
}

void glFinish(void)
{
  ++t.finish;
}

void glFlush(void)
{
  ++t.flush;
}

void glGenTextures(GLsizei count, GLuint * textures)
{
  for (GLsizei i = 0; i < count; ++i)
    textures[i] = ++t.nextObj;
}

void glGenSamplers(GLsizei count, GLuint * samplers)
{
  for (GLsizei i = 0; i < count; ++i)
    samplers[i] = ++t.nextObj;
}

void glSamplerParameteri(GLuint sampler, GLenum pname, GLint param)
{
}

void glTexImage2D(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void * pixels)
{
}

void glGenFramebuffers(GLsizei count, GLuint * framebuffers)
{
  for (GLsizei i = 0; i < count; ++i)
    framebuffers[i] = ++t.nextObj;
}

void glFramebufferTexture2D(GLenum target, GLenum attachment,
    GLenum textarget, GLuint texture, GLint level)
{
}

void glDrawBuffers(GLsizei count, const GLenum * buffers)
{
}

GLenum glCheckFramebufferStatus(GLenum target)
{
  return GL_FRAMEBUFFER_COMPLETE;
}

void glDeleteTextures(GLsizei count, const GLuint * textures)
{
}

void glDeleteFramebuffers(GLsizei count, const GLuint * framebuffers)
{
}

void glDeleteSamplers(GLsizei count, const GLuint * samplers)
{
}

void glActiveTexture(GLenum texture)
{
}

void glBindTexture(GLenum target, GLuint texture)
{
}

void glBindFramebuffer(GLenum target, GLuint framebuffer)
{
}

void glBindSampler(GLuint unit, GLuint sampler)
{
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
}

void glEnable(GLenum cap)
{
}

void glDisable(GLenum cap)
{
}

void glColorMask(GLboolean red, GLboolean green, GLboolean blue,
    GLboolean alpha)
{
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
}

static EGLImage fakeImage(EGLDisplay display, EGLContext context,
    EGLenum target, EGLClientBuffer buffer, const EGLAttrib * attribs)
{
  ++t.imageNew;
  return (EGLImage)(uintptr_t)t.imageNew;
}

static EGLBoolean fakeImageFree(EGLDisplay display, EGLImage image)
{
  ++t.imageFree;
  return EGL_TRUE;
}

static void fakeImageTarget(GLenum target, GLeglImageOES image)
{
}

const char * eglQueryString(EGLDisplay display, EGLint name)
{
  return "";
}

void egl_stateInvalidateShared(void)
{
  ++t.invalidate;
  memset(&g_eglState, 0, sizeof(g_eglState));
}

void egl_shaderUse(EGL_Shader * shader)
{
}

bool egl_shaderInit(EGL_Shader ** shader)
{
  *shader = (EGL_Shader *)(uintptr_t)1;
  return true;
}

bool egl_shaderCompile(EGL_Shader * shader, const char * vertex,
    size_t vertexSize, const char * fragment, size_t fragmentSize,
    bool useDMA, const EGL_ShaderDefine * defines)
{
  return true;
}

void egl_shaderAssocTextures(EGL_Shader * shader, int count)
{
}

void egl_shaderFree(EGL_Shader ** shader)
{
  *shader = NULL;
}

void egl_modelRender(EGL_Model * model)
{
}

bool egl_modelInit(EGL_Model ** model)
{
  *model = (EGL_Model *)(uintptr_t)1;
  return true;
}

void egl_modelSetDefault(EGL_Model * model, bool flipped)
{
}

void egl_modelSetShader(EGL_Model * model, EGL_Shader * shader)
{
}

void egl_modelFree(EGL_Model ** model)
{
  *model = NULL;
}

void egl_texBufferFree(EGL_Texture * texture)
{
}

bool egl_texBufferStreamInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  return true;
}

bool util_hasGLExt(const char * exts, const char * ext)
{
  return true;
}

const char * egl_getErrorStr(void)
{
  return "fake";
}

const char * gl_getErrorStr(void)
{
  return "fake";
}

static void onRelease(void * opaque, uint64_t handle)
{
  struct Releases * releases = opaque;
  CHECK(releases->count < MAX_REL);
  releases->handle[releases->count++] = handle;
}

static unsigned int relCount(
    const struct Releases * releases, uint64_t handle)
{
  unsigned int count = 0;
  for (unsigned int i = 0; i < releases->count; ++i)
    if (releases->handle[i] == handle)
      ++count;
  return count;
}

static TexDMABUF * newTex(void)
{
  TexDMABUF * texture = calloc(1, sizeof(*texture));
  CHECK(texture);

  t.nextObj = 100;
  texture->display                    = (EGLDisplay)(uintptr_t)1;
  texture->renderIndex                = -1;
  texture->base.rIndex                = -1;
  texture->base.base.type             = EGL_TEXTYPE_DMABUF;
  texture->base.base.format.pixFmt    = EGL_PF_RGBA;
  texture->base.base.format.width     = 4;
  texture->base.base.format.height    = 4;
  texture->base.base.format.pitch     = 16;
  texture->base.base.frameToken       = LG_RENDERER_FRAME_TOKEN_NONE;
  texture->pixFmt                     = EGL_PF_RGBA;
  texture->width                      = 4;
  texture->fourcc                     = DRM_FORMAT_ARGB8888;
  texture->intFormat                  = GL_RGBA8;
  texture->dataType                   = GL_UNSIGNED_BYTE;
  texture->copyShader                 = (EGL_Shader *)(uintptr_t)1;
  texture->copyModel                  = (EGL_Model *)(uintptr_t)1;
  texture->copySampler                = 1;
  LG_LOCK_INIT(texture->base.copyLock);

  for (int i = 0; i < ARRAY_LENGTH(texture->images); ++i)
  {
    texture->images[i].fd    = -1;
    texture->images[i].image = EGL_NO_IMAGE;
  }
  for (int i = 0; i < ARRAY_LENGTH(texture->snapshots); ++i)
  {
    texture->snapshots[i].texture     = 200 + i;
    texture->snapshots[i].framebuffer = 300 + i;
  }

  g_egl_dynProcs.eglCreateImage                = fakeImage;
  g_egl_dynProcs.eglDestroyImage               = fakeImageFree;
  g_egl_dynProcs.glEGLImageTargetTexture2DOES = fakeImageTarget;
  memset(&g_eglState, 0, sizeof(g_eglState));
  return texture;
}

static bool update(TexDMABUF * texture, LG_RendererFrameToken token,
    struct Releases * releases, uint64_t handle)
{
  const EGL_TexUpdate value =
  {
    .type          = EGL_TEXTYPE_DMABUF,
    .frameToken    = token,
    .releaseFn     = onRelease,
    .releaseOpaque = releases,
    .releaseHandle = handle,
    .dmaFD         = 7,
  };
  return egl_texDMABUFUpdate(&texture->base.base, &value);
}

static void setSlot(TexDMABUF * texture, int index,
    LG_RendererFrameToken token, bool pending)
{
  texture->snapshots[index].frameToken = token;
  texture->snapshots[index].copySync   = pending ?
    newSync(GL_TIMEOUT_EXPIRED) : NULL;
}

static void testSelect(void)
{
  TexDMABUF * texture = newTex();
  setSlot(texture, 0, 2, false);
  setSlot(texture, 1, 5, true);
  setSlot(texture, 2, 8, false);

  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 1) == EGL_TEX_STATUS_NOTREADY);
  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 6) == EGL_TEX_STATUS_UPDATED);
  CHECK(texture->renderIndex == 1);
  CHECK(texture->base.rIndex == 1);
  CHECK(texture->base.base.frameToken == 5);
  CHECK(t.serverWait == 1);

  GLuint          tex = 0;
  EGL_PixelFormat fmt = EGL_PF_BGRA;
  CHECK(egl_texDMABUFGet(
        &texture->base.base, &tex, &fmt) == EGL_TEX_STATUS_OK);
  CHECK(tex == texture->snapshots[1].texture);
  CHECK(fmt == EGL_PF_RGBA);
  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 6) == EGL_TEX_STATUS_OK);
  CHECK(t.serverWait == 1);

  setSlot(texture, 3, 6, false);
  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 6) == EGL_TEX_STATUS_UPDATED);
  CHECK(texture->renderIndex == 3);
  CHECK(texture->base.base.frameToken == 6);
  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 1) == EGL_TEX_STATUS_OK);
  CHECK(texture->base.base.frameToken == 6);

  egl_texDMABUFFree(&texture->base.base);
}

static void testPoll(void)
{
  struct Releases releases = { 0 };
  TexDMABUF * texture = newTex();
  CHECK(update(texture, 1, &releases, 11));
  CHECK(releases.count == 0);
  GLsync copy = texture->snapshots[0].copySync;

  egl_texDMABUFPoll(&texture->base.base);
  CHECK(releases.count == 0);
  CHECK(texture->snapshots[0].copySync == copy);
  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 1) == EGL_TEX_STATUS_UPDATED);
  CHECK(t.serverWait == 1);

  egl_texDMABUFMarkUsed(&texture->base.base);
  GLsync firstUse = texture->snapshots[0].useSync;
  CHECK(firstUse);
  egl_texDMABUFMarkUsed(&texture->base.base);
  CHECK(syncPtr(firstUse)->deleted);
  GLsync use = texture->snapshots[0].useSync;
  CHECK(use);

  signalSync(copy);
  signalSync(use);
  egl_texDMABUFPoll(&texture->base.base);
  CHECK(releases.count == 1);
  CHECK(releases.handle[0] == 11);
  CHECK(texture->snapshots[0].copySync == NULL);
  CHECK(texture->snapshots[0].useSync == NULL);
  egl_texDMABUFPoll(&texture->base.base);
  CHECK(releases.count == 1);

  t.failFence = 1;
  CHECK(update(texture, 2, &releases, 12));
  CHECK(t.finish == 1);
  CHECK(releases.count == 2);
  CHECK(relCount(&releases, 12) == 1);
  egl_texDMABUFReset(&texture->base.base);
  CHECK(releases.count == 2);
  egl_texDMABUFFree(&texture->base.base);
  CHECK(releases.count == 2);
}

static void testSaturation(void)
{
  struct Releases releases = { 0 };
  TexDMABUF * texture = newTex();
  CHECK(update(texture, 10, &releases, 10));
  CHECK(update(texture, 20, &releases, 20));
  CHECK(update(texture, 30, &releases, 30));
  CHECK(update(texture, 40, &releases, 40));
  CHECK(releases.count == 0);
  CHECK(t.imageNew == 1);

  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 40) == EGL_TEX_STATUS_UPDATED);
  CHECK(texture->renderIndex == 3);
  texture->snapshots[0].useSync = newSync(GL_TIMEOUT_EXPIRED);
  const unsigned int wait = t.wait;
  CHECK(update(texture, 50, &releases, 50));
  CHECK(t.wait == wait + 2);
  CHECK(relCount(&releases, 10) == 1);
  CHECK(texture->snapshots[0].frameToken == 50);
  CHECK(texture->snapshots[3].frameToken == 40);

  CHECK(egl_texDMABUFProcess(
        &texture->base.base, 50) == EGL_TEX_STATUS_UPDATED);
  CHECK(texture->renderIndex == 0);
  CHECK(update(texture, 60, &releases, 60));
  CHECK(relCount(&releases, 20) == 1);
  CHECK(texture->snapshots[1].frameToken == 60);

  egl_texDMABUFFree(&texture->base.base);
  CHECK(releases.count == 6);
  for (uint64_t handle = 10; handle <= 60; handle += 10)
    CHECK(relCount(&releases, handle) == 1);
}

static void testRelease(void)
{
  struct Releases releases = { 0 };
  TexDMABUF * first = newTex();
  CHECK(update(first, 1, &releases, 101));
  egl_texDMABUFReset(&first->base.base);
  CHECK(relCount(&releases, 101) == 1);
  egl_texDMABUFReset(&first->base.base);
  CHECK(relCount(&releases, 101) == 1);
  egl_texDMABUFFree(&first->base.base);
  CHECK(relCount(&releases, 101) == 1);

  TexDMABUF * second = newTex();
  CHECK(update(second, 2, &releases, 202));
  egl_texDMABUFFree(&second->base.base);
  CHECK(relCount(&releases, 202) == 1);
  CHECK(releases.count == 2);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "select"    , testSelect     },
  { "poll"      , testPoll       },
  { "saturation", testSaturation },
  { "release"   , testRelease    },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  debug_init();
  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
