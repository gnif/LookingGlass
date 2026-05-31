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

#include "backend.h"
#include "com_ref.h"

#include <assert.h>
#include <unistd.h>
#include "common/debug.h"
#include "common/runningavg.h"
#include "common/windebug.h"

struct D3D11Backend
{
  ComScope * comScope;

  RunningAvg avgMapTime;
  uint64_t   usleepMapTime;

  unsigned textures;
  struct
  {
    uint64_t           copyTime;
    ID3D11Texture2D ** tex;
  }
  texture[0];
};

static struct D3D11Backend  * this = NULL;

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this->comScope, dst, src)

static bool d3d11_create(
  void     * ivshmemBase,
  unsigned * alignSize,
  unsigned   frameBuffers,
  unsigned   textures)
{
  DEBUG_ASSERT(!this);
  this = calloc(1,
    sizeof(struct D3D11Backend) +
    sizeof(*this->texture) * textures);

  if (!this)
  {
    DEBUG_ERROR("failed to allocate D3D11Backend struct");
    return false;
  }

  this->avgMapTime = runningavg_new(10);
  this->textures   = textures;

  comRef_initGlobalScope(10, this->comScope);
  return true;
}

static bool d3d11_configure(
  unsigned    width,
  unsigned    height,
  DXGI_FORMAT format,
  unsigned    bpp,
  unsigned *  pitch)
{
  comRef_scopePush(10);
  HRESULT status;

  D3D11_TEXTURE2D_DESC texTexDesc =
  {
    .Width              = width,
    .Height             = height,
    .MipLevels          = 1,
    .ArraySize          = 1,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Usage              = D3D11_USAGE_STAGING,
    .Format             = format,
    .BindFlags          = 0,
    .CPUAccessFlags     = D3D11_CPU_ACCESS_READ,
    .MiscFlags          = 0
  };

  comRef_defineLocal(ID3D11Texture2D, tex);
  for(int i = 0; i < this->textures; ++i)
  {
    status = ID3D11Device_CreateTexture2D(dxgi_getDevice(), &texTexDesc, NULL, tex);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create CPU texture", status);
      goto fail;
    }
    comRef_toGlobal(this->texture[i].tex, tex);
  }

  // map the texture simply to get the pitch and stride
  D3D11_MAPPED_SUBRESOURCE mapping;
  status = ID3D11DeviceContext_Map(dxgi_getContext(),
    *(ID3D11Resource **)this->texture[0].tex, 0, D3D11_MAP_READ, 0, &mapping);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    goto fail;
  }

  ID3D11DeviceContext_Unmap(dxgi_getContext(),
    *(ID3D11Resource **)this->texture[0].tex, 0);

  *pitch = mapping.RowPitch;
  comRef_scopePop();
  return true;

fail:
  comRef_scopePop();
  return false;
}

static void d3d11_free(void)
{
  if (!this)
    return;

  runningavg_free(&this->avgMapTime);
  comRef_freeScope(&this->comScope);
  free(this);
  this = NULL;
}

static bool d3d11_preCopy(
  ID3D11Texture2D * src,
  unsigned          textureIndex,
  unsigned          frameBufferIndex,
  FrameBuffer     * frameBuffer)
{
  dxgi_contextLock();
  this->texture[textureIndex].copyTime = microtime();
  return true;
}

static bool d3d11_copyFull(ID3D11Texture2D * src, unsigned textureIndex)
{
  ID3D11Texture2D * dst = *this->texture[textureIndex].tex;

  ID3D11DeviceContext_CopyResource(dxgi_getContext(),
    (ID3D11Resource *)dst, (ID3D11Resource *)src);

  return true;
}


static bool d3d11_copyRect(ID3D11Texture2D * src, unsigned textureIndex,
  FrameDamageRect * rect)
{
  ID3D11Texture2D * dst = *this->texture[textureIndex].tex;

  D3D11_BOX box =
  {
    .left   = rect->x,
    .top    = rect->y,
    .front  = 0,
    .back   = 1,
    .right  = rect->x + rect->width,
    .bottom = rect->y + rect->height,
  };

  ID3D11DeviceContext_CopySubresourceRegion(
    dxgi_getContext(),
    (ID3D11Resource *)dst, 0, box.left, box.top, 0,
    (ID3D11Resource *)src, 0, &box);

  return true;
}

static bool d3d11_postCopy(ID3D11Texture2D * src, unsigned textureIndex)
{
  ID3D11DeviceContext_Flush(dxgi_getContext());
  dxgi_contextUnlock();
  return true;
}

static CaptureResult d3d11_mapTexture(unsigned textureIndex, void ** map)
{
  ID3D11Resource * tex = *(ID3D11Resource **)this->texture[textureIndex].tex;

  // sleep until it's close to time to map
  const uint64_t delta = microtime() - this->texture[textureIndex].copyTime;
  if (delta < this->usleepMapTime)
    usleep(this->usleepMapTime - delta);

  D3D11_MAPPED_SUBRESOURCE mappedRes;

  // try to map the resource, but don't wait for it
  for (int i = 0; ; ++i)
  {
    HRESULT status;

    dxgi_contextLock();
    status = ID3D11DeviceContext_Map(dxgi_getContext(), tex, 0, D3D11_MAP_READ,
      0x100000L, &mappedRes);
    dxgi_contextUnlock();
    if (status == DXGI_ERROR_WAS_STILL_DRAWING)
    {
      if (i == 100)
        return CAPTURE_RESULT_TIMEOUT;

      usleep(1);
      continue;
    }

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to map the texture", status);
      return CAPTURE_RESULT_ERROR;
    }

    break;
  }

  *map = mappedRes.pData;

  // update the sleep average and sleep for 80% of the average on the next call
  runningavg_push(this->avgMapTime,
    microtime() - this->texture[textureIndex].copyTime);

  this->usleepMapTime = (uint64_t)(runningavg_calc(this->avgMapTime) * 0.8);
  return CAPTURE_RESULT_OK;
}

static void d3d11_unmapTexture(unsigned textureIndex)
{
  ID3D11Resource * tex = *(ID3D11Resource **)this->texture[textureIndex].tex;

  dxgi_contextLock();
  ID3D11DeviceContext_Unmap(dxgi_getContext(), tex, 0);
  dxgi_contextUnlock();
}

static void d3d11_preRelease(void)
{
  // Nothing needs to be done.
}

struct DXGICopyBackend copyBackendD3D11 = {
  .name         = "Direct3D 11",
  .code         = "d3d11",
  .create       = d3d11_create,
  .configure    = d3d11_configure,
  .free         = d3d11_free,
  .preCopy      = d3d11_preCopy,
  .copyFull     = d3d11_copyFull,
  .copyRect     = d3d11_copyRect,
  .postCopy     = d3d11_postCopy,
  .mapTexture   = d3d11_mapTexture,
  .unmapTexture = d3d11_unmapTexture,
  .preRelease   = d3d11_preRelease,
};
