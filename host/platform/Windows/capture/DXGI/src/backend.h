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
  bool (*create)(struct DXGIInterface * intf);

  // configure the copy backend with the specified format
  bool (*configure)(unsigned width, unsigned height,
    DXGI_FORMAT format, unsigned * pitch);

  // free the copy backend
  void (*free)(void);

  // called each captured frame after post processing to copy the frame
  bool (*copyFrame)(struct Texture * tex, ID3D11Texture2D * src);

  // maps the copied frame into memory
  CaptureResult (*mapTexture)(struct Texture * tex);

  // unmaps the copied frame from memory
  void (*unmapTexture)(struct Texture * tex);

  // called just before the frame is released by the frontend
  void (*preRelease)(void);
};

#endif
