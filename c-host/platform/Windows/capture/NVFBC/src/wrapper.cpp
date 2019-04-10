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

#include "wrapper.h"
#include "debug.h"
#include "windows/windebug.h"
#include <windows.h>

#ifdef _WIN64
#define NVFBC_DLL "NvFBC64.dll"
#else
#define NVFBC_DLL "NvFBC.dll"
#endif

struct NVAPI
{
  bool initialized;
  HMODULE dll;

  NvFBC_CreateFunctionExType    createEx;
  NvFBC_SetGlobalFlagsType      setGlobalFlags;
  NvFBC_GetStatusExFunctionType getStatusEx;
  NvFBC_EnableFunctionType      enable;
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

  nvapi.createEx       = (NvFBC_CreateFunctionExType   )GetProcAddress(nvapi.dll, "NvFBC_CreateEx"      );
  nvapi.setGlobalFlags = (NvFBC_SetGlobalFlagsType     )GetProcAddress(nvapi.dll, "NvFBC_SetGlobalFlags");
  nvapi.getStatusEx    = (NvFBC_GetStatusExFunctionType)GetProcAddress(nvapi.dll, "NvFBC_GetStatusEx"   );
  nvapi.enable         = (NvFBC_EnableFunctionType     )GetProcAddress(nvapi.dll, "NvFBC_Enable"        );

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

bool NvFBCToSysCreate(void * privData, unsigned int privDataSize, NvFBCToSys ** nvfbc)
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
    DEBUG_ERROR("Failed to create an instance of NvFBCToSys");
    *nvfbc = NULL;
    return false;
  }

  *nvfbc = static_cast<NvFBCToSys *>(params.pNvFBC);
  return true;
}

void NvFBCToSysRelease(NvFBCToSys ** nvfbc)
{
  if (!*nvfbc)
    return;

  (*nvfbc)->NvFBCToSysRelease();
  (*nvfbc) = NULL;
}

bool NvFBCToSysSetup(
  NvFBCToSys          * nvfbc,
  enum                  BufferFormat format,
  bool                  hwCursor,
  bool                  useDiffMap,
  enum DiffMapBlockSize diffMapBlockSize,
  void **               frameBuffer,
  void **               diffMap
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
    case BUFFER_FMT_ARGB10    : params.eMode = NVFBC_TOSYS_ARGB10    ; break;

    default:
      DEBUG_INFO("Invalid format");
      return false;
  }

  params.bWithHWCursor = hwCursor   ? TRUE : FALSE;
  params.bDiffMap      = useDiffMap ? TRUE : FALSE;

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

  return nvfbc->NvFBCToSysSetUp(&params) == NVFBC_SUCCESS;
}

CaptureResult NvFBCToSysCapture(
  NvFBCToSys * nvfbc,
  const unsigned int waitTime,
  const unsigned int x,
  const unsigned int y,
  const unsigned int width,
  const unsigned int height,
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

  NVFBCRESULT status = nvfbc->NvFBCToSysGrabFrame(&params);
  switch(status)
  {
    case NVFBC_SUCCESS:
      break;

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