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

#include "interface/capture.h"
#include "interface/platform.h"
#include "common/array.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/option.h"
#include "common/locking.h"
#include "common/event.h"
#include "common/rects.h"
#include "common/runningavg.h"
#include "common/KVMFR.h"

#include <stdatomic.h>
#include <unistd.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <versionhelpers.h>

#include "dxgi_extra.h"

#define LOCKED(...) INTERLOCKED_SECTION(this->deviceContextLock, __VA_ARGS__)

enum TextureState
{
  TEXTURE_STATE_UNUSED,
  TEXTURE_STATE_PENDING_MAP,
  TEXTURE_STATE_MAPPED
};

typedef struct Texture
{
  unsigned int               formatVer;
  volatile enum TextureState state;
  ID3D11Texture2D          * tex;
  D3D11_MAPPED_SUBRESOURCE   map;
  uint64_t                   copyTime;
  uint32_t                   damageRectsCount;
  FrameDamageRect            damageRects[KVMFR_MAX_DAMAGE_RECTS];
  int32_t                    texDamageCount;
  FrameDamageRect            texDamageRects[KVMFR_MAX_DAMAGE_RECTS];
}
Texture;

struct FrameDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
};

// locals
struct iface
{
  bool                       initialized;
  LARGE_INTEGER              perfFreq;
  LARGE_INTEGER              frameTime;
  bool                       stop;
  HDESK                      desktop;
  IDXGIFactory1            * factory;
  IDXGIAdapter1            * adapter;
  IDXGIOutput              * output;
  ID3D11Device             * device;
  ID3D11DeviceContext      * deviceContext;
  LG_Lock                    deviceContextLock;
  bool                       useAcquireLock;
  D3D_FEATURE_LEVEL          featureLevel;
  IDXGIOutputDuplication   * dup;
  int                        maxTextures;
  Texture                  * texture;
  int                        texRIndex;
  int                        texWIndex;
  atomic_int                 texReady;
  bool                       needsRelease;

  RunningAvg                 avgMapTime;
  uint64_t                   usleepMapTime;

  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  LGEvent                  * frameEvent;

  unsigned int    formatVer;
  unsigned int    width;
  unsigned int    height;
  unsigned int    pitch;
  unsigned int    stride;
  CaptureFormat   format;
  CaptureRotation rotation;

  int  lastPointerX, lastPointerY;
  bool lastPointerVisible;

  struct FrameDamage frameDamage[LGMP_Q_FRAME_LEN];
};

static struct iface * this    = NULL;

// forwards

static bool          dxgi_deinit();
static CaptureResult dxgi_releaseFrame();

// implementation

static const char * dxgi_getName(void)
{
  return "DXGI";
}

static void dxgi_initOptions(void)
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
      .value.x_int    = 4
    },
    {
      .module         = "dxgi",
      .name           = "useAcquireLock",
      .description    = "Enable locking around `AcquireNextFrame` (EXPERIMENTAL, leave enabled if you're not sure!)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {0}
  };

  option_register(options);
}

static bool dxgi_create(CaptureGetPointerBuffer getPointerBufferFn, CapturePostPointerBuffer postPointerBufferFn)
{
  DEBUG_ASSERT(!this);
  this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("failed to allocate iface struct");
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

  this->useAcquireLock      = option_get_bool("dxgi", "useAcquireLock");
  this->texture             = calloc(this->maxTextures, sizeof(*this->texture));
  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;
  this->avgMapTime          = runningavg_new(10);
  return true;
}

static bool dxgi_init(void)
{
  DEBUG_ASSERT(this);

  this->desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
  if (!this->desktop)
    DEBUG_WINERROR("Failed to open the desktop", GetLastError());
  else
  {
    if (!SetThreadDesktop(this->desktop))
    {
      DEBUG_WINERROR("Failed to set thread desktop", GetLastError());
      CloseDesktop(this->desktop);
      this->desktop = NULL;
    }
  }

  if (!this->desktop)
  {
    DEBUG_INFO("The above error(s) will prevent LG from being able to capture the secure desktop (UAC dialogs)");
    DEBUG_INFO("This is not a failure, please do not report this as an issue.");
    DEBUG_INFO("To fix this, install and run the Looking Glass host as a service.");
    DEBUG_INFO("looking-glass-host.exe InstallService");
  }

  HRESULT          status;
  DXGI_OUTPUT_DESC outputDesc;

  this->stop      = false;
  this->texRIndex = 0;
  this->texWIndex = 0;
  atomic_store(&this->texReady, 0);

  runningavg_reset(this->avgMapTime);
  this->usleepMapTime = 0;

  lgResetEvent(this->frameEvent);

  status = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&this->factory);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create DXGIFactory1", status);
    goto fail;
  }

  const char * optAdapter = option_get_string("dxgi", "adapter");
  const char * optOutput  = option_get_string("dxgi", "output" );

  for (int i = 0; IDXGIFactory1_EnumAdapters1(this->factory, i, &this->adapter) != DXGI_ERROR_NOT_FOUND; ++i)
  {
    if (optAdapter)
    {
      DXGI_ADAPTER_DESC1 adapterDesc;
      status = IDXGIAdapter1_GetDesc1(this->adapter, &adapterDesc);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to get the device description", status);
        goto fail;
      }

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

    for (int n = 0; IDXGIAdapter1_EnumOutputs(this->adapter, n, &this->output) != DXGI_ERROR_NOT_FOUND; ++n)
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

  DXGI_ADAPTER_DESC1 adapterDesc;
  IDXGIAdapter1_GetDesc1(this->adapter, &adapterDesc);
  DEBUG_INFO("Device Name       : %ls"    , outputDesc.DeviceName);
  DEBUG_INFO("Device Description: %ls"    , adapterDesc.Description);
  DEBUG_INFO("Device Vendor ID  : 0x%x"   , adapterDesc.VendorId);
  DEBUG_INFO("Device Device ID  : 0x%x"   , adapterDesc.DeviceId);
  DEBUG_INFO("Device Video Mem  : %u MiB" , (unsigned)(adapterDesc.DedicatedVideoMemory  / 1048576));
  DEBUG_INFO("Device Sys Mem    : %u MiB" , (unsigned)(adapterDesc.DedicatedSystemMemory / 1048576));
  DEBUG_INFO("Shared Sys Mem    : %u MiB" , (unsigned)(adapterDesc.SharedSystemMemory    / 1048576));

  static const D3D_FEATURE_LEVEL win8[] =
  {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1
  };

  static const D3D_FEATURE_LEVEL win10[] =
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

  const D3D_FEATURE_LEVEL * featureLevels;
  unsigned int featureLevelCount;
  if (IsWindows10OrGreater())
  {
    featureLevels     = win10;
    featureLevelCount = ARRAY_LENGTH(win10);
  }
  else
  {
    featureLevels     = win8;
    featureLevelCount = ARRAY_LENGTH(win8);
  }

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
    featureLevels, featureLevelCount,
    D3D11_SDK_VERSION,
    &this->device,
    &this->featureLevel,
    &this->deviceContext);

  LG_LOCK_INIT(this->deviceContextLock);

  IDXGIAdapter_Release(tmp);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    goto fail;
  }

  switch(outputDesc.Rotation)
  {
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE270:
      this->width    = outputDesc.DesktopCoordinates.bottom -
                       outputDesc.DesktopCoordinates.top;
      this->height   = outputDesc.DesktopCoordinates.right -
                       outputDesc.DesktopCoordinates.left;
      break;

    default:
      this->width    = outputDesc.DesktopCoordinates.right  -
                       outputDesc.DesktopCoordinates.left;
      this->height   = outputDesc.DesktopCoordinates.bottom -
                       outputDesc.DesktopCoordinates.top;
      break;
  }

  switch(outputDesc.Rotation)
  {
    case DXGI_MODE_ROTATION_ROTATE90:
      this->rotation = CAPTURE_ROT_270;
      break;

    case DXGI_MODE_ROTATION_ROTATE180:
      this->rotation = CAPTURE_ROT_180;
      break;

    case DXGI_MODE_ROTATION_ROTATE270:
      this->rotation = CAPTURE_ROT_90;
      break;

    default:
      this->rotation = CAPTURE_ROT_0;
      break;
  }

  ++this->formatVer;

  DEBUG_INFO("Feature Level     : 0x%x"   , this->featureLevel);
  DEBUG_INFO("Capture Size      : %u x %u", this->width, this->height);
  DEBUG_INFO("AcquireLock       : %s"     , this->useAcquireLock ? "enabled" : "disabled");

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
      DXGI_FORMAT_R10G10B10A2_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      status = IDXGIOutput5_DuplicateOutput1(
        output5,
        (IUnknown *)this->device,
        0,
        ARRAY_LENGTH(supportedFormats),
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
  DEBUG_INFO("Source Format     : %s", GetDXGIFormatStr(dupDesc.ModeDesc.Format));

  uint8_t bpp = 4;
  switch(dupDesc.ModeDesc.Format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM    : this->format = CAPTURE_FMT_BGRA   ; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM    : this->format = CAPTURE_FMT_RGBA   ; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM : this->format = CAPTURE_FMT_RGBA10 ; break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      this->format = CAPTURE_FMT_RGBA16F;
      bpp = 8;
      break;

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

  for (int i = 0; i < this->maxTextures; ++i)
  {
    this->texture[i].texDamageCount = -1;
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
  this->stride = mapping.RowPitch / bpp;
  ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource *)this->texture[0].tex, 0);

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->frameDamage[i].count = -1;

  QueryPerformanceFrequency(&this->perfFreq) ;
  QueryPerformanceCounter  (&this->frameTime);
  this->initialized = true;
  return true;

fail:
  dxgi_deinit();
  return false;
}

static void dxgi_stop(void)
{
  this->stop = true;
}

static bool dxgi_deinit(void)
{
  DEBUG_ASSERT(this);

  for (int i = 0; i < this->maxTextures; ++i)
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

  LG_LOCK_FREE(this->deviceContextLock);

  if (this->desktop)
  {
    CloseDesktop(this->desktop);
    this->desktop = NULL;
  }

  this->initialized = false;
  return true;
}

static void dxgi_free(void)
{
  DEBUG_ASSERT(this);

  if (this->initialized)
    dxgi_deinit();

  free(this->texture);

  runningavg_free(&this->avgMapTime);
  free(this);
  this = NULL;
}

static CaptureResult dxgi_hResultToCaptureResult(const HRESULT status)
{
  switch(status)
  {
    case S_OK:
      return CAPTURE_RESULT_OK;

    case DXGI_ERROR_WAIT_TIMEOUT:
      return CAPTURE_RESULT_TIMEOUT;

    case WAIT_ABANDONED:
    case DXGI_ERROR_ACCESS_LOST:
      return CAPTURE_RESULT_REINIT;

    default:
      return CAPTURE_RESULT_ERROR;
  }
}

static void rectToFrameDamageRect(RECT * src, FrameDamageRect * dst)
{
  *dst = (FrameDamageRect)
  {
    .x = src->left,
    .y = src->top,
    .width = src->right - src->left,
    .height = src->bottom - src->top
  };
}

static void computeFrameDamage(Texture * tex)
{
  // By default, damage the full frame.
  tex->damageRectsCount = 0;

  const int maxDamageRectsCount = ARRAY_LENGTH(tex->damageRects);

  // Compute dirty rectangles.
  RECT dirtyRects[maxDamageRectsCount];
  UINT dirtyRectsBufferSizeRequired;
  if (FAILED(IDXGIOutputDuplication_GetFrameDirtyRects(this->dup,
        ARRAY_LENGTH(dirtyRects), dirtyRects,
        &dirtyRectsBufferSizeRequired)))
    return;

  const int dirtyRectsCount = dirtyRectsBufferSizeRequired / sizeof(*dirtyRects);

  // Compute moved rectangles.
  //
  // Move rects are seemingly not generated on Windows 10, but may be present
  // on Windows 8 and earlier.
  //
  // Divide by two here since each move generates two dirty regions.
  DXGI_OUTDUPL_MOVE_RECT moveRects[(maxDamageRectsCount - dirtyRectsCount) / 2];
  UINT moveRectsBufferSizeRequired;
  if (FAILED(IDXGIOutputDuplication_GetFrameMoveRects(this->dup,
        ARRAY_LENGTH(moveRects), moveRects,
        &moveRectsBufferSizeRequired)))
    return;

  const int moveRectsCount = moveRectsBufferSizeRequired / sizeof(*moveRects);

  FrameDamageRect * texDamageRect = tex->damageRects;
  for (RECT *dirtyRect = dirtyRects;
       dirtyRect < dirtyRects + dirtyRectsCount;
       dirtyRect++)
    rectToFrameDamageRect(dirtyRect, texDamageRect++);

  int actuallyMovedRectsCount = 0;
  for (DXGI_OUTDUPL_MOVE_RECT *moveRect = moveRects;
       moveRect < moveRects + moveRectsCount;
       moveRect++)
  {
    // According to WebRTC source comments, the DirectX capture API may randomly
    // return unmoved rects, which should be skipped to avoid unnecessary work.
    if (moveRect->SourcePoint.x == moveRect->DestinationRect.left &&
        moveRect->SourcePoint.y == moveRect->DestinationRect.top)
      continue;

    *texDamageRect++ = (FrameDamageRect)
    {
      .x = moveRect->SourcePoint.x,
      .y = moveRect->SourcePoint.y,
      .width = moveRect->DestinationRect.right - moveRect->DestinationRect.left,
      .height = moveRect->DestinationRect.bottom - moveRect->DestinationRect.top
    };

    rectToFrameDamageRect(&moveRect->DestinationRect, texDamageRect++);
    actuallyMovedRectsCount += 2;
  }

  tex->damageRectsCount = dirtyRectsCount + actuallyMovedRectsCount;
}

static CaptureResult dxgi_capture(void)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  Texture                 * tex = NULL;
  CaptureResult             result;
  HRESULT                   status;
  DXGI_OUTDUPL_FRAME_INFO   frameInfo;
  IDXGIResource           * res;

  bool copyFrame   = false;
  bool copyPointer = false;
  ID3D11Texture2D * src;

  bool           postPointer      = false;
  CapturePointer pointer          = { 0 };
  void *         pointerShape     = NULL;
  UINT           pointerShapeSize = 0;

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

  result = dxgi_hResultToCaptureResult(status);
  if (result != CAPTURE_RESULT_OK)
  {
    if (result == CAPTURE_RESULT_ERROR)
      DEBUG_WINERROR("AcquireNextFrame failed", status);
    return result;
  }

  this->needsRelease = true;

  if (frameInfo.LastPresentTime.QuadPart != 0)
  {
    tex = &this->texture[this->texWIndex];

    // check if the texture is free, if not skip the frame to keep up
    if (tex->state == TEXTURE_STATE_UNUSED)
    {
      copyFrame = true;
      status = IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&src);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to get the texture from the dxgi resource", status);
        IDXGIResource_Release(res);
        return CAPTURE_RESULT_ERROR;
      }
    }
  }

  IDXGIResource_Release(res);

  // if the pointer shape has changed
  uint32_t bufferSize;
  if (frameInfo.PointerShapeBufferSize > 0)
  {
    if (!this->getPointerBufferFn(&pointerShape, &bufferSize))
      DEBUG_WARN("Failed to obtain a buffer for the pointer shape");
    else
      copyPointer = true;
  }

  if (copyFrame || copyPointer)
  {
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
    LOCKED(
    {
      if (copyFrame)
      {
        computeFrameDamage(tex);

        if (tex->texDamageCount < 0 || tex->damageRectsCount == 0 ||
            tex->texDamageCount + tex->damageRectsCount > KVMFR_MAX_DAMAGE_RECTS)
          tex->texDamageCount = -1;
        else
        {
          memcpy(tex->texDamageRects + tex->texDamageCount, tex->damageRects,
            tex->damageRectsCount * sizeof(*tex->damageRects));
          tex->texDamageCount += tex->damageRectsCount;
          tex->texDamageCount = rectsMergeOverlapping(tex->texDamageRects, tex->texDamageCount);
        }

        // issue the copy from GPU to CPU RAM
        tex->copyTime = microtime();
        if (tex->texDamageCount < 0)
          ID3D11DeviceContext_CopyResource(this->deviceContext,
            (ID3D11Resource *)tex->tex, (ID3D11Resource *)src);
        else
        {
          for (int i = 0; i < tex->texDamageCount; ++i)
          {
            FrameDamageRect * rect = tex->texDamageRects + i;
            D3D11_BOX box = {
              .left = rect->x, .top = rect->y, .front = 0, .back = 1,
              .right = rect->x + rect->width, .bottom = rect->y + rect->height,
            };
            ID3D11DeviceContext_CopySubresourceRegion(this->deviceContext,
              (ID3D11Resource *)tex->tex, 0, rect->x, rect->y, 0,
              (ID3D11Resource *)src, 0, &box);
          }
        }
      }

      if (copyPointer)
      {
        // grab the pointer shape
        status = IDXGIOutputDuplication_GetFramePointerShape(
            this->dup, bufferSize, pointerShape, &pointerShapeSize, &shapeInfo);
      }

      ID3D11DeviceContext_Flush(this->deviceContext);
    });

    if (copyFrame)
    {
      ID3D11Texture2D_Release(src);

      for (int i = 0; i < this->maxTextures; ++i)
      {
        Texture * t = this->texture + i;
        if (i == this->texWIndex)
          t->texDamageCount = 0;
        else if (tex->damageRectsCount > 0 && t->texDamageCount >= 0 &&
                 t->texDamageCount + tex->damageRectsCount <= KVMFR_MAX_DAMAGE_RECTS)
        {
          memcpy(t->texDamageRects + t->texDamageCount, tex->damageRects,
            tex->damageRectsCount * sizeof(*tex->damageRects));
          t->texDamageCount += tex->damageRectsCount;
        }
        else
          t->texDamageCount = -1;
      }

      // set the state, and signal
      tex->state     = TEXTURE_STATE_PENDING_MAP;
      tex->formatVer = this->formatVer;
      if (atomic_fetch_add_explicit(&this->texReady, 1, memory_order_relaxed) == 0)
        lgSignalEvent(this->frameEvent);

      // advance the write index
      if (++this->texWIndex == this->maxTextures)
        this->texWIndex = 0;

      // update the last frame time
      this->frameTime.QuadPart = frameInfo.LastPresentTime.QuadPart;
    }

    if (copyPointer)
    {
      result = dxgi_hResultToCaptureResult(status);
      if (result != CAPTURE_RESULT_OK)
      {
        if (result == CAPTURE_RESULT_ERROR)
          DEBUG_WINERROR("Failed to get the new pointer shape", status);
        return result;
      }

      switch(shapeInfo.Type)
      {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR       : pointer.format = CAPTURE_FMT_COLOR ; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: pointer.format = CAPTURE_FMT_MASKED; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME  : pointer.format = CAPTURE_FMT_MONO  ; break;
        default:
          DEBUG_ERROR("Unsupported cursor format");
          return CAPTURE_RESULT_ERROR;
      }

      pointer.shapeUpdate = true;
      pointer.width       = shapeInfo.Width;
      pointer.height      = shapeInfo.Height;
      pointer.pitch       = shapeInfo.Pitch;
      pointer.hx          = shapeInfo.HotSpot.x;
      pointer.hy          = shapeInfo.HotSpot.y;
      postPointer         = true;
    }
  }

  if (frameInfo.LastMouseUpdateTime.QuadPart)
  {
    /* the pointer position is only valid if the pointer is visible */
    if (frameInfo.PointerPosition.Visible &&
      (frameInfo.PointerPosition.Position.x != this->lastPointerX ||
       frameInfo.PointerPosition.Position.y != this->lastPointerY))
    {
      pointer.positionUpdate = true;
      pointer.x =
        this->lastPointerX =
        frameInfo.PointerPosition.Position.x;
      pointer.y =
        this->lastPointerY =
        frameInfo.PointerPosition.Position.y;
      postPointer = true;
    }

    if (this->lastPointerVisible != frameInfo.PointerPosition.Visible)
    {
      this->lastPointerVisible = frameInfo.PointerPosition.Visible;
      postPointer = true;
    }
  }

  // post back the pointer information
  if (postPointer)
  {
    pointer.visible = this->lastPointerVisible;
    this->postPointerBufferFn(pointer);
  }

  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_waitFrame(CaptureFrame * frame, const size_t maxFrameSize)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  // NOTE: the event may be signaled when there are no frames available
  if (atomic_load_explicit(&this->texReady, memory_order_acquire) == 0)
  {
    if (!lgWaitEvent(this->frameEvent, 1000))
      return CAPTURE_RESULT_TIMEOUT;

    // the count will still be zero if we are stopping
    if (atomic_load_explicit(&this->texReady, memory_order_acquire) == 0)
      return CAPTURE_RESULT_TIMEOUT;
  }

  Texture * tex = &this->texture[this->texRIndex];

  // sleep until it's close to time to map
  const uint64_t delta = microtime() - tex->copyTime;
  if (delta < this->usleepMapTime)
    usleep(this->usleepMapTime - delta);

  // try to map the resource, but don't wait for it
  for (int i = 0; ; ++i)
  {
    HRESULT status;
    LOCKED({status = ID3D11DeviceContext_Map(this->deviceContext, (ID3D11Resource*)tex->tex, 0, D3D11_MAP_READ, 0x100000L, &tex->map);});
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

  // update the sleep average and sleep for 80% of the average on the next call
  runningavg_push(this->avgMapTime, microtime() - tex->copyTime);
  this->usleepMapTime = (uint64_t)(runningavg_calc(this->avgMapTime) * 0.8);

  tex->state = TEXTURE_STATE_MAPPED;

  const unsigned int maxHeight = maxFrameSize / this->pitch;

  frame->formatVer        = tex->formatVer;
  frame->width            = this->width;
  frame->height           = maxHeight > this->height ? this->height : maxHeight;
  frame->realHeight       = this->height;
  frame->pitch            = this->pitch;
  frame->stride           = this->stride;
  frame->format           = this->format;
  frame->rotation         = this->rotation;

  frame->damageRectsCount = tex->damageRectsCount;
  memcpy(frame->damageRects, tex->damageRects,
      tex->damageRectsCount * sizeof(*tex->damageRects));

  atomic_fetch_sub_explicit(&this->texReady, 1, memory_order_release);
  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_getFrame(FrameBuffer * frame,
    const unsigned int height, int frameIndex)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  Texture * tex = &this->texture[this->texRIndex];

  struct FrameDamage * damage = this->frameDamage + frameIndex;
  bool damageAll = tex->damageRectsCount == 0 || damage->count < 0 ||
      damage->count + tex->damageRectsCount > KVMFR_MAX_DAMAGE_RECTS;

  if (damageAll)
    framebuffer_write(frame, tex->map.pData, this->pitch * height);
  else
  {
    memcpy(damage->rects + damage->count, tex->damageRects,
      tex->damageRectsCount * sizeof(*tex->damageRects));
    damage->count += tex->damageRectsCount;
    rectsBufferToFramebuffer(damage->rects, damage->count, frame, this->pitch,
      height, tex->map.pData, this->pitch);
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    struct FrameDamage * damage = this->frameDamage + i;
    if (i == frameIndex)
      damage->count = 0;
    else if (tex->damageRectsCount > 0 && damage->count >= 0 &&
             damage->count + tex->damageRectsCount <= KVMFR_MAX_DAMAGE_RECTS)
    {
      memcpy(damage->rects + damage->count, tex->damageRects,
        tex->damageRectsCount * sizeof(*tex->damageRects));
      damage->count += tex->damageRectsCount;
    }
    else
      damage->count = -1;
  }

  LOCKED({ID3D11DeviceContext_Unmap(this->deviceContext, (ID3D11Resource*)tex->tex, 0);});
  tex->state = TEXTURE_STATE_UNUSED;

  if (++this->texRIndex == this->maxTextures)
    this->texRIndex = 0;

  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_releaseFrame(void)
{
  DEBUG_ASSERT(this);
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
  .shortName       = "DXGI",
  .asyncCapture    = true,
  .getName         = dxgi_getName,
  .initOptions     = dxgi_initOptions,
  .create          = dxgi_create,
  .init            = dxgi_init,
  .stop            = dxgi_stop,
  .deinit          = dxgi_deinit,
  .free            = dxgi_free,
  .capture         = dxgi_capture,
  .waitFrame       = dxgi_waitFrame,
  .getFrame        = dxgi_getFrame
};
