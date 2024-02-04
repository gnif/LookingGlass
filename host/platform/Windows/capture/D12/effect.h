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

#include <stdbool.h>
#include <d3d12.h>

typedef struct D12Effect D12Effect;

struct D12Effect
{
  const char * name;

  bool (*create)(D12Effect ** instance, ID3D12Device3 * device);

  void (*free)(D12Effect ** instance);

  // set the input format, and get the output format of the effect
  bool (*setFormat)(D12Effect * effect,
    ID3D12Device3             * device,
    const D3D12_RESOURCE_DESC * src,
          D3D12_RESOURCE_DESC * dst);

  ID3D12Resource * (*run)(D12Effect * effect,
    ID3D12Device3 * device, ID3D12GraphicsCommandList * commandList,
    ID3D12Resource * src);
};

static inline bool d12_effectCreate(const D12Effect * effect,
  D12Effect ** instance, ID3D12Device3 * device)
{
  if (!effect->create(instance, device))
    return false;
  memcpy(*instance, effect, sizeof(*effect));
  return true;
}

static inline void d12_effectFree(D12Effect ** instance)
{
  if (*instance)
    (*instance)->free(instance);
  *instance = NULL;
}

static inline bool d12_effectSetFormat(D12Effect * effect,
  ID3D12Device3             * device,
  const D3D12_RESOURCE_DESC * src,
        D3D12_RESOURCE_DESC * dst)
  { return effect->setFormat(effect, device, src, dst); }

static inline ID3D12Resource * d12_effectRun(D12Effect * effect,
  ID3D12Device3 * device, ID3D12GraphicsCommandList  * commandList,
  ID3D12Resource * src)
  { return effect->run(effect, device, commandList, src); }

// effect defines

extern const D12Effect D12Effect_RGB24;

#endif
