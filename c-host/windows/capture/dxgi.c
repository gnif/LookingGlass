/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "capture/interface.h"
#include "debug.h"
#include "windows/windebug.h"

#include <assert.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include "dxgi_extra.h"

// locals
struct iface
{
  bool                     initialized;
  IDXGIFactory1          * factory;
  IDXGIAdapter1          * adapter;
  IDXGIOutput            * output;
  ID3D11Device           * device;
  ID3D11DeviceContext    * deviceContext;
  D3D_FEATURE_LEVEL        featureLevel;
  IDXGIOutputDuplication * dup;
  ID3D11Texture2D        * texture;
  bool                     hasFrame;

  unsigned int  width;
  unsigned int  height;
  unsigned int  pitch;
  unsigned int  stride;
  CaptureFormat format;
};

static bool           dpiDone = false;
static struct iface * this    = NULL;

// forwards

static bool          dxgi_deinit();
static CaptureResult dxgi_releaseFrame();

// implementation

static const char * dxgi_getName()
{
  return "DXGI";
}

static bool dxgi_create()
{
  assert(!this);
  this = calloc(sizeof(struct iface), 1);
  if (!this)
  {
    DEBUG_ERROR("failed to allocate iface struct");
    return false;
  }

  return true;
}

static bool dxgi_init()
{
  assert(this);

  // this is required for DXGI 1.5 support to function
  if (!dpiDone)
  {
    DECLARE_HANDLE(DPI_AWARENESS_CONTEXT);
    #define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2  ((DPI_AWARENESS_CONTEXT)-4)
    typedef BOOL (*User32_SetProcessDpiAwarenessContext)(DPI_AWARENESS_CONTEXT value);

    HMODULE user32 = LoadLibraryA("user32.dll");
    User32_SetProcessDpiAwarenessContext fn;
    fn = (User32_SetProcessDpiAwarenessContext)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (fn)
      fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FreeLibrary(user32);
    dpiDone = true;
  }

  HRESULT          status;
  IDXGIFactory1  * factory;
  IDXGIAdapter1  * adapter;
  IDXGIOutput    * output;
  DXGI_OUTPUT_DESC outputDesc;

  status = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create DXGIFactory1", status);
    goto fail;
  }

  for(int i = 0; IDXGIFactory1_EnumAdapters1(factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    for(int n = 0; IDXGIAdapter1_EnumOutputs(adapter, n, &output) != DXGI_ERROR_NOT_FOUND; ++n)
    {
      IDXGIOutput_GetDesc(output, &outputDesc);
      if (!outputDesc.AttachedToDesktop)
      {
        IDXGIOutput_Release(output);
        output = NULL;
        continue;
      }

      break;
    }

    if (output)
      break;

    IDXGIAdapter1_Release(adapter);
    adapter = NULL;
  }

  if (!output)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    goto fail;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1
  };

  IDXGIAdapter * tmp;
  status = IDXGIAdapter1_QueryInterface(adapter, &IID_IDXGIAdapter, (void **)&tmp);
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to query IDXGIAdapter interface");
    goto fail_release;
  }

  status = D3D11CreateDevice(
    tmp,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
    featureLevels, sizeof(featureLevels) / sizeof(D3D_FEATURE_LEVEL),
    D3D11_SDK_VERSION,
    &this->device,
    &this->featureLevel,
    &this->deviceContext);

  IDXGIAdapter_Release(tmp);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    goto fail_release;
 }

  DXGI_ADAPTER_DESC1 adapterDesc;
  IDXGIAdapter1_GetDesc1(adapter, &adapterDesc);
  this->width  = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
  this->height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

  DEBUG_INFO("Device Descripion: %ls"  , adapterDesc.Description);
  DEBUG_INFO("Device Vendor ID : 0x%x" , adapterDesc.VendorId);
  DEBUG_INFO("Device Device ID : 0x%x" , adapterDesc.DeviceId);
  DEBUG_INFO("Device Video Mem : %u MiB", (unsigned)(adapterDesc.DedicatedVideoMemory  / 1048576));
  DEBUG_INFO("Device Sys Mem   : %u MiB", (unsigned)(adapterDesc.DedicatedSystemMemory / 1048576));
  DEBUG_INFO("Shared Sys Mem   : %u MiB", (unsigned)(adapterDesc.SharedSystemMemory    / 1048576));
  DEBUG_INFO("Feature Level    : 0x%x", this->featureLevel);
  DEBUG_INFO("Capture Size     : %u x %u", this->width, this->height);

  // bump up our priority
  {
    IDXGIDevice * dxgi;
    status = ID3D11Device_QueryInterface(this->device, &IID_IDXGIDevice, (void **)&dxgi);
    if (FAILED(status))
    {
      DEBUG_WINERROR("failed to query DXGI interface from device", status);
      goto fail_release_device;
    }

    IDXGIDevice_SetGPUThreadPriority(dxgi, 7);
    IDXGIDevice_Release(dxgi);
  }

  IDXGIOutput5 * output5 = NULL;
  status = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput5, (void **)&output5);
  if (FAILED(status))
  {
    DEBUG_WARN("IDXGIOutput5 is not available, please update windows for improved performance!");
    DEBUG_WARN("Falling back to IDXIGOutput1");

    IDXGIOutput1 * output1 = NULL;
    status = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)&output1);
    if (FAILED(status))
    {
      DEBUG_ERROR("Failed to query IDXGIOutput1 from the output");
      goto fail_release_device;
    }

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      status = IDXGIOutput1_DuplicateOutput(output1, (IUnknown *)this->device, &this->dup);
      if (SUCCEEDED(status))
        break;
      Sleep(200);
    }

    if (FAILED(status))
    {
      DEBUG_WINERROR("DuplicateOutput Failed", status);
      IDXGIOutput1_Release(output1);
      goto fail_release_device;
    }
  }
  else
  {
    const DXGI_FORMAT supportedFormats[] =
    {
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R10G10B10A2_UNORM
    };

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      status = IDXGIOutput5_DuplicateOutput1(
        output5,
        (IUnknown *)this->device,
        0,
        sizeof(supportedFormats) / sizeof(DXGI_FORMAT),
        supportedFormats,
        &this->dup);

      if (SUCCEEDED(status))
        break;

      // if access is denied we just keep trying until it isn't
      if (status == E_ACCESSDENIED)
        --i;

      Sleep(200);
    }

    if (FAILED(status))
    {
      DEBUG_WINERROR("DuplicateOutput1 Failed", status);
      IDXGIOutput5_Release(output5);
      goto fail_release_device;
    }
  }

  DXGI_OUTDUPL_DESC dupDesc;
  IDXGIOutputDuplication_GetDesc(this->dup, &dupDesc);
  DEBUG_INFO("Source Format    : %s", GetDXGIFormatStr(dupDesc.ModeDesc.Format));

  switch(dupDesc.ModeDesc.Format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM   : this->format = CAPTURE_FMT_BGRA  ; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM   : this->format = CAPTURE_FMT_RGBA  ; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: this->format = CAPTURE_FMT_RGBA10; break;
    default:
      DEBUG_ERROR("Unsupported source format");
      goto fail_release_output;
  }

  D3D11_TEXTURE2D_DESC texDesc;
  memset(&texDesc, 0, sizeof(texDesc));
  texDesc.Width              = this->width;
  texDesc.Height             = this->height;
  texDesc.MipLevels          = 1;
  texDesc.ArraySize          = 1;
  texDesc.SampleDesc.Count   = 1;
  texDesc.SampleDesc.Quality = 0;
  texDesc.Usage              = D3D11_USAGE_STAGING;
  texDesc.Format             = dupDesc.ModeDesc.Format;
  texDesc.BindFlags          = 0;
  texDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
  texDesc.MiscFlags          = 0;

  status = ID3D11Device_CreateTexture2D(this->device, &texDesc, NULL, &this->texture);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    goto fail_release_output;
  }

  // map the texture simply to get the pitch and stride
  D3D11_MAPPED_SUBRESOURCE mapping;
  status = ID3D11DeviceContext_Map(this->deviceContext, (ID3D11Resource *)this->texture, 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    goto fail_release_output;
  }
  this->pitch  = mapping.RowPitch;
  this->stride = mapping.RowPitch / 4;
  ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource *)this->texture, 0);

  this->factory     = factory;
  this->adapter     = adapter;
  this->output      = output;
  this->initialized = true;
  return true;

fail_release_output:
  IDXGIOutputDuplication_Release(this->dup);
  this->dup = NULL;
fail_release_device:
  ID3D11Device_Release(this->device);
  this->device = NULL;
fail_release:
  IDXGIOutput_Release  (output );
  IDXGIAdapter1_Release(adapter);
  IDXGIFactory1_Release(factory);
fail:
  return false;
}

static bool dxgi_deinit()
{
  assert(this);

  if (this->texture)
  {
    ID3D11Texture2D_Release(this->texture);
    this->texture = NULL;
  }

  if (this->dup)
  {
    dxgi_releaseFrame();
    IDXGIOutputDuplication_Release(this->dup);
    this->dup = NULL;
  }

  if (this->device)
  {
    ID3D11Device_Release(this->device);
    this->device = NULL;
  }

  if (this->output)
  {
    IDXGIOutput_Release(this->output);
    this->output = NULL;
  }

  if (this->adapter)
  {
    IDXGIAdapter1_Release(this->adapter);
    this->adapter = NULL;
  }

  if (this->factory)
  {
    IDXGIFactory1_Release(this->factory);
    this->factory = NULL;
  }

  this->initialized = false;
  return true;
}

static void dxgi_free()
{
  assert(this);

  if (this->initialized)
    dxgi_deinit();

  free(this);
  this = NULL;
}

static unsigned int dxgi_getMaxFrameSize()
{
  assert(this);
  assert(this->initialized);

  return this->height * this->pitch;
}

static CaptureResult dxgi_capture(bool * hasFrameUpdate, bool * hasPointerUpdate)
{
  assert(this);
  assert(this->initialized);

  CaptureResult             result;
  HRESULT                   status;
  DXGI_OUTDUPL_FRAME_INFO   frameInfo;
  IDXGIResource           * res;

  result = dxgi_releaseFrame();
  if (result != CAPTURE_RESULT_OK)
    return result;

  status = IDXGIOutputDuplication_AcquireNextFrame(this->dup, 1000, &frameInfo, &res);
  switch(status)
  {
    case S_OK:
      this->hasFrame = true;
      break;

    case DXGI_ERROR_WAIT_TIMEOUT:
      return CAPTURE_RESULT_TIMEOUT;

    case WAIT_ABANDONED:
    case DXGI_ERROR_ACCESS_LOST:
      return CAPTURE_RESULT_REINIT;

    default:
      DEBUG_WINERROR("AcquireNextFrame failed", status);
      return CAPTURE_RESULT_ERROR;
  }

  ID3D11Texture2D * src;
  status = IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&src);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to get the texture from the dxgi resource", status);
    return CAPTURE_RESULT_ERROR;
  }

  ID3D11DeviceContext_CopyResource(this->deviceContext,
    (ID3D11Resource *)this->texture, (ID3D11Resource *)src);

  ID3D11Texture2D_Release(src);
  IDXGIResource_Release(res);

  *hasFrameUpdate = true;

  if (frameInfo.PointerShapeBufferSize > 0)
    *hasPointerUpdate = true;

  return CAPTURE_RESULT_OK;
}

static bool dxgi_getFrame(CaptureFrame * frame)
{
  assert(this);
  assert(this->initialized);

  frame->width  = this->width;
  frame->height = this->height;
  frame->pitch  = this->pitch;
  frame->format = this->format;

  HRESULT status;
  D3D11_MAPPED_SUBRESOURCE mapping;
  status = ID3D11DeviceContext_Map(this->deviceContext, (ID3D11Resource*)this->texture, 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    return false;
  }

  memcpy(frame->data, mapping.pData, this->pitch * this->height);

  ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource*)this->texture, 0);
  return true;
}

static CaptureResult dxgi_releaseFrame()
{
  assert(this);
  if (!this->hasFrame)
    return CAPTURE_RESULT_OK;

  HRESULT status = IDXGIOutputDuplication_ReleaseFrame(this->dup);
  switch(status)
  {
    case S_OK:
      break;

    case DXGI_ERROR_INVALID_CALL:
      DEBUG_WINERROR("Frame was already released", status);
      return CAPTURE_RESULT_ERROR;

    case WAIT_ABANDONED:
    case DXGI_ERROR_ACCESS_LOST:
    {
      this->hasFrame = false;
      return CAPTURE_RESULT_REINIT;
    }

    default:
      DEBUG_WINERROR("ReleaseFrame failed", status);
      return CAPTURE_RESULT_ERROR;
  }

  this->hasFrame = false;
  return CAPTURE_RESULT_OK;
}

struct CaptureInterface Capture_DXGI =
{
  .getName         = dxgi_getName,
  .create          = dxgi_create,
  .init            = dxgi_init,
  .deinit          = dxgi_deinit,
  .free            = dxgi_free,
  .getMaxFrameSize = dxgi_getMaxFrameSize,
  .capture         = dxgi_capture,
  .getFrame        = dxgi_getFrame
};