/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#include "wrapper.h"
#include "common/windebug.h"
#include <windows.h>
#include <NvFBC/nvFBCToSys.h>

#ifdef _WIN64
#define NVFBC_DLL "NvFBC64.dll"
#else
#define NVFBC_DLL "NvFBC.dll"
#endif

struct NVAPI
{
  bool initialized;
  HMODULE dll;

  NvFBC_CreateFunctionExType      createEx;
  NvFBC_SetGlobalFlagsType        setGlobalFlags;
  NvFBC_GetStatusExFunctionType   getStatusEx;
  NvFBC_EnableFunctionType        enable;
  NvFBC_GetSDKVersionFunctionType getVersion;
};

struct stNvFBCHandle
{
  NvFBCToSys * nvfbc;
  HANDLE       cursorEvent;
  int          retry;
};

static NVAPI nvapi;

bool NvFBCInit()
{
  if (nvapi.initialized)
    return true;

  nvapi.dll = LoadLibraryA(NVFBC_DLL);
  if (!nvapi.dll)
  {
    DEBUG_WINERROR("Failed to load " NVFBC_DLL, GetLastError());
    return false;
  }

  nvapi.createEx       = (NvFBC_CreateFunctionExType     )GetProcAddress(nvapi.dll, "NvFBC_CreateEx"      );
  nvapi.setGlobalFlags = (NvFBC_SetGlobalFlagsType       )GetProcAddress(nvapi.dll, "NvFBC_SetGlobalFlags");
  nvapi.getStatusEx    = (NvFBC_GetStatusExFunctionType  )GetProcAddress(nvapi.dll, "NvFBC_GetStatusEx"   );
  nvapi.enable         = (NvFBC_EnableFunctionType       )GetProcAddress(nvapi.dll, "NvFBC_Enable"        );
  nvapi.getVersion     = (NvFBC_GetSDKVersionFunctionType)GetProcAddress(nvapi.dll, "NvFBC_GetSDKVersion" );

  if (
    !nvapi.createEx       ||
    !nvapi.setGlobalFlags ||
    !nvapi.getStatusEx    ||
    !nvapi.enable         ||
    !nvapi.getVersion)
  {
    DEBUG_ERROR("Failed to get the required proc addresses");
    return false;
  }

  NvU32 version;
  if (nvapi.getVersion(&version) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the NvFBC SDK version");
    return false;
  }

  DEBUG_INFO("NvFBC SDK Version: %lu", version);

  if (nvapi.enable(NVFBC_STATE_ENABLE) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to enable the NvFBC interface");
    return false;
  }

  nvapi.initialized = true;
  return true;
}

void NvFBCFree()
{
  if (!nvapi.initialized)
    return;

  FreeLibrary(nvapi.dll);
  nvapi.initialized = false;
}

bool NvFBCToSysCreate(
  void         * privData,
  unsigned int   privDataSize,
  NvFBCHandle  * handle,
  unsigned int * maxWidth,
  unsigned int * maxHeight
)
{
  NvFBCCreateParams params = {0};

  params.dwVersion         = NVFBC_CREATE_PARAMS_VER;
  params.dwInterfaceType   = NVFBC_TO_SYS;
  params.pDevice           = NULL;
  params.dwAdapterIdx      = 0;
  params.dwPrivateDataSize = privDataSize;
  params.pPrivateData      = privData;

  if (nvapi.createEx(&params) != NVFBC_SUCCESS)
  {
    *handle = NULL;
    return false;
  }

  *handle = (NvFBCHandle)calloc(sizeof(struct stNvFBCHandle), 1);
  (*handle)->nvfbc = static_cast<NvFBCToSys *>(params.pNvFBC);

  if (maxWidth)
    *maxWidth = params.dwMaxDisplayWidth;

  if (maxHeight)
    *maxHeight = params.dwMaxDisplayHeight;

  return true;
}

void NvFBCToSysRelease(NvFBCHandle * handle)
{
  if (!*handle)
    return;

  (*handle)->nvfbc->NvFBCToSysRelease();
  free(*handle);
  *handle = NULL;
}

bool NvFBCToSysSetup(
  NvFBCHandle           handle,
  enum                  BufferFormat format,
  bool                  hwCursor,
  bool                  seperateCursorCapture,
  bool                  useDiffMap,
  enum DiffMapBlockSize diffMapBlockSize,
  void               ** frameBuffer,
  void               ** diffMap,
  HANDLE              * cursorEvent
)
{
  NVFBC_TOSYS_SETUP_PARAMS params = {0};
  params.dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER;

  switch(format)
  {
    case BUFFER_FMT_ARGB      : params.eMode = NVFBC_TOSYS_ARGB      ; break;
    case BUFFER_FMT_RGB       : params.eMode = NVFBC_TOSYS_RGB       ; break;
    case BUFFER_FMT_YYYYUV420p: params.eMode = NVFBC_TOSYS_YYYYUV420p; break;
    case BUFFER_FMT_RGB_PLANAR: params.eMode = NVFBC_TOSYS_RGB_PLANAR; break;
    case BUFFER_FMT_XOR       : params.eMode = NVFBC_TOSYS_XOR       ; break;
    case BUFFER_FMT_YUV444p   : params.eMode = NVFBC_TOSYS_YUV444p   ; break;
    case BUFFER_FMT_ARGB10    :
      params.eMode       = NVFBC_TOSYS_ARGB10;
      params.bHDRRequest = TRUE;
      break;

    default:
      DEBUG_INFO("Invalid format");
      return false;
  }

  params.bWithHWCursor                = hwCursor              ? TRUE : FALSE;
  params.bEnableSeparateCursorCapture = seperateCursorCapture ? TRUE : FALSE;
  params.bDiffMap                     = useDiffMap            ? TRUE : FALSE;

  switch(diffMapBlockSize)
  {
    case DIFFMAP_BLOCKSIZE_128X128: params.eDiffMapBlockSize = NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_128X128; break;
    case DIFFMAP_BLOCKSIZE_16X16  : params.eDiffMapBlockSize = NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_16X16  ; break;
    case DIFFMAP_BLOCKSIZE_32X32  : params.eDiffMapBlockSize = NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_32X32  ; break;
    case DIFFMAP_BLOCKSIZE_64X64  : params.eDiffMapBlockSize = NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_64X64  ; break;

    default:
      DEBUG_ERROR("Invalid diffMapBlockSize");
      return false;
  }

  params.ppBuffer  = frameBuffer;
  params.ppDiffMap = diffMap;

  NVFBCRESULT status = handle->nvfbc->NvFBCToSysSetUp(&params);
  if (status != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to setup NVFBCToSys");
    return false;
  }

  if (cursorEvent)
    *cursorEvent = params.hCursorCaptureEvent;

  return true;
}

CaptureResult NvFBCToSysCapture(
  NvFBCHandle          handle,
  const unsigned int   waitTime,
  const unsigned int   x,
  const unsigned int   y,
  const unsigned int   width,
  const unsigned int   height,
  NvFBCFrameGrabInfo * grabInfo
)
{
  NVFBC_TOSYS_GRAB_FRAME_PARAMS params = {0};

  params.dwVersion           = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;
  params.dwFlags             = NVFBC_TOSYS_WAIT_WITH_TIMEOUT;
  params.dwWaitTime          = waitTime;
  params.eGMode              = NVFBC_TOSYS_SOURCEMODE_CROP;
  params.dwStartX            = x;
  params.dwStartY            = y;
  params.dwTargetWidth       = width;
  params.dwTargetHeight      = height;
  params.pNvFBCFrameGrabInfo = grabInfo;

  grabInfo->bMustRecreate = FALSE;
  NVFBCRESULT status = handle->nvfbc->NvFBCToSysGrabFrame(&params);
  if (grabInfo->bMustRecreate)
  {
    DEBUG_INFO("NvFBC reported recreation is required");
    return CAPTURE_RESULT_REINIT;
  }

  switch(status)
  {
    case NVFBC_SUCCESS:
      handle->retry = 0;
      break;

    case NVFBC_ERROR_INVALID_PARAM:
      if (handle->retry < 2)
      {
        Sleep(100);
        ++handle->retry;
        return CAPTURE_RESULT_TIMEOUT;
      }
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_DYNAMIC_DISABLE:
      DEBUG_ERROR("NvFBC was disabled by someone else");
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_INVALIDATED_SESSION:
      DEBUG_WARN("Session was invalidated, attempting to restart");
      return CAPTURE_RESULT_REINIT;

    default:
      DEBUG_ERROR("Unknown NVFBCRESULT failure 0x%x", status);
      return CAPTURE_RESULT_ERROR;
  }

  return CAPTURE_RESULT_OK;
}

CaptureResult NvFBCToSysGetCursor(NvFBCHandle handle, CapturePointer * pointer, void * buffer, unsigned int size)
{
  NVFBC_CURSOR_CAPTURE_PARAMS params;
  params.dwVersion = NVFBC_CURSOR_CAPTURE_PARAMS_VER;

  if (handle->nvfbc->NvFBCToSysCursorCapture(&params) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the cursor");
    return CAPTURE_RESULT_ERROR;
  }

  pointer->hx          = params.dwXHotSpot;
  pointer->hy          = params.dwYHotSpot;
  pointer->width       = params.dwWidth;
  pointer->height      = params.dwHeight;
  pointer->pitch       = params.dwPitch;
  pointer->visible     = params.bIsHwCursor;
  pointer->shapeUpdate = params.bIsHwCursor;

  if (!params.bIsHwCursor)
    return CAPTURE_RESULT_OK;

  switch(params.dwPointerFlags & 0x7)
  {
    case 0x1:
      pointer->format  = CAPTURE_FMT_MONO;
      pointer->height *= 2;
      break;

    case 0x2:
      pointer->format = CAPTURE_FMT_COLOR;
      break;

    case 0x4:
      pointer->format = CAPTURE_FMT_MASKED;
      break;

    default:
      DEBUG_ERROR("Invalid/unknown pointer data format");
      return CAPTURE_RESULT_ERROR;
  }

  if (params.dwBufferSize > size)
  {
    DEBUG_WARN("Cursor data larger then provided buffer");
    params.dwBufferSize = size;
  }

  memcpy(buffer, params.pBits, params.dwBufferSize);
  return CAPTURE_RESULT_OK;
}
