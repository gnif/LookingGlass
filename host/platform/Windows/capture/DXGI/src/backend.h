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

#ifndef _H_DXGI_BACKEND_
#define _H_DXGI_BACKEND_

#include "common/types.h"
#include "interface/capture.h"

#include <stdbool.h>
#include <d3d11.h>

struct DXGICopyBackend
{
  // friendly name
  const char * name;

  // internal code name
  const char * code;

  // create the copy backend
  bool (*create)(
    void * ivshmemBase,
    unsigned * alignSize,
    unsigned frameBuffers,
    unsigned textures);

  // configure the copy backend with the specified format
  bool (*configure)(
    unsigned width,
    unsigned height,
    DXGI_FORMAT format,
    unsigned bpp,
    unsigned * pitch);

  // free the copy backend
  void (*free)(void);

  // called just before the copy starts
  bool (*preCopy)(ID3D11Texture2D * src,
    unsigned textureIndex,
    unsigned frameBufferIndex,
    FrameBuffer * frameBuffer);

  // called to copy the full frame
  bool (*copyFull)(ID3D11Texture2D * src, unsigned textureIndex);

  // called for each damage rect that needs to be copied
  bool (*copyRect)(ID3D11Texture2D * src, unsigned textureIndex,
    FrameDamageRect * rect);

  // called just after the copy has finished
  bool (*postCopy)(ID3D11Texture2D * src, unsigned textureIndex);

  // maps the copied frame into memory
  CaptureResult (*mapTexture)(unsigned textureIndex, void ** map);

  // [optional] backend specific write into the FrameBuffer
  CaptureResult (*writeFrame)(int textureIndex, FrameBuffer * frame);

  // unmaps the copied frame from memory
  void (*unmapTexture)(unsigned textureIndex);

  // called just before the frame is released by the frontend
  void (*preRelease)(void);
};

// these are functions exported by dxgi.c for use by the backends
IDXGIAdapter1       * dxgi_getAdapter(void);
ID3D11Device        * dxgi_getDevice(void);
ID3D11DeviceContext * dxgi_getContext(void);

// lock and unlock the context lock
void                  dxgi_contextLock(void);
void                  dxgi_contextUnlock(void);

/* returns true if dxgi:debug is enabled */
bool                  dxgi_debug(void);

#endif
