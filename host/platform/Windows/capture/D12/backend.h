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

typedef struct D12Backend
{
  // friendly name
  const char * name;

  // internal name
  const char * codeName;

  // creation/init/free
  bool (*create)(unsigned frameBuffers);
  bool (*init)(
    bool                 debug,
    ID3D12Device3      * device,
    IDXGIAdapter1      * adapter,
    IDXGIOutput        * output);
  bool (*deinit)(void);
  void (*free)(void);

  // capture callbacks
  CaptureResult    (*capture)(unsigned frameBufferIndex);
  CaptureResult    (*sync   )(ID3D12CommandQueue * commandQueue);
  ID3D12Resource * (*fetch  )(unsigned frameBufferIndex);
}
D12Backend;

// apis for the backend
void d12_updatePointer(
  CapturePointer * pointer, void * shape, size_t shapeSize);

extern D12Backend D12Backend_DD;

#endif
