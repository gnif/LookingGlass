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
#include "d12.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"

#include <d3d11.h>
#include <d3d11_4.h>

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>

#define CACHE_SIZE 10

typedef struct DDCacheInfo
{
  D3D11_TEXTURE2D_DESC format;

  /* this value is likely released, only used to check if the texture supplied
  by DD is different, do not rely on it pointing to valid memory! */
  ID3D11Texture2D    * srcTex;

  ID3D12Resource    ** d12Res;
  ID3D11Fence       ** fence;
  ID3D12Fence       ** d12Fence;
  UINT64               fenceValue;
  bool                 ready;

  RECT     dirtyRects[D12_MAX_DIRTY_RECTS];
  unsigned nbDirtyRects;
}
DDCacheInfo;

typedef struct DDInstance
{
  D12Backend base;

  HDESK desktop;

  ID3D12Device3          ** d12device;
  ID3D11Device5          ** device;
  ID3D11DeviceContext4   ** context;
  IDXGIOutputDuplication ** dup;
  CaptureRotation           rotation;
  DXGI_COLOR_SPACE_TYPE     colorSpace;
  bool                      release;

  DDCacheInfo cache[CACHE_SIZE];
  DDCacheInfo * current;

  bool                          lastPosValid;
  DXGI_OUTDUPL_POINTER_POSITION lastPos;

  void   * shapeBuffer;
  unsigned shapeBufferSize;
}
DDInstance;

static void d12_dd_openDesktop(DDInstance * this);
static bool d12_dd_handleFrameUpdate(DDInstance * this, IDXGIResource * res);

static void d12_dd_handlePointerMovement(DDInstance * this,
  DXGI_OUTDUPL_POINTER_POSITION * pos, CapturePointer * pointer, bool * changed);
static void d12_dd_handlePointerShape(DDInstance * this,
  CapturePointer * pointer, size_t size, bool * changed);

static bool d12_dd_getCache(DDInstance * this,
  ID3D11Texture2D * srcTex, DDCacheInfo ** result);
static bool d12_dd_convertResource(DDInstance * this,
  ID3D11Texture2D * srcTex, DDCacheInfo * cache);

static bool d12_dd_create(D12Backend ** instance, unsigned frameBuffers)
{
  DDInstance * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  *instance = &this->base;
  return true;
}

static bool d12_dd_init(
  D12Backend         * instance,
  bool                 debug,
  ID3D12Device3      * device,
  IDXGIAdapter1      * adapter,
  IDXGIOutput        * output)
{
  DDInstance * this = UPCAST(DDInstance, instance);

  bool result = false;
  HRESULT hr;

  comRef_scopePush(10);

  // try to open the desktop so we can capture the secure desktop
  d12_dd_openDesktop(this);

  comRef_defineLocal(IDXGIAdapter, _adapter);
  hr = IDXGIAdapter1_QueryInterface(
    adapter, &IID_IDXGIAdapter, (void **)_adapter);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to get the IDXGIAdapter interface");
    goto exit;
  }

  // only 11.1 supports DX12 interoperability
  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_11_1
  };
  D3D_FEATURE_LEVEL featureLevel;

  // create a DirectX11 context
  comRef_defineLocal(ID3D11Device       , d11device);
  comRef_defineLocal(ID3D11DeviceContext, d11context);

  DEBUG_TRACE("D3D11CreateDevice");
  hr = D3D11CreateDevice(
    *_adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
      (debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
    featureLevels,
    ARRAY_LENGTH(featureLevels),
    D3D11_SDK_VERSION,
    d11device,
    &featureLevel,
    d11context);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the D3D11Device", hr);
    goto exit;
  }

  DEBUG_INFO("Feature Level     : 0x%x", featureLevel);

  // get the updated interfaces
  comRef_defineLocal(ID3D11DeviceContext4, d11context4);
  hr = ID3D11DeviceContext_QueryInterface(
    *d11context, &IID_ID3D11DeviceContext4, (void **)d11context4);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Context4 interface", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D11Device5, d11device5);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_ID3D11Device5, (void **)d11device5);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Device5 interface", hr);
    goto exit;
  }

  // try to reduce the latency
  comRef_defineLocal(IDXGIDevice1, dxgi1);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_IDXGIDevice1, (void **)dxgi1);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("failed to query the DXGI interface from the device", hr);
    goto exit;
  }
  IDXGIDevice1_SetMaximumFrameLatency(*dxgi1, 1);

  // duplicate the output
  comRef_defineLocal(IDXGIOutput5          , output5);
  comRef_defineLocal(IDXGIOutputDuplication, dup    );
  hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput5, (void **)output5);
  if (FAILED(hr))
  {
    DEBUG_WARN("IDXGIOutput5 is not available, "
               "please update windows for improved performance!");
    DEBUG_WARN("Falling back to IDXGIOutput1");

    comRef_defineLocal(IDXGIOutput1, output1);
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)output1);
    if (FAILED(hr))
    {
      DEBUG_ERROR("Failed to query IDXGIOutput1 from the output");
      goto exit;
    }

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      DEBUG_TRACE("IDXGIOutput1_DuplicateOutput");
      hr = IDXGIOutput1_DuplicateOutput(*output1, *(IUnknown **)d11device, dup);
      if (SUCCEEDED(hr))
        break;
      Sleep(200);
    }
  }
  else
  {
    static const DXGI_FORMAT supportedFormats[] =
    {
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      DEBUG_TRACE("IDXGIOutput5_DuplicateOutput1");
      hr = IDXGIOutput5_DuplicateOutput1(
        *output5,
        *(IUnknown **)d11device,
        0,
        ARRAY_LENGTH(supportedFormats),
        supportedFormats,
        dup);

      if (SUCCEEDED(hr))
        break;

      // if access is denied we just keep trying until it isn't
      if (hr == E_ACCESSDENIED)
        --i;

      Sleep(200);
    }
  }

  if (FAILED(hr))
  {
    DEBUG_WINERROR("DuplicateOutput Failed", hr);
    goto exit;
  }

  DXGI_OUTDUPL_DESC dupDesc;
  IDXGIOutputDuplication_GetDesc(*dup, &dupDesc);
  switch(dupDesc.Rotation)
  {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
      this->rotation = CAPTURE_ROT_0;
      break;

    case DXGI_MODE_ROTATION_ROTATE90:
      this->rotation = CAPTURE_ROT_90;
      break;

    case DXGI_MODE_ROTATION_ROTATE180:
      this->rotation = CAPTURE_ROT_180;
      break;

    case DXGI_MODE_ROTATION_ROTATE270:
      this->rotation = CAPTURE_ROT_270;
      break;
  }

  comRef_defineLocal(IDXGIOutput6, output6);
  hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)output6);
  if (FAILED(hr))
    this->colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  else
  {
    DXGI_OUTPUT_DESC1 desc1;
    IDXGIOutput6_GetDesc1(*output6, &desc1);
    this->colorSpace = desc1.ColorSpace;
  }

  ID3D12Device3_AddRef(device);
  comRef_toGlobal(this->d12device, &device    );
  comRef_toGlobal(this->device   , d11device5 );
  comRef_toGlobal(this->context  , d11context4);
  comRef_toGlobal(this->dup      , dup        );
  result = true;

exit:
  comRef_scopePop();

  return result;
}

static bool d12_dd_deinit(D12Backend * instance)
{
  DDInstance * this = UPCAST(DDInstance, instance);

  if (this->release)
  {
    DEBUG_TRACE("IDXGIOutputDuplication_ReleaseFrame");
    IDXGIOutputDuplication_ReleaseFrame(*this->dup);
    this->release = false;
  }

  if (this->desktop)
  {
    DEBUG_TRACE("CloseDesktop");
    CloseDesktop(this->desktop);
    this->desktop = NULL;
  }

  memset(this->cache, 0, sizeof(this->cache));
  return true;
}

static void d12_dd_free(D12Backend ** instance)
{
  DDInstance * this = UPCAST(DDInstance, *instance);

  free(this->shapeBuffer);
  free(this);
  *instance = NULL;
}

static CaptureResult d12_dd_hResultToCaptureResult(const HRESULT status)
{
  switch(status)
  {
    case S_OK:
      return CAPTURE_RESULT_OK;

    case DXGI_ERROR_WAIT_TIMEOUT:
      return CAPTURE_RESULT_TIMEOUT;

    case WAIT_ABANDONED:
    case DXGI_ERROR_ACCESS_LOST:
    case DXGI_ERROR_INVALID_CALL:
      return CAPTURE_RESULT_REINIT;

    default:
      return CAPTURE_RESULT_ERROR;
  }
}

static CaptureResult d12_dd_capture(D12Backend * instance,
  unsigned frameBufferIndex)
{
  DDInstance * this = UPCAST(DDInstance, instance);

  HRESULT hr;
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(10);

  DXGI_OUTDUPL_FRAME_INFO frameInfo = {0};
  comRef_defineLocal(IDXGIResource, res);

retry:
  if (this->release)
  {
    IDXGIOutputDuplication_ReleaseFrame(*this->dup);
    this->release = false;
  }

  hr = IDXGIOutputDuplication_AcquireNextFrame(
    *this->dup, 1000, &frameInfo, res);

  result = d12_dd_hResultToCaptureResult(hr);
  if (result != CAPTURE_RESULT_OK)
  {
    if (result == CAPTURE_RESULT_ERROR)
      DEBUG_WINERROR("AcquireNextFrame failed", hr);

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
      hr = ID3D11Device5_GetDeviceRemovedReason(*this->device);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("Device Removed", hr);
        result = CAPTURE_RESULT_ERROR;
      }
    }

    goto exit;
  }

  this->release = true;

  // if we have a new frame
  if (frameInfo.LastPresentTime.QuadPart != 0)
    if (!d12_dd_handleFrameUpdate(this, *res))
    {
      result = CAPTURE_RESULT_ERROR;
      goto exit;
    }

  bool postPointer = false;
  CapturePointer pointer = {0};

  // if the pointer has moved
  if (frameInfo.LastMouseUpdateTime.QuadPart != 0)
    d12_dd_handlePointerMovement(this,
      &frameInfo.PointerPosition, &pointer, &postPointer);

  // if the pointer shape has changed
  if (frameInfo.PointerShapeBufferSize > 0)
    d12_dd_handlePointerShape(this,
      &pointer, frameInfo.PointerShapeBufferSize, &postPointer);

  if (postPointer)
    d12_updatePointer(&pointer, this->shapeBuffer, this->shapeBufferSize);

  // if this was not a frame update, go back and try again
  if (frameInfo.LastPresentTime.QuadPart == 0)
  {
    comRef_release(res);
    goto retry;
  }

exit:
  comRef_scopePop();
  return result;
}

static CaptureResult d12_dd_sync(D12Backend * instance,
  ID3D12CommandQueue * commandQueue)
{
  DDInstance * this = UPCAST(DDInstance, instance);

  if (!this->current)
    return CAPTURE_RESULT_TIMEOUT;

  DDCacheInfo * cache = this->current;
  if (ID3D11Fence_GetCompletedValue(*cache->fence) < cache->fenceValue)
    ID3D12CommandQueue_Wait(commandQueue, *cache->d12Fence, cache->fenceValue);

  return CAPTURE_RESULT_OK;
}

static ID3D12Resource * d12_dd_fetch(D12Backend * instance,
  unsigned frameBufferIndex, D12FrameDesc * desc)
{
  DDInstance * this = UPCAST(DDInstance, instance);

  if (!this->current)
    return NULL;

  desc->dirtyRects   = this->current->dirtyRects;
  desc->nbDirtyRects = this->current->nbDirtyRects;
  desc->rotation     = this->rotation;
  desc->colorSpace   = this->colorSpace;

  ID3D12Resource_AddRef(*this->current->d12Res);
  return *this->current->d12Res;
}

static void d12_dd_openDesktop(DDInstance * this)
{
  DEBUG_TRACE("OpenInputDesktop");
  this->desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
  if (!this->desktop)
    DEBUG_WINERROR("Failed to open the desktop", GetLastError());
  else
  {
    DEBUG_TRACE("SetThreadDesktop");
    if (!SetThreadDesktop(this->desktop))
    {
      DEBUG_WINERROR("Failed to set the thread desktop", GetLastError());
      DEBUG_TRACE("CloseDesktop");
      CloseDesktop(this->desktop);
      this->desktop = NULL;
    }
  }

  if (!this->desktop)
  {
    DEBUG_INFO("The above error(s) will prevent LG from being able to capture "
               "the secure desktop (UAC dialogs)");
    DEBUG_INFO("This is not a failure, please do not report this as an issue.");
    DEBUG_INFO("To fix this, install and run the Looking Glass host as a "
               "service.");
    DEBUG_INFO("looking-glass-host.exe InstallService");
  }
}

static bool d12_dd_handleFrameUpdate(DDInstance * this, IDXGIResource * res)
{
  bool result = false;
  comRef_scopePush(1);

  comRef_defineLocal(ID3D11Texture2D, srcTex);
  HRESULT hr = IDXGIResource_QueryInterface(
    res, &IID_ID3D11Texture2D, (void **)srcTex);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the D3D11Texture2D interface", hr);
    goto exit;
  }

  if (!d12_dd_getCache(this, *srcTex, &this->current))
    goto exit;

  /**
   * Even though we have not performed any copy/draw operations we still need to
   * use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before the desktop duplication API has finished updating it.*/
  ++this->current->fenceValue;
  ID3D11DeviceContext4_Signal(
    *this->context, *this->current->fence, this->current->fenceValue);

  // handle damage tracking
  this->current->nbDirtyRects = 0;
  if (this->base.trackDamage)
  {
    /* Get the frame damage, if there is too many damage rects, we disable
     * damage tracking for the frame and assume full frame damage */

    UINT requiredSize;
    hr = IDXGIOutputDuplication_GetFrameDirtyRects(*this->dup,
      sizeof(this->current->dirtyRects),
      this->current->dirtyRects,
      &requiredSize);
    if (FAILED(hr))
    {
      if (hr != DXGI_ERROR_MORE_DATA)
      {
        DEBUG_WINERROR("GetFrameDirtyRects failed", hr);
        goto exit;
      }
    }
    else
    {
      unsigned nbDirtyRects = requiredSize / sizeof(*this->current->dirtyRects);

      // if there is only one damage rect and it covers the entire frame
      if (nbDirtyRects                      == 1 &&
        this->current->dirtyRects[0].left   == 0 &&
        this->current->dirtyRects[0].top    == 0 &&
        this->current->dirtyRects[0].right  == this->current->format.Width &&
        this->current->dirtyRects[0].bottom == this->current->format.Height)
        goto fullDamage;

      this->current->nbDirtyRects = nbDirtyRects;
    }

    DXGI_OUTDUPL_MOVE_RECT moveRects[
      (ARRAY_LENGTH(this->current->dirtyRects) - this->current->nbDirtyRects) / 2
    ];
    hr = IDXGIOutputDuplication_GetFrameMoveRects(*this->dup,
      sizeof(moveRects), moveRects, &requiredSize);
    if (FAILED(hr))
    {
      this->current->nbDirtyRects = 0;
      if (hr != DXGI_ERROR_MORE_DATA)
      {
        DEBUG_WINERROR("GetFrameMoveRects failed", hr);
        goto exit;
      }
    }

    /* Move rects are seemingly not generated on Windows 10, but incase it
     * becomes a thing in the future we still need to implement this */
    const unsigned moveRectCount = requiredSize / sizeof(*moveRects);
    for(DXGI_OUTDUPL_MOVE_RECT *moveRect = moveRects; moveRect < moveRects +
      moveRectCount; ++moveRect)
    {
      /* According to WebRTC source comments, the DirectX capture API may
       * randomly return unmoved rects, which should be skipped to avoid
       * unnecessary work */
      if (moveRect->SourcePoint.x == moveRect->DestinationRect.left &&
          moveRect->SourcePoint.y == moveRect->DestinationRect.top)
        continue;

      /* Add the source rect to the dirty array */
      this->current->dirtyRects[this->current->nbDirtyRects++] = (RECT)
      {
        .left  = moveRect->SourcePoint.x,
        .top   = moveRect->SourcePoint.y,
        .right = moveRect->SourcePoint.x +
          (moveRect->DestinationRect.right - moveRect->DestinationRect.left),
        .bottom = moveRect->SourcePoint.y +
          (moveRect->DestinationRect.bottom - moveRect->DestinationRect.top)
      };

      /* Add the destination rect to the dirty array */
      this->current->dirtyRects[this->current->nbDirtyRects++] =
        moveRect->DestinationRect;
    }
  }

fullDamage:
  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void d12_dd_handlePointerMovement(DDInstance * this,
  DXGI_OUTDUPL_POINTER_POSITION * pos, CapturePointer * pointer, bool * changed)
{
  bool setPos = false;

  // if the last position is valid, check against it for changes
  if (this->lastPosValid)
  {
    // update the position only if the pointer is visible and it has moved
    if (pos->Visible && (
        pos->Position.x != this->lastPos.Position.x ||
        pos->Position.y != this->lastPos.Position.y))
      setPos = true;

    // if the visibillity has changed
    if (pos->Visible != this->lastPos.Visible)
      *changed = true;
  }
  else
  {
    // update the position only if the pointer is visible
    setPos   = pos->Visible;

    // this is the first update, we need to send it
    *changed = true;
  }

  pointer->visible = pos->Visible;
  if (setPos)
  {
    pointer->positionUpdate = true;
    pointer->x              = pos->Position.x;
    pointer->y              = pos->Position.y;

    *changed = true;
  }

  memcpy(&this->lastPos, pos, sizeof(*pos));
  this->lastPosValid = true;
}

static void d12_dd_handlePointerShape(DDInstance * this,
  CapturePointer * pointer, size_t size, bool * changed)
{
  HRESULT hr;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO info;

retry:
  if (this->shapeBufferSize < size)
  {
    free(this->shapeBuffer);
    this->shapeBuffer = malloc(size);
    if (!this->shapeBuffer)
    {
      DEBUG_ERROR("out of memory");
      this->shapeBufferSize = 0;
      return;
    }
    this->shapeBufferSize = size;
  }

  UINT s;
  hr = IDXGIOutputDuplication_GetFramePointerShape(
    *this->dup,
    this->shapeBufferSize,
    this->shapeBuffer,
    &s,
    &info);

  if (FAILED(hr))
  {
    if (hr == DXGI_ERROR_MORE_DATA)
    {
      size = s;
      goto retry;
    }
    DEBUG_WINERROR("Failed to get the pointer shape", hr);
    return;
  }

  switch(info.Type)
  {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      pointer->format = CAPTURE_FMT_COLOR;
      break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      pointer->format = CAPTURE_FMT_MASKED;
      break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      pointer->format = CAPTURE_FMT_MONO;
      break;

    default:
      DEBUG_ERROR("Unsupporter cursor format");
      return;
  }

  pointer->shapeUpdate = true;
  pointer->width       = info.Width;
  pointer->height      = info.Height;
  pointer->pitch       = info.Pitch;
  pointer->hx          = info.HotSpot.x;
  pointer->hy          = info.HotSpot.y;

  *changed = true;
}

static bool d12_dd_getCache(DDInstance * this,
  ID3D11Texture2D * srcTex, DDCacheInfo ** result)
{
  *result = NULL;
  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(srcTex, &srcDesc);

  unsigned freeSlot = CACHE_SIZE;
  for(unsigned i = 0; i < CACHE_SIZE; ++i)
  {
    DDCacheInfo * cache = &this->cache[i];
    if (!cache->ready)
    {
      freeSlot = min(freeSlot, i);
      continue;
    }

    // check for a resource match
    if (cache->srcTex != srcTex)
      continue;

    // check if the match is not valid
    if (cache->format.Width  != srcDesc.Width  ||
        cache->format.Height != srcDesc.Height ||
        cache->format.Format != srcDesc.Format)
    {
      // break out and allow this entry to be rebuilt
      cache->ready = false;
      freeSlot = i;
      break;
    }

    // found, so return it
    *result = cache;
    return true;
  }

  // cache is full
  if (freeSlot == CACHE_SIZE)
    return false;

  // convert the resource
  if (!d12_dd_convertResource(this, srcTex, &this->cache[freeSlot]))
    return false;

  // return the new cache entry
  *result = &this->cache[freeSlot];
  return true;
}

static bool d12_dd_convertResource(DDInstance * this,
  ID3D11Texture2D * srcTex, DDCacheInfo * cache)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(10);

  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(srcTex, &srcDesc);

  // get the DXGI resource interface so we can create the shared handle
  comRef_defineLocal(IDXGIResource1, dxgiRes);
  hr = ID3D11Texture2D_QueryInterface(
    srcTex, &IID_IDXGIResource1, (void **)dxgiRes);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the shared ID3D11Resource1 interface", hr);
    goto exit;
  }

  // create the shared handle
  HANDLE sharedHandle;
  hr = IDXGIResource1_CreateSharedHandle(
    *dxgiRes, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the shared handle", hr);
    goto exit;
  }

  // open the resource as a DirectX12 resource
  comRef_defineLocal(ID3D12Resource, dst);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Resource, (void **)dst);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the D3D12Resource from the handle", hr);
    CloseHandle(sharedHandle);
    goto exit;
  }

  // close the shared handle
  CloseHandle(sharedHandle);

  // create the sync fence
  comRef_defineLocal(ID3D11Fence, fence);
  hr = ID3D11Device5_CreateFence(
    *this->device, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the fence", hr);
    goto exit;
  }

  // create the fence shared handle
  hr = ID3D11Fence_CreateSharedHandle(
    *fence, NULL, GENERIC_ALL, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the fence shared handle", hr);
    goto exit;
  }

  // open the fence as a DirectX12 fence
  comRef_defineLocal(ID3D12Fence, d12Fence);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Fence, (void **)d12Fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the D3D12Fence from the handle", hr);
    CloseHandle(sharedHandle);
    goto exit;
  }

  // close the shared handle
  CloseHandle(sharedHandle);

  // store the details
  cache->srcTex = srcTex;

  comRef_toGlobal(cache->d12Res  , dst     );
  comRef_toGlobal(cache->fence   , fence   );
  comRef_toGlobal(cache->d12Fence, d12Fence);
  memcpy(&cache->format, &srcDesc, sizeof(srcDesc));
  cache->fenceValue = 0;
  cache->ready      = true;

  result = true;
exit:
  comRef_scopePop();
  return result;
}

const D12Backend D12Backend_DD =
{
  .name     = "Desktop Duplication",
  .codeName = "DD",

  .create   = d12_dd_create,
  .init     = d12_dd_init,
  .deinit   = d12_dd_deinit,
  .free     = d12_dd_free,
  .capture  = d12_dd_capture,
  .sync     = d12_dd_sync,
  .fetch    = d12_dd_fetch
};
