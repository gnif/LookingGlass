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
#include "common/vector.h"

#include <math.h>
#include <stdatomic.h>
#include <unistd.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <versionhelpers.h>
#include <dwmapi.h>

#include "com_ref.h"
#include "util.h"
#include "backend.h"
#include "pp.h"

#define LOCKED(...) INTERLOCKED_SECTION(this->deviceContextLock, __VA_ARGS__)

//post processers
extern const DXGIPostProcess DXGIPP_Downsample;
extern const DXGIPostProcess DXGIPP_HDR16to10;
extern const DXGIPostProcess DXGIPP_RGB24;

const DXGIPostProcess * postProcessors[] =
{
  &DXGIPP_Downsample,
  &DXGIPP_HDR16to10,
  &DXGIPP_RGB24
};

typedef struct
{
  ID3D11Texture2D          * src;
  ID3D11ShaderResourceView * srv;
  const DXGIPostProcess    * pp;
  void                     * opaque;

  bool configured;
  int  rows, cols;
}
PostProcessInstance;

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
  void                     * map;
  uint32_t                   damageRectsCount;
  FrameDamageRect            damageRects[KVMFR_MAX_DAMAGE_RECTS];
  int                        texDamageCount;
  FrameDamageRect            texDamageRects[KVMFR_MAX_DAMAGE_RECTS];

  // post processing
  Vector                     pp;
}
Texture;

typedef struct FrameDamage
{
  int             count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
}
FrameDamage;

struct DXGIInterface
{
  ComScope * comScope;

  bool                       initialized;
  LARGE_INTEGER              perfFreq;
  LARGE_INTEGER              frameTime;
  bool                       stop;
  HDESK                      desktop;
  IDXGIFactory1           ** factory;
  IDXGIAdapter1           ** adapter;
  IDXGIOutput             ** output;
  ID3D11Device            ** device;
  ID3D11DeviceContext     ** deviceContext;
  LG_Lock                    deviceContextLock;
  bool                       debug;
  bool                       useAcquireLock;
  bool                       allowRGB24;
  bool                       dwmFlush;
  bool                       disableDamage;
  D3D_FEATURE_LEVEL          featureLevel;
  IDXGIOutputDuplication  ** dup;
  int                        maxTextures;
  Texture                  * texture;
  int                        texRIndex;
  int                        texWIndex;
  atomic_int                 texReady;
  bool                       needsRelease;
  DXGI_FORMAT                dxgiSrcFormat, dxgiFormat;
  bool                       hdr;
  DXGI_COLOR_SPACE_TYPE      dxgiColorSpace;
  ID3D11VertexShader      ** vshader;
  struct DXGICopyBackend   * backend;
  bool                       backendConfigured;

  CaptureGetPointerBuffer    getPointerBufferFn;
  CapturePostPointerBuffer   postPointerBufferFn;
  unsigned                   frameBuffers;
  LGEvent                  * frameEvent;

  unsigned int    formatVer;
  unsigned int    width , outputWidth , dataWidth;
  unsigned int    height, outputHeight, dataHeight;
  unsigned int    pitch;
  unsigned int    stride;
  unsigned int    padding;
  unsigned int    bpp;
  double          scaleX, scaleY;
  CaptureFormat   format, outputFormat;
  CaptureRotation rotation;

  int  lastPointerX, lastPointerY;
  bool lastPointerVisible;

  FrameDamage frameDamage[LGMP_Q_FRAME_LEN];
};

// locals

static struct DXGIInterface * this = NULL;

extern struct DXGICopyBackend copyBackendD3D11;
static struct DXGICopyBackend * backends[] = {
  &copyBackendD3D11,
};

// defines

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this->comScope, dst, src)

// forwards

static bool          dxgi_deinit(void);
static CaptureResult dxgi_releaseFrame(void);

static void ppEarlyInit(void);
static bool ppInit(const DXGIPostProcess * pp, bool shareable);
static ID3D11Texture2D * ppRun(Texture * tex, ID3D11Texture2D * src,
  int * width, int * height,
  int * cols , int * rows,
  CaptureFormat * format);
static void ppFreeAll(void);

// implementation

static const char * dxgi_getName(void)
{
  if (!this)
    return "DXGI";

  static char name[64];
  snprintf(name, sizeof(name), "DXGI %s", this->backend->name);

  return name;
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
    {
      .module         = "dxgi",
      .name           = "dwmFlush",
      .description    = "Use DwmFlush to sync the capture to the windows presentation inverval",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "dxgi",
      .name           = "disableDamage",
      .description    = "Do not do damage-aware copies, i.e. always do full frame copies",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "dxgi",
      .name           = "debug",
      .description    = "Enable Direct3D debugging (developers only, massive performance penalty)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = false
    },
    {
      .module         = "dxgi",
      .name           = "allowRGB24",
      .description    = "Losslessly pack 32-bit RGBA8 into 24-bit RGB (saves bandwidth)",
      .type           = OPTION_TYPE_BOOL,
      .value.x_bool   = true
    },
    {0}
  };

  option_register(options);
  ppEarlyInit();
}

static bool dxgi_create(
  CaptureGetPointerBuffer  getPointerBufferFn,
  CapturePostPointerBuffer postPointerBufferFn,
  unsigned                 frameBuffers)
{
  DEBUG_ASSERT(!this);
  this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("failed to allocate DXGIInterface struct");
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

  this->debug               = option_get_bool("dxgi", "debug");
  this->useAcquireLock      = option_get_bool("dxgi", "useAcquireLock");
  this->allowRGB24          = option_get_bool("dxgi", "allowRGB24");
  this->dwmFlush            = option_get_bool("dxgi", "dwmFlush");
  this->disableDamage       = option_get_bool("dxgi", "disableDamage");
  this->texture             = calloc(this->maxTextures, sizeof(*this->texture));
  this->getPointerBufferFn  = getPointerBufferFn;
  this->postPointerBufferFn = postPointerBufferFn;
  this->frameBuffers        = frameBuffers;

  return true;
}

static bool initVertexShader(void)
{
  comRef_scopePush(10);

  static const char * vshaderSrc =
    "void main(\n"
    "  in  uint   vertexID : SV_VERTEXID,\n"
    "  out float4 position : SV_POSITION,\n"
    "  out float2 texCoord : TEXCOORD0)\n"
    "{\n"
    "  float2 positions[4] =\n"
    "  {\n"
    "    float2(-1.0,  1.0),\n"
    "    float2( 1.0,  1.0),\n"
    "    float2(-1.0, -1.0),\n"
    "    float2( 1.0, -1.0)\n"
    "  };\n"
    "\n"
    "  float2 texCoords[4] =\n"
    "  {\n"
    "    float2(0.0, 0.0),\n"
    "    float2(1.0, 0.0),\n"
    "    float2(0.0, 1.0),\n"
    "    float2(1.0, 1.0)\n"
    "  };\n"
    "\n"
    "  position = float4(positions[vertexID], 0.0, 1.0);\n"
    "  texCoord = texCoords[vertexID];\n"
    "}";

  // compile and create the vertex shader
  comRef_defineLocal(ID3DBlob, byteCode);
  if (!compileShader(byteCode, "main", "vs_5_0", vshaderSrc, NULL))
    return false;

  comRef_defineLocal(ID3D11VertexShader, vshader);
  HRESULT status = ID3D11Device_CreateVertexShader(
    *this->device,
    ID3D10Blob_GetBufferPointer(*byteCode),
    ID3D10Blob_GetBufferSize   (*byteCode),
    NULL,
    vshader);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create the vertex shader", status);
    return false;
  }

  comRef_toGlobal(this->vshader, vshader);
  comRef_scopePop();
  return true;
}

static bool dxgi_init(void * ivshmemBase, unsigned * alignSize)
{
  DEBUG_ASSERT(this);

  comRef_initGlobalScope(20 + this->maxTextures * 16, this->comScope);
  comRef_scopePush(20);

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

  lgResetEvent(this->frameEvent);

  comRef_defineLocal(IDXGIFactory1, factory);
  status = CreateDXGIFactory2(this->debug ? DXGI_CREATE_FACTORY_DEBUG : 0,
    &IID_IDXGIFactory1,
    (void **)factory);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create DXGIFactory1", status);
    goto fail;
  }

  const char * optAdapter = option_get_string("dxgi", "adapter");
  const char * optOutput  = option_get_string("dxgi", "output" );

  comRef_defineLocal(IDXGIAdapter1, adapter);
  comRef_defineLocal(IDXGIOutput  , output );

  for (
    int i = 0;
    IDXGIFactory1_EnumAdapters1(*factory, i, adapter)
      != DXGI_ERROR_NOT_FOUND;
    ++i, comRef_release(adapter))
  {
    DXGI_ADAPTER_DESC1 adapterDesc;
    status = IDXGIAdapter1_GetDesc1(*adapter, &adapterDesc);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to get the device description", status);
      goto fail;
    }

    // check for virtual devices without D3D support
    if (
        // Microsoft Basic Render Driver
        (adapterDesc.VendorId == 0x1414 && adapterDesc.DeviceId == 0x008c) ||
        // QXL
        (adapterDesc.VendorId == 0x1b36 && adapterDesc.DeviceId == 0x000d) ||
        // QEMU Standard VGA
        (adapterDesc.VendorId == 0x1234 && adapterDesc.DeviceId == 0x1111))
    {
      DEBUG_INFO("Not using unsupported adapter: %ls",
          adapterDesc.Description);
      continue;
    }

    if (optAdapter)
    {
      const size_t s = (wcslen(adapterDesc.Description)+1) * 2;
      char * desc = malloc(s);
      wcstombs(desc, adapterDesc.Description, s);

      if (strstr(desc, optAdapter) == NULL)
      {
        DEBUG_INFO("Not using adapter: %ls", adapterDesc.Description);
        free(desc);
        continue;
      }
      free(desc);

      DEBUG_INFO("Adapter matched, trying: %ls", adapterDesc.Description);
    }

    for (
      int n = 0;
      IDXGIAdapter1_EnumOutputs(*adapter, n, output) != DXGI_ERROR_NOT_FOUND;
      ++n, comRef_release(output))
    {
      IDXGIOutput_GetDesc(*output, &outputDesc);
      if (optOutput)
      {
        const size_t s = (wcslen(outputDesc.DeviceName)+1) * 2;
        char * desc = malloc(s);
        wcstombs(desc, outputDesc.DeviceName, s);

        if (strstr(desc, optOutput) == NULL)
        {
          DEBUG_INFO("Not using adapter output: %ls", outputDesc.DeviceName);
          free(desc);
          continue;
        }
        free(desc);

        DEBUG_INFO("Adapter output matched, trying: %ls", outputDesc.DeviceName);
      }

      if (outputDesc.AttachedToDesktop)
        break;
    }

    if (*output)
      break;
  }

  if (!*output)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    goto fail;
  }

  comRef_toGlobal(this->factory, factory);
  comRef_toGlobal(this->adapter, adapter);
  comRef_toGlobal(this->output , output );

  DXGI_ADAPTER_DESC1 adapterDesc;
  IDXGIAdapter1_GetDesc1(*this->adapter, &adapterDesc);
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

  comRef_defineLocal(IDXGIAdapter, tmp);
  status = IDXGIAdapter1_QueryInterface(
    *this->adapter, &IID_IDXGIAdapter, (void **)tmp);

  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to query IDXGIAdapter interface");
    goto fail;
  }

  comRef_defineLocal(ID3D11Device       , device       );
  comRef_defineLocal(ID3D11DeviceContext, deviceContext);
  status = D3D11CreateDevice(
    *tmp,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
      (this->debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
    featureLevels, featureLevelCount,
    D3D11_SDK_VERSION,
    device,
    &this->featureLevel,
    deviceContext);

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    goto fail;
  }

  comRef_toGlobal(this->device       , device       );
  comRef_toGlobal(this->deviceContext, deviceContext);
  LG_LOCK_INIT(this->deviceContextLock);

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
  DEBUG_INFO("Debug mode        : %s"     , this->debug ? "enabled" : "disabled");

  // try to reduce the latency
  {
    comRef_defineLocal(IDXGIDevice1, dxgi);
    status = ID3D11Device_QueryInterface(
      *this->device, &IID_IDXGIDevice1, (void **)dxgi);

    if (FAILED(status))
    {
      DEBUG_WINERROR("failed to query DXGI interface from device", status);
      goto fail;
    }

    IDXGIDevice1_SetMaximumFrameLatency(*dxgi, 1);
  }

  comRef_defineLocal(IDXGIOutputDuplication, dup);

  comRef_defineLocal(IDXGIOutput5, output5);
  status = IDXGIOutput_QueryInterface(
    *this->output, &IID_IDXGIOutput5, (void **)output5);

  if (FAILED(status))
  {
    DEBUG_WARN("IDXGIOutput5 is not available, please update windows for improved performance!");
    DEBUG_WARN("Falling back to IDXIGOutput1");

    comRef_defineLocal(IDXGIOutput1, output1);
    status = IDXGIOutput_QueryInterface(
      *this->output, &IID_IDXGIOutput1, (void **)output1);

    if (FAILED(status))
    {
      DEBUG_ERROR("Failed to query IDXGIOutput1 from the output");
      goto fail;
    }

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      status = IDXGIOutput1_DuplicateOutput(
        *output1, *(IUnknown **)this->device, dup);

      if (SUCCEEDED(status))
        break;
      Sleep(200);
    }

    if (FAILED(status))
    {
      DEBUG_WINERROR("DuplicateOutput Failed", status);
      goto fail;
    }
  }
  else
  {
    const DXGI_FORMAT supportedFormats[] =
    {
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      status = IDXGIOutput5_DuplicateOutput1(
        *output5,
        (IUnknown *)*this->device,
        0,
        ARRAY_LENGTH(supportedFormats),
        supportedFormats,
        dup);

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
      goto fail;
    }

    comRef_toGlobal(this->dup, dup);

    comRef_defineLocal(IDXGIOutput6, output6);
    status = IDXGIOutput_QueryInterface(
      *this->output, &IID_IDXGIOutput6, (void **)output6);

    if (SUCCEEDED(status))
    {
      DXGI_OUTPUT_DESC1 desc1;
      IDXGIOutput6_GetDesc1(*output6, &desc1);
      this->dxgiColorSpace = desc1.ColorSpace;

      DEBUG_INFO("Bits Per Color    : %u"   , desc1.BitsPerColor);
      DEBUG_INFO("Color Space       : %s"   , getDXGIColorSpaceTypeStr(this->dxgiColorSpace));
      DEBUG_INFO("Min/Max Luminance : %f/%f", desc1.MinLuminance, desc1.MaxLuminance);
      DEBUG_INFO("Frame Luminance   : %f"   , desc1.MaxFullFrameLuminance);
    }
  }

  {
    DXGI_OUTDUPL_DESC dupDesc;
    IDXGIOutputDuplication_GetDesc(*this->dup, &dupDesc);
    DEBUG_INFO("Source Format     : %s", getDXGIFormatStr(dupDesc.ModeDesc.Format));

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    comRef_defineLocal(IDXGIResource, res);

    if (FAILED(status = IDXGIOutputDuplication_AcquireNextFrame(*this->dup,
      INFINITE, &frameInfo, res)))
    {
      DEBUG_WINERROR("AcquireNextFrame Failed", status);
      goto fail;
    }

    comRef_defineLocal(ID3D11Texture2D, src);
    if (FAILED(status = IDXGIResource_QueryInterface(*res, &IID_ID3D11Texture2D,
      (void **)src)))
    {
      DEBUG_WINERROR("ResourceQueryInterface failed", status);
      IDXGIOutputDuplication_ReleaseFrame(*this->dup);
      goto fail;
    }

    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D_GetDesc(*src, &desc);
    this->dxgiSrcFormat = desc.Format;
    this->dxgiFormat    = desc.Format;

    DEBUG_INFO("Capture Format    : %s", getDXGIFormatStr(desc.Format));

    IDXGIOutputDuplication_ReleaseFrame(*this->dup);
    this->hdr = this->dxgiColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
  }

  // set the initial format
  switch(this->dxgiFormat)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      this->format = CAPTURE_FMT_BGRA;
      break;

    case DXGI_FORMAT_R8G8B8A8_UNORM:
      this->format = CAPTURE_FMT_RGBA;
      break;

    case DXGI_FORMAT_R10G10B10A2_UNORM:
      this->format = CAPTURE_FMT_RGBA10;
      break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      this->format = CAPTURE_FMT_RGBA16F;
      break;

    default:
      DEBUG_ERROR("Unsupported source format");
      goto fail;
  }

  this->outputWidth  = this->width;
  this->outputHeight = this->height;

  if (!backends[0]->create(
    ivshmemBase,
    alignSize,
    this->frameBuffers,
    this->maxTextures))
  {
    DEBUG_ERROR("Failed to initialize selected capture backend: %s",
      backends[0]->name);
    backends[0]->free();
    goto fail;
  }

  this->backend = backends[0];

  DEBUG_INFO("Output Size       : %u x %u", this->outputWidth, this->outputHeight);
  DEBUG_INFO("Copy backend      : %s", this->backend->name);
  DEBUG_INFO("Damage-aware copy : %s", this->disableDamage  ? "disabled" : "enabled" );

  for (int i = 0; i < this->maxTextures; ++i)
  {
    this->texture[i].texDamageCount = -1;
    vector_create(&this->texture[i].pp, sizeof(PostProcessInstance), 0);
  }

  if (!initVertexShader())
    goto fail;

  bool shareable = this->backend != &copyBackendD3D11;
  if (this->hdr)
  {
    //HDR content needs to be converted to HDR10
    if (!ppInit(&DXGIPP_HDR16to10, shareable))
    {
      DEBUG_ERROR("Failed to initialize the HDR16to10 post processor");
      goto fail;
    }
  }

  //Downsampling must happen before conversion to RGB24
  if (!ppInit(&DXGIPP_Downsample, shareable))
  {
    DEBUG_ERROR("Failed to intiailize the downsample post processor");
    goto fail;
  }

  //If not HDR, pack to RGB24
  if (!this->hdr && this->allowRGB24)
  {
    if (!ppInit(&DXGIPP_RGB24, shareable))
      DEBUG_WARN("Failed to initialize the RGB24 post processor");
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    this->frameDamage[i].count = -1;

  QueryPerformanceFrequency(&this->perfFreq) ;
  QueryPerformanceCounter  (&this->frameTime);
  this->initialized = true;

  comRef_scopePop();
  return true;

fail:
  comRef_scopePop();
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
    Texture * tex = &this->texture[i];
    switch(tex->state)
    {
      case TEXTURE_STATE_MAPPED:
        this->backend->unmapTexture(i);
        tex->map = NULL;
        //fallthrough

      case TEXTURE_STATE_PENDING_MAP:
        tex->state = TEXTURE_STATE_UNUSED;
        //fallthrough

      case TEXTURE_STATE_UNUSED:
        break;
    }
  }

  if (this->dup && *this->dup)
    dxgi_releaseFrame();

  // this MUST run before backend->free() & ppFreeAll.
  comRef_freeScope(&this->comScope);

  ppFreeAll();
  if (this->backend)
  {
    this->backendConfigured = false;
    this->backend->free();
    this->backend = NULL;
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
    .x      = floor((double)src->left * this->scaleX),
    .y      = floor((double)src->top  * this->scaleY),
    .width  = ceil ((double)(src->right - src->left) * this->scaleX),
    .height = ceil ((double)(src->bottom - src->top) * this->scaleY)
  };
}

static void computeFrameDamage(Texture * tex)
{
  // By default, damage the full frame.
  tex->damageRectsCount = 0;

  if (this->disableDamage)
    return;

  const int maxDamageRectsCount = ARRAY_LENGTH(tex->damageRects);

  // Compute dirty rectangles.
  RECT dirtyRects[maxDamageRectsCount];
  UINT dirtyRectsBufferSizeRequired;
  if (FAILED(IDXGIOutputDuplication_GetFrameDirtyRects(*this->dup,
        sizeof(dirtyRects), dirtyRects,
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
  if (FAILED(IDXGIOutputDuplication_GetFrameMoveRects(*this->dup,
        sizeof(moveRects), moveRects,
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
      .x      = floor((double)moveRect->SourcePoint.x * this->scaleX),
      .y      = floor((double)moveRect->SourcePoint.y * this->scaleY),
      .width  = ceil((double)(moveRect->DestinationRect.right  -
        moveRect->DestinationRect.left) * this->scaleX),
      .height = ceil((double)(moveRect->DestinationRect.bottom -
        moveRect->DestinationRect.top ) * this->scaleY)
    };

    rectToFrameDamageRect(&moveRect->DestinationRect, texDamageRect++);
    actuallyMovedRectsCount += 2;
  }

  tex->damageRectsCount = dirtyRectsCount + actuallyMovedRectsCount;
}

static void computeTexDamage(Texture * tex)
{
  if (tex->texDamageCount < 0 || tex->damageRectsCount == 0 ||
      tex->texDamageCount + tex->damageRectsCount > KVMFR_MAX_DAMAGE_RECTS)
    tex->texDamageCount = -1;
  else
  {
    memcpy(tex->texDamageRects + tex->texDamageCount, tex->damageRects,
      tex->damageRectsCount * sizeof(FrameDamageRect));
    tex->texDamageCount += tex->damageRectsCount;
    tex->texDamageCount = rectsMergeOverlapping(tex->texDamageRects, tex->texDamageCount);
  }
}

static CaptureResult dxgi_capture(unsigned frameBufferIndex,
  FrameBuffer * frameBuffer)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);
  comRef_scopePush(10);

  Texture                 * tex = NULL;
  CaptureResult             result;
  HRESULT                   status;
  DXGI_OUTDUPL_FRAME_INFO   frameInfo;
  comRef_defineLocal(IDXGIResource, res);

  bool copyFrame   = false;
  bool copyPointer = false;
  comRef_defineLocal(ID3D11Texture2D, src);

  bool           postPointer      = false;
  CapturePointer pointer          = { 0 };
  void *         pointerShape     = NULL;
  UINT           pointerShapeSize = 0;

  // release the prior frame
  result = dxgi_releaseFrame();
  if (result != CAPTURE_RESULT_OK)
    goto exit;

  // this is a bit of a hack as it causes this thread to block until the next
  // present, by doing this we can allow the mouse updates to accumulate instead
  // of being called to process every single one. The only caveat is we are
  // limited to the refresh rate of the monitor.
  if (this->dwmFlush)
    DwmFlush();

  if (this->useAcquireLock)
  {
    LOCKED({
        status = IDXGIOutputDuplication_AcquireNextFrame(
          *this->dup, 1, &frameInfo, res);
    });
  }
  else
    status = IDXGIOutputDuplication_AcquireNextFrame(
      *this->dup, 1000, &frameInfo, res);

  result = dxgi_hResultToCaptureResult(status);
  if (result != CAPTURE_RESULT_OK)
  {
    if (result == CAPTURE_RESULT_ERROR)
      DEBUG_WINERROR("AcquireNextFrame failed", status);

    goto exit;
  }

  this->needsRelease = true;
  if (frameInfo.LastPresentTime.QuadPart != 0)
  {
    tex = &this->texture[this->texWIndex];

    // check if the texture is free, if not skip the frame to keep up
    if (tex->state == TEXTURE_STATE_UNUSED)
    {
      copyFrame = true;
      status = IDXGIResource_QueryInterface(
        *res, &IID_ID3D11Texture2D, (void **)src);

      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to get the texture from the dxgi resource", status);
        result = CAPTURE_RESULT_ERROR;
        goto exit;
      }
    }
    else
    {
      // If we are skipping the frame, then we lose track of the damage,
      // and must invalidate all the textures.
      for (int i = 0; i < this->maxTextures; ++i)
        this->texture[i].texDamageCount = -1;
    }
  }

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
    if (copyFrame)
    {
      // run any postprocessors
      int width   = this->width;
      int height  = this->height;
      int cols    = this->width;
      int rows    = this->height;
      CaptureFormat format  = this->format;
      ID3D11Texture2D *dst = ppRun(
        tex, *src, &width, &height, &cols, &rows, &format);

      if (!this->backendConfigured)
      {
        switch(format)
        {
          case CAPTURE_FMT_RGBA:
            this->bpp        = 4;
            this->dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            break;

          case CAPTURE_FMT_BGRA:
            this->bpp        = 4;
            this->dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;

          case CAPTURE_FMT_RGBA10:
            this->bpp        = 4;
            this->dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            break;

          case CAPTURE_FMT_RGBA16F:
            this->bpp        = 8;
            this->dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;

          case CAPTURE_FMT_BGR_32:
            this->bpp        = 4;
            this->dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
            break;

          default:
            DEBUG_ERROR("Unsupported input format");
            result = CAPTURE_RESULT_ERROR;
            goto exit;
        }

        unsigned pitch = 0;
        LG_LOCK(this->deviceContextLock);
        if (!this->backend->configure(
          cols,
          rows,
          this->dxgiFormat,
          this->bpp,
          &pitch))
        {
          LG_UNLOCK(this->deviceContextLock);
          DEBUG_ERROR("Failed to configure the copy backend");
          result = CAPTURE_RESULT_ERROR;
          goto exit;
        }
        LG_UNLOCK(this->deviceContextLock);

        DEBUG_ASSERT(pitch && "copy backend did not return the pitch");

        this->backendConfigured = true;
        this->outputWidth       = width;
        this->outputHeight      = height;
        this->outputFormat      = format;
        this->dataWidth         = cols;
        this->dataHeight        = rows;
        this->pitch             = pitch;
        this->stride            = pitch / this->bpp;

        this->scaleX = (double)width  / this->width;
        this->scaleY = (double)height / this->height;
      }

      // compute the frame damage
      if (this->useAcquireLock)
      {
        LOCKED({ computeFrameDamage(tex); });
      }
      else
        computeFrameDamage(tex);
      computeTexDamage(tex);

      if (!this->backend->preCopy(dst, this->texWIndex,
        frameBufferIndex, frameBuffer))
      {
        result = CAPTURE_RESULT_ERROR;
        goto exit;
      }

      if (tex->texDamageCount <= 0)
      {
        if (!this->backend->copyFull(dst, this->texWIndex))
        {
          // call this so the backend can cleanup
          this->backend->postCopy(dst, this->texWIndex);
          result = CAPTURE_RESULT_ERROR;
          goto exit;
        }
      }
      else
      {
        for (int i = 0; i < tex->texDamageCount; ++i)
        {
          FrameDamageRect rect = tex->texDamageRects[i];

          // correct the damage rect for BGR packed data
          if (this->outputFormat == CAPTURE_FMT_BGR_32)
          {
            rect.x     = (rect.x     * 3    ) / 4; // round down
            rect.width = (rect.width * 3 + 3) / 4; // round up
          }

          if (!this->backend->copyRect(dst, this->texWIndex, &rect))
          {
            result = CAPTURE_RESULT_ERROR;
            goto exit;
          }
        }
      }

      if (!this->backend->postCopy(dst, this->texWIndex))
      {
        result = CAPTURE_RESULT_ERROR;
        goto exit;
      }

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
      DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
      if (this->useAcquireLock)
      {
        LOCKED({
          status = IDXGIOutputDuplication_GetFramePointerShape(
              *this->dup, bufferSize, pointerShape, &pointerShapeSize, &shapeInfo);
        });
      }
      else
        status = IDXGIOutputDuplication_GetFramePointerShape(
            *this->dup, bufferSize, pointerShape, &pointerShapeSize, &shapeInfo);

      result = dxgi_hResultToCaptureResult(status);
      if (result != CAPTURE_RESULT_OK)
      {
        if (result == CAPTURE_RESULT_ERROR)
          DEBUG_WINERROR("Failed to get the new pointer shape", status);
        goto exit;
      }

      switch(shapeInfo.Type)
      {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR       : pointer.format = CAPTURE_FMT_COLOR ; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: pointer.format = CAPTURE_FMT_MASKED; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME  : pointer.format = CAPTURE_FMT_MONO  ; break;
        default:
          DEBUG_ERROR("Unsupported cursor format");
          result = CAPTURE_RESULT_ERROR;
          goto exit;
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
    this->postPointerBufferFn(&pointer);
  }

  result = CAPTURE_RESULT_OK;
exit:
  comRef_scopePop();
  return result;
}

static CaptureResult dxgi_waitFrame(unsigned frameBufferIndex,
  CaptureFrame * frame, const size_t maxFrameSize)
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

  CaptureResult result = this->backend->mapTexture(this->texRIndex, &tex->map);
  if (result != CAPTURE_RESULT_OK)
    return result;

  tex->state = TEXTURE_STATE_MAPPED;

  const unsigned int maxRows = maxFrameSize / this->pitch;

  frame->formatVer        = tex->formatVer;
  frame->screenWidth      = this->width;
  frame->screenHeight     = this->height;
  frame->dataWidth        = this->dataWidth;
  frame->dataHeight       = min(maxRows, this->dataHeight);
  frame->frameWidth       = this->outputWidth;
  frame->frameHeight      = this->outputHeight;
  frame->truncated        = maxRows < this->dataHeight;
  frame->pitch            = this->pitch;
  frame->stride           = this->stride;
  frame->format           = this->outputFormat;
  frame->hdr              = this->hdr;
  frame->hdrPQ            = this->outputFormat == CAPTURE_FMT_RGBA10;
  frame->rotation         = this->rotation;

  frame->damageRectsCount = tex->damageRectsCount;
  memcpy(frame->damageRects, tex->damageRects,
      tex->damageRectsCount * sizeof(*tex->damageRects));

  atomic_fetch_sub_explicit(&this->texReady, 1, memory_order_release);
  return CAPTURE_RESULT_OK;
}

static CaptureResult dxgi_getFrame(unsigned frameBufferIndex,
  FrameBuffer * frame, const size_t maxFrameSize)
{
  DEBUG_ASSERT(this);
  DEBUG_ASSERT(this->initialized);

  Texture     * tex    = &this->texture[this->texRIndex];
  FrameDamage * damage = &this->frameDamage[frameBufferIndex];

  if (this->backend->writeFrame)
  {
    CaptureResult result = this->backend->writeFrame(frameBufferIndex, frame);
    if (result != CAPTURE_RESULT_OK)
      return result;
  }
  else
  {
    if (tex->damageRectsCount                 == 0 ||
        damage->count                          < 0 ||
        damage->count + tex->damageRectsCount  > KVMFR_MAX_DAMAGE_RECTS)
    {
      // damage all
      framebuffer_write(frame, tex->map, this->pitch * this->dataHeight);
    }
    else
    {
      memcpy(damage->rects + damage->count, tex->damageRects,
        tex->damageRectsCount * sizeof(*tex->damageRects));
      damage->count += tex->damageRectsCount;

      if (this->outputFormat == CAPTURE_FMT_BGR_32)
      {
        FrameDamageRect scaledDamageRects[damage->count];
        for (int i = 0; i < ARRAYSIZE(scaledDamageRects); i++) {
          FrameDamageRect rect = damage->rects[i];
          int originalX = rect.x;
          int scaledX = originalX * 3 / 4;
          rect.x = scaledX;
          rect.width = (((originalX + rect.width) * 3 + 3) / 4) - scaledX;

          scaledDamageRects[i] = rect;
        }

        rectsBufferToFramebuffer(scaledDamageRects, damage->count, this->bpp, frame,
          this->pitch, this->dataHeight, tex->map, this->pitch);
      }
      else
      {
        rectsBufferToFramebuffer(damage->rects, damage->count, this->bpp, frame,
          this->pitch, this->dataHeight, tex->map, this->pitch);
      }
    }
  }

  for (int i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    struct FrameDamage * damage = this->frameDamage + i;
    if (i == frameBufferIndex)
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

  this->backend->unmapTexture(this->texRIndex);
  tex->map   = NULL;
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

  this->backend->preRelease();

  HRESULT status;
  LOCKED({status = IDXGIOutputDuplication_ReleaseFrame(*this->dup);});
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

static void ppEarlyInit(void)
{
  for(int i = 0; i < ARRAY_LENGTH(postProcessors); ++i)
    if (postProcessors[i]->earlyInit)
      postProcessors[i]->earlyInit();
}

static bool ppInit(const DXGIPostProcess * pp, bool shareable)
{
  if (!pp->setup(this->device, this->deviceContext, this->output, shareable))
    return false;

  for(int i = 0; i < this->maxTextures; ++i)
  {
    PostProcessInstance inst = { .pp = pp };
    if (!pp->init(&inst.opaque))
    {
      DEBUG_ERROR("Failed to init a post processor");
      return false;
    }
    vector_push(&this->texture[i].pp, &inst);
  }

  return true;
}

static ID3D11Texture2D * ppRun(Texture * tex, ID3D11Texture2D * src,
  int * width, int * height,
  int * cols , int * rows,
  CaptureFormat * format)
{
  PostProcessInstance * inst;
  vector_forEachRef(inst, &tex->pp)
  {
    // if the srv exists and the src has changed, release it
    if (inst->src != src && inst->srv)
    {
      ID3D11ShaderResourceView_Release(inst->srv);
      inst->srv = NULL;
    }

    // if the srv is not set, create one
    if (!inst->srv)
    {
      D3D11_TEXTURE2D_DESC desc;
      ID3D11Texture2D_GetDesc(src, &desc);

      const D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc =
      {
        .Format              = desc.Format,
        .ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D,
        .Texture2D.MipLevels = 1
      };

      HRESULT status = ID3D11Device_CreateShaderResourceView(
        *this->device, (ID3D11Resource *)src, &srvDesc, &inst->srv);

      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to create the source resource view", status);
        return NULL;
      }

      inst->src = src;
    }

    LG_LOCK(this->deviceContextLock);
    if (!inst->configured)
    {
      if (!inst->pp->configure(inst->opaque,
        width, height,
        cols , rows,
        format))
      {
        LG_UNLOCK(this->deviceContextLock);
        DEBUG_ERROR("setFormat failed on a post processor (%s)", inst->pp->name);
        return NULL;
      }

      inst->configured = true;
      inst->rows = *rows;
      inst->cols = *cols;
    }

    // set the viewport
    const D3D11_VIEWPORT vp =
    {
      .TopLeftX = 0.0f,
      .TopLeftY = 0.0f,
      .Width    = inst->cols,
      .Height   = inst->rows,
      .MinDepth = 0.0f,
      .MaxDepth = 1.0f,
    };
    ID3D11DeviceContext_RSSetViewports(*this->deviceContext, 1, &vp);

    // set the vertex shader
    ID3D11DeviceContext_VSSetShader(
      *this->deviceContext, *this->vshader, NULL, 0);

    // run the post processor
    ID3D11Texture2D * out = inst->pp->run(inst->opaque, inst->srv);

    // if the post processor does nothing, just continue
    if (!out)
    {
      LG_UNLOCK(this->deviceContextLock);
      continue;
    }

    // draw the full screen quad
    ID3D11DeviceContext_IASetPrimitiveTopology(
      *this->deviceContext, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_Draw(*this->deviceContext, 4, 0);

    // unset the target render view
    static ID3D11RenderTargetView * nullTarget = NULL;
    ID3D11DeviceContext_OMSetRenderTargets(
      *this->deviceContext, 1, &nullTarget, NULL);

    // the output is now the input
    src = out;
    LG_UNLOCK(this->deviceContextLock);
  }

  return src;
}

static void ppFreeAll(void)
{
  for(int i = 0; i < this->maxTextures; ++i)
  {
    Texture * tex = &this->texture[i];
    if (!tex->pp.data)
      continue;

    PostProcessInstance * inst;
    vector_forEachRef(inst, &tex->pp)
    {
      if(inst->srv)
        ID3D11ShaderResourceView_Release(inst->srv);

      inst->pp->free(inst->opaque);
      if (i == this->maxTextures - 1)
        inst->pp->finish();
    }
    vector_destroy(&tex->pp);
  }
}

IDXGIAdapter1 * dxgi_getAdapter(void)
{
  return *this->adapter;
}

ID3D11Device * dxgi_getDevice(void)
{
  return *this->device;
}

ID3D11DeviceContext * dxgi_getContext(void)
{
  return *this->deviceContext;
}

void dxgi_contextLock(void)
{
  LG_LOCK(this->deviceContextLock);
};

void dxgi_contextUnlock(void)
{
  LG_UNLOCK(this->deviceContextLock);
};

bool dxgi_debug(void)
{
  return this->debug;
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
