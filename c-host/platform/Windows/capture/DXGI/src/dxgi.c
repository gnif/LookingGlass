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

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/event.h"

#include <assert.h>
#include <dxgi.h>
#include <d3d11.h>
#include <d3dcommon.h>

#include "dxgi_extra.h"

#define LOCKED(x) INTERLOCKED_SECTION(this->deviceContextLock, x)

enum TextureState
{
  TEXTURE_STATE_UNUSED,
  TEXTURE_STATE_PENDING_MAP,
  TEXTURE_STATE_MAPPED
};

typedef struct Texture
{
  enum TextureState          state;
  ID3D11Texture2D          * tex;
  D3D11_MAPPED_SUBRESOURCE   map;
}
Texture;

typedef struct Pointer
{
  unsigned int  version;

  unsigned int  x, y;
  unsigned int  w, h;
  bool          visible;
  unsigned int  pitch;
  CaptureFormat format;
}
Pointer;

// locals
struct iface
{
  bool                       initialized;
  LARGE_INTEGER              perfFreq;
  LARGE_INTEGER              frameTime;
  bool                       stop;
  IDXGIFactory1            * factory;
  IDXGIAdapter1            * adapter;
  IDXGIOutput              * output;
  ID3D11Device             * device;
  ID3D11DeviceContext      * deviceContext;
  volatile int               deviceContextLock;
  bool                       useAcquireLock;
  D3D_FEATURE_LEVEL          featureLevel;
  IDXGIOutputDuplication   * dup;
  int                        maxTextures;
  Texture                  * texture;
  int                        texRIndex;
  int                        texWIndex;
  volatile int               texReady;
  bool                       needsRelease;
  LGEvent                  * pointerEvent;
  LGEvent                  * frameEvent;

  unsigned int  width;
  unsigned int  height;
  unsigned int  pitch;
  unsigned int  stride;
  CaptureFormat format;

  // pointer state
  Pointer lastPointer;
  Pointer pointer;

  // pointer shape
  void         * pointerShape;
  unsigned int   pointerSize;
  unsigned int   pointerUsed;
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

static void dxgi_initOptions()
{
  struct Option options[] =
  {
    {
      .module         = "dxgi",
      .name           = "adapter",
      .description    = "The name of the adapter to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "dxgi",
      .name           = "output",
      .description    = "The name of the adapter's output to capture",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL
    },
    {
      .module         = "dxgi",
      .name           = "maxTextures",
      .description    = "The maximum number of frames to buffer before skipping",
      .type           = OPTION_TYPE_INT,
      .value.x_int    = 3
    },
    {
      .module         = "dxgi",
      .name           = "useAcquireLock",
      .description    = "Enable locking around `AcquireFrame` (use if freezing, may lower performance)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {0}
  };

  option_register(options);
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

  this->pointerEvent = lgCreateEvent(true, 10);
  if (!this->pointerEvent)
  {
    DEBUG_ERROR("failed to create the pointer event");
    free(this);
    return false;
  }

  this->frameEvent = lgCreateEvent(true, 17); // 60Hz = 16.7ms
  if (!this->frameEvent)
  {
    DEBUG_ERROR("failed to create the frame event");
    free(this);
    return false;
  }

  this->maxTextures = option_get_int("dxgi", "maxTextures");
  if (this->maxTextures <= 0)
    this->maxTextures = 1;

  this->useAcquireLock = option_get_bool("dxgi", "useAcquireLock");
  this->texture        = calloc(sizeof(struct Texture), this->maxTextures);
  return true;
}

static bool dxgi_init(void * pointerShape, const unsigned int pointerSize)
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
  DXGI_OUTPUT_DESC outputDesc;

  this->pointerShape = pointerShape;
  this->pointerSize  = pointerSize;
  this->pointerUsed  = 0;

  this->stop       = false;
  this->texRIndex  = 0;
  this->texWIndex  = 0;
  this->texReady   = 0;

  lgResetEvent(this->frameEvent  );
  lgResetEvent(this->pointerEvent);

  status = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&this->factory);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create DXGIFactory1", status);
    goto fail;
  }

  const char * optAdapter = option_get_string("dxgi", "adapter");
  const char * optOutput  = option_get_string("dxgi", "output" );

  for(int i = 0; IDXGIFactory1_EnumAdapters1(this->factory, i, &this->adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    if (optAdapter)
    {
      DXGI_ADAPTER_DESC1 adapterDesc;
      IDXGIAdapter1_GetDesc1(this->adapter, &adapterDesc);

      const size_t s = (wcslen(adapterDesc.Description)+1) * 2;
      char * desc = malloc(s);
      wcstombs(desc, adapterDesc.Description, s);

      if (strstr(desc, optAdapter) == NULL)
      {
        DEBUG_INFO("Not using adapter: %ls", adapterDesc.Description);
        free(desc);
        IDXGIAdapter1_Release(this->adapter);
        this->adapter = NULL;
        continue;
      }
      free(desc);

      DEBUG_INFO("Adapter matched, trying: %ls", adapterDesc.Description);
    }

    for(int n = 0; IDXGIAdapter1_EnumOutputs(this->adapter, n, &this->output) != DXGI_ERROR_NOT_FOUND; ++n)
    {
      IDXGIOutput_GetDesc(this->output, &outputDesc);
      if (optOutput)
      {
        const size_t s = (wcslen(outputDesc.DeviceName)+1) * 2;
        char * desc = malloc(s);
        wcstombs(desc, outputDesc.DeviceName, s);

        if (strstr(desc, optOutput) == NULL)
        {
          DEBUG_INFO("Not using adapter output: %ls", outputDesc.DeviceName);
          free(desc);
          IDXGIOutput_Release(this->output);
          this->output = NULL;
          continue;
        }

        free(desc);

        DEBUG_INFO("Adapter output matched, trying: %ls", outputDesc.DeviceName);
      }

      if (outputDesc.AttachedToDesktop)
        break;

      IDXGIOutput_Release(this->output);
      this->output = NULL;
    }

    if (this->output)
      break;

    IDXGIAdapter1_Release(this->adapter);
    this->adapter = NULL;
  }

  if (!this->output)
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
  status = IDXGIAdapter1_QueryInterface(this->adapter, &IID_IDXGIAdapter, (void **)&tmp);
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to query IDXGIAdapter interface");
    goto fail;
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
  this->deviceContextLock = 0;

  IDXGIAdapter_Release(tmp);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    goto fail;
  }

  DXGI_ADAPTER_DESC1 adapterDesc;
  IDXGIAdapter1_GetDesc1(this->adapter, &adapterDesc);
  this->width  = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
  this->height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

  DEBUG_INFO("Device Descripion: %ls"    , adapterDesc.Description);
  DEBUG_INFO("Device Vendor ID : 0x%x"   , adapterDesc.VendorId);
  DEBUG_INFO("Device Device ID : 0x%x"   , adapterDesc.DeviceId);
  DEBUG_INFO("Device Video Mem : %u MiB" , (unsigned)(adapterDesc.DedicatedVideoMemory  / 1048576));
  DEBUG_INFO("Device Sys Mem   : %u MiB" , (unsigned)(adapterDesc.DedicatedSystemMemory / 1048576));
  DEBUG_INFO("Shared Sys Mem   : %u MiB" , (unsigned)(adapterDesc.SharedSystemMemory    / 1048576));
  DEBUG_INFO("Feature Level    : 0x%x"   , this->featureLevel);
  DEBUG_INFO("Capture Size     : %u x %u", this->width, this->height);
  DEBUG_INFO("AcquireLock      : %s"     , this->useAcquireLock ? "enabled" : "disabled");

  // bump up our priority
  {
    IDXGIDevice * dxgi;
    status = ID3D11Device_QueryInterface(this->device, &IID_IDXGIDevice, (void **)&dxgi);
    if (FAILED(status))
    {
      DEBUG_WINERROR("failed to query DXGI interface from device", status);
      goto fail;
    }

    IDXGIDevice_SetGPUThreadPriority(dxgi, 7);
    IDXGIDevice_Release(dxgi);
  }

  // try to reduce the latency
  {
    IDXGIDevice1 * dxgi;
    status = ID3D11Device_QueryInterface(this->device, &IID_IDXGIDevice1, (void **)&dxgi);
    if (FAILED(status))
    {
      DEBUG_WINERROR("failed to query DXGI interface from device", status);
      goto fail;
    }

    IDXGIDevice1_SetMaximumFrameLatency(dxgi, 1);
    IDXGIDevice1_Release(dxgi);
  }

  IDXGIOutput5 * output5 = NULL;
  status = IDXGIOutput_QueryInterface(this->output, &IID_IDXGIOutput5, (void **)&output5);
  if (FAILED(status))
  {
    DEBUG_WARN("IDXGIOutput5 is not available, please update windows for improved performance!");
    DEBUG_WARN("Falling back to IDXIGOutput1");

    IDXGIOutput1 * output1 = NULL;
    status = IDXGIOutput_QueryInterface(this->output, &IID_IDXGIOutput1, (void **)&output1);
    if (FAILED(status))
    {
      DEBUG_ERROR("Failed to query IDXGIOutput1 from the output");
      goto fail;
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
      goto fail;
    }
    IDXGIOutput1_Release(output1);
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
      goto fail;
    }
    IDXGIOutput5_Release(output5);
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
      goto fail;
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

  for(int i = 0; i < this->maxTextures; ++i)
  {
    status = ID3D11Device_CreateTexture2D(this->device, &texDesc, NULL, &this->texture[i].tex);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to create texture", status);
      goto fail;
    }
  }

  // map the texture simply to get the pitch and stride
  D3D11_MAPPED_SUBRESOURCE mapping;
  status = ID3D11DeviceContext_Map(this->deviceContext, (ID3D11Resource *)this->texture[0].tex, 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    goto fail;
  }
  this->pitch  = mapping.RowPitch;
  this->stride = mapping.RowPitch / 4;
  ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource *)this->texture[0].tex, 0);

  QueryPerformanceFrequency(&this->perfFreq) ;
  QueryPerformanceCounter  (&this->frameTime);
  this->initialized = true;
  return true;

fail:
  dxgi_deinit();
  return false;
}

static void dxgi_stop()
{
  this->stop = true;
  lgSignalEvent(this->pointerEvent);
}

static bool dxgi_deinit()
{
  assert(this);

  for(int i = 0; i < this->maxTextures; ++i)
  {
    this->texture[i].state = TEXTURE_STATE_UNUSED;

    if (this->texture[i].map.pData)
    {
      ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource*)this->texture[i].tex, 0);
      this->texture[i].map.pData = NULL;
    }

    if (this->texture[i].tex)
    {
      ID3D11Texture2D_Release(this->texture[i].tex);
      this->texture[i].tex = NULL;
    }
  }

  if (this->dup)
  {
    dxgi_releaseFrame();
    IDXGIOutputDuplication_Release(this->dup);
    this->dup = NULL;
  }

  if (this->deviceContext)
  {
    ID3D11DeviceContext_Release(this->deviceContext);
    this->deviceContext = NULL;
  }

  if (this->output)
  {
    IDXGIOutput_Release(this->output);
    this->output = NULL;
  }

  if (this->device)
  {
    ID3D11Device_Release(this->device);
    this->device = NULL;
  }

  if (this->adapter)
  {
    IDXGIAdapter1_Release(this->adapter);
    this->adapter = NULL;
  }

  if (this->factory)
  {
    // if this doesn't free we have a memory leak
    DWORD count = IDXGIFactory1_Release(this->factory);
    this->factory = NULL;
    if (count != 0)
    {
      DEBUG_ERROR("Factory release is %lu, there is a memory leak!", count);
      return false;
    }
  }

  this->initialized = false;
  return true;
}

static void dxgi_free()
{
  assert(this);

  if (this->initialized)
    dxgi_deinit();

  lgFreeEvent(this->pointerEvent);
  free(this->texture);

  free(this);
  this = NULL;
}

static unsigned int dxgi_getMaxFrameSize()
{
  assert(this);
  assert(this->initialized);

  return this->height * this->pitch;
}

static CaptureResult dxgi_capture()
{
  assert(this);
  assert(this->initialized);

  Texture                 * tex;
  CaptureResult             result;
  HRESULT                   status;
  DXGI_OUTDUPL_FRAME_INFO   frameInfo;
  IDXGIResource           * res;

  // release the prior frame
  result = dxgi_releaseFrame();
  if (result != CAPTURE_RESULT_OK)
    return result;

  if (this->useAcquireLock)
  {
    LOCKED({
        status = IDXGIOutputDuplication_AcquireNextFrame(this->dup, 1, &frameInfo, &res);
    });
  }
  else
    status = IDXGIOutputDuplication_AcquireNextFrame(this->dup, 1000, &frameInfo, &res);

  switch(status)
  {
    case S_OK:
      this->needsRelease = true;
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

  if (frameInfo.LastPresentTime.QuadPart != 0)
  {
    tex = &this->texture[this->texWIndex];

    // check if the texture is free, if not skip the frame to keep up
    if (tex->state == TEXTURE_STATE_UNUSED)
    {
      ID3D11Texture2D * src;
      status = IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&src);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to get the texture from the dxgi resource", status);
        IDXGIResource_Release(res);
        return CAPTURE_RESULT_ERROR;
      }

      LOCKED({
        // issue the copy from GPU to CPU RAM and release the src
        ID3D11DeviceContext_CopyResource(this->deviceContext,
          (ID3D11Resource *)tex->tex, (ID3D11Resource *)src);
      });

      ID3D11Texture2D_Release(src);

      // set the state, and signal
      tex->state = TEXTURE_STATE_PENDING_MAP;
      INTERLOCKED_INC(&this->texReady);
      lgSignalEvent(this->frameEvent);

      // advance the write index
      if (++this->texWIndex == this->maxTextures)
        this->texWIndex = 0;

      // update the last frame time
      this->frameTime.QuadPart = frameInfo.LastPresentTime.QuadPart;
    }
  }

  IDXGIResource_Release(res);

  // if the pointer has moved or changed state
  bool signalPointer = false;
  if (frameInfo.LastMouseUpdateTime.QuadPart)
  {
    if (
      frameInfo.PointerPosition.Position.x != this->lastPointer.x ||
      frameInfo.PointerPosition.Position.y != this->lastPointer.y ||
      frameInfo.PointerPosition.Visible    != this->lastPointer.visible
      )
    {
      this->pointer.x       = frameInfo.PointerPosition.Position.x;
      this->pointer.y       = frameInfo.PointerPosition.Position.y;
      this->pointer.visible = frameInfo.PointerPosition.Visible;
      signalPointer = true;
    }
  }

  // if the pointer shape has changed
  if (frameInfo.PointerShapeBufferSize > 0)
  {
    // update the buffer
    if (frameInfo.PointerShapeBufferSize > this->pointerSize)
      DEBUG_WARN("The pointer shape is too large to fit in the buffer, ignoring the shape");
    else
    {
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
      LOCKED({status = IDXGIOutputDuplication_GetFramePointerShape(this->dup, this->pointerSize, this->pointerShape, &this->pointerUsed, &shapeInfo);});
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to get the new pointer shape", status);
        return CAPTURE_RESULT_ERROR;
      }

      switch(shapeInfo.Type)
      {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR       : this->pointer.format = CAPTURE_FMT_COLOR ; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: this->pointer.format = CAPTURE_FMT_MASKED; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME  : this->pointer.format = CAPTURE_FMT_MONO  ; break;
        default:
          DEBUG_ERROR("Unsupported cursor format");
          return CAPTURE_RESULT_ERROR;
      }

      this->pointer.w     = shapeInfo.Width;
      this->pointer.h     = shapeInfo.Height;
      this->pointer.pitch = shapeInfo.Pitch;
      ++this->pointer.version;
      signalPointer = true;
    }
  }

  // signal about the pointer update
  if (signalPointer)
    lgSignalEvent(this->pointerEvent);

  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_waitFrame(CaptureFrame * frame)
{
  assert(this);
  assert(this->initialized);

  // NOTE: the event may be signaled when there are no frames available
  if(this->texReady == 0)
  {
    if (!lgWaitEvent(this->frameEvent, 1000))
      return CAPTURE_RESULT_TIMEOUT;

    if (this->texReady == 0)
      return CAPTURE_RESULT_TIMEOUT;
  }

  Texture * tex = &this->texture[this->texRIndex];

  // try to map the resource, but don't wait for it
  HRESULT status;
  LOCKED({status = ID3D11DeviceContext_Map(this->deviceContext, (ID3D11Resource*)tex->tex, 0, D3D11_MAP_READ, 0x100000L, &tex->map);});
  if (status == DXGI_ERROR_WAS_STILL_DRAWING)
    return CAPTURE_RESULT_TIMEOUT;

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    return CAPTURE_RESULT_ERROR;
  }

  tex->state = TEXTURE_STATE_MAPPED;

  frame->width  = this->width;
  frame->height = this->height;
  frame->pitch  = this->pitch;
  frame->stride = this->stride;
  frame->format = this->format;

  INTERLOCKED_DEC(&this->texReady);
  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_getFrame(FrameBuffer frame)
{
  assert(this);
  assert(this->initialized);

  Texture * tex = &this->texture[this->texRIndex];

  framebuffer_write(frame, tex->map.pData, this->pitch * this->height);
  LOCKED({ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource*)tex->tex, 0);});
  tex->state = TEXTURE_STATE_UNUSED;

  if (++this->texRIndex == this->maxTextures)
    this->texRIndex = 0;

  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_getPointer(CapturePointer * pointer)
{
  assert(this);
  assert(this->initialized);

  if (!lgWaitEvent(this->pointerEvent, 1000))
    return CAPTURE_RESULT_TIMEOUT;

  if (this->stop)
    return CAPTURE_RESULT_REINIT;

  Pointer p;
  memcpy(&p, &this->pointer, sizeof(Pointer));

  pointer->x           = p.x;
  pointer->y           = p.y;
  pointer->width       = p.w;
  pointer->height      = p.h;
  pointer->pitch       = p.pitch;
  pointer->visible     = p.visible;
  pointer->format      = p.format;
  pointer->shapeUpdate = p.version > this->lastPointer.version;

  memcpy(&this->lastPointer, &p, sizeof(Pointer));

  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_releaseFrame()
{
  assert(this);
  if (!this->needsRelease)
    return CAPTURE_RESULT_OK;

  HRESULT status;
  LOCKED({status = IDXGIOutputDuplication_ReleaseFrame(this->dup);});
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
      this->needsRelease = false;
      return CAPTURE_RESULT_REINIT;
    }

    default:
      DEBUG_WINERROR("ReleaseFrame failed", status);
      return CAPTURE_RESULT_ERROR;
  }

  this->needsRelease = false;
  return CAPTURE_RESULT_OK;
}

struct CaptureInterface Capture_DXGI =
{
  .getName         = dxgi_getName,
  .initOptions     = dxgi_initOptions,
  .create          = dxgi_create,
  .init            = dxgi_init,
  .stop            = dxgi_stop,
  .deinit          = dxgi_deinit,
  .free            = dxgi_free,
  .getMaxFrameSize = dxgi_getMaxFrameSize,
  .capture         = dxgi_capture,
  .waitFrame       = dxgi_waitFrame,
  .getFrame        = dxgi_getFrame,
  .getPointer      = dxgi_getPointer
};