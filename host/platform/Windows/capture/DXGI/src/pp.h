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

#ifndef _H_DXGI_PP_
#define _H_DXGI_PP_

#include <d3d11.h>
#include <stdbool.h>

#include "interface/capture.h"
#include "common/locking.h"

typedef struct
{
  /* the friendly name of the processor for debugging */
  const char * name;

  /* early initialization for registering options */
  void (*earlyInit)(void);

  /* common setup */
  bool (*setup)(
    ID3D11Device        ** device,
    ID3D11DeviceContext ** context,
    IDXGIOutput         ** output,
    bool                   shareable);

  /* instance initialization */
  bool (*init)(void ** opaque);

  /* showtime configuration */
  bool (*configure)(void * opaque,
    int * width, int * height, // the image dimensions
    int * cols , int * rows  , // the texture dimensions for packed data
    CaptureFormat * type);

  /* perform the processing */
  ID3D11Texture2D * (*run)(void * opaque, ID3D11ShaderResourceView * srv);

  /* instance destruction */
  void (*free)(void * opaque);

  /* cleanup */
  void (*finish)(void);
}
DXGIPostProcess;

#endif
