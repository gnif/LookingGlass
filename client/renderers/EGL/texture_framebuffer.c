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

#include "texture.h"

#include <assert.h>

#include "texture_buffer.h"
#include "common/debug.h"
#include "common/KVMFR.h"
#include "common/rects.h"

struct TexDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
};

typedef struct TexFB
{
  TextureBuffer base;
  struct TexDamage damage[EGL_TEX_BUFFER_MAX];
}
TexFB;

static bool eglTexFB_init(EGL_Texture ** texture, EGLDisplay * display)
{
  TexFB * this = calloc(sizeof(*this), 1);
  *texture = &this->base.base;

  EGL_Texture * parent = &this->base.base;
  if (!eglTexBuffer_init(&parent, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
    this->damage[i].count = -1;

  return true;
}

static bool eglTexFB_update(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * this = UPCAST(TextureBuffer, texture);
  TexFB * fb = UPCAST(TexFB, this);
  assert(update->type == EGL_TEXTYPE_FRAMEBUFFER);

  LG_LOCK(this->copyLock);

  struct TexDamage * damage = fb->damage + this->bufIndex;
  bool damageAll = !update->rects || update->rectCount == 0 || damage->count < 0 ||
    damage->count + update->rectCount > KVMFR_MAX_DAMAGE_RECTS;

  if (damageAll)
    framebuffer_read(
      update->frame,
      this->buf[this->bufIndex].map,
      this->format.stride,
      this->format.height,
      this->format.width,
      this->format.bpp,
      this->format.stride
    );
  else
  {
    memcpy(damage->rects + damage->count, update->rects,
      update->rectCount * sizeof(FrameDamageRect));
    damage->count += update->rectCount;
    rectsFramebufferToBuffer(
      damage->rects,
      damage->count,
      this->buf[this->bufIndex].map,
      this->format.stride,
      this->format.height,
      update->frame,
      this->format.stride
    );
  }

  this->buf[this->bufIndex].updated = true;

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    struct TexDamage * damage = fb->damage + i;
    if (i == this->bufIndex)
      damage->count = 0;
    else if (update->rects && update->rectCount > 0 && damage->count >= 0 &&
             damage->count + update->rectCount <= KVMFR_MAX_DAMAGE_RECTS)
    {
      memcpy(damage->rects + damage->count, update->rects,
        update->rectCount * sizeof(FrameDamageRect));
      damage->count += update->rectCount;
    }
    else
      damage->count = -1;
  }

  LG_UNLOCK(this->copyLock);

  return true;
}

EGL_TextureOps EGL_TextureFrameBuffer =
{
  .init        = eglTexFB_init,
  .free        = eglTexBuffer_free,
  .setup       = eglTexBuffer_stream_setup,
  .update      = eglTexFB_update,
  .process     = eglTexBuffer_stream_process,
  .bind        = eglTexBuffer_stream_bind
};
