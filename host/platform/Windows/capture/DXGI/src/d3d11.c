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

#include "dxgi_capture.h"
#include "com_ref.h"

#include <assert.h>
#include <unistd.h>
#include "common/debug.h"
#include "common/runningavg.h"
#include "common/windebug.h"

struct D3D11Backend
{
  RunningAvg avgMapTime;
  uint64_t   usleepMapTime;
};

struct D3D11TexImpl
{
  ID3D11Texture2D ** cpu;
};

#define TEXIMPL(x) ((struct D3D11TexImpl *)(x).impl)

static struct DXGIInterface * dxgi = NULL;
static struct D3D11Backend  * this = NULL;

static void d3d11_free(void);

static bool d3d11_create(struct DXGIInterface * intf)
{
  dxgi = intf;

  DEBUG_ASSERT(!this);
  this = calloc(1, sizeof(struct D3D11Backend));
  if (!this)
  {
    DEBUG_ERROR("failed to allocate D3D11Backend struct");
    return false;
  }

  this->avgMapTime = runningavg_new(10);
  return true;
}

static bool d3d11_configure(unsigned width, unsigned height, DXGI_FORMAT format,
  unsigned * pitch)
{
  HRESULT status;

  D3D11_TEXTURE2D_DESC cpuTexDesc =
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

  for (int i = 0; i < dxgi->maxTextures; ++i)
  {
    if (!(dxgi->texture[i].impl =
      (struct D3D11TexImpl *)calloc(sizeof(struct D3D11TexImpl), 1)))
    {
      DEBUG_ERROR("Failed to allocate D3D11TexImpl struct");
      goto fail;
    }

    struct D3D11TexImpl * teximpl = TEXIMPL(dxgi->texture[i]);

    status = ID3D11Device_CreateTexture2D(*dxgi->device, &cpuTexDesc, NULL,
      (ID3D11Texture2D **)comRef_newGlobal(&teximpl->cpu));

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create CPU texture", status);
      goto fail;
    }
  }

  // map the texture simply to get the pitch and stride
  D3D11_MAPPED_SUBRESOURCE mapping;
  status = ID3D11DeviceContext_Map(*dxgi->deviceContext,
    *(ID3D11Resource **)TEXIMPL(dxgi->texture[0])->cpu, 0,
    D3D11_MAP_READ, 0, &mapping);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    goto fail;
  }

  ID3D11DeviceContext_Unmap(*dxgi->deviceContext,
    *(ID3D11Resource **)TEXIMPL(dxgi->texture[0])->cpu, 0);

  *pitch = mapping.RowPitch;
  return true;

fail:
  return false;
}

static void d3d11_free(void)
{
  DEBUG_ASSERT(this);

  for (int i = 0; i < dxgi->maxTextures; ++i)
  {
    struct D3D11TexImpl * teximpl = TEXIMPL(dxgi->texture[i]);
    free(teximpl);
    teximpl = NULL;
  }

  runningavg_free(&this->avgMapTime);
  free(this);
  this = NULL;
}

static void copyFrameFull(Texture * tex, ID3D11Texture2D * src)
{
  struct D3D11TexImpl * teximpl = TEXIMPL(*tex);
  ID3D11Texture2D * dst = *teximpl->cpu;

  if (tex->texDamageCount < 0)
    ID3D11DeviceContext_CopyResource(*dxgi->deviceContext,
      (ID3D11Resource *)dst, (ID3D11Resource *)src);
  else
  {
    for (int i = 0; i < tex->texDamageCount; ++i)
    {
      FrameDamageRect * rect = tex->texDamageRects + i;
      D3D11_BOX box =
      {
        .left   = rect->x,
        .top    = rect->y,
        .front  = 0,
        .back   = 1,
        .right  = rect->x + rect->width ,
        .bottom = rect->y + rect->height,
      };
      ID3D11DeviceContext_CopySubresourceRegion(*dxgi->deviceContext,
        (ID3D11Resource *)dst, 0, box.left, box.top, 0,
        (ID3D11Resource *)src, 0, &box);
    }
  }
}

static bool d3d11_copyFrame(Texture * tex, ID3D11Texture2D * src)
{
  INTERLOCKED_SECTION(dxgi->deviceContextLock,
  {
    tex->copyTime = microtime();
    copyFrameFull(tex, src);
    ID3D11DeviceContext_Flush(*dxgi->deviceContext);
  });
  return true;
}

static CaptureResult d3d11_mapTexture(Texture * tex)
{
  struct D3D11TexImpl * teximpl = TEXIMPL(*tex);
  D3D11_MAPPED_SUBRESOURCE map;

  // sleep until it's close to time to map
  const uint64_t delta = microtime() - tex->copyTime;
  if (delta < this->usleepMapTime)
    usleep(this->usleepMapTime - delta);

  // try to map the resource, but don't wait for it
  for (int i = 0; ; ++i)
  {
    HRESULT status;

    INTERLOCKED_SECTION(dxgi->deviceContextLock, {
      status = ID3D11DeviceContext_Map(*dxgi->deviceContext,
        (ID3D11Resource *)*teximpl->cpu, 0, D3D11_MAP_READ, 0x100000L, &map);
    });
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

  tex->map = map.pData;

  // update the sleep average and sleep for 80% of the average on the next call
  runningavg_push(this->avgMapTime, microtime() - tex->copyTime);
  this->usleepMapTime = (uint64_t)(runningavg_calc(this->avgMapTime) * 0.8);
  return CAPTURE_RESULT_OK;
}

static void d3d11_unmapTexture(Texture * tex)
{
  struct D3D11TexImpl * teximpl = TEXIMPL(*tex);

  INTERLOCKED_SECTION(dxgi->deviceContextLock, {
    ID3D11DeviceContext_Unmap(*dxgi->deviceContext,
      (ID3D11Resource *)*teximpl->cpu, 0);
  });
  tex->map = NULL;
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
  .copyFrame    = d3d11_copyFrame,
  .mapTexture   = d3d11_mapTexture,
  .unmapTexture = d3d11_unmapTexture,
  .preRelease   = d3d11_preRelease,
};
