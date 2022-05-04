/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

  NVFBCRESULT status;
  NvU32 version;
  if ((status = nvapi.getVersion(&version)) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the NvFBC SDK version: %d", status);
    return false;
  }

  DEBUG_INFO("NvFBC SDK Version: %lu", version);

  if (nvapi.enable(NVFBC_STATE_ENABLE) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to enable the NvFBC interface: %d", status);
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
  int            adapterIndex,
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
  params.dwAdapterIdx      = adapterIndex;
  params.dwPrivateDataSize = privDataSize;
  params.pPrivateData      = privData;

  NVFBCRESULT status = nvapi.createEx(&params);
  if (status != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to create nvfbc: %d", status);
    *handle = NULL;
    return false;
  }

  *handle = (NvFBCHandle)calloc(1, sizeof(**handle));
  (*handle)->nvfbc = static_cast<NvFBCToSys *>(params.pNvFBC);

  if (maxWidth)
    *maxWidth = params.dwMaxDisplayWidth;

  if (maxHeight)
    *maxHeight = params.dwMaxDisplayHeight;

  return true;
}

void NvFBCGetDiffMapBlockSize(
  int                     diffRes,
  enum DiffMapBlockSize * diffMapBlockSize,
  int                   * diffShift,
  void                  * privData,
  unsigned int            privDataSize
)
{
  NvFBCStatusEx status = {0};
  status.dwVersion = NVFBC_STATUS_VER;
  status.dwPrivateDataSize = privDataSize;
  status.pPrivateData      = privData;

  NVFBCRESULT result = nvapi.getStatusEx(&status);
  if (result != NVFBC_SUCCESS)
    status.bSupportConfigurableDiffMap = FALSE;

  if (!status.bSupportConfigurableDiffMap)
  {
    *diffMapBlockSize = DIFFMAP_BLOCKSIZE_128X128;
    *diffShift        = 7;
    return;
  }

  switch (diffRes)
  {
    case 16:
      *diffMapBlockSize = DIFFMAP_BLOCKSIZE_16X16;
      *diffShift        = 4;
      break;

    case 32:
      *diffMapBlockSize = DIFFMAP_BLOCKSIZE_32X32;
      *diffShift        = 5;
      break;

    case 64:
      *diffMapBlockSize = DIFFMAP_BLOCKSIZE_64X64;
      *diffShift        = 6;
      break;

    default:
      *diffMapBlockSize = DIFFMAP_BLOCKSIZE_128X128;
      *diffShift        = 7;
      break;
  }
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
    DEBUG_ERROR("Failed to setup NVFBCToSys: %d", status);
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
  bool                 scale,
  NvFBCFrameGrabInfo * grabInfo
)
{
  NVFBC_TOSYS_GRAB_FRAME_PARAMS params = {0};

  params.dwVersion           = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;
  params.dwFlags             = NVFBC_TOSYS_WAIT_WITH_TIMEOUT;
  params.dwWaitTime          = waitTime;
  params.eGMode              = scale ?
    NVFBC_TOSYS_SOURCEMODE_SCALE : NVFBC_TOSYS_SOURCEMODE_CROP;
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

    case NVFBC_ERROR_PROTECTED_CONTENT:
      DEBUG_WARN("Protected content is playing, can't capture");
      Sleep(100);
      return CAPTURE_RESULT_TIMEOUT;

    case NVFBC_ERROR_INVALID_PARAM:
      if (handle->retry < 2)
      {
        Sleep(100);
        ++handle->retry;
        return CAPTURE_RESULT_TIMEOUT;
      }
      DEBUG_ERROR("Invalid parameter");
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_DYNAMIC_DISABLE:
      DEBUG_ERROR("NvFBC was disabled by someone else");
      return CAPTURE_RESULT_ERROR;

    case NVFBC_ERROR_INVALIDATED_SESSION:
      DEBUG_WARN("Session was invalidated, attempting to restart");
      return CAPTURE_RESULT_REINIT;

    default:
      DEBUG_ERROR("Unknown NVFBCRESULT failure %d", status);
      return CAPTURE_RESULT_ERROR;
  }

  return CAPTURE_RESULT_OK;
}

CaptureResult NvFBCToSysGetCursor(NvFBCHandle handle, CapturePointer * pointer, void * buffer, unsigned int size)
{
  NVFBC_CURSOR_CAPTURE_PARAMS params;
  params.dwVersion = NVFBC_CURSOR_CAPTURE_PARAMS_VER;

  NVFBCRESULT status = handle->nvfbc->NvFBCToSysCursorCapture(&params);
  if (status != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get the cursor: %d", status);
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
