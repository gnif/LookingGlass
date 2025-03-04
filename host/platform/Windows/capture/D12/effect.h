/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#ifndef _H_D12_EFFECT_
#define _H_D12_EFFECT_

#include "d12.h"

#include <stdbool.h>
#include <d3d12.h>

typedef struct D12Effect D12Effect;

typedef enum D12EffectStatus
{
  D12_EFFECT_STATUS_OK,
  D12_EFFECT_STATUS_ERROR,
  D12_EFFECT_STATUS_BYPASS
}
D12EffectStatus;

struct D12Effect
{
  const char * name;

  bool enabled;

  void (*initOptions)(void);

  D12EffectStatus (*create)(D12Effect ** instance, ID3D12Device3 * device);

  void (*free)(D12Effect ** instance);

  // set the input format, and get the output format of the effect
  D12EffectStatus (*setFormat)(D12Effect * effect,
    ID3D12Device3             * device,
    const D12FrameFormat     * src,
          D12FrameFormat     * dst);

  void (*adjustDamage)(D12Effect * effect,
    RECT       dirtyRects[],
    unsigned * nbDirtyRects);

  ID3D12Resource * (*run)(D12Effect * effect,
    ID3D12Device3             * device,
    ID3D12GraphicsCommandList * commandList,
    ID3D12Resource            * src,
    RECT                        dirtyRects[],
    unsigned                  * nbDirtyRects);
};

static inline void d12_effectInitOptions(const D12Effect * effect)
  {  if (effect->initOptions) effect->initOptions(); }

static inline D12EffectStatus d12_effectCreate(const D12Effect * effect,
  D12Effect ** instance, ID3D12Device3 * device)
{
  *instance = NULL;
  D12EffectStatus status = effect->create(instance, device);
  if (status == D12_EFFECT_STATUS_OK)
    memcpy(*instance, effect, sizeof(*effect));
  return status;
}

static inline void d12_effectFree(D12Effect ** instance)
{
  if (*instance)
    (*instance)->free(instance);
  *instance = NULL;
}

static inline D12EffectStatus d12_effectSetFormat(D12Effect * effect,
  ID3D12Device3         * device,
  const D12FrameFormat * src,
        D12FrameFormat * dst)
  { return effect->setFormat(effect, device, src, dst); }

static inline void d12_effectAdjustDamage(D12Effect * effect,
  RECT dirtyRects[],
  unsigned * nbDirtyRects)
  { if (effect->adjustDamage)
      effect->adjustDamage(effect, dirtyRects, nbDirtyRects); }

static inline ID3D12Resource * d12_effectRun(D12Effect * effect,
  ID3D12Device3              * device,
  ID3D12GraphicsCommandList  * commandList,
  ID3D12Resource             * src,
  RECT                         dirtyRects[],
  unsigned                   * nbDirtyRects)
  { return effect->run(effect, device, commandList, src,
    dirtyRects, nbDirtyRects); }

#endif
