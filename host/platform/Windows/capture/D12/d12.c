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

#include "d12.h"

#include "interface/capture.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "com_ref.h"

#include "backend.h"
#include "effect.h"
#include "command_group.h"

#include <dxgi.h>
#include <dxgi1_3.h>
#include <d3dcommon.h>

// definitions
struct D12Interface
{
  HMODULE d3d12;

  IDXGIFactory2      ** factory;
  ID3D12Device3      ** device;

  ID3D12CommandQueue ** copyQueue;
  ID3D12CommandQueue ** computeQueue;
  D12CommandGroup       copyCommand;
  D12CommandGroup       computeCommand;

  void        * ivshmemBase;
  ID3D12Heap ** ivshmemHeap;

  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;

  D12Backend * backend;
  D12Effect  * effectRGB24;

  // capture format tracking
  D3D12_RESOURCE_DESC captureFormat;
  unsigned            formatVer;

  // output format tracking
  D3D12_RESOURCE_DESC dstFormat;

  // options
  bool debug;
  bool allowRGB24;

  unsigned frameBufferCount;
  // must be last
  struct
  {
    // the size of the frame buffer
    unsigned          size;
    // the frame buffer it itself
    FrameBuffer    *  frameBuffer;
    // the resource backed by the framebuffer
    ID3D12Resource ** resource;
  }
  frameBuffers[0];
};

// gloabls

struct DX12 DX12 = {0};
ComScope * d12_comScope = NULL;

// defines

// locals

static struct D12Interface * this = NULL;

// forwards

static bool d12_enumerateDevices(
  IDXGIFactory2 ** factory,
  IDXGIAdapter1 ** adapter,
  IDXGIOutput   ** output);

static ID3D12Resource * d12_frameBufferToResource(
  unsigned      frameBufferIndex,
  FrameBuffer * frameBuffer,
  unsigned size);

// implementation

static const char * d12_getName(void)
{
  return "D12";
}

static void d12_initOptions(void)
{
  struct Option options[] =
  {
    {
      .module       = "d12",
      .name         = "allowRGB24",
      .description  =
        "Losslessly pack 32-bit RGBA8 into 24-bit RGB (saves bandwidth)",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = false
    },
    {0}
  };

  option_register(options);
}

static bool d12_create(
  CaptureGetPointerBuffer  getPointerBufferFn,
  CapturePostPointerBuffer postPointerBufferFn,
  unsigned                 frameBuffers)
{
  this = calloc(1, offsetof(struct D12Interface, frameBuffers) +
    sizeof(this->frameBuffers[0]) * frameBuffers);
  if (!this)
  {
    DEBUG_ERROR("failed to allocate D12Interface struct");
    return false;
  }

  this->debug = false;
  this->d3d12 = LoadLibrary("d3d12.dll");
  if (!this->d3d12)
  {
    DEBUG_ERROR("failed to load d3d12.dll");
    free(this);
    return false;
  }

  DX12.D3D12CreateDevice = (typeof(DX12.D3D12CreateDevice))
    GetProcAddress(this->d3d12, "D3D12CreateDevice");

  DX12.D3D12GetDebugInterface = (typeof(DX12.D3D12GetDebugInterface))
    GetProcAddress(this->d3d12, "D3D12GetDebugInterface");

  DX12.D3D12SerializeVersionedRootSignature =
    (typeof(DX12.D3D12SerializeVersionedRootSignature))
      GetProcAddress(this->d3d12, "D3D12SerializeVersionedRootSignature");

  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  if (!d12_backendCreate(&D12Backend_DD, &this->backend, frameBuffers))
  {
    DEBUG_ERROR("backend \"%s\" failed to create", this->backend->codeName);
    CloseHandle(this->d3d12);
    free(this);
    return false;
  }

  this->frameBufferCount = frameBuffers;

  this->allowRGB24 = option_get_bool("d12", "allowRGB24");

  return true;
}

static bool d12_init(void * ivshmemBase, unsigned * alignSize)
{
  bool result = false;
  comRef_initGlobalScope(100, d12_comScope);
  comRef_scopePush(10);

  // create a DXGI factory
  comRef_defineLocal(IDXGIFactory2, factory);
  HRESULT hr = CreateDXGIFactory2(
    this->debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
    &IID_IDXGIFactory2,
    (void **)factory);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the DXGI factory", hr);
    goto exit;
  }

  // find the adapter and output we want to use
  comRef_defineLocal(IDXGIAdapter1, adapter);
  comRef_defineLocal(IDXGIOutput  , output );
  if (!d12_enumerateDevices(factory, adapter, output))
    goto exit;

  if (this->debug)
  {
    comRef_defineLocal(ID3D12Debug1, debug);
    hr = DX12.D3D12GetDebugInterface(&IID_ID3D12Debug1, (void **)debug);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("D3D12GetDebugInterface", hr);
      goto exit;
    }

    ID3D12Debug1_EnableDebugLayer(*debug);
    ID3D12Debug1_SetEnableGPUBasedValidation(*debug, TRUE);
    ID3D12Debug1_SetEnableSynchronizedCommandQueueValidation(*debug, TRUE);
  }

  // create the D3D12 device
  comRef_defineLocal(ID3D12Device3, device);
  hr = DX12.D3D12CreateDevice(
    (IUnknown *)*adapter,
    D3D_FEATURE_LEVEL_12_0,
    &IID_ID3D12Device3,
    (void **)device);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the DirectX12 device", hr);
    goto exit;
  }

  /* make this static as we downgrade the priority on failure and we want to
  remember it */
  static D3D12_COMMAND_QUEUE_DESC queueDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE,
  };

  comRef_defineLocal(ID3D12CommandQueue, copyQueue);
retryCreateCommandQueue:
  hr = ID3D12Device3_CreateCommandQueue(
    *device, &queueDesc, &IID_ID3D12CommandQueue, (void **)copyQueue);
  if (FAILED(hr))
  {
    if (queueDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
    {
      DEBUG_WARN("Failed to create queue with real time priority");
      queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      goto retryCreateCommandQueue;
    }

    DEBUG_WINERROR("Failed to create ID3D12CommandQueue (copy)", hr);
    goto exit;
  }
  ID3D12CommandQueue_SetName(*copyQueue, L"Copy");

  // create the compute queue
  D3D12_COMMAND_QUEUE_DESC computeQueueDesc =
  {
    .Type  = D3D12_COMMAND_LIST_TYPE_COMPUTE,
    .Flags = D3D12_COMMAND_QUEUE_FLAG_NONE,
  };
  queueDesc.Priority = queueDesc.Priority;

  comRef_defineLocal(ID3D12CommandQueue, computeQueue);
  hr = ID3D12Device3_CreateCommandQueue(
    *device, &computeQueueDesc, &IID_ID3D12CommandQueue, (void **)computeQueue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the ID3D12CommandQueue (compute)", hr);
    goto exit;
  }
  ID3D12CommandQueue_SetName(*computeQueue, L"Compute");

  if (!d12_commandGroupCreate(
    *device, D3D12_COMMAND_LIST_TYPE_COPY, &this->copyCommand, L"Copy"))
    goto exit;

  if (!d12_commandGroupCreate(
    *device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &this->computeCommand, L"Compute"))
    goto exit;

  // Create the IVSHMEM heap
  this->ivshmemBase = ivshmemBase;
  comRef_defineLocal(ID3D12Heap, ivshmemHeap);
  hr = ID3D12Device3_OpenExistingHeapFromAddress(
    *device, ivshmemBase, &IID_ID3D12Heap, (void **)ivshmemHeap);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the framebuffer as a D3D12Heap", hr);
    goto exit;
  }

  // Adjust the alignSize based on the required heap alignment
  D3D12_HEAP_DESC heapDesc = ID3D12Heap_GetDesc(*ivshmemHeap);
  *alignSize = heapDesc.Alignment;

  // initialize the backend
  if (!d12_backendInit(this->backend, this->debug, *device, *adapter, *output))
    goto exit;

  if (this->allowRGB24)
  {
    if (!d12_effectCreate(&D12Effect_RGB24, &this->effectRGB24, *device))
      goto exit;
  }

  comRef_toGlobal(this->factory     , factory      );
  comRef_toGlobal(this->device      , device       );
  comRef_toGlobal(this->copyQueue   , copyQueue    );
  comRef_toGlobal(this->computeQueue, computeQueue );
  comRef_toGlobal(this->ivshmemHeap , ivshmemHeap  );

  result = true;

exit:
  comRef_scopePop();
  if (!result)
    comRef_freeScope(&d12_comScope);

  return result;
}

static void d12_stop(void)
{
}

static bool d12_deinit(void)
{
  bool result = true;
  d12_effectFree(&this->effectRGB24);

  if (!d12_backendDeinit(this->backend))
    result = false;

  d12_commandGroupFree(&this->copyCommand   );
  d12_commandGroupFree(&this->computeCommand);

  IDXGIFactory2 * factory = *this->factory;
  IDXGIFactory2_AddRef(factory);
  comRef_freeScope(&d12_comScope);
  if (IDXGIFactory2_Release(factory) != 0)
    DEBUG_WARN("MEMORY LEAK");

  // zero the framebuffers
  memset(this->frameBuffers, 0,
    sizeof(*this->frameBuffers) * this->frameBufferCount);

  /* zero the formats so we properly reinit otherwise we wont detect the format
  change and setup the effect chain */
  memset(&this->captureFormat, 0, sizeof(this->captureFormat));
  memset(&this->dstFormat    , 0, sizeof(this->dstFormat    ));

  return result;
}

static void d12_free(void)
{
  d12_backendFree(&this->backend);
  FreeLibrary(this->d3d12);
  free(this);
  this = NULL;
}

static CaptureResult d12_capture(
  unsigned frameBufferIndex, FrameBuffer * frameBuffer)
{
  return d12_backendCapture(this->backend, frameBufferIndex);
}

static CaptureResult d12_waitFrame(unsigned frameBufferIndex,
  CaptureFrame * frame, const size_t maxFrameSize)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(1);

  comRef_defineLocal(ID3D12Resource, src);
  *src = d12_backendFetch(this->backend, frameBufferIndex);
  if (!*src)
  {
    DEBUG_ERROR("D12 backend failed to produce an expected frame: %u",
      frameBufferIndex);
    result = CAPTURE_RESULT_ERROR;
    goto exit;
  }


  D3D12_RESOURCE_DESC srcFormat = ID3D12Resource_GetDesc(*src);
  D3D12_RESOURCE_DESC dstFormat = this->dstFormat;

  // if the input format changed, reconfigure the effects
  if (dstFormat.Width  == 0 ||
      dstFormat.Width  != this->captureFormat.Width  ||
      dstFormat.Height != this->captureFormat.Height ||
      dstFormat.Format != this->captureFormat.Format)
  {
    dstFormat           = srcFormat;
    this->captureFormat = srcFormat;

    //TODO: loop through an effect array
    if (this->allowRGB24)
    {
      if (!d12_effectSetFormat(
        this->effectRGB24, *this->device, &srcFormat, &dstFormat))
      {
        DEBUG_ERROR("Failed to set the effect input format");
        goto exit;
      }
    }

    // if the output format changed
    if (dstFormat.Width  != this->dstFormat.Width  ||
        dstFormat.Height != this->dstFormat.Height ||
        dstFormat.Format != this->dstFormat.Format)
    {
      ++this->formatVer;
      this->dstFormat = dstFormat;
    }
  }

  const unsigned int maxRows = maxFrameSize / (dstFormat.Width * 4);

  frame->formatVer        = this->formatVer;
  frame->screenWidth      = srcFormat.Width;
  frame->screenHeight     = srcFormat.Height;
  frame->dataWidth        = dstFormat.Width;
  frame->dataHeight       = min(maxRows, dstFormat.Height);
  frame->frameWidth       = srcFormat.Width;
  frame->frameHeight      = srcFormat.Height;
  frame->truncated        = maxRows < dstFormat.Height;
  frame->pitch            = dstFormat.Width * 4;
  frame->stride           = dstFormat.Width;
  frame->format           = this->allowRGB24 ? CAPTURE_FMT_BGR_32 : CAPTURE_FMT_BGRA;
  frame->hdr              = false;
  frame->hdrPQ            = false;
  frame->rotation         = CAPTURE_ROT_0;
  frame->damageRectsCount = 0;

  result = CAPTURE_RESULT_OK;

exit:
  comRef_scopePop();
  return result;
}

static CaptureResult d12_getFrame(unsigned frameBufferIndex,
  FrameBuffer * frameBuffer, const size_t maxFrameSize)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(3);

  comRef_defineLocal(ID3D12Resource, src);
  *src = d12_backendFetch(this->backend, frameBufferIndex);
  if (!*src)
  {
    DEBUG_ERROR("D12 backend failed to produce an expected frame: %u",
      frameBufferIndex);
    goto exit;
  }

  comRef_defineLocal(ID3D12Resource, dst)
  *dst = d12_frameBufferToResource(frameBufferIndex, frameBuffer, maxFrameSize);
  if (!*dst)
    goto exit;

  // place a fence into the queue
  result = d12_backendSync(this->backend,
    this->allowRGB24 ? *this->computeQueue : *this->copyQueue);

  if (result != CAPTURE_RESULT_OK)
    goto exit;

  ID3D12Resource * next = *src;
  if (this->allowRGB24)
  {
    next = d12_effectRun(
      this->effectRGB24, *this->device, *this->computeCommand.gfxList, next);
  }

  // copy into the framebuffer resource
  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = next,
    .Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
    .SubresourceIndex = 0
  };

  D3D12_TEXTURE_COPY_LOCATION dstLoc =
  {
    .pResource       = *dst,
    .Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
    .PlacedFootprint =
    {
      .Offset = 0,
      .Footprint =
      {
        .Format   = this->dstFormat.Format,
        .Width    = this->dstFormat.Width,
        .Height   = this->dstFormat.Height,
        .Depth    = 1,
        .RowPitch = this->dstFormat.Width * 4
      }
    }
  };

  ID3D12GraphicsCommandList_CopyTextureRegion(
    *this->copyCommand.gfxList, &dstLoc, 0, 0, 0, &srcLoc, NULL);

  // execute the compute commands
  if (this->allowRGB24)
  {
    d12_commandGroupExecute(*this->computeQueue, &this->computeCommand);

    // insert a fence to wait for the compute commands to finish
    ID3D12CommandQueue_Wait(*this->copyQueue,
      *this->computeCommand.fence, this->computeCommand.fenceValue);
  }

  // execute the copy commands
  d12_commandGroupExecute(*this->copyQueue, &this->copyCommand);

  // wait for the copy to complete
  d12_commandGroupWait(&this->copyCommand);

  // signal the frame is complete
  framebuffer_set_write_ptr(frameBuffer,
    this->dstFormat.Height * this->dstFormat.Width * 4);

  // reset the command queues
  if (this->allowRGB24)
    if (!d12_commandGroupReset(&this->computeCommand))
      goto exit;

  if (!d12_commandGroupReset(&this->copyCommand))
    goto exit;

  result = CAPTURE_RESULT_OK;

exit:
  comRef_scopePop();
  return result;
}

static bool d12_enumerateDevices(
  IDXGIFactory2 ** factory,
  IDXGIAdapter1 ** adapter,
  IDXGIOutput   ** output)
{
  DXGI_ADAPTER_DESC1 adapterDesc;
  DXGI_OUTPUT_DESC   outputDesc;

  for(
    int i = 0;
    IDXGIFactory2_EnumAdapters1(*factory, i, adapter)
      != DXGI_ERROR_NOT_FOUND;
    ++i, comRef_release(adapter))
  {
    HRESULT hr = IDXGIAdapter1_GetDesc1(*adapter, &adapterDesc);
    if (FAILED(hr))
    {
      DEBUG_WINERROR("Failed to get the device description", hr);
      comRef_release(adapter);
      return false;
    }

    // check for devices without D3D support
    static const UINT blacklist[][2] =
    {
      //VID  , PID
      {0x1414, 0x008c}, // Microsoft Basic Render Driver
      {0x1b36, 0x000d}, // QXL
      {0x1234, 0x1111}  // QEMU Standard VGA
    };

    bool skip = false;
    for(int n = 0; n < ARRAY_LENGTH(blacklist); ++n)
    {
      if (adapterDesc.VendorId == blacklist[n][0] &&
          adapterDesc.DeviceId == blacklist[n][1])
      {
        DEBUG_INFO("Not using unsupported adapter: %ls",
          adapterDesc.Description);
        skip = true;
        break;
      }
    }
    if (skip)
      continue;

    // FIXME: Allow specifying the specific adapter

    for(
      int n = 0;
      IDXGIAdapter1_EnumOutputs(*adapter, n, output) != DXGI_ERROR_NOT_FOUND;
      ++n, comRef_release(output))
    {
      IDXGIOutput_GetDesc(*output, &outputDesc);
      // FIXME: Allow specifying the specific output

      if (outputDesc.AttachedToDesktop)
        break;
    }

    if (*output)
      break;
  }

  if (!*output)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    return false;
  }

  DEBUG_INFO("Device Name       : %ls"    , outputDesc.DeviceName);
  DEBUG_INFO("Device Description: %ls"    , adapterDesc.Description);
  DEBUG_INFO("Device Vendor ID  : 0x%x"   , adapterDesc.VendorId);
  DEBUG_INFO("Device Device ID  : 0x%x"   , adapterDesc.DeviceId);
  DEBUG_INFO("Device Video Mem  : %u MiB" ,
    (unsigned)(adapterDesc.DedicatedVideoMemory  / 1048576));
  DEBUG_INFO("Device Sys Mem    : %u MiB" ,
    (unsigned)(adapterDesc.DedicatedSystemMemory / 1048576));
  DEBUG_INFO("Shared Sys Mem    : %u MiB" ,
    (unsigned)(adapterDesc.SharedSystemMemory    / 1048576));

  return true;
}

static ID3D12Resource * d12_frameBufferToResource(unsigned frameBufferIndex,
  FrameBuffer * frameBuffer, unsigned size)
{
  ID3D12Resource * result = NULL;
  comRef_scopePush(10);

  typeof(this->frameBuffers[0]) * fb = &this->frameBuffers[frameBufferIndex];

  // nothing to do if the resource is already setup and is big enough
  if (fb->resource && fb->frameBuffer == frameBuffer && fb->size >= size)
  {
    result = *fb->resource;
    ID3D12Resource_AddRef(result);
    goto exit;
  }

  fb->size        = size;
  fb->frameBuffer = frameBuffer;

  D3D12_RESOURCE_DESC desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
    .Width              = size,
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
  HRESULT hr = ID3D12Device3_CreatePlacedResource(
    *this->device,
    *this->ivshmemHeap,
    (uintptr_t)framebuffer_get_data(frameBuffer) - (uintptr_t)this->ivshmemBase,
    &desc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    NULL,
    &IID_ID3D12Resource,
    (void **)resource);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the FrameBuffer ID3D12Resource", hr);
    goto exit;
  }

  // cache the resource
  comRef_toGlobal(fb->resource, resource);
  result = *fb->resource;
  ID3D12Resource_AddRef(result);

exit:
  comRef_scopePop();
  return result;
}

void d12_updatePointer(CapturePointer * pointer, void * shape, size_t shapeSize)
{
  if (pointer->shapeUpdate)
  {
    void * dst;
    UINT   dstSize;
    if (!this->getPointerBufferFn(&dst, &dstSize))
    {
      DEBUG_ERROR("Failed to obtain a buffer for the pointer shape");
      pointer->shapeUpdate = false;
    }

    size_t copySize = min(dstSize, shapeSize);
    memcpy(dst, shape, copySize);
  }

  this->postPointerBufferFn(pointer);
}

struct CaptureInterface Capture_D12 =
{
  .shortName       = "D12",
  .asyncCapture    = false,
  .getName         = d12_getName,
  .initOptions     = d12_initOptions,
  .create          = d12_create,
  .init            = d12_init,
  .stop            = d12_stop,
  .deinit          = d12_deinit,
  .free            = d12_free,
  .capture         = d12_capture,
  .waitFrame       = d12_waitFrame,
  .getFrame        = d12_getFrame
};
