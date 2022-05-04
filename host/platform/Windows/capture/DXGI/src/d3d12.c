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

#include "dxgi_capture.h"

#include <assert.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include "common/time.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/windebug.h"
#include "common/array.h"
#include "ods_capture.h"

#define ALIGN_TO(value, align) (((value) + (align) - 1) & -(align))

struct D3D12Texture
{
  ID3D12Resource            * tex;
  ID3D12CommandAllocator    * commandAllocator;
  ID3D12CommandList         * commandList;
  ID3D12GraphicsCommandList * graphicsCommandList;
  UINT64                      fenceValue;
  ID3D12Fence               * fence;
  HANDLE                      event;
};

struct D3D12Backend
{
  float                 copySleep;
  ID3D12Device        * device;
  ID3D12InfoQueue1    * debugInfoQueue;
  ID3D12CommandQueue  * commandQueue;
  ID3D12Resource      * src;
  struct D3D12Texture * texture;
  UINT64                fenceValue;
  ID3D12Fence         * fence;
  HANDLE                event;

  // shared handle cache
  struct
  {
    ID3D11Texture2D * tex;
    HANDLE            handle;
  }
  handleCache[10];
  int handleCacheCount;
};

static struct DXGIInterface * dxgi = NULL;
static struct D3D12Backend  * this = NULL;

typedef HRESULT (*D3D12CreateDevice_t)(
  IUnknown          *pAdapter,
  D3D_FEATURE_LEVEL MinimumFeatureLevel,
  REFIID            riid,
  void              **ppDevice
);

typedef HRESULT (*D3D12GetDebugInterface_t)(
  REFIID riid,
  void   **ppvDebug
);

static void d3d12_free(void);

static bool d3d12_create(struct DXGIInterface * intf)
{
  HRESULT status;
  dxgi = intf;

  HMODULE d3d12 = LoadLibrary("d3d12.dll");
  if (!d3d12)
    return false;

  if (dxgi->downsampleLevel > 0)
  {
    DEBUG_WARN("The D3D12 backend does not support downsampling yet");
    dxgi->downsampleLevel = 0;
    dxgi->targetWidth     = dxgi->width;
    dxgi->targetHeight    = dxgi->height;
  }

  if (dxgi->debug)
  {
    D3D12GetDebugInterface_t D3D12GetDebugInterface = (D3D12GetDebugInterface_t)
      GetProcAddress(d3d12, "D3D12GetDebugInterface");
    ID3D12Debug1 * debug;
    if (FAILED(status = D3D12GetDebugInterface(&IID_ID3D12Debug1, (void **)&debug)))
      DEBUG_WINERROR("D3D12GetDebugInterface", status);
    else
    {
      captureOutputDebugString();
      ID3D12Debug1_EnableDebugLayer(debug);
      ID3D12Debug1_SetEnableGPUBasedValidation(debug, TRUE);
      ID3D12Debug1_SetEnableSynchronizedCommandQueueValidation(debug, TRUE);
    }
  }

  D3D12CreateDevice_t D3D12CreateDevice = (D3D12CreateDevice_t)
    GetProcAddress(d3d12, "D3D12CreateDevice");

  if (!D3D12CreateDevice)
    return false;

  DEBUG_ASSERT(!this);
  this = calloc(1, sizeof(struct D3D12Backend));
  if (!this)
  {
    DEBUG_ERROR("failed to allocate D3D12Backend struct");
    return false;
  }

  this->copySleep = option_get_float("dxgi", "d3d12CopySleep");
  DEBUG_INFO("Sleep before copy : %f ms", this->copySleep);

  status = D3D12CreateDevice((IUnknown *) dxgi->adapter, D3D_FEATURE_LEVEL_11_0,
    &IID_ID3D12Device, (void **)&this->device);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D12 device", status);
    goto fail;
  }

  D3D12_COMMAND_QUEUE_DESC queueDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
  };

  status = ID3D12Device_CreateCommandQueue(this->device, &queueDesc,
    &IID_ID3D12CommandQueue, (void **)&this->commandQueue);
  if (FAILED(status))
  {
    DEBUG_WARN("Failed to create queue with real time priority");
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;

    status = ID3D12Device_CreateCommandQueue(this->device, &queueDesc,
      &IID_ID3D12CommandQueue, (void **)&this->commandQueue);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command allocator", status);
      goto fail;
    }
  }

  dxgi->pitch  = ALIGN_TO(dxgi->width * dxgi->bpp,
      D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  dxgi->stride = dxgi->pitch / dxgi->bpp;

  this->texture = calloc(dxgi->maxTextures, sizeof(struct D3D12Texture));
  if (!this->texture)
  {
    DEBUG_ERROR("Failed to allocate memory");
    goto fail;
  }

  this->event = CreateEvent(NULL, TRUE, TRUE, NULL);
  if (!this->event)
  {
    DEBUG_WINERROR("Failed to create capture event", status);
    goto fail;
  }

  this->fenceValue = 0;
  status = ID3D12Device_CreateFence(this->device, 0, D3D12_FENCE_FLAG_NONE,
    &IID_ID3D12Fence, (void **)&this->fence);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create capture fence", status);
    goto fail;
  }

  D3D12_HEAP_PROPERTIES readbackHeapProperties =
  {
    .Type = D3D12_HEAP_TYPE_READBACK,
  };

  D3D12_RESOURCE_DESC texDesc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
    .Width              = dxgi->pitch * dxgi->height,
    .Height             = 1,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = DXGI_FORMAT_UNKNOWN,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags              = D3D12_RESOURCE_FLAG_NONE
  };

  for (int i = 0; i < dxgi->maxTextures; ++i)
  {
    status = ID3D12Device_CreateCommittedResource(this->device,
        &readbackHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        NULL,
        &IID_ID3D12Resource,
        (void **)&this->texture[i].tex);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create texture", status);
      goto fail;
    }

    this->texture[i].event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!this->texture[i].event)
    {
      DEBUG_WINERROR("Failed to create texture event", status);
      goto fail;
    }

    this->texture[i].fenceValue = 0;
    status = ID3D12Device_CreateFence(this->device, 0, D3D12_FENCE_FLAG_NONE,
      &IID_ID3D12Fence, (void **)&this->texture[i].fence);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create fence", status);
      goto fail;
    }

    status = ID3D12Device_CreateCommandAllocator(this->device,
        D3D12_COMMAND_LIST_TYPE_COPY,
        &IID_ID3D12CommandAllocator,
        (void **)&this->texture[i].commandAllocator);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command allocator", status);
      goto fail;
    }

    status = ID3D12Device_CreateCommandList(this->device,
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        this->texture[i].commandAllocator,
        NULL,
        &IID_ID3D12GraphicsCommandList,
        (void **)&this->texture[i].graphicsCommandList);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command list", status);
      goto fail;
    }

    status = ID3D12GraphicsCommandList_QueryInterface(
        this->texture[i].graphicsCommandList,
        &IID_ID3D12CommandList,
        (void **)&this->texture[i].commandList);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to convert D3D12 command list", status);
      goto fail;
    }

    dxgi->texture[i].impl = this->texture + i;
  }

  dxgi->useAcquireLock = false;
  return true;

fail:
  d3d12_free();
  return false;
}

static void d3d12_free(void)
{
  DEBUG_ASSERT(this);

  if (this->texture)
  {
    for (int i = 0; i < dxgi->maxTextures; ++i)
    {
      if (this->texture[i].tex)
        ID3D12Resource_Release(this->texture[i].tex);

      if (this->texture[i].fence)
        ID3D12Fence_Release(this->texture[i].fence);

      if (this->texture[i].event)
        CloseHandle(this->texture[i].event);

      if (this->texture[i].commandAllocator)
        ID3D12CommandAllocator_Release(this->texture[i].commandAllocator);

      if (this->texture[i].commandList)
        ID3D12CommandList_Release(this->texture[i].commandList);

      if (this->texture[i].graphicsCommandList)
        ID3D12GraphicsCommandList_Release(this->texture[i].graphicsCommandList);
    }

    free(this->texture);
  }

  if (this->fence)
    ID3D12Fence_Release(this->fence);

  if (this->event)
    CloseHandle(this->event);

  if (this->src)
    ID3D12Resource_Release(this->src);

  for(int i = 0; i < this->handleCacheCount; ++i)
    CloseHandle(this->handleCache[i].handle);

  if (this->commandQueue)
    ID3D12CommandQueue_Release(this->commandQueue);

  if (this->device)
  {
    DWORD count = ID3D12Device_Release(this->device);
    if (count != 0)
      DEBUG_ERROR("ID3D12Device release is %lu, there is a memory leak!", count);
  }

  free(this);
  this = NULL;
}

static bool d3d12_copyFrame(Texture * parent, ID3D11Texture2D * src)
{
  struct D3D12Texture * tex = parent->impl;
  bool fail = false;
  IDXGIResource1 * res1 = NULL;
  HRESULT status;

  if (this->copySleep > 0)
    nsleep((uint64_t)(this->copySleep * 1000000));

  HANDLE handle = INVALID_HANDLE_VALUE;

  if (this->handleCacheCount > -1)
  {
    // see if there is a cached handle already available for this texture
    for(int i = 0; i < this->handleCacheCount; ++i)
      if (this->handleCache[i].tex == src)
      {
        handle = this->handleCache[i].handle;
        break;
      }
  }

  if (handle == INVALID_HANDLE_VALUE)
  {
    status = ID3D11Texture2D_QueryInterface(src,
        &IID_IDXGIResource1, (void **)&res1);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to get IDXGIResource1 from texture", status);
      return CAPTURE_RESULT_ERROR;
    }

    status = IDXGIResource1_CreateSharedHandle(res1,
        NULL, DXGI_SHARED_RESOURCE_READ, NULL, &handle);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to get create shared handle for texture", status);
      fail = true;
      goto cleanup;
    }

    // store the handle for later use
    if (this->handleCacheCount < ARRAY_LENGTH(this->handleCache))
    {
      this->handleCache[this->handleCacheCount].tex    = src;
      this->handleCache[this->handleCacheCount].handle = handle;
      ++this->handleCacheCount;
    }
    else
    {
      // too many handles to cache, disable the cache entirely
      for(int i = 0; i < this->handleCacheCount; ++i)
        CloseHandle(this->handleCache[i].handle);
      this->handleCacheCount = -1;
    }
  }

  status = ID3D12Device_OpenSharedHandle(this->device,
      handle, &IID_ID3D12Resource, (void **)&this->src);

  if (this->handleCacheCount == -1)
    CloseHandle(handle);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get create shared handle for texture", status);
    fail = true;
    goto cleanup;
  }

  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = this->src,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  D3D12_TEXTURE_COPY_LOCATION destLoc =
  {
    .pResource       = tex->tex,
    .Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint =
    {
      .Offset    = 0,
      .Footprint =
      {
        .Format   = dxgi->dxgiFormat,
        .Width    = dxgi->width,
        .Height   = dxgi->height,
        .Depth    = 1,
        .RowPitch = dxgi->pitch,
      }
    }
  };

  if (parent->texDamageCount < 0)
    ID3D12GraphicsCommandList_CopyTextureRegion(tex->graphicsCommandList,
        &destLoc, 0, 0, 0, &srcLoc, NULL);
  else
  {
    for (int i = 0; i < parent->texDamageCount; ++i)
    {
      FrameDamageRect * rect = parent->texDamageRects + i;
      D3D12_BOX box =
      {
        .left   = rect->x,
        .top    = rect->y,
        .front  = 0,
        .back   = 1,
        .right  = rect->x + rect->width,
        .bottom = rect->y + rect->height,
      };
      ID3D12GraphicsCommandList_CopyTextureRegion(tex->graphicsCommandList,
          &destLoc, rect->x, rect->y, 0, &srcLoc, &box);
    }
  }

  status = ID3D12GraphicsCommandList_Close(tex->graphicsCommandList);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to close command list", status);
    fail = true;
    goto cleanup;
  }

  ID3D12CommandQueue_ExecuteCommandLists(this->commandQueue,
      1, &tex->commandList);

  status = ID3D12CommandQueue_Signal(this->commandQueue,
      this->fence, ++this->fenceValue);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal capture fence", status);
    fail = true;
    goto cleanup;
  }

  ResetEvent(this->event);
  status = ID3D12Fence_SetEventOnCompletion(this->fence,
      this->fenceValue, this->event);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal capture fence event", status);
    fail = true;
    goto cleanup;
  }

  status = ID3D12CommandQueue_Signal(this->commandQueue,
      tex->fence, ++tex->fenceValue);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal texture fence", status);
    fail = true;
    goto cleanup;
  }

  status = ID3D12Fence_SetEventOnCompletion(tex->fence,
      tex->fenceValue, tex->event);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal texture fence event", status);
    fail = true;
    goto cleanup;
  }

cleanup:
  if (res1)
    IDXGIResource1_Release(res1);
  return !fail;
}

static CaptureResult d3d12_mapTexture(Texture * parent)
{
  struct D3D12Texture * tex = parent->impl;
  HRESULT status;

  WaitForSingleObject(tex->event, INFINITE);

  status = ID3D12CommandAllocator_Reset(tex->commandAllocator);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to reset command allocator", status);
    return CAPTURE_RESULT_ERROR;
  }

  status = ID3D12GraphicsCommandList_Reset(tex->graphicsCommandList,
      tex->commandAllocator, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to reset command list", status);
    return CAPTURE_RESULT_ERROR;
  }

  D3D12_RANGE range =
  {
    .Begin = 0,
    .End   = dxgi->pitch * dxgi->height
  };
  status = ID3D12Resource_Map(tex->tex, 0, &range, &parent->map);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    return CAPTURE_RESULT_ERROR;
  }

  return CAPTURE_RESULT_OK;
}

static void d3d12_unmapTexture(Texture * parent)
{
  struct D3D12Texture * tex = parent->impl;

  D3D12_RANGE range =
  {
    .Begin = 0,
    .End   = 0
  };
  ID3D12Resource_Unmap(tex->tex, 0, &range);
  parent->map = NULL;
}

static void d3d12_preRelease(void)
{
  WaitForSingleObject(this->event, INFINITE);

  if (this->src)
  {
    ID3D12Resource_Release(this->src);
    this->src = NULL;
  }
}

struct DXGICopyBackend copyBackendD3D12 =
{
  .name         = "Direct3D 12",
  .code         = "d3d12",
  .create       = d3d12_create,
  .free         = d3d12_free,
  .copyFrame    = d3d12_copyFrame,
  .mapTexture   = d3d12_mapTexture,
  .unmapTexture = d3d12_unmapTexture,
  .preRelease   = d3d12_preRelease,
};
