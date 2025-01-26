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
#include "common/rects.h"
#include "common/vector.h"
#include "common/display.h"
#include "com_ref.h"

#include "backend.h"
#include "effects.h"
#include "command_group.h"

#include <dxgi.h>
#include <dxgi1_3.h>
#include <dxgi1_6.h>
#include <d3dcommon.h>

// definitions
struct D12Interface
{
  HMODULE d3d12;

  IDXGIFactory2      ** factory;
  ID3D12Device3      ** device;

  DISPLAYCONFIG_PATH_INFO displayPathInfo;
  ColorMetadata           colorMetadata;

  ID3D12CommandQueue ** copyQueue;
  ID3D12CommandQueue ** computeQueue;
  D12CommandGroup       copyCommand;
  D12CommandGroup       computeCommand;

  void        * ivshmemBase;
  ID3D12Heap ** ivshmemHeap;

  CaptureGetPointerBuffer  getPointerBufferFn;
  CapturePostPointerBuffer postPointerBufferFn;

  D12Backend * backend;
  Vector       effects;
  bool         effectsActive;

  // capture format tracking
  D12FrameFormat captureFormat;
  unsigned       formatVer;
  unsigned       pitch;

  // output format tracking
  D12FrameFormat dstFormat;

  // prior frame dirty rects
  RECT      dirtyRects[D12_MAX_DIRTY_RECTS];
  unsigned  nbDirtyRects;

  // options
  bool debug;
  bool trackDamage;

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

static bool d12_heapTest(ID3D12Device3 * device, ID3D12Heap * heap);

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
      .module         = "d12",
      .name           = "adapter",
      .description    = "The name of the adapter to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "d12",
      .name           = "output",
      .description    = "The name of the adapter's output to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "d12",
      .name           = "trackDamage",
      .description    = "Perform damage-aware copies (saves bandwidth)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {
      .module         = "d12",
      .name           = "debug",
      .description    = "Enable DirectX12 debugging and validation (SLOW!)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {0}
  };

  option_register(options);

  for(const D12Effect ** effect = D12Effects; *effect; ++effect)
    d12_effectInitOptions(*effect);
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

  this->debug       = option_get_bool("d12", "debug"       );
  this->trackDamage = option_get_bool("d12", "trackDamage" );

  DEBUG_INFO(
    "debug:%d trackDamage:%d",
    this->debug, this->trackDamage);

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

  if (!DX12.D3D12CreateDevice      ||
      !DX12.D3D12GetDebugInterface ||
      !DX12.D3D12SerializeVersionedRootSignature)
  {
    DEBUG_ERROR("Failed to get required exports from d3d12.dll");
    CloseHandle(this->d3d12);
    free(this);
    return false;
  }

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

  return true;
}

static bool d12_init(void * ivshmemBase, unsigned * alignSize)
{
  bool result = false;
  comRef_initGlobalScope(100, d12_comScope);
  comRef_scopePush(10);

  // create a DXGI factory
  comRef_defineLocal(IDXGIFactory2, factory);
  DEBUG_TRACE("CreateDXGIFactory2");
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

  // get the display path info
  comRef_defineLocal(IDXGIOutput6, output6);
  hr = IDXGIOutput_QueryInterface(*output, &IID_IDXGIOutput6, (void **)output6);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the IDXGIOutput6 interface", hr);
    goto exit;
  }

  DXGI_OUTPUT_DESC1 desc1;
  IDXGIOutput6_GetDesc1(*output6, &desc1);
  if (!display_getPathInfo(desc1.Monitor, &this->displayPathInfo))
  {
    DEBUG_ERROR("Failed to get the display path info");
    goto exit;
  }

  this->colorMetadata.redPrimaryX = desc1.RedPrimary[0];
  this->colorMetadata.redPrimaryY = desc1.RedPrimary[1];
  this->colorMetadata.greenPrimaryX = desc1.GreenPrimary[0];
  this->colorMetadata.greenPrimaryY = desc1.GreenPrimary[1];
  this->colorMetadata.bluePrimaryX = desc1.BluePrimary[0];
  this->colorMetadata.bluePrimaryY = desc1.BluePrimary[1];
  this->colorMetadata.whitePointX = desc1.WhitePoint[0];
  this->colorMetadata.whitePointY = desc1.WhitePoint[1];
  this->colorMetadata.minLuminance = desc1.MinLuminance;
  this->colorMetadata.maxLuminance = desc1.MaxLuminance;
  this->colorMetadata.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;

  // create the D3D12 device
  comRef_defineLocal(ID3D12Device3, device);
  DEBUG_TRACE("D3D12CreateDevice");
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

  D3D12_COMMAND_QUEUE_DESC copyQueueDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COPY,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
  };

  comRef_defineLocal(ID3D12CommandQueue, copyQueue);
  DEBUG_TRACE("D3D12Device3_CreateCommandQueue");
  hr = ID3D12Device3_CreateCommandQueue(
    *device, &copyQueueDesc, &IID_ID3D12CommandQueue, (void **)copyQueue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create ID3D12CommandQueue (copy)", hr);
    goto exit;
  }
  ID3D12CommandQueue_SetName(*copyQueue, L"Copy");

  // create the compute queue
  D3D12_COMMAND_QUEUE_DESC computeQueueDesc =
  {
    .Type     = D3D12_COMMAND_LIST_TYPE_COMPUTE,
    .Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
    .Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE
  };

  comRef_defineLocal(ID3D12CommandQueue, computeQueue);
  DEBUG_TRACE("D3D12Device3_CreateCommandQueue");
  hr = ID3D12Device3_CreateCommandQueue(
    *device, &computeQueueDesc, &IID_ID3D12CommandQueue, (void **)computeQueue);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the ID3D12CommandQueue (compute)", hr);
    goto exit;
  }
  ID3D12CommandQueue_SetName(*computeQueue, L"Compute");

  DEBUG_TRACE("d12_commandGroupCreate Copy");
  if (!d12_commandGroupCreate(
    *device, D3D12_COMMAND_LIST_TYPE_COPY, &this->copyCommand, L"Copy"))
    goto exit;

  DEBUG_TRACE("d12_commandGroupCreate Compute");
  if (!d12_commandGroupCreate(
    *device, D3D12_COMMAND_LIST_TYPE_COMPUTE, &this->computeCommand, L"Compute"))
    goto exit;

  // Create the IVSHMEM heap
  this->ivshmemBase = ivshmemBase;
  comRef_defineLocal(ID3D12Heap, ivshmemHeap);
  DEBUG_TRACE("ID3D12Device3_OpenExistingHeapFromAddress");
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

  /* Ensure we can create resources in the ivshmem heap
   * NOTE: It is safe to do this as the application has not yet setup the KVMFR
   * headers, so we can just attempt to create a resource at the start of the
   * heap without corrupting anything */
  DEBUG_TRACE("d12_heapTest");
  if (!d12_heapTest(*device, *ivshmemHeap))
  {
    DEBUG_ERROR(
      "Unable to create resources in the IVSHMEM heap, is REBAR working?");
    goto exit;
  }

  // initialize the backend
  DEBUG_TRACE("d12_backendInit");
  if (!d12_backendInit(this->backend, this->debug, *device, *adapter, *output,
    this->trackDamage))
    goto exit;

  // create the vector of effects
  vector_create(&this->effects, sizeof(D12Effect *), 0);

  // create all the effects
  for(const D12Effect ** effect = D12Effects; *effect; ++effect)
  {
    D12Effect * instance;
    switch(d12_effectCreate(*effect, &instance, *device, &this->displayPathInfo))
    {
      case D12_EFFECT_STATUS_OK:
        DEBUG_INFO("D12 Created Effect: %s", (*effect)->name);
        vector_push(&this->effects, &instance);
        break;

      case D12_EFFECT_STATUS_BYPASS:
        continue;

      case D12_EFFECT_STATUS_ERROR:
        DEBUG_ERROR("Failed to create effect: %s", (*effect)->name);
        goto exit;
    }
  }

  comRef_toGlobal(this->factory     , factory      );
  comRef_toGlobal(this->device      , device       );
  comRef_toGlobal(this->copyQueue   , copyQueue    );
  comRef_toGlobal(this->computeQueue, computeQueue );
  comRef_toGlobal(this->ivshmemHeap , ivshmemHeap  );

  DEBUG_TRACE("Init success");
  result = true;

exit:
  comRef_scopePop();
  if (!result)
  {
    DEBUG_TRACE("Init failed");
    D12Effect * effect;
    vector_forEach(effect, &this->effects)
      d12_effectFree(&effect);
    vector_destroy(&this->effects);
    comRef_freeScope(&d12_comScope);
  }

  return result;
}

static void d12_stop(void)
{
}

static bool d12_deinit(void)
{
  bool result = true;

  D12Effect * effect;
  vector_forEach(effect, &this->effects)
    d12_effectFree(&effect);
  vector_destroy(&this->effects);

  DEBUG_TRACE("Backend deinit");
  if (!d12_backendDeinit(this->backend))
    result = false;

  DEBUG_TRACE("commandGroupFree");
  d12_commandGroupFree(&this->copyCommand   );
  d12_commandGroupFree(&this->computeCommand);

  DEBUG_TRACE("comRef_freeScope");
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

  /* dirty rect history is no longer valid */
  this->nbDirtyRects = 0;

  return result;
}

static void d12_free(void)
{
  DEBUG_TRACE("d12_backendFree");
  d12_backendFree(&this->backend);
  FreeLibrary(this->d3d12);
  free(this);
  this = NULL;
}

static CaptureResult d12_capture(
  unsigned frameBufferIndex, FrameBuffer * frameBuffer)
{
  DEBUG_TRACE("d12_backendCapture");
  return d12_backendCapture(this->backend, frameBufferIndex);
}

static CaptureResult d12_waitFrame(unsigned frameBufferIndex,
  CaptureFrame * frame, const size_t maxFrameSize)
{
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(1);

  D12FrameDesc desc;

  comRef_defineLocal(ID3D12Resource, src);
  DEBUG_TRACE("d12_backendFetch");
  *src = d12_backendFetch(this->backend, frameBufferIndex, &desc);
  if (!*src)
  {
    DEBUG_ERROR("D12 backend failed to produce an expected frame: %u",
      frameBufferIndex);
    goto exit;
  }

  D12FrameFormat srcFormat =
  {
    .desc       = ID3D12Resource_GetDesc(*src),
    .colorSpace = desc.colorSpace,
    .width      = srcFormat.desc.Width,
    .height     = srcFormat.desc.Height
  };

  switch(srcFormat.desc.Format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      srcFormat.format = CAPTURE_FMT_BGRA;
      break;

    case DXGI_FORMAT_R8G8B8A8_UNORM:
      srcFormat.format = CAPTURE_FMT_RGBA;
      break;

    case DXGI_FORMAT_R10G10B10A2_UNORM:
      srcFormat.format = CAPTURE_FMT_RGBA10;
      break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      srcFormat.format = CAPTURE_FMT_RGBA16F;
      break;

    default:
      DEBUG_ERROR("Unsupported source format");
      goto exit;
  }

  // if the input format changed, reconfigure the effects
  if (srcFormat.desc.Width  == 0 ||
      srcFormat.desc.Width  != this->captureFormat.desc.Width  ||
      srcFormat.desc.Height != this->captureFormat.desc.Height ||
      srcFormat.desc.Format != this->captureFormat.desc.Format ||
      srcFormat.colorSpace  != this->captureFormat.colorSpace)
  {
    DEBUG_TRACE("Capture format changed");

    D12FrameFormat dstFormat = this->dstFormat;
    this->captureFormat      = srcFormat;
    this->effectsActive      = false;

    D12Effect * effect;
    D12FrameFormat curFormat = srcFormat;
    vector_forEach(effect, &this->effects)
    {
      dstFormat = curFormat;
      switch(d12_effectSetFormat(effect, *this->device, &curFormat, &dstFormat))
      {
        case D12_EFFECT_STATUS_OK:
          this->effectsActive = true;
          curFormat           = dstFormat;
          effect->enabled     = true;
          DEBUG_INFO("D12 Effect Active: %s", effect->name);
          break;

        case D12_EFFECT_STATUS_ERROR:
          DEBUG_ERROR("Failed to set the effect input format");
          goto exit;

        case D12_EFFECT_STATUS_BYPASS:
          effect->enabled = false;
          break;
      }
    }

    // if the output format changed
    if (dstFormat.desc.Width  != this->dstFormat.desc.Width  ||
        dstFormat.desc.Height != this->dstFormat.desc.Height ||
        dstFormat.desc.Format != this->dstFormat.desc.Format ||
        dstFormat.colorSpace  != this->dstFormat.colorSpace  ||
        dstFormat.width       != this->dstFormat.width       ||
        dstFormat.height      != this->dstFormat.height      ||
        dstFormat.format      != this->dstFormat.format)
    {
      DEBUG_TRACE("Output format changed");
      ++this->formatVer;
      this->dstFormat = dstFormat;
    }
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
  ID3D12Device3_GetCopyableFootprints(*this->device,
    &this->dstFormat.desc,
    0       , // FirstSubresource
    1       , // NumSubresources
    0       , // BaseOffset,
    &layout , // pLayouts
    NULL    , // pNumRows,
    NULL    , // pRowSizeInBytes,
    NULL);  // pTotalBytes
  this->pitch = layout.Footprint.RowPitch;

  const unsigned maxRows = maxFrameSize / layout.Footprint.RowPitch;
  const unsigned bpp     = this->dstFormat.format == CAPTURE_FMT_RGBA16F ? 8 : 4;

  frame->formatVer        = this->formatVer;
  frame->screenWidth      = srcFormat.width;
  frame->screenHeight     = srcFormat.height;
  frame->dataWidth        = this->dstFormat.desc.Width;
  frame->dataHeight       = min(maxRows, this->dstFormat.desc.Height);
  frame->frameWidth       = this->dstFormat.width;
  frame->frameHeight      = this->dstFormat.height;
  frame->truncated        = maxRows < this->dstFormat.desc.Height;
  frame->pitch            = this->pitch;
  frame->stride           = this->pitch / bpp;
  frame->format           = this->dstFormat.format;
  frame->hdr              = this->dstFormat.colorSpace ==
    DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  frame->hdrPQ            = false;
  frame->rotation         = desc.rotation;
  frame->colorMetadata    = this->colorMetadata;

  D12Effect * effect;
  vector_forEach(effect, &this->effects)
    if (effect->enabled)
      d12_effectAdjustDamage(effect, desc.dirtyRects, &desc.nbDirtyRects);

  {
    // create a clean list of rects
    FrameDamageRect allRects[desc.nbDirtyRects];
    unsigned count = 0;
    for(const RECT * rect = desc.dirtyRects;
      rect < desc.dirtyRects + desc.nbDirtyRects; ++rect)
      allRects[count++] = (FrameDamageRect){
        .x      = rect->left,
        .y      = rect->top,
        .width  = (rect->right  - rect->left),
        .height = (rect->bottom - rect->top)
      };

    count = rectsMergeOverlapping(allRects, count);

    // if there are too many rects
    if (unlikely(count > ARRAY_LENGTH(frame->damageRects)))
      frame->damageRectsCount = 0;
    else
    {
      // send the list of dirty rects for this frame
      frame->damageRectsCount = count;
      memcpy(frame->damageRects, allRects, sizeof(*allRects) * count);
    }
  }

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

  D12FrameDesc desc;

  comRef_defineLocal(ID3D12Resource, src);
  DEBUG_TRACE("d12_backendFetch");
  *src = d12_backendFetch(this->backend, frameBufferIndex, &desc);
  if (!*src)
  {
    DEBUG_ERROR("D12 backend failed to produce an expected frame: %u",
      frameBufferIndex);
    goto exit;
  }

  comRef_defineLocal(ID3D12Resource, dst)
  DEBUG_TRACE("d12_frameBufferToResource");
  *dst = d12_frameBufferToResource(frameBufferIndex, frameBuffer, maxFrameSize);
  if (!*dst)
    goto exit;

  // place a fence into the queue
  DEBUG_TRACE("d12_backendSync");
  result = d12_backendSync(this->backend,
    this->effectsActive ? *this->computeQueue : *this->copyQueue);

  if (result != CAPTURE_RESULT_OK)
    goto exit;

  ID3D12Resource * next = *src;
  D12Effect * effect;
  vector_forEach(effect, &this->effects)
  {
    if (!effect->enabled)
      continue;

    DEBUG_TRACE("d12_effectRun: %s", effect->name);
    next = d12_effectRun(effect,
      *this->device,
      *this->computeCommand.gfxList,
      next,
      desc.dirtyRects,
      &desc.nbDirtyRects);
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
        .Format   = this->dstFormat.desc.Format,
        .Width    = this->dstFormat.desc.Width,
        .Height   = this->dstFormat.desc.Height,
        .Depth    = 1,
        .RowPitch = this->pitch
      }
    }
  };

  // if full frame damage
  if (desc.nbDirtyRects == 0)
  {
    DEBUG_TRACE("Full frame damage");
    this->nbDirtyRects = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(
      *this->copyCommand.gfxList, &dstLoc, 0, 0, 0, &srcLoc, NULL);
  }
  else
  {
    /* if the prior frame was a full update */
    if (this->nbDirtyRects == 0)
    {
      DEBUG_TRACE("Full frame update");

      /* the prior frame was fully damaged, we must update everything */
      ID3D12GraphicsCommandList_CopyTextureRegion(
        *this->copyCommand.gfxList, &dstLoc, 0, 0, 0, &srcLoc, NULL);
    }
    else
    {
      DEBUG_TRACE("Damage aware update");

      FrameDamageRect allRects[this->nbDirtyRects + desc.nbDirtyRects];
      unsigned count = 0;

      /* we must update the rects that were dirty in the prior frame also,
       * otherwise the frame in memory will not be consistent when areas need to
       * be redrawn by the client, such as under the cursor */
      for(const RECT * rect = this->dirtyRects;
        rect < this->dirtyRects + this->nbDirtyRects; ++rect)
        allRects[count++] = (FrameDamageRect){
          .x      = rect->left,
          .y      = rect->top,
          .width  = rect->right  - rect->left,
          .height = rect->bottom - rect->top
        };

      /* add the new dirtyRects to the array */
      for(const RECT * rect = desc.dirtyRects;
        rect < desc.dirtyRects + desc.nbDirtyRects; ++rect)
        allRects[count++] = (FrameDamageRect){
          .x      = rect->left,
          .y      = rect->top,
          .width  = rect->right  - rect->left,
          .height = rect->bottom - rect->top
        };

      /* resolve the rects */
      count = rectsMergeOverlapping(allRects, count);

      /* copy all the rects */
      for(FrameDamageRect * rect = allRects; rect < allRects + count; ++rect)
      {
        D3D12_BOX box =
        {
          .left   = rect->x,
          .top    = rect->y,
          .front  = 0,
          .back   = 1,
          .right  = rect->x + rect->width,
          .bottom = rect->y + rect->height
        };

        ID3D12GraphicsCommandList_CopyTextureRegion(
          *this->copyCommand.gfxList, &dstLoc,
          box.left, box.top, 0, &srcLoc, &box);
      }
    }

    /* store the dirty rects for the next frame */
    memcpy(this->dirtyRects, desc.dirtyRects,
      desc.nbDirtyRects * sizeof(*this->dirtyRects));
    this->nbDirtyRects = desc.nbDirtyRects;
  }

  // execute the compute commands
  if (this->effectsActive)
  {
    DEBUG_TRACE("Execute compute commands");
    d12_commandGroupExecute(*this->computeQueue, &this->computeCommand);

    // insert a fence to wait for the compute commands to finish
    DEBUG_TRACE("Fence wait");
    ID3D12CommandQueue_Wait(*this->copyQueue,
      *this->computeCommand.fence, this->computeCommand.fenceValue);
  }

  // execute the copy commands
  DEBUG_TRACE("Execute copy commands");
  d12_commandGroupExecute(*this->copyQueue, &this->copyCommand);

  // wait for the copy to complete
  DEBUG_TRACE("Fence wait");
  d12_commandGroupWait(&this->copyCommand);

  // signal the frame is complete
  framebuffer_set_write_ptr(frameBuffer,
    this->dstFormat.desc.Height * this->pitch);

  // reset the command queues
  if (this->effectsActive)
  {
    DEBUG_TRACE("Reset compute command group");
    if (!d12_commandGroupReset(&this->computeCommand))
      goto exit;
  }

  DEBUG_TRACE("Reset copy command group");
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

  const char * _optAdapter = option_get_string("d12", "adapter");
  const char * _optOutput  = option_get_string("d12", "output" );

  wchar_t * optAdapter = NULL;
  wchar_t * optOutput  = NULL;
  if (_optAdapter)
  {
    optAdapter = malloc((strlen(_optAdapter) + 1) * 2);
    mbstowcs(optAdapter, _optAdapter, strlen(_optAdapter));
  }

  if (_optOutput)
  {
    optOutput = malloc((strlen(_optOutput) + 1) * 2);
    mbstowcs(optOutput, _optOutput, strlen(_optOutput));
  }

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

    if (optAdapter)
    {
      if (wcsstr(adapterDesc.Description, optAdapter) == NULL)
      {
        DEBUG_INFO("Not using adapter: %ls", adapterDesc.Description);
        continue;
      }
      DEBUG_INFO("Adapter matched, trying: %ls", adapterDesc.Description);
    }

    for(
      int n = 0;
      IDXGIAdapter1_EnumOutputs(*adapter, n, output) != DXGI_ERROR_NOT_FOUND;
      ++n, comRef_release(output))
    {
      IDXGIOutput_GetDesc(*output, &outputDesc);

      if (optOutput)
      {
        if (wcsstr(outputDesc.DeviceName, optOutput) == NULL)
        {
          DEBUG_INFO("Not using adapter output: %ls", outputDesc.DeviceName);
          continue;
        }

        DEBUG_INFO("Adapter output matched, trying: %ls", outputDesc.DeviceName);
      }

      if (outputDesc.AttachedToDesktop)
        break;
    }

    if (*output)
      break;
  }

  free(optAdapter);
  free(optOutput );

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

static bool d12_heapTest(ID3D12Device3 * device, ID3D12Heap * heap)
{
  bool result = false;
  comRef_scopePush(1);

  D3D12_RESOURCE_DESC desc =
  {
    .Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
    .Width              = 1048576,
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
    device,
    heap,
    0,
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

  /* the above may succeed even if there was a fault, as such we also need to
   * check if the device was removed */
  hr = ID3D12Device3_GetDeviceRemovedReason(device);
  if (hr != S_OK)
  {
    DEBUG_WINERROR("Device Removed", hr);
    goto exit;
  }

  result = true;

exit:
  comRef_scopePop();
  return result;
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
