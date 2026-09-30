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
#include "state.h"

#include "egldebug.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/locking.h"

#include <stdlib.h>
#include <string.h>

#define EGL_EXTERNAL_RETIRED_COUNT 4

struct ExternalTextureFrame
{
  GLuint                 texture;
  GLsync                 readySync;
  GLsync                 useSync;
  LG_RendererFrameToken  frameToken;
  LG_ExternalFrame     * externalFrame;
  LG_FrameReleaseFn      releaseFn;
  void                 * releaseOpaque;
  uint64_t               releaseHandle;
};

struct ExternalTextureRelease
{
  LG_ExternalFrame * externalFrame;
  LG_FrameReleaseFn  releaseFn;
  void             * releaseOpaque;
  uint64_t           releaseHandle;
};

typedef struct TexExternal
{
  EGL_Texture                 base;
  LG_Lock                     lock;
  struct ExternalTextureFrame pending;
  struct ExternalTextureFrame current;
  struct ExternalTextureFrame retired[EGL_EXTERNAL_RETIRED_COUNT];
  bool                        deactivateRequested;
}
TexExternal;

static void externalFrameTakeRelease(struct ExternalTextureFrame * frame,
    struct ExternalTextureRelease * release)
{
  release->externalFrame = frame->externalFrame;
  release->releaseFn     = frame->releaseFn;
  release->releaseOpaque = frame->releaseOpaque;
  release->releaseHandle = frame->releaseHandle;

  frame->externalFrame = NULL;
  frame->releaseFn     = NULL;
  frame->releaseOpaque = NULL;
  frame->releaseHandle = 0;
}

static void externalFrameInvokeReleases(
    struct ExternalTextureRelease * releases, int count)
{
  for (int i = 0; i < count; ++i)
  {
    lgExternalFrameRelease(releases[i].externalFrame);
    if (releases[i].releaseFn)
      releases[i].releaseFn(
          releases[i].releaseOpaque, releases[i].releaseHandle);
  }
}

static void externalFrameWait(GLsync * sync)
{
  if (!*sync)
    return;

  const GLenum result = glClientWaitSync(*sync, 0, GL_TIMEOUT_IGNORED);
  if (result == GL_WAIT_FAILED)
    DEBUG_FATAL("Failed to wait for an external texture fence");

  glDeleteSync(*sync);
  *sync = 0;
}

static bool externalFramePending(const struct ExternalTextureFrame * frame)
{
  return frame->externalFrame || frame->releaseFn;
}

static int externalFrameCleanupLocked(TexExternal * this,
    struct ExternalTextureRelease * releases)
{
  int releaseCount = 0;

  if (externalFramePending(&this->pending))
    externalFrameTakeRelease(
        &this->pending, &releases[releaseCount++]);

  externalFrameWait(&this->current.useSync);
  if (externalFramePending(&this->current))
    externalFrameTakeRelease(
        &this->current, &releases[releaseCount++]);

  for (int i = 0; i < ARRAY_LENGTH(this->retired); ++i)
  {
    externalFrameWait(&this->retired[i].useSync);
    if (externalFramePending(&this->retired[i]))
      externalFrameTakeRelease(
          &this->retired[i], &releases[releaseCount++]);
  }

  memset(&this->pending, 0, sizeof(this->pending));
  memset(&this->current, 0, sizeof(this->current));
  memset(this->retired, 0, sizeof(this->retired));
  this->base.frameToken     = LG_RENDERER_FRAME_TOKEN_NONE;
  this->deactivateRequested = false;

  return releaseCount;
}

static void egl_texExternalCleanup(EGL_Texture * texture)
{
  TexExternal * this = UPCAST(TexExternal, texture);
  struct ExternalTextureRelease releases[EGL_EXTERNAL_RETIRED_COUNT + 2];
  int                           releaseCount;

  LG_LOCK(this->lock);
  releaseCount = externalFrameCleanupLocked(this, releases);
  LG_UNLOCK(this->lock);
  externalFrameInvokeReleases(releases, releaseCount);
}

static bool egl_texExternalInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  (void)display;

  TexExternal * this = calloc(1, sizeof(*this));
  if (!this)
    return false;

  LG_LOCK_INIT(this->lock);
  this->base.type = type;
  *texture        = &this->base;
  return true;
}

static void egl_texExternalFree(EGL_Texture * texture)
{
  TexExternal * this = UPCAST(TexExternal, texture);

  egl_texExternalCleanup(texture);
  LG_LOCK_FREE(this->lock);
  free(this);
}

static bool egl_texExternalSetup(EGL_Texture * texture,
    const EGL_TexSetup * setup)
{
  (void)setup;
  egl_texExternalCleanup(texture);
  return true;
}

static bool egl_texExternalUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TexExternal * this = UPCAST(TexExternal, texture);

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_EXTERNAL);
  DEBUG_ASSERT(update->externalFrame);

  LG_ExternalFrameEGLTexture descriptor =
  {
    .version = LG_EXTERNAL_FRAME_EGL_TEXTURE_VERSION,
    .size    = sizeof(descriptor),
  };
  if (!lgExternalFrameQuery(update->externalFrame,
        LG_EXTERNAL_FRAME_EGL_TEXTURE, &descriptor, sizeof(descriptor)))
  {
    DEBUG_ERROR("Failed to query the external EGL texture");
    return false;
  }

  if (descriptor.version != LG_EXTERNAL_FRAME_EGL_TEXTURE_VERSION ||
      descriptor.size    <  sizeof(descriptor) ||
      !descriptor.texture || descriptor.target != (uint32_t)GL_TEXTURE_2D)
  {
    DEBUG_ERROR("Invalid external EGL texture descriptor");
    return false;
  }

  struct ExternalTextureFrame pending =
  {
    .texture       = (GLuint)descriptor.texture,
    .readySync     = (GLsync)descriptor.readySync,
    .frameToken    = update->frameToken,
    .externalFrame = lgExternalFrameRetain(update->externalFrame),
    .releaseFn     = update->releaseFn,
    .releaseOpaque = update->releaseOpaque,
    .releaseHandle = update->releaseHandle,
  };
  struct ExternalTextureRelease release = {0};
  bool                          haveRelease;

  LG_LOCK(this->lock);
  this->deactivateRequested = false;
  haveRelease               = externalFramePending(&this->pending);
  if (haveRelease)
    externalFrameTakeRelease(&this->pending, &release);
  this->pending             = pending;
  LG_UNLOCK(this->lock);

  if (haveRelease)
    externalFrameInvokeReleases(&release, 1);
  return true;
}

static void egl_texExternalPoll(EGL_Texture * texture)
{
  TexExternal * this = UPCAST(TexExternal, texture);
  struct ExternalTextureRelease releases[EGL_EXTERNAL_RETIRED_COUNT + 2];
  int                           releaseCount = 0;

  LG_LOCK(this->lock);
  if (this->deactivateRequested)
  {
    releaseCount = externalFrameCleanupLocked(this, releases);
    LG_UNLOCK(this->lock);
    externalFrameInvokeReleases(releases, releaseCount);
    return;
  }

  for (int i = 0; i < ARRAY_LENGTH(this->retired); ++i)
  {
    struct ExternalTextureFrame * retired = &this->retired[i];
    if (!retired->useSync)
      continue;

    const GLenum result = glClientWaitSync(retired->useSync, 0, 0);
    if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
    {
      glDeleteSync(retired->useSync);
      retired->useSync = 0;
      externalFrameTakeRelease(retired, &releases[releaseCount++]);
      memset(retired, 0, sizeof(*retired));
    }
    else if (result == GL_WAIT_FAILED)
      DEBUG_FATAL("Failed to poll an external texture fence");
  }
  LG_UNLOCK(this->lock);

  externalFrameInvokeReleases(releases, releaseCount);
}

static EGL_TexStatus egl_texExternalProcess(EGL_Texture * texture,
    LG_RendererFrameToken frameTokenLimit)
{
  TexExternal * this = UPCAST(TexExternal, texture);
  struct ExternalTextureRelease releases[2];
  int                           releaseCount = 0;
  bool                          haveTexture;
  bool                          updated      = false;

  egl_texExternalPoll(texture);

  LG_LOCK(this->lock);
  if (externalFramePending(&this->pending) &&
      egl_textureFrameAllowed(this->pending.frameToken, frameTokenLimit))
  {
    if (externalFramePending(&this->current))
    {
      if (this->current.useSync)
      {
        int index = -1;
        for (int i = 0; i < ARRAY_LENGTH(this->retired); ++i)
          if (!externalFramePending(&this->retired[i]))
          {
            index = i;
            break;
          }

        if (index < 0)
        {
          index = 0;
          externalFrameWait(&this->retired[index].useSync);
          externalFrameTakeRelease(
              &this->retired[index], &releases[releaseCount++]);
        }

        this->retired[index] = this->current;
      }
      else
        externalFrameTakeRelease(
            &this->current, &releases[releaseCount++]);
    }

    if (this->pending.readySync)
      glWaitSync(this->pending.readySync, 0, GL_TIMEOUT_IGNORED);
    this->pending.readySync = 0;

    this->current = this->pending;
    memset(&this->pending, 0, sizeof(this->pending));
    texture->frameToken = this->current.frameToken;
    updated             = true;
  }

  haveTexture = externalFramePending(&this->current);
  LG_UNLOCK(this->lock);

  externalFrameInvokeReleases(releases, releaseCount);

  if (!haveTexture)
    return EGL_TEX_STATUS_NOTREADY;
  return updated ? EGL_TEX_STATUS_UPDATED : EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texExternalGet(EGL_Texture * texture, GLuint * tex,
    EGL_PixelFormat * format)
{
  TexExternal * this = UPCAST(TexExternal, texture);
  GLuint        selected;

  LG_LOCK(this->lock);
  selected = this->current.texture;
  LG_UNLOCK(this->lock);

  if (!selected)
    return EGL_TEX_STATUS_NOTREADY;

  *tex = selected;
  if (format)
    *format = texture->format.pixFmt;
  return EGL_TEX_STATUS_OK;
}

static EGL_TexStatus egl_texExternalBind(EGL_Texture * texture, GLuint unit)
{
  GLuint        selected;
  EGL_TexStatus status = egl_texExternalGet(texture, &selected, NULL);
  if (status != EGL_TEX_STATUS_OK)
    return status;

  egl_stateBindTexture(unit, GL_TEXTURE_2D, selected);
  return EGL_TEX_STATUS_OK;
}

static void egl_texExternalReset(EGL_Texture * texture)
{
  egl_texExternalCleanup(texture);
}

static void egl_texExternalDeactivate(EGL_Texture * texture)
{
  TexExternal * this = UPCAST(TexExternal, texture);

  LG_LOCK(this->lock);
  this->deactivateRequested = true;
  LG_UNLOCK(this->lock);
}

static void egl_texExternalMarkUsed(EGL_Texture * texture)
{
  TexExternal * this = UPCAST(TexExternal, texture);

  LG_LOCK(this->lock);
  if (externalFramePending(&this->current))
  {
    if (this->current.useSync)
      glDeleteSync(this->current.useSync);
    this->current.useSync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!this->current.useSync)
    {
      DEBUG_GL_ERROR("Failed to fence external texture use");
      glFinish();
    }
    else
      glFlush();
  }
  LG_UNLOCK(this->lock);
}

const EGL_TextureOps EGL_TextureExternal =
{
  .init       = egl_texExternalInit,
  .free       = egl_texExternalFree,
  .setup      = egl_texExternalSetup,
  .update     = egl_texExternalUpdate,
  .process    = egl_texExternalProcess,
  .get        = egl_texExternalGet,
  .bind       = egl_texExternalBind,
  .reset      = egl_texExternalReset,
  .deactivate = egl_texExternalDeactivate,
  .poll       = egl_texExternalPoll,
  .markUsed   = egl_texExternalMarkUsed,
};
