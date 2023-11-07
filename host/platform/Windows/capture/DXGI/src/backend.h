/**
 * Looking Glass
 * Copyright © 2017-2023 The Looking Glass Authors
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

#include "dxgi_capture.h"

struct DXGIInterface;
struct Texture;

struct DXGICopyBackend
{
  // friendly name
  const char * name;

  // internal code name
  const char * code;

  // create the copy backend
  bool (*create)(unsigned textures);

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
  bool (*preCopy)(ID3D11Texture2D * src, unsigned textureIndex);

  // called to copy the full frame
  bool (*copyFull)(ID3D11Texture2D * src, unsigned textureIndex);

  // called for each damage rect that needs to be copied
  bool (*copyRect)(ID3D11Texture2D * src, unsigned textureIndex,
    FrameDamageRect * rect);

  // called just after the copy has finished
  bool (*postCopy)(ID3D11Texture2D * src, unsigned textureIndex);

  // maps the copied frame into memory
  CaptureResult (*mapTexture)(unsigned textureIndex, void ** map);

  // unmaps the copied frame from memory
  void (*unmapTexture)(unsigned textureIndex);

  // called just before the frame is released by the frontend
  void (*preRelease)(void);
};

#endif
