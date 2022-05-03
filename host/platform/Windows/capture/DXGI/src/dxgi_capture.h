/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include <stdint.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include "common/KVMFR.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/types.h"
#include "interface/capture.h"

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
  uint64_t                   copyTime;
  uint32_t                   damageRectsCount;
  FrameDamageRect            damageRects[KVMFR_MAX_DAMAGE_RECTS];
  int32_t                    texDamageCount;
  FrameDamageRect            texDamageRects[KVMFR_MAX_DAMAGE_RECTS];

  void                     * impl;
}
Texture;

struct FrameDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
};

struct DXGICopyBackend;

struct DXGIInterface
{
  bool                       initialized;
  LARGE_INTEGER              perfFreq;
  LARGE_INTEGER              frameTime;
  bool                       stop;
  HDESK                      desktop;
  IDXGIFactory1            * factory;
  IDXGIAdapter1            * adapter;
  IDXGIOutput              * output;
  ID3D11Device             * device;
  ID3D11DeviceContext      * deviceContext;
  LG_Lock                    deviceContextLock;
  bool                       debug;
  bool                       useAcquireLock;
  bool                       dwmFlush;
  bool                       disableDamage;
  D3D_FEATURE_LEVEL          featureLevel;
  IDXGIOutputDuplication   * dup;
  int                        maxTextures;
  Texture                  * texture;
  int                        texRIndex;
  int                        texWIndex;
  atomic_int                 texReady;
  bool                       needsRelease;
  DXGI_FORMAT                dxgiFormat;
  struct DXGICopyBackend   * backend;

  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGEvent                  * frameEvent;

  unsigned int    formatVer;
  unsigned int    width , targetWidth ;
  unsigned int    height, targetHeight;
  unsigned int    downsampleLevel;
  unsigned int    pitch;
  unsigned int    stride;
  unsigned int    bpp;
  CaptureFormat   format;
  CaptureRotation rotation;

  int  lastPointerX, lastPointerY;
  bool lastPointerVisible;

  struct FrameDamage frameDamage[LGMP_Q_FRAME_LEN];
};

struct DXGICopyBackend
{
  const char * name;
  const char * code;
  bool (*create)(struct DXGIInterface * intf);
  void (*free)(void);
  bool (*copyFrame)(Texture * tex, ID3D11Texture2D * src);
  CaptureResult (*mapTexture)(Texture * tex);
  void (*unmapTexture)(Texture * tex);
  void (*preRelease)(void);
};

const char * GetDXGIFormatStr(DXGI_FORMAT format);
