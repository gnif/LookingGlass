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

static bool egl_texFBInit(EGL_Texture ** texture, EGLDisplay * display)
{
  TexFB * this = calloc(1, sizeof(*this));
  *texture = &this->base.base;

  EGL_Texture * parent = &this->base.base;
  if (!egl_texBufferStreamInit(&parent, display))
  {
    free(this);
    *texture = NULL;
    return false;
  }

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
    this->damage[i].count = -1;

  return true;
}

void egl_texFBFree(EGL_Texture * texture)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  egl_texBufferFree(texture);
  free(this);
}

bool egl_texFBSetup(EGL_Texture * texture, const EGL_TexSetup * setup)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
    this->damage[i].count = -1;

  return egl_texBufferStreamSetup(texture, setup);
}

static bool egl_texFBUpdate(EGL_Texture * texture, const EGL_TexUpdate * update)
{
  TextureBuffer * parent = UPCAST(TextureBuffer, texture);
  TexFB         * this   = UPCAST(TexFB        , parent );

  DEBUG_ASSERT(update->type == EGL_TEXTYPE_FRAMEBUFFER);

  LG_LOCK(parent->copyLock);

  struct TexDamage * damage = this->damage + parent->bufIndex;
  bool damageAll = !update->rects || update->rectCount == 0 || damage->count < 0 ||
    damage->count + update->rectCount > KVMFR_MAX_DAMAGE_RECTS;

  if (damageAll)
    framebuffer_read(
      update->frame,
      parent->buf[parent->bufIndex].map,
      texture->format.stride,
      texture->format.height,
      texture->format.width,
      texture->format.bpp,
      texture->format.stride
    );
  else
  {
    memcpy(damage->rects + damage->count, update->rects,
      update->rectCount * sizeof(FrameDamageRect));
    damage->count += update->rectCount;
    rectsFramebufferToBuffer(
      damage->rects,
      damage->count,
      parent->buf[parent->bufIndex].map,
      texture->format.stride,
      texture->format.height,
      update->frame,
      texture->format.stride
    );
  }

  parent->buf[parent->bufIndex].updated = true;

  for (int i = 0; i < EGL_TEX_BUFFER_MAX; ++i)
  {
    struct TexDamage * damage = this->damage + i;
    if (i == parent->bufIndex)
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

  LG_UNLOCK(parent->copyLock);

  return true;
}

EGL_TextureOps EGL_TextureFrameBuffer =
{
  .init    = egl_texFBInit,
  .free    = egl_texFBFree,
  .setup   = egl_texFBSetup,
  .update  = egl_texFBUpdate,
  .process = egl_texBufferStreamProcess,
  .get     = egl_texBufferStreamGet
};
