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

#include "texture_buffer.h"
#include "state.h"

#include "egldebug.h"

#include <string.h>

// forwards
extern const EGL_TextureOps EGL_TextureBuffer;
extern const EGL_TextureOps EGL_TextureBufferStream;

// internal functions

static uint64_t egl_texBufferDamageArea(const FrameDamageRect * rect)
{
  return (uint64_t)rect->width * rect->height;
}

static FrameDamageRect egl_texBufferDamageUnion(
    const FrameDamageRect * a, const FrameDamageRect * b)
{
  const uint32_t left   = min(a->x, b->x);
  const uint32_t top    = min(a->y, b->y);
  const uint32_t right  = max(a->x + a->width, b->x + b->width);
  const uint32_t bottom = max(a->y + a->height, b->y + b->height);

  return (FrameDamageRect)
  {
    .x      = left,
    .y      = top,
    .width  = right  - left,
    .height = bottom - top,
  };
}

static bool egl_texBufferDamageTouches(const FrameDamageRect * a,
    const FrameDamageRect * b)
{
  return a->x <= b->x + b->width  && b->x <= a->x + a->width &&
         a->y <= b->y + b->height && b->y <= a->y + a->height;
}

static void egl_texBufferDamageAdd(TextureBuffer * this, int index,
    const EGL_TexUpdate * update)
{
  EGL_TexBufferDamage * damage = &this->damage[index];
  const EGL_TexFormat * format = &this->base.format;

  if (damage->full)
    return;

  if (update->x == 0 && update->y == 0 &&
      update->width  == format->width &&
      update->height == format->height)
  {
    damage->full  = true;
    damage->count = 0;
    return;
  }

  FrameDamageRect rect =
  {
    .x      = update->x,
    .y      = update->y,
    .width  = update->width,
    .height = update->height,
  };

  for (;;)
  {
    for (int i = 0; i < damage->count;)
    {
      if (!egl_texBufferDamageTouches(&rect, &damage->rects[i]))
      {
        ++i;
        continue;
      }

      rect = egl_texBufferDamageUnion(&rect, &damage->rects[i]);
      damage->rects[i] = damage->rects[--damage->count];
      i = 0;
    }

    if (damage->count < EGL_TEX_BUFFER_DAMAGE_MAX)
      break;

    int      best     = 0;
    uint64_t bestCost = UINT64_MAX;
    for (int i = 0; i < damage->count; ++i)
    {
      const FrameDamageRect merged =
        egl_texBufferDamageUnion(&rect, &damage->rects[i]);
      const uint64_t cost = egl_texBufferDamageArea(&merged) -
        egl_texBufferDamageArea(&damage->rects[i]);
      if (cost < bestCost)
      {
        best     = i;
        bestCost = cost;
      }
    }

    rect = egl_texBufferDamageUnion(&rect, &damage->rects[best]);
    damage->rects[best] = damage->rects[--damage->count];
  }

  damage->rects[damage->count++] = rect;

  uint64_t area = 0;
  for (int i = 0; i < damage->count; ++i)
    area += egl_texBufferDamageArea(&damage->rects[i]);

  const uint64_t fullArea = (uint64_t)format->width * format->height;
  if (area * 4 >= fullArea * 3)
  {
    damage->full  = true;
    damage->count = 0;
  }
}

static void egl_texBufferDamageReset(EGL_TexBufferDamage * damage)
{
  damage->full  = false;
  damage->count = 0;
}

static void egl_texBuffer_cleanup(TextureBuffer * this)
{
  egl_texUtilFreeBuffers(this->buf, this->texCount);

  if (this->tex[0])
  {
    glDeleteTextures(this->texCount, this->tex);
    egl_stateInvalidateShared();
  }

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    if (!this->sync[i])
      continue;

    glDeleteSync(this->sync[i]);
    this->sync[i] = 0;
  }
}

// common functions

bool egl_texBufferInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  TextureBuffer * this;
  if (!*texture)
  {
    this = calloc(1, sizeof(*this));
    if (!this)
    {
      DEBUG_ERROR("Failed to malloc TexB");
      return false;
    }
    *texture = &this->base;
    this->free = true;
  }
  else
    this = UPCAST(TextureBuffer, *texture);

  this->texCount = 1;
  return true;
}

void egl_texBufferFree(EGL_Texture * texture)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  egl_texBuffer_cleanup(this);
  LG_LOCK_FREE(this->copyLock);

  if (this->free)
    free(this);
}

bool egl_texBufferSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  egl_texBuffer_cleanup(this);

  glGenTextures(this->texCount, this->tex);
  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  for(int i = 0; i < this->texCount; ++i)
  {
    egl_stateBindTexture(0, GL_TEXTURE_2D, this->tex[i]);
    glTexImage2D(GL_TEXTURE_2D,
        0,
        texture->format.intFormat,
        texture->format.width,
        texture->format.height,
        0,
        texture->format.format,
        texture->format.dataType,
        NULL);
  }

  this->bufIndex      = 0;
  this->rIndex        = -1;
  texture->frameToken = LG_RENDERER_FRAME_TOKEN_NONE;

  for (int i = 0; i < this->texCount; ++i)
  {
    this->buf[i].updated = false;
    this->slotToken[i]   = LG_RENDERER_FRAME_TOKEN_NONE;
    egl_texBufferDamageReset(&this->damage[i]);
  }

  return true;
}

static bool egl_texBufferUpdate(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  DEBUG_ASSERT(update->type == EGL_TEXTYPE_BUFFER);

  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  egl_stateBindTexture(0, GL_TEXTURE_2D, this->tex[0]);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, update->stride);
  glTexSubImage2D(GL_TEXTURE_2D,
      0,
      update->x,
      update->y,
      update->width,
      update->height,
      texture->format.format,
      texture->format.dataType,
      update->buffer);
  return true;
}

EGL_TexStatus egl_texBufferProcess(EGL_Texture * texture,
    LG_RendererFrameToken frameTokenLimit)
{
  (void)frameTokenLimit;
  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus egl_texBufferGet(EGL_Texture * texture, GLuint * tex,
    EGL_PixelFormat * fmt)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  *tex = this->tex[0];
  return EGL_TEX_STATUS_OK;
}

// streaming functions

bool egl_texBufferStreamInit(EGL_Texture ** texture, EGL_TexType type,
    EGLDisplay * display)
{
  if (!egl_texBufferInit(texture, type, display))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, *texture);

  switch(type)
  {
    case EGL_TEXTYPE_BUFFER_STREAM:
    case EGL_TEXTYPE_FRAMEBUFFER:
      this->texCount = 2;
      break;

    case EGL_TEXTYPE_DMABUF:
      this->texCount = LGMP_Q_FRAME_BUFFER_LEN;
      break;

    case EGL_TEXTYPE_BUFFER_MAP:
      this->texCount = 1;
      break;

    default:
      DEBUG_UNREACHABLE();
  }

  LG_LOCK_INIT(this->copyLock);
  return true;
}

bool egl_texBufferStreamSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  if (!egl_texBufferSetup(texture, setup))
    return false;

  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  return egl_texUtilGenBuffers(&texture->format, this->buf, this->texCount);
}

bool egl_texBufferStreamLock(TextureBuffer * this)
{
  for (;;)
  {
    LG_LOCK(this->copyLock);

    GLsync sync = this->sync[this->bufIndex];
    if (!sync)
      return true;

    /* The slot cannot be submitted again until this function marks it
     * updated, so ownership of its fence can be transferred while unlocked. */
    this->sync[this->bufIndex] = 0;
    LG_UNLOCK(this->copyLock);

    GLenum result = glClientWaitSync(sync, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(sync);

    switch(result)
    {
      case GL_ALREADY_SIGNALED:
      case GL_CONDITION_SATISFIED:
        break;

      case GL_TIMEOUT_EXPIRED:
        DEBUG_ERROR("Upload buffer sync unexpectedly timed out");
        return false;

      case GL_WAIT_FAILED:
      case GL_INVALID_VALUE:
        DEBUG_GL_ERROR("glClientWaitSync failed while reusing upload buffer");
        return false;
    }
  }
}

static bool egl_texBufferStreamUpdate(EGL_Texture * texture,
    const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  DEBUG_ASSERT(update->type == EGL_TEXTYPE_BUFFER);

  if (!egl_texBufferStreamLock(this))
    return false;

  uint8_t * dst = this->buf[this->bufIndex].map +
    texture->format.pitch * update->y +
    update->x * texture->format.bpp;

  if (update->topDown)
  {
    const uint8_t * src = update->buffer;
    for(int y = 0; y < update->height; ++y)
    {
      memcpy(dst, src, update->width * texture->format.bpp);
      dst += texture->format.pitch;
      src += update->pitch;
    }
  }
  else
  {
    const uint8_t * src = update->buffer + update->pitch * update->height;
    for(int y = 0; y < update->height; ++y)
    {
      src -= update->pitch;
      memcpy(dst, src, update->width * texture->format.bpp);
      dst += texture->format.pitch;
    }
  }

  this->slotToken[this->bufIndex]   = update->frameToken;
  this->buf[this->bufIndex].updated = true;
  egl_texBufferDamageAdd(this, this->bufIndex, update);
  LG_UNLOCK(this->copyLock);

  return true;
}

EGL_TexStatus egl_texBufferStreamProcess(EGL_Texture * texture,
    LG_RendererFrameToken frameTokenLimit)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  LG_LOCK(this->copyLock);

  const int       index  = this->bufIndex;
  GLuint          tex    = this->tex[index];
  EGL_TexBuffer * buffer = &this->buf[index];

  if (!buffer->updated ||
      !egl_textureFrameAllowed(this->slotToken[index], frameTokenLimit))
  {
    LG_UNLOCK(this->copyLock);
    return EGL_TEX_STATUS_OK;
  }

  DEBUG_ASSERT(!this->sync[index]);

  this->rIndex        = index;
  texture->frameToken = this->slotToken[index];
  if (++this->bufIndex == this->texCount)
    this->bufIndex = 0;
  buffer->updated = false;

  /* With more than one slot the producer can fill the next PBO while this
   * upload is queued. A single-slot texture must remain locked until its
   * fence exists so the mapped buffer cannot be overwritten concurrently. */
  const bool keepLocked = this->texCount == 1;
  if (!keepLocked)
    LG_UNLOCK(this->copyLock);

  egl_stateBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer->pbo);
  egl_stateBindTexture(0, GL_TEXTURE_2D, tex);

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->format.stride);

  EGL_TexBufferDamage * damage = &this->damage[index];
  if (this->texCount > 1 || damage->full || !damage->count)
    glTexSubImage2D(GL_TEXTURE_2D,
        0, 0, 0,
        texture->format.width,
        texture->format.height,
        texture->format.format,
        texture->format.dataType,
        (const void *)0);
  else
    for (int i = 0; i < damage->count; ++i)
    {
      const FrameDamageRect * rect = &damage->rects[i];
      const uintptr_t offset =
        (uintptr_t)rect->y * texture->format.pitch +
        (uintptr_t)rect->x * texture->format.bpp;
      glTexSubImage2D(GL_TEXTURE_2D,
          0,
          rect->x,
          rect->y,
          rect->width,
          rect->height,
          texture->format.format,
          texture->format.dataType,
          (const void *)offset);
    }

  /* Keep damage intact until all of its upload commands have been issued. */
  egl_texBufferDamageReset(damage);
  this->sync[index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  if (unlikely(!this->sync[index]))
  {
    DEBUG_GL_ERROR("Failed to create upload buffer sync");
    glFinish();
    if (keepLocked)
      LG_UNLOCK(this->copyLock);
    return EGL_TEX_STATUS_ERROR;
  }

  /* A different shared context may wait when this PBO is reused. Flushing
   * here makes the fence visible without making the render thread wait. */
  glFlush();

  if (keepLocked)
    LG_UNLOCK(this->copyLock);

  return EGL_TEX_STATUS_UPDATED;
}

EGL_TexStatus egl_texBufferStreamGet(EGL_Texture * texture, GLuint * tex,
    EGL_PixelFormat * fmt)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);

  if (this->rIndex == -1)
    return EGL_TEX_STATUS_NOTREADY;

  /* Upload and sampling are ordered in the same context. Waiting here would
   * only stall submission of the draw and delay presentation. */
  *tex = this->tex[this->rIndex];
  return EGL_TEX_STATUS_OK;
}

EGL_TexStatus egl_texBufferBind(EGL_Texture * texture, GLuint unit)
{
  GLuint tex;
  EGL_TexStatus status;

  if ((status = texture->ops.get(texture, &tex, NULL)) != EGL_TEX_STATUS_OK)
    return status;

  egl_stateBindTexture(unit, GL_TEXTURE_2D, tex);
  return EGL_TEX_STATUS_OK;
}


const EGL_TextureOps EGL_TextureBuffer =
{
  .init        = egl_texBufferInit,
  .free        = egl_texBufferFree,
  .setup       = egl_texBufferSetup,
  .update      = egl_texBufferUpdate,
  .process     = egl_texBufferProcess,
  .get         = egl_texBufferGet,
  .bind        = egl_texBufferBind
};

const EGL_TextureOps EGL_TextureBufferStream =
{
  .init        = egl_texBufferStreamInit,
  .free        = egl_texBufferFree,
  .setup       = egl_texBufferStreamSetup,
  .update      = egl_texBufferStreamUpdate,
  .process     = egl_texBufferStreamProcess,
  .get         = egl_texBufferStreamGet,
  .bind        = egl_texBufferBind
};
