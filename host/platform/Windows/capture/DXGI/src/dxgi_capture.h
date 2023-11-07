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

#ifndef _H_DXGI_CAPTURE_
#define _H_DXGI_CAPTURE_

#include "pp.h"

#include <stdint.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include "common/KVMFR.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/types.h"
#include "common/vector.h"
#include "interface/capture.h"

#include "backend.h"

enum TextureState
{
  TEXTURE_STATE_UNUSED,
  TEXTURE_STATE_PENDING_MAP,
  TEXTURE_STATE_MAPPED
};

typedef struct Texture
{
  unsigned int               formatVer;
  volatile enum TextureState state;
  void                     * map;
  uint32_t                   damageRectsCount;
  FrameDamageRect            damageRects[KVMFR_MAX_DAMAGE_RECTS];
  int                        texDamageCount;
  FrameDamageRect            texDamageRects[KVMFR_MAX_DAMAGE_RECTS];

  // post processing
  Vector                     pp;
}
Texture;

typedef struct FrameDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
}
FrameDamage;

struct DXGIInterface
{
  bool                       initialized;
  LARGE_INTEGER              perfFreq;
  LARGE_INTEGER              frameTime;
  bool                       stop;
  HDESK                      desktop;
  IDXGIFactory1           ** factory;
  IDXGIAdapter1           ** adapter;
  IDXGIOutput             ** output;
  ID3D11Device            ** device;
  ID3D11DeviceContext     ** deviceContext;
  LG_Lock                    deviceContextLock;
  bool                       debug;
  bool                       useAcquireLock;
  bool                       dwmFlush;
  bool                       disableDamage;
  D3D_FEATURE_LEVEL          featureLevel;
  IDXGIOutputDuplication  ** dup;
  int                        maxTextures;
  Texture                  * texture;
  int                        texRIndex;
  int                        texWIndex;
  atomic_int                 texReady;
  bool                       needsRelease;
  DXGI_FORMAT                dxgiSrcFormat, dxgiFormat;
  bool                       hdr;
  DXGI_COLOR_SPACE_TYPE      dxgiColorSpace;
  ID3D11VertexShader      ** vshader;
  struct DXGICopyBackend   * backend;
  bool                       backendConfigured;

  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGEvent                  * frameEvent;

  unsigned int    formatVer;
  unsigned int    width , outputWidth , dataWidth;
  unsigned int    height, outputHeight, dataHeight;
  unsigned int    pitch;
  unsigned int    stride;
  unsigned int    padding;
  unsigned int    bpp;
  double          scaleX, scaleY;
  CaptureFormat   format, outputFormat;
  CaptureRotation rotation;

  int  lastPointerX, lastPointerY;
  bool lastPointerVisible;

  struct FrameDamage frameDamage[LGMP_Q_FRAME_LEN];
};

IDXGIAdapter1       * dxgi_getAdapter(void);
ID3D11Device        * dxgi_getDevice(void);
ID3D11DeviceContext * dxgi_getContext(void);
void                  dxgi_contextLock(void);
void                  dxgi_contextUnlock(void);
bool                  dxgi_debug(void);

#endif
