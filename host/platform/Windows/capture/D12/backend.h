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

#ifndef _H_D12_BACKEND_
#define _H_D12_BACKEND_

#include <stdbool.h>
#include <d3d12.h>
#include "interface/capture.h"

typedef struct D12Backend D12Backend;

struct D12Backend
{
  // friendly name
  const char * name;

  // internal name
  const char * codeName;

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

  ID3D12Resource * (*fetch)(D12Backend * instance,
    unsigned frameBufferIndex);
};

// helpers for the interface

static inline bool d12_createBackend(
  D12Backend * backend, D12Backend ** instance, unsigned frameBuffers)
{
  if (!backend->create(instance, frameBuffers))
    return false;
  memcpy(*instance, backend, sizeof(*backend));
  return true;
}

// apis for the backend
void d12_updatePointer(
  CapturePointer * pointer, void * shape, size_t shapeSize);

extern D12Backend D12Backend_DD;

#endif
