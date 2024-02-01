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

#include "backend.h"
#include "com_ref.h"

#include <assert.h>
#include <dxgi1_2.h>
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
  ID3D12CommandAllocator    ** commandAllocator;
  ID3D12CommandList         ** commandList;
  ID3D12GraphicsCommandList ** graphicsCommandList;
  UINT64                       fenceValue;
  ID3D12Fence               ** fence;
  HANDLE                       event;
};

struct SharedCache
{
  ID3D11Texture2D  * tex;
  ID3D12Resource ** d12src;
};

struct D3D12Backend
{
  ComScope * comScope;

  HMODULE d3d12;
  unsigned width, height, pitch;
  DXGI_FORMAT format;

  float                  copySleep;
  ID3D12Device        ** device;
  ID3D12Device3       ** device3;
  void                 * ivshmemBase;
  ID3D12Heap          ** heap;
  ID3D12Resource     *** frameBuffers;
  ID3D12Resource      ** dstResource;
  ID3D12CommandQueue  ** commandQueue;
  UINT64                 fenceValue;
  ID3D12Fence         ** fence;
  HANDLE                 event;

  // shared handle cache
  struct SharedCache sharedCache[10];
  int sharedCacheCount;
  ID3D12Resource * d12src;

  unsigned textures;
  struct D3D12Texture texture[0];
};

static struct D3D12Backend * this = NULL;

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this->comScope, dst, src)

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

static D3D12_HEAP_DESC _ID3D12Heap_GetDesc(ID3D12Heap* This)
{
  D3D12_HEAP_DESC __ret;
  return *This->lpVtbl->GetDesc(This, &__ret);
}

static void d3d12_free(void);

static bool d3d12_create(
  void     * ivshmemBase,
  unsigned * alignSize,
  unsigned   frameBuffers,
  unsigned   textures)
{
  DEBUG_ASSERT(!this);
  bool result = false;
  HRESULT status;

  this = calloc(1,
    sizeof(struct D3D12Backend) +
    sizeof(*this->texture) * textures);

  if (!this)
  {
    DEBUG_ERROR("failed to allocate D3D12Backend struct");
    goto exit;
  }

  comRef_initGlobalScope(10, this->comScope);
  comRef_scopePush(10);

  this->d3d12 = LoadLibrary("d3d12.dll");
  if (!this->d3d12)
    goto exit;

  this->frameBuffers = calloc(frameBuffers, sizeof(*this->frameBuffers));
  if (!this->frameBuffers)
  {
    DEBUG_ERROR("Failed to allocate the ID3D12Resource frame buffer array");
    goto exit;
  }

  if (dxgi_debug())
  {
    D3D12GetDebugInterface_t D3D12GetDebugInterface = (D3D12GetDebugInterface_t)
      GetProcAddress(this->d3d12, "D3D12GetDebugInterface");
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
    GetProcAddress(this->d3d12, "D3D12CreateDevice");

  if (!D3D12CreateDevice)
    goto exit;

  this->textures  = textures;
  this->copySleep = option_get_float("dxgi", "d3d12CopySleep");
  DEBUG_INFO("Sleep before copy : %f ms", this->copySleep);

  comRef_defineLocal(ID3D12Device, device);
  status = D3D12CreateDevice((IUnknown *)dxgi_getAdapter(),
    D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)device);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D12 device", status);
    goto exit;
  }

  comRef_defineLocal(ID3D12Device3, device3);
  status = ID3D12Device_QueryInterface(
    *device, &IID_ID3D12Device3, (void **)device3);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to obtain the ID3D12Device3 interface", status);
    goto exit;
  }

  comRef_defineLocal(ID3D12Heap, heap);
  status = ID3D12Device3_OpenExistingHeapFromAddress(
    *device3,
    ivshmemBase,
    &IID_ID3D12Heap,
    (void **)heap);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to open the framebuffer as a heap", status);
    return false;
  }

  D3D12_HEAP_DESC heapDesc = _ID3D12Heap_GetDesc(*heap);
  DEBUG_INFO("ID3D12Heap        : Size:%I64u Alignment:%I64u",
    heapDesc.SizeInBytes, heapDesc.Alignment);
  *alignSize = heapDesc.Alignment;

  D3D12_COMMAND_QUEUE_DESC queueDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
  };

  comRef_defineLocal(ID3D12CommandQueue, commandQueue);
  status = ID3D12Device_CreateCommandQueue(*device, &queueDesc,
    &IID_ID3D12CommandQueue, (void **)commandQueue);
  if (FAILED(status))
  {
    DEBUG_WARN("Failed to create queue with real time priority");
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;

    status = ID3D12Device_CreateCommandQueue(*device, &queueDesc,
      &IID_ID3D12CommandQueue, (void **)commandQueue);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command allocator", status);
      goto exit;
    }
  }

  this->event = CreateEvent(NULL, TRUE, TRUE, NULL);
  if (!this->event)
  {
    DEBUG_WINERROR("Failed to create capture event", status);
    goto exit;
  }

  comRef_defineLocal(ID3D12Fence, fence);
  this->fenceValue = 0;
  status = ID3D12Device_CreateFence(*device, 0, D3D12_FENCE_FLAG_NONE,
    &IID_ID3D12Fence, (void**)fence);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create capture fence", status);
    goto exit;
  }

  this->ivshmemBase = ivshmemBase;

  comRef_toGlobal(this->device      , device      );
  comRef_toGlobal(this->device3     , device3     );
  comRef_toGlobal(this->heap        , heap        );
  comRef_toGlobal(this->commandQueue, commandQueue);
  comRef_toGlobal(this->fence       , fence       );

  result = true;

exit:
  comRef_scopePop();
  return result;
}

static bool d3d12_configure(
  unsigned    width,
  unsigned    height,
  DXGI_FORMAT format,
  unsigned    bpp,
  unsigned *  pitch)
{
  bool result = false;
  HRESULT status;
  comRef_scopePush(10);

  this->width  = width;
  this->height = height;
  this->format = format;
  this->pitch  = ALIGN_TO(width * bpp, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

  comRef_defineLocal(ID3D12Fence              , fence              );
  comRef_defineLocal(ID3D12CommandAllocator   , commandAllocator   );
  comRef_defineLocal(ID3D12GraphicsCommandList, graphicsCommandList);
  comRef_defineLocal(ID3D12CommandList        , commandList        );
  for (int i = 0; i < this->textures; ++i)
  {
    this->texture[i].event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!this->texture[i].event)
    {
      DEBUG_WINERROR("Failed to create texture event", status);
      goto exit;
    }

    this->texture[i].fenceValue = 0;
    status = ID3D12Device_CreateFence(*this->device, 0, D3D12_FENCE_FLAG_NONE,
      &IID_ID3D12Fence, (void **)fence);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create fence", status);
      goto exit;
    }

    status = ID3D12Device_CreateCommandAllocator(*this->device,
        D3D12_COMMAND_LIST_TYPE_COPY,
        &IID_ID3D12CommandAllocator,
        (void **)commandAllocator);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command allocator", status);
      goto exit;
    }

    status = ID3D12Device_CreateCommandList(*this->device,
        0,
        D3D12_COMMAND_LIST_TYPE_COPY,
        *commandAllocator,
        NULL,
        &IID_ID3D12GraphicsCommandList,
        (void **)graphicsCommandList);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create D3D12 command list", status);
      goto exit;
    }

    status = ID3D12GraphicsCommandList_QueryInterface(
        *graphicsCommandList,
        &IID_ID3D12CommandList,
        (void **)commandList);

    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to convert D3D12 command list", status);
      goto exit;
    }

    comRef_toGlobal(this->texture[i].fence              , fence              );
    comRef_toGlobal(this->texture[i].commandAllocator   , commandAllocator   );
    comRef_toGlobal(this->texture[i].graphicsCommandList, graphicsCommandList);
    comRef_toGlobal(this->texture[i].commandList        , commandList        );
  }


  *pitch = this->pitch;
  result = true;

exit:
  comRef_scopePop();

  return result;
}

static void d3d12_free(void)
{
  if (!this)
    return;

  for(int i = 0; i < this->textures; ++i)
    if (this->texture[i].event)
      CloseHandle(this->texture[i].event);

  if (this->event)
    CloseHandle(this->event);

  free(this->frameBuffers);

  if (this->d3d12)
    FreeLibrary(this->d3d12);

  comRef_freeScope(&this->comScope);
  free(this);
  this = NULL;
}

static bool d3d12_preCopy(
  ID3D11Texture2D * src,
  unsigned          textureIndex,
  unsigned          frameBufferIndex,
  FrameBuffer     * frameBuffer)
{
  comRef_scopePush(10);
  bool result = false;

  // we need to flush the DX11 context explicity or we get tons of lag
  dxgi_contextLock();
  ID3D11DeviceContext_Flush(dxgi_getContext());
  dxgi_contextUnlock();

  this->d12src = NULL;
  if (this->sharedCacheCount > -1)
  {
    // see if there is a cached handle already available for this texture
    for(int i = 0; i < this->sharedCacheCount; ++i)
      if (this->sharedCache[i].tex == src)
      {
        this->d12src = *this->sharedCache[i].d12src;
        goto framebuffer;
      }
  }

  comRef_defineLocal(IDXGIResource1, res1);
  HRESULT status = ID3D11Texture2D_QueryInterface(src,
    &IID_IDXGIResource1, (void **)res1);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get IDXGIResource1 from texture", status);
    goto exit;
  }

  HANDLE handle;
  status = IDXGIResource1_CreateSharedHandle(*res1,
      NULL, DXGI_SHARED_RESOURCE_READ, NULL, &handle);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get create shared handle for texture", status);
    goto exit;
  }

  comRef_defineLocal(ID3D12Resource, d12src);
  status = ID3D12Device_OpenSharedHandle(*this->device,
      handle, &IID_ID3D12Resource, (void **)d12src);

  CloseHandle(handle);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to open the shared handle for texture", status);
    goto exit;
  }

  // store the texture for later use
  if (this->sharedCacheCount < ARRAY_LENGTH(this->sharedCache))
  {
    struct SharedCache *cache = &this->sharedCache[this->sharedCacheCount++];
    cache->tex = src;
    comRef_toGlobal(cache->d12src, d12src);
    this->d12src = *cache->d12src;
  }
  else
  {
    // too many handles to cache, disable the cache entirely
    for(int i = 0; i < this->sharedCacheCount; ++i)
    {
      struct SharedCache *cache = &this->sharedCache[i];
      comRef_release(cache->d12src);
    }
    this->sharedCacheCount = -1;
  }

framebuffer:
  if (this->frameBuffers[frameBufferIndex])
    goto done;

  // prepare a IVSHMEM backed texture for the provided framebuffer
  D3D12_RESOURCE_DESC desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
    .Width              = this->height * this->pitch,
    .Height             = 1,
    .DepthOrArraySize   = 1,
    .MipLevels          = 1,
    .Format             = DXGI_FORMAT_UNKNOWN,
    .SampleDesc.Count   = 1,
    .SampleDesc.Quality = 0,
    .Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER
  };

  comRef_defineLocal(ID3D12Resource, resource);
  status = ID3D12Device3_CreatePlacedResource(
    *this->device3,
    *this->heap,
    (uintptr_t)framebuffer_get_data(frameBuffer) - (uintptr_t)this->ivshmemBase,
    &desc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    NULL,
    &IID_ID3D12Resource,
    (void**)resource);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D12 resource from heap", status);
    goto exit;
  }

  comRef_toGlobal(this->frameBuffers[frameBufferIndex], resource);

done:
  this->dstResource = this->frameBuffers[frameBufferIndex];
  result = true;

  if (this->copySleep > 0)
    nsleep((uint64_t)(this->copySleep * 1000000));

exit:
  comRef_scopePop();
  return result;
}

static bool d3d12_copyFull(ID3D11Texture2D * src, unsigned textureIndex)
{
  struct D3D12Texture * tex = &this->texture[textureIndex];

  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = this->d12src,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  D3D12_TEXTURE_COPY_LOCATION destLoc =
  {
    .pResource       = *this->dstResource,
    .Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint =
    {
      .Offset    = 0,
      .Footprint =
      {
        .Format   = this->format,
        .Width    = this->width,
        .Height   = this->height,
        .Depth    = 1,
        .RowPitch = this->pitch,
      }
    }
  };

  ID3D12GraphicsCommandList_CopyTextureRegion(*tex->graphicsCommandList,
      &destLoc, 0, 0, 0, &srcLoc, NULL);

  return true;
}

static bool d3d12_copyRect(ID3D11Texture2D * src, unsigned textureIndex,
  FrameDamageRect * rect)
{
  struct D3D12Texture * tex = &this->texture[textureIndex];

  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = this->d12src,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  D3D12_TEXTURE_COPY_LOCATION destLoc =
  {
    .pResource       = *this->dstResource,
    .Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint =
    {
      .Offset    = 0,
      .Footprint =
      {
        .Format   = this->format,
        .Width    = this->width,
        .Height   = this->height,
        .Depth    = 1,
        .RowPitch = this->pitch,
      }
    }
  };

  D3D12_BOX box =
  {
    .left   = rect->x,
    .top    = rect->y,
    .front  = 0,
    .back   = 1,
    .right  = rect->x + rect->width,
    .bottom = rect->y + rect->height,
  };

  ID3D12GraphicsCommandList_CopyTextureRegion(*tex->graphicsCommandList,
      &destLoc, box.left, box.top, 0, &srcLoc, &box);

  return true;
}

static bool d3d12_postCopy(ID3D11Texture2D * src, unsigned textureIndex)
{
  bool result = false;
  struct D3D12Texture * tex = &this->texture[textureIndex];

  HRESULT status = ID3D12GraphicsCommandList_Close(*tex->graphicsCommandList);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to close command list", status);
    goto exit;
  }

  ID3D12CommandQueue_ExecuteCommandLists(*this->commandQueue,
      1, tex->commandList);

  status = ID3D12CommandQueue_Signal(*this->commandQueue,
      *this->fence, ++this->fenceValue);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal capture fence", status);
    goto exit;
  }

  ResetEvent(this->event);
  status = ID3D12Fence_SetEventOnCompletion(*this->fence,
      this->fenceValue, this->event);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal capture fence event", status);
    goto exit;
  }

  status = ID3D12CommandQueue_Signal(*this->commandQueue,
      *tex->fence, ++tex->fenceValue);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal texture fence", status);
    goto exit;
  }

  status = ID3D12Fence_SetEventOnCompletion(*tex->fence,
      tex->fenceValue, tex->event);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to signal texture fence event", status);
    goto exit;
  }

  result = true;

exit:
  return result;
}

static CaptureResult d3d12_mapTexture(unsigned textureIndex, void ** map)
{
  struct D3D12Texture * tex = &this->texture[textureIndex];
  HRESULT status;

  WaitForSingleObject(tex->event, INFINITE);

  status = ID3D12CommandAllocator_Reset(*tex->commandAllocator);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to reset command allocator", status);
    return CAPTURE_RESULT_ERROR;
  }

  status = ID3D12GraphicsCommandList_Reset(*tex->graphicsCommandList,
      *tex->commandAllocator, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to reset command list", status);
    return CAPTURE_RESULT_ERROR;
  }

  return CAPTURE_RESULT_OK;
}

static CaptureResult d3d12_writeFrame(int textureIndex, FrameBuffer * frame)
{
  framebuffer_set_write_ptr(frame, this->height * this->pitch);
  return CAPTURE_RESULT_OK;
}

static void d3d12_unmapTexture(unsigned textureIndex)
{
}

static void d3d12_preRelease(void)
{
  WaitForSingleObject(this->event, INFINITE);
}

struct DXGICopyBackend copyBackendD3D12 =
{
  .name         = "Direct3D 12",
  .code         = "d3d12",
  .create       = d3d12_create,
  .configure    = d3d12_configure,
  .free         = d3d12_free,
  .preCopy      = d3d12_preCopy,
  .copyFull     = d3d12_copyFull,
  .copyRect     = d3d12_copyRect,
  .postCopy     = d3d12_postCopy,
  .mapTexture   = d3d12_mapTexture,
  .writeFrame   = d3d12_writeFrame,
  .unmapTexture = d3d12_unmapTexture,
  .preRelease   = d3d12_preRelease,
};
