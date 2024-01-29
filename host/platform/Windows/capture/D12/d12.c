#include "interface/capture.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "com_ref.h"

#include "backend.h"

#include <dxgi.h>
#include <dxgi1_3.h>
#include <d3dcommon.h>

// definitions

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

typedef struct D12CommandGroup
{
  ID3D12CommandAllocator    ** allocator;
  ID3D12GraphicsCommandList ** gfxList;
  ID3D12CommandList         ** cmdList;
  ID3D12Fence               ** fence;
  HANDLE                       event;
  UINT64                       fenceValue;
}
D12CommandGroup;

struct D12Interface
{
  ComScope * comScope;

  HMODULE                  d3d12;
  D3D12CreateDevice_t      D3D12CreateDevice;
  D3D12GetDebugInterface_t D3D12GetDebugInterface;

  IDXGIFactory2      ** factory;
  ID3D12Device3      ** device;

  ID3D12CommandQueue ** commandQueue;
  D12CommandGroup       copyCommand;

  void        * ivshmemBase;
  ID3D12Heap ** ivshmemHeap;

  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;

  D12Backend * backend;

  // capture format tracking
  D3D12_RESOURCE_DESC lastFormat;
  unsigned            formatVer;

  // options
  bool debug;

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

// defines

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this->comScope, dst, src)

// locals

static struct D12Interface * this = NULL;

// forwards

static bool d12_enumerateDevices(
  IDXGIFactory2 ** factory,
  IDXGIAdapter1 ** adapter,
  IDXGIOutput   ** output);

static bool d12_createCommandGroup(
  ID3D12Device3            * device,
  D3D12_COMMAND_LIST_TYPE    type,
  D12CommandGroup          * dst,
  LPCWSTR name);

static void d12_freeCommandGroup(
  D12CommandGroup * grp);

static bool d12_executeCommandGroup(
  D12CommandGroup * grp);

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

  this->D3D12CreateDevice = (D3D12CreateDevice_t)
    GetProcAddress(this->d3d12, "D3D12CreateDevice");

  this->D3D12GetDebugInterface = (D3D12GetDebugInterface_t)
    GetProcAddress(this->d3d12, "D3D12GetDebugInterface");

  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;

  this->backend = &D12Backend_DD;
  if (!this->backend->create(frameBuffers))
  {
    DEBUG_ERROR("backend \"%s\" failed to create", this->backend->codeName);
    CloseHandle(this->d3d12);
    free(this);
    return false;
  }

  return true;
}

static bool d12_init(void * ivshmemBase, unsigned * alignSize)
{
  bool result = false;
  comRef_initGlobalScope(100, this->comScope);
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
    hr = this->D3D12GetDebugInterface(&IID_ID3D12Debug1, (void **)debug);
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
  hr = this->D3D12CreateDevice(
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

  comRef_defineLocal(ID3D12CommandQueue, commandQueue);
retryCreateCommandQueue:
  hr = ID3D12Device3_CreateCommandQueue(
    *device, &queueDesc, &IID_ID3D12CommandQueue, (void **)commandQueue);
  if (FAILED(hr))
  {
    if (queueDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME)
    {
      DEBUG_WARN("Failed to create queue with real time priority");
      queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
      goto retryCreateCommandQueue;
    }

    DEBUG_WINERROR("Failed to create ID3D12CommandQueue", hr);
    goto exit;
  }
  ID3D12CommandQueue_SetName(*commandQueue, L"Command Queue");

  if (!d12_createCommandGroup(
    *device, D3D12_COMMAND_LIST_TYPE_COPY, &this->copyCommand, L"Copy"))
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
  if (!this->backend->init(this->debug, *device, *adapter, *output))
    goto exit;

  comRef_toGlobal(this->factory     , factory     );
  comRef_toGlobal(this->device      , device      );
  comRef_toGlobal(this->commandQueue, commandQueue);
  comRef_toGlobal(this->ivshmemHeap , ivshmemHeap );

  result = true;

exit:
  comRef_scopePop();
  if (!result)
    comRef_freeScope(&this->comScope);

  return result;
}

static void d12_stop(void)
{
}

static bool d12_deinit(void)
{
  bool result = true;
  if (!this->backend->deinit())
    result = false;

  d12_freeCommandGroup(&this->copyCommand);
  comRef_freeScope(&this->comScope);
  return result;
}

static void d12_free(void)
{
  this->backend->free();
  FreeLibrary(this->d3d12);
  free(this);
  this = NULL;
}

static CaptureResult d12_capture(
  unsigned frameBufferIndex, FrameBuffer * frameBuffer)
{
  return this->backend->capture(frameBufferIndex);
}

static CaptureResult d12_waitFrame(unsigned frameBufferIndex,
  CaptureFrame * frame, const size_t maxFrameSize)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(1);

  comRef_defineLocal(ID3D12Resource, src);
  *src = this->backend->fetch(frameBufferIndex);
  if (!*src)
  {
    DEBUG_ERROR("D12 backend failed to produce an expected frame: %u",
      frameBufferIndex);
    result = CAPTURE_RESULT_ERROR;
    goto exit;
  }

  D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(*src);
  if (desc.Width != this->lastFormat.Width ||
      desc.Height != this->lastFormat.Height ||
      desc.Format != this->lastFormat.Format)
  {
    ++this->formatVer;
    memcpy(&this->lastFormat, &desc, sizeof(desc));
  }

  const unsigned int maxRows = maxFrameSize / (desc.Width * 4);

  frame->formatVer        = this->formatVer;
  frame->screenWidth      = desc.Width;
  frame->screenHeight     = desc.Height;
  frame->dataWidth        = desc.Width;
  frame->dataHeight       = min(maxRows, desc.Height);
  frame->frameWidth       = desc.Width;
  frame->frameHeight      = desc.Height;
  frame->truncated        = maxRows < desc.Height;
  frame->pitch            = desc.Width * 4;
  frame->stride           = desc.Width;
  frame->format           = CAPTURE_FMT_BGRA;
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
  comRef_scopePush(2);

  comRef_defineLocal(ID3D12Resource, src);
  *src = this->backend->fetch(frameBufferIndex);
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

  // copy into the framebuffer resource
  D3D12_RESOURCE_DESC desc = ID3D12Resource_GetDesc(*src);
  D3D12_TEXTURE_COPY_LOCATION srcLoc =
  {
    .pResource        = *src,
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
        .Format   = desc.Format,
        .Width    = desc.Width,
        .Height   = desc.Height,
        .Depth    = 1,
        .RowPitch = desc.Width * 4
      }
    }
  };

  ID3D12GraphicsCommandList_CopyTextureRegion(
    *this->copyCommand.gfxList, &dstLoc, 0, 0, 0, &srcLoc, NULL);

  // allow the backend to insert a fence into the command queue if it needs it
  result = this->backend->sync(*this->commandQueue);
  if (result != CAPTURE_RESULT_OK)
    goto exit;

  d12_executeCommandGroup(&this->copyCommand);

  framebuffer_set_write_ptr(frameBuffer, desc.Height * desc.Width * 4);
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

static bool d12_createCommandGroup(
  ID3D12Device3            * device,
  D3D12_COMMAND_LIST_TYPE    type,
  D12CommandGroup          * dst,
  LPCWSTR name)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(10);

  comRef_defineLocal(ID3D12CommandAllocator, allocator);
  hr = ID3D12Device3_CreateCommandAllocator(
    device,
    type,
    &IID_ID3D12CommandAllocator,
    (void **)allocator);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to create the ID3D12CommandAllocator");
    goto exit;
  }
  ID3D12CommandAllocator_SetName(*allocator, name);

  comRef_defineLocal(ID3D12GraphicsCommandList, gfxList);
  hr = ID3D12Device3_CreateCommandList(
    device,
    0,
    type,
    *allocator,
    NULL,
    &IID_ID3D12GraphicsCommandList,
    (void **)gfxList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create ID3D12GraphicsCommandList", hr);
    goto exit;
  }
  ID3D12GraphicsCommandList_SetName(*gfxList, name);

  comRef_defineLocal(ID3D12CommandList, cmdList);
  hr = ID3D12GraphicsCommandList_QueryInterface(
    *gfxList, &IID_ID3D12CommandList, (void **)cmdList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to query the ID3D12CommandList interface", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D12Fence, fence);
  hr = ID3D12Device3_CreateFence(
    device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create ID3D12Fence", hr);
    goto exit;
  }

  // Create the completion event for the fence
  HANDLE event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!event)
  {
    DEBUG_WINERROR("Failed to create the completion event", GetLastError());
    goto exit;
  }

  comRef_toGlobal(dst->allocator, allocator);
  comRef_toGlobal(dst->gfxList  , gfxList  );
  comRef_toGlobal(dst->cmdList  , cmdList  );
  comRef_toGlobal(dst->fence    , fence    );
  dst->event      = event;
  dst->fenceValue = 0;

  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void d12_freeCommandGroup(
  D12CommandGroup * grp)
{
  // com objet release is handled by comRef, but the handle is not
  if (grp->event)
  {
    CloseHandle(grp->event);
    grp->event = NULL;
  }
}

static bool d12_executeCommandGroup(
  D12CommandGroup * grp)
{
  HRESULT hr = ID3D12GraphicsCommandList_Close(*grp->gfxList);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to close the command list", hr);
    return false;
  }

  ID3D12CommandQueue_ExecuteCommandLists(
    *this->commandQueue, 1, grp->cmdList);

  hr = ID3D12CommandQueue_Signal(
    *this->commandQueue, *grp->fence, ++grp->fenceValue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to set the fence signal", hr);
    return false;
  }

  if (ID3D12Fence_GetCompletedValue(*grp->fence) < grp->fenceValue)
  {
    ID3D12Fence_SetEventOnCompletion(*grp->fence, grp->fenceValue, grp->event);
    WaitForSingleObject(grp->event, INFINITE);
  }

  hr = ID3D12CommandAllocator_Reset(*grp->allocator);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to reset the command allocator", hr);
    return false;
  }

  hr = ID3D12GraphicsCommandList_Reset(*grp->gfxList, *grp->allocator, NULL);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to reset the graphics command list", hr);
    return false;
  }

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
