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

#include "texture.h"
#include "texture_buffer.h"
#include "model.h"
#include "shader.h"
#include "state.h"
#include "util.h"

#include "common/array.h"
#include "egl_dynprocs.h"
#include "egldebug.h"

#include <string.h>

#include "hdr_compose.vert.h"
#include "downscale_linear.frag.h"

#define EGL_DMABUF_SNAPSHOT_COUNT (LGMP_Q_FRAME_BUFFER_LEN + 1)

struct FdImage
{
  int      fd;
  EGLImage image;
  GLuint   texture;
};

struct Snapshot
{
  GLuint                 texture;
  GLuint                 framebuffer;
  GLsync                 copySync;
  GLsync                 useSync;
  LG_RendererFrameToken  frameToken;
  LG_FrameReleaseFn      releaseFn;
  void                 * releaseOpaque;
  uint64_t               releaseHandle;
};

typedef struct TexDMABUF
{
  TextureBuffer base;

  EGLDisplay display;

  struct FdImage  images[EGL_TEX_BUFFER_MAX];
  struct Snapshot snapshots[EGL_DMABUF_SNAPSHOT_COUNT];
  int             renderIndex;

  EGL_Shader * copyShader;
  EGL_Model  * copyModel;
  GLuint       copySampler;

  EGL_PixelFormat pixFmt;
  unsigned        fourcc;
  unsigned        width;
  GLenum          intFormat;
  GLenum          dataType;
}
TexDMABUF;

struct SnapshotRelease
{
  LG_FrameReleaseFn fn;
  void            * opaque;
  uint64_t          handle;
};

EGL_TextureOps EGL_TextureDMABUF;

static bool initDone           = false;
static bool has24BitSupport    = true;
static bool hasImportModifiers = true;

// internal functions

static void egl_texDMABUFFree(EGL_Texture * texture);
static void egl_texDMABUFPoll(EGL_Texture * texture);

static void snapshotRelease(struct Snapshot * snapshot,
    struct SnapshotRelease * release)
{
  release->fn     = snapshot->releaseFn;
  release->opaque = snapshot->releaseOpaque;
  release->handle = snapshot->releaseHandle;

  snapshot->releaseFn     = NULL;
  snapshot->releaseOpaque = NULL;
  snapshot->releaseHandle = 0;
}

static void snapshotInvokeReleases(struct SnapshotRelease * releases,
    int count)
{
  for (int i = 0; i < count; ++i)
    releases[i].fn(releases[i].opaque, releases[i].handle);
}

static void waitSync(GLsync * sync)
{
  if (!*sync)
    return;

  const GLenum result = glClientWaitSync(*sync, 0, GL_TIMEOUT_IGNORED);
  if (result == GL_WAIT_FAILED)
    DEBUG_FATAL("Failed to wait for DMABUF snapshot fence");
  glDeleteSync(*sync);
  *sync = 0;
}

static void egl_texDMABUFCleanup(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  struct SnapshotRelease releases[EGL_DMABUF_SNAPSHOT_COUNT];
  int releaseCount = 0;

  LG_LOCK(parent->copyLock);

  for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
  {
    struct Snapshot * snapshot = &this->snapshots[i];
    waitSync(&snapshot->copySync);
    waitSync(&snapshot->useSync);
    if (snapshot->releaseFn)
      snapshotRelease(snapshot, &releases[releaseCount++]);

    if (snapshot->framebuffer)
      glDeleteFramebuffers(1, &snapshot->framebuffer);
    if (snapshot->texture)
      glDeleteTextures(1, &snapshot->texture);
    memset(snapshot, 0, sizeof(*snapshot));
  }

  for(int i = 0; i < ARRAY_LENGTH(this->images); ++i)
  {
    if (this->images[i].image != EGL_NO_IMAGE)
    {
      g_egl_dynProcs.eglDestroyImage(this->display, this->images[i].image);
      this->images[i].image = EGL_NO_IMAGE;
    }
    if (this->images[i].texture)
      glDeleteTextures(1, &this->images[i].texture);
    this->images[i].fd      = -1;
    this->images[i].texture = 0;
  }

  this->renderIndex   = -1;
  parent->rIndex      = -1;
  texture->frameToken = LG_RENDERER_FRAME_TOKEN_NONE;
  egl_stateInvalidateShared();

  LG_UNLOCK(parent->copyLock);
  snapshotInvokeReleases(releases, releaseCount);
}

// dmabuf functions

static bool egl_texDMABUFInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TexDMABUF * this = calloc(1, sizeof(*this));
  *texture = &this->base.base;

  for(int i = 0; i < ARRAY_LENGTH(this->images); ++i)
  {
    this->images[i].fd    = -1;
    this->images[i].image = EGL_NO_IMAGE;
  }
  this->renderIndex = -1;

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, type, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  this->display = display;

  if (!egl_shaderInit(&this->copyShader) ||
      !egl_shaderCompile(this->copyShader,
        b_shader_hdr_compose_vert, b_shader_hdr_compose_vert_size,
        b_shader_downscale_linear_frag, b_shader_downscale_linear_frag_size,
        true, NULL))
  {
    DEBUG_ERROR("Failed to initialize the DMABUF snapshot pass");
    egl_texDMABUFFree(*texture);
    *texture = NULL;
    return false;
  }

  egl_shaderAssocTextures(this->copyShader, 1);

  glGenSamplers(1, &this->copySampler);
  glSamplerParameteri(this->copySampler,
      GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glSamplerParameteri(this->copySampler,
      GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glSamplerParameteri(this->copySampler,
      GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glSamplerParameteri(this->copySampler,
      GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (!initDone)
  {
    const char * client_exts = eglQueryString(this->display, EGL_EXTENSIONS);
    hasImportModifiers =
      util_hasGLExt(client_exts, "EGL_EXT_image_dma_buf_import_modifiers");
    initDone = true;
  }

  return true;
}

static void egl_texDMABUFFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(texture);
  egl_modelFree(&this->copyModel);
  egl_shaderFree(&this->copyShader);
  if (this->copySampler)
    glDeleteSamplers(1, &this->copySampler);
  egl_texBufferFree(&parent->base);
  free(this);
}

static bool texDMABUFSetup(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  if (texture->format.pixFmt == EGL_PF_RGB_24 && !has24BitSupport)
  {
    this->pixFmt = EGL_PF_RGB_24_32;
    this->width  = texture->format.pitch / 4;
    this->fourcc = DRM_FORMAT_ARGB8888;
  }

  egl_texDMABUFCleanup(texture);

  if (!this->copyModel)
  {
    if (!egl_modelInit(&this->copyModel))
      return false;
    egl_modelSetDefault(this->copyModel, false);
    egl_modelSetShader(this->copyModel, this->copyShader);
  }

  for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
  {
    struct Snapshot * snapshot = &this->snapshots[i];

    glGenTextures(1, &snapshot->texture);
    egl_stateBindTexture(0, GL_TEXTURE_2D, snapshot->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, this->intFormat,
        this->width, texture->format.height, 0,
        GL_RGBA, this->dataType, NULL);

    glGenFramebuffers(1, &snapshot->framebuffer);
    egl_stateBindFramebuffer(snapshot->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D, snapshot->texture, 0);
    glDrawBuffers(1, &(GLenum){ GL_COLOR_ATTACHMENT0 });
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
      DEBUG_ERROR("Failed to create DMABUF snapshot framebuffer");
      egl_stateBindFramebuffer(0);
      egl_texDMABUFCleanup(texture);
      return false;
    }
  }

  egl_stateBindFramebuffer(0);
  parent->rIndex = -1;

  return true;
}

static bool egl_texDMABUFSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  this->pixFmt    = texture->format.pixFmt;
  this->width     = texture->format.width;
  this->fourcc    = texture->format.fourcc;
  this->dataType  = GL_UNSIGNED_BYTE;
  this->intFormat = GL_RGBA8;

  switch (texture->format.pixFmt)
  {
    case EGL_PF_RGBA10:
      this->intFormat = GL_RGB10_A2;
      this->dataType  = GL_UNSIGNED_INT_2_10_10_10_REV;
      break;

    case EGL_PF_RGBA16F:
      this->intFormat = GL_RGBA16F;
      this->dataType  = GL_HALF_FLOAT;
      break;

    case EGL_PF_RGB_24:
      /* A native RGB888 import is expanded by the snapshot shader. */
      this->pixFmt = EGL_PF_RGBA;
      break;

    default:
      break;
  }

  return texDMABUFSetup(texture);
}

static EGLImage createImage(EGL_Texture * texture, int fd)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  EGLAttrib attribs[] =
  {
    EGL_WIDTH                         , this->width ,
    EGL_HEIGHT                        , texture->format.height,
    EGL_LINUX_DRM_FOURCC_EXT          , this->fourcc,
    EGL_DMA_BUF_PLANE0_FD_EXT         , fd,
    EGL_DMA_BUF_PLANE0_OFFSET_EXT     , 0,
    EGL_DMA_BUF_PLANE0_PITCH_EXT      , texture->format.pitch,
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (modifier & 0xffffffff),
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (modifier >> 32),
    EGL_NONE                          , EGL_NONE
  };

  if (!hasImportModifiers)
    attribs[12] = attribs[13] =
    attribs[14] = attribs[15] = EGL_NONE;

  return g_egl_dynProcs.eglCreateImage(
      this->display,
      EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT,
      (EGLClientBuffer)NULL,
      attribs);
}

static bool egl_texDMABUFUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_DMABUF);

  /* Retire completed copies before deciding whether a snapshot slot must be
   * waited on and coalesced. */
  egl_texDMABUFPoll(texture);

  struct FdImage *fdImage = NULL;
  for (int i = 0; i < ARRAY_LENGTH(this->images); ++i)
    if (this->images[i].fd == update->dmaFD)
    {
      fdImage = &this->images[i];
      break;
    }

  if (!fdImage)
    for (int i = 0; i < ARRAY_LENGTH(this->images); ++i)
      if (this->images[i].fd == -1)
      {
        fdImage = &this->images[i];
        break;
      }

  if (!fdImage)
  {
    DEBUG_ERROR("No free DMABUF image slot");
    return false;
  }

  if (unlikely(fdImage->image == EGL_NO_IMAGE))
  {
    bool setup = false;
    if (texture->format.pixFmt == EGL_PF_RGB_24 && has24BitSupport)
    {
      fdImage->image = createImage(texture, update->dmaFD);
      if (fdImage->image == EGL_NO_IMAGE)
      {
        DEBUG_INFO("Using 24-bit in 32-bit for DMA");
        has24BitSupport = false;
        setup = true;
      }
    }

    if (!has24BitSupport && setup)
    {
      if (!texDMABUFSetup(texture))
        return false;
      fdImage = &this->images[0];
    }

    if (fdImage->image == EGL_NO_IMAGE)
      fdImage->image = createImage(texture, update->dmaFD);

    if (unlikely(fdImage->image == EGL_NO_IMAGE))
    {
      DEBUG_EGL_ERROR("Failed to create EGLImage for DMA transfer");
      return false;
    }

    fdImage->fd    = update->dmaFD;
    glGenTextures(1, &fdImage->texture);
    egl_stateBindTexture(0, GL_TEXTURE_EXTERNAL_OES, fdImage->texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
        GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
        GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
        GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
        GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_egl_dynProcs.glEGLImageTargetTexture2DOES(
        GL_TEXTURE_EXTERNAL_OES, fdImage->image);
  }

  struct SnapshotRelease releases[2];
  int releaseCount = 0;
  LG_LOCK(parent->copyLock);

  int index = -1;
  for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
    if (this->snapshots[i].frameToken == LG_RENDERER_FRAME_TOKEN_NONE)
    {
      index = i;
      break;
    }

  if (index < 0)
  {
    /* Drop the oldest snapshot that is no longer selected. If the GPU is
     * still finishing it, wait only when all snapshot slots are saturated. */
    for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
      if (i != this->renderIndex &&
          (index < 0 || this->snapshots[i].frameToken <
            this->snapshots[index].frameToken))
        index = i;

    if (unlikely(index < 0))
    {
      LG_UNLOCK(parent->copyLock);
      DEBUG_ERROR("No reusable DMABUF snapshot slot");
      return false;
    }

    struct Snapshot * old = &this->snapshots[index];
    waitSync(&old->copySync);
    waitSync(&old->useSync);
    if (old->releaseFn)
      snapshotRelease(old, &releases[releaseCount++]);
    old->frameToken = LG_RENDERER_FRAME_TOKEN_NONE;
  }

  struct Snapshot * snapshot = &this->snapshots[index];
  egl_stateBindFramebuffer(snapshot->framebuffer);
  egl_stateViewport(0, 0, this->width, texture->format.height);
  egl_stateBlend(false);
  egl_stateScissor(false);
  glDisable(GL_DITHER);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  egl_stateBindSampler(0, this->copySampler);
  egl_stateBindTexture(0, GL_TEXTURE_EXTERNAL_OES, fdImage->texture);
  egl_shaderUse(this->copyShader);
  egl_modelRender(this->copyModel);

  snapshot->copySync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (unlikely(!snapshot->copySync))
  {
    DEBUG_GL_ERROR("Failed to fence DMABUF snapshot copy");
    glFinish();
  }
  snapshot->frameToken    = update->frameToken;
  snapshot->releaseFn     = update->releaseFn;
  snapshot->releaseOpaque = update->releaseOpaque;
  snapshot->releaseHandle = update->releaseHandle;

  if (!snapshot->copySync && snapshot->releaseFn)
    snapshotRelease(snapshot, &releases[releaseCount++]);

  glFlush();
  LG_UNLOCK(parent->copyLock);

  snapshotInvokeReleases(releases, releaseCount);

  return true;
}

static void egl_texDMABUFPoll(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  struct SnapshotRelease releases[EGL_DMABUF_SNAPSHOT_COUNT];
  int releaseCount = 0;

  LG_LOCK(parent->copyLock);
  for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
  {
    struct Snapshot * snapshot = &this->snapshots[i];
    if (snapshot->copySync)
    {
      const GLenum result = glClientWaitSync(snapshot->copySync, 0, 0);
      if (result == GL_ALREADY_SIGNALED ||
          result == GL_CONDITION_SATISFIED)
      {
        glDeleteSync(snapshot->copySync);
        snapshot->copySync = 0;
        if (snapshot->releaseFn)
          snapshotRelease(snapshot, &releases[releaseCount++]);
      }
      else if (result == GL_WAIT_FAILED)
        DEBUG_FATAL("Failed to poll DMABUF snapshot copy fence");
    }

    if (snapshot->useSync)
    {
      const GLenum result = glClientWaitSync(snapshot->useSync, 0, 0);
      if (result == GL_ALREADY_SIGNALED ||
          result == GL_CONDITION_SATISFIED)
      {
        glDeleteSync(snapshot->useSync);
        snapshot->useSync = 0;
      }
      else if (result == GL_WAIT_FAILED)
        DEBUG_FATAL("Failed to poll DMABUF snapshot use fence");
    }
  }
  LG_UNLOCK(parent->copyLock);

  snapshotInvokeReleases(releases, releaseCount);
}

static EGL_TexStatus egl_texDMABUFProcess(EGL_Texture * texture,
    LG_RendererFrameToken frameTokenLimit)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFPoll(texture);

  int  index     = -1;
  bool haveImage = false;
  bool updated   = false;
  INTERLOCKED_SECTION(parent->copyLock,
  {
    haveImage = this->renderIndex >= 0;
    for (int i = 0; i < ARRAY_LENGTH(this->snapshots); ++i)
      if (this->snapshots[i].frameToken != LG_RENDERER_FRAME_TOKEN_NONE &&
          egl_textureFrameAllowed(
            this->snapshots[i].frameToken, frameTokenLimit) &&
          (index < 0 || this->snapshots[i].frameToken >
            this->snapshots[index].frameToken))
        index = i;

    if (index >= 0)
    {
      const struct Snapshot * pending = &this->snapshots[index];
      if (this->renderIndex != index ||
          texture->frameToken != pending->frameToken)
      {
        if (pending->copySync)
          glWaitSync(pending->copySync, 0, GL_TIMEOUT_IGNORED);
        this->renderIndex    = index;
        parent->rIndex       = index;
        texture->frameToken = pending->frameToken;
        updated              = true;
        haveImage            = true;
      }
    }
  });

  if (unlikely(!haveImage))
    return EGL_TEX_STATUS_NOTREADY;
  return updated ? EGL_TEX_STATUS_UPDATED : EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texDMABUFGet(EGL_Texture * texture, GLuint * tex,
    EGL_PixelFormat * fmt)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  int index = -1;

  INTERLOCKED_SECTION(parent->copyLock,
  {
    index = this->renderIndex;
  });

  if (unlikely(index < 0))
    return EGL_TEX_STATUS_NOTREADY;

  *tex = this->snapshots[index].texture;

  if (fmt)
    *fmt = this->pixFmt;

  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texDMABUFBind(EGL_Texture * texture, GLuint unit)
{
  GLuint tex;
  EGL_TexStatus status;

  if ((status = egl_texDMABUFGet(texture, &tex, NULL)) != EGL_TEX_STATUS_OK)
    return status;

  egl_stateBindTexture(unit, GL_TEXTURE_2D, tex);
  return EGL_TEX_STATUS_OK;
}

static void egl_texDMABUFReset(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  egl_texDMABUFCleanup(texture);
  egl_modelFree(&this->copyModel);
}

static void egl_texDMABUFMarkUsed(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexDMABUF     * this   = UPCAST(TexDMABUF    , parent);

  LG_LOCK(parent->copyLock);
  if (this->renderIndex >= 0)
  {
    struct Snapshot * snapshot = &this->snapshots[this->renderIndex];
    if (snapshot->useSync)
      glDeleteSync(snapshot->useSync);
    snapshot->useSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (unlikely(!snapshot->useSync))
    {
      DEBUG_GL_ERROR("Failed to fence DMABUF snapshot use");
      glFinish();
    }
    else
      glFlush();
  }
  LG_UNLOCK(parent->copyLock);
}

EGL_TextureOps EGL_TextureDMABUF =
{
  .init        = egl_texDMABUFInit,
  .free        = egl_texDMABUFFree,
  .setup       = egl_texDMABUFSetup,
  .update      = egl_texDMABUFUpdate,
  .process     = egl_texDMABUFProcess,
  .get         = egl_texDMABUFGet,
  .bind        = egl_texDMABUFBind,
  .reset       = egl_texDMABUFReset,
  .poll        = egl_texDMABUFPoll,
  .markUsed    = egl_texDMABUFMarkUsed,
};
