/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#ifndef _H_D12_BACKEND_
#define _H_D12_BACKEND_

#include "d12.h"

#include <stdbool.h>
#include <d3d12.h>
#include "interface/capture.h"

#define D12_MAX_DIRTY_RECTS 256

typedef struct D12Backend D12Backend;

struct D12Backend
{
  // friendly name
  const char * name;

  // internal name
  const char * codeName;

  // enable damage tracking
  bool trackDamage;

  // creation/init/free
  bool (*create)(D12Backend ** instance, unsigned frameBuffers);
  bool (*init)(
    D12Backend         * instance,
    bool                 debug,
    ID3D12Device3      * device,
    IDXGIAdapter1      * adapter,
    IDXGIOutput        * output);
  bool (*deinit)(D12Backend * instance);
  void (*free)(D12Backend ** instance);

  // capture callbacks
  CaptureResult (*capture)(D12Backend * instance,
    unsigned frameBufferIndex);

  CaptureResult (*sync)(D12Backend * instance,
    ID3D12CommandQueue * commandQueue);

  ID3D12Resource * (*fetch)(D12Backend * instance, unsigned frameBufferIndex,
    D12FrameDesc * meta);
};

static inline bool d12_backendCreate(const D12Backend * backend,
  D12Backend ** instance, unsigned frameBuffers)
{
  if (!backend->create(instance, frameBuffers))
    return false;
  memcpy(*instance, backend, sizeof(*backend));
  return true;
}

static inline bool d12_backendInit(D12Backend * instance, bool debug,
  ID3D12Device3 * device, IDXGIAdapter1 * adapter, IDXGIOutput * output,
  bool trackDamage)
{
  instance->trackDamage = trackDamage;
  return instance->init(instance, debug, device, adapter, output);
}

static inline bool d12_backendDeinit(D12Backend * instance)
  { return instance->deinit(instance); }

static inline void d12_backendFree(D12Backend ** instance)
  { (*instance)->free(instance); }

static inline CaptureResult d12_backendCapture(D12Backend * instance,
  unsigned frameBufferIndex)
  { return instance->capture(instance, frameBufferIndex); }

static inline CaptureResult d12_backendSync(D12Backend * instance,
  ID3D12CommandQueue * commandQueue)
  { return instance->sync(instance, commandQueue); }

static inline ID3D12Resource * d12_backendFetch(D12Backend * instance,
  unsigned frameBufferIndex, D12FrameDesc * desc)
  { return instance->fetch(instance, frameBufferIndex, desc); }

// Backend defines

extern const D12Backend D12Backend_DD;

#endif
