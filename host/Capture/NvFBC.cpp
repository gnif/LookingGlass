/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#if CONFIG_CAPTURE_NVFBC

#include "NvFBC.h"
using namespace Capture;

#include <string>

#include "common/debug.h"
#include "common/memcpySSE.h"
#include "Util.h"

#ifdef _WIN64
#define NVFBC_LIBRARY_NAME "NvFBC64.dll"
#else
#define NVFBC_LIBRARY_NAME "NvFBC.dll"
#endif

#define MOPT "privData"

NvFBC::NvFBC() :
  m_options(NULL),
  m_optNoCrop(false),
  m_optNoWait(false),
  m_initialized(false),
  m_first(true),
  m_hDLL(NULL),
  m_nvFBC(NULL)
{
}

NvFBC::~NvFBC()
{
}

bool Capture::NvFBC::CanInitialize()
{
  return true;
}

bool NvFBC::Initialize(CaptureOptions * options)
{
  if (m_initialized)
    DeInitialize();

  m_first     = true;
  m_options   = options;
  m_optNoCrop = false;

  uint8_t * privData     = NULL;
  NvU32     privDataSize = 0;

  for (CaptureOptions::const_iterator it = options->cbegin(); it != options->cend(); ++it)
  {
    if (_strcmpi(*it, "nocrop") == 0) { m_optNoCrop = false; continue; }
    if (_strcmpi(*it, "nowait") == 0) { m_optNoWait = true ; continue; }

    if (_strnicmp(*it, MOPT " ", sizeof(MOPT)) == 0)
    {
      std::string value(*it);
      value.erase(0, sizeof(MOPT));

      if (value.empty() || value.length() & 1)
        continue;

      privDataSize = (NvU32)(value.length() / 2);
      privData     = new uint8_t[privDataSize];
      uint8_t *p   = privData;
      for (int i = 0; i < value.length(); i += 2, ++p)
      {
        char hex[3];
        #pragma warning(disable:4996)
        value.copy(hex, 2, i);
        #pragma warning(restore:4996)
        hex[2] = 0;
        *p = (uint8_t)strtoul(hex, NULL, 16);
      }
    }
  }

  std::string nvfbc = Util::GetSystemRoot() + "\\" + NVFBC_LIBRARY_NAME;
  m_hDLL = LoadLibraryA(nvfbc.c_str());
  if (!m_hDLL)
  {
    DEBUG_ERROR("Failed to load the NvFBC library: %d - %s", (int)GetLastError(), nvfbc.c_str());
    return false;
  }

  m_fnCreateEx       = (NvFBC_CreateFunctionExType   )GetProcAddress(m_hDLL, "NvFBC_CreateEx"      );
  m_fnSetGlobalFlags = (NvFBC_SetGlobalFlagsType     )GetProcAddress(m_hDLL, "NvFBC_SetGlobalFlags");
  m_fnGetStatusEx    = (NvFBC_GetStatusExFunctionType)GetProcAddress(m_hDLL, "NvFBC_GetStatusEx"   );
  m_fnEnable         = (NvFBC_EnableFunctionType     )GetProcAddress(m_hDLL, "NvFBC_Enable"        );

  if (!m_fnCreateEx || !m_fnSetGlobalFlags || !m_fnGetStatusEx || !m_fnEnable)
  {
    DEBUG_ERROR("Unable to locate required entry points in %s", nvfbc.c_str());
    DeInitialize();
    return false;
  }

  NvFBCStatusEx status;
  ZeroMemory(&status, sizeof(NvFBCStatusEx));
  status.dwVersion     = NVFBC_STATUS_VER;
  status.dwAdapterIdx  = 0;

  NVFBCRESULT ret = m_fnGetStatusEx(&status);
  if (ret != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to get NvFBC status");
    DeInitialize();
    return false;
  }

  if (!status.bIsCapturePossible)
  {
    DEBUG_INFO("Attempting to enable NvFBC");
    switch(m_fnEnable(NVFBC_STATE_ENABLE))
    {
      case NVFBC_SUCCESS:
        DEBUG_INFO("Success, attempting to get status again");
        if (m_fnGetStatusEx(&status) != NVFBC_SUCCESS)
        {
          DEBUG_ERROR("Failed to get NvFBC status");
          DeInitialize();
          return false;
        }
        break;

      case NVFBC_ERROR_INSUFFICIENT_PRIVILEGES:
        DEBUG_ERROR("Please run once as administrator to enable the NvFBC API");
        DeInitialize();
        return false;

      default:
        DEBUG_ERROR("Unknown failure enabling NvFBC");
        DeInitialize();
        return false;
    }

    if (!status.bIsCapturePossible)
    {
      DEBUG_ERROR("Capture is not possible, unsupported device or driver");
      DeInitialize();
      return false;
    }
  }

  if (!status.bCanCreateNow)
  {
    DEBUG_ERROR("Can not create an instance of NvFBC at this time");
    DeInitialize();
    return false;
  }

  NvFBCCreateParams params;
  ZeroMemory(&params, sizeof(NvFBCCreateParams));
  params.dwVersion         = NVFBC_CREATE_PARAMS_VER;
  params.dwInterfaceType   = NVFBC_TO_SYS;
  params.pDevice           = NULL;
  params.dwAdapterIdx      = 0;
  params.dwPrivateDataSize = privDataSize;
  params.pPrivateData      = privData;

  if (m_fnCreateEx(&params) != NVFBC_SUCCESS)
  {
    if (privData)
      delete [] privData;

    DEBUG_ERROR("Failed to create an instance of NvFBC");
    DeInitialize();
    return false;
  }

  if (privData)
    delete[] privData;

  m_maxCaptureWidth = params.dwMaxDisplayWidth;
  m_maxCaptureHeight = params.dwMaxDisplayHeight;
  m_nvFBC = static_cast<NvFBCToSys *>(params.pNvFBC);

  NVFBC_TOSYS_SETUP_PARAMS setupParams;
  ZeroMemory(&setupParams, sizeof(NVFBC_TOSYS_SETUP_PARAMS));
  setupParams.dwVersion         = NVFBC_TOSYS_SETUP_PARAMS_VER;
  setupParams.eMode             = NVFBC_TOSYS_ARGB;
  setupParams.bWithHWCursor     = TRUE;
  setupParams.bDiffMap          = TRUE;
  setupParams.eDiffMapBlockSize = NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_128X128;
  setupParams.ppBuffer          = (void **)&m_frameBuffer;
  setupParams.ppDiffMap         = (void **)&m_diffMap;

  if (m_nvFBC->NvFBCToSysSetUp(&setupParams) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("NvFBCToSysSetUp Failed");
    DeInitialize();
    return false;
  }

  // this is required according to NVidia sample code
  Sleep(100);

  HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo;
  monitorInfo.cbSize = sizeof(MONITORINFO);
  unsigned int screenWidth, screenHeight;
  GetMonitorInfo(monitor, &monitorInfo);
  screenWidth  = monitorInfo.rcMonitor.right  - monitorInfo.rcMonitor.left;
  screenHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

  ZeroMemory(&m_grabFrameParams, sizeof(NVFBC_TOSYS_GRAB_FRAME_PARAMS));
  ZeroMemory(&m_grabInfo, sizeof(NvFBCFrameGrabInfo));
  m_grabFrameParams.dwVersion           = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;
  m_grabFrameParams.dwFlags             = m_optNoWait ? NVFBC_TOSYS_NOWAIT : NVFBC_TOSYS_WAIT_WITH_TIMEOUT;
  m_grabFrameParams.dwWaitTime          = 1000;
  m_grabFrameParams.eGMode              = NVFBC_TOSYS_SOURCEMODE_CROP;
  m_grabFrameParams.dwStartX            = 0;
  m_grabFrameParams.dwStartY            = 0;
  m_grabFrameParams.dwTargetWidth       = screenWidth;
  m_grabFrameParams.dwTargetHeight      = screenHeight;
  m_grabFrameParams.pNvFBCFrameGrabInfo = &m_grabInfo;

  m_initialized = true;
  return true;
}

void NvFBC::DeInitialize()
{
  m_frameBuffer = NULL;

  if (m_nvFBC)
  {
    m_nvFBC->NvFBCToSysRelease();
    m_nvFBC = NULL;
  }

  m_maxCaptureWidth  = 0;
  m_maxCaptureHeight = 0;
  m_fnCreateEx       = NULL;
  m_fnSetGlobalFlags = NULL;
  m_fnGetStatusEx    = NULL;
  m_fnEnable         = NULL;

  if (m_hDLL)
  {
    FreeLibrary(m_hDLL);
    m_hDLL = NULL;
  }

  m_initialized = false;
}

FrameType NvFBC::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;

  return FRAME_TYPE_BGRA;
}

size_t NvFBC::GetMaxFrameSize()
{
  if (!m_initialized)
    return false;

  return m_maxCaptureWidth * m_maxCaptureHeight * 4;
}

unsigned int Capture::NvFBC::Capture()
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

  for (int i = 0; i < 2; ++i)
  {
    NVFBCRESULT status = m_nvFBC->NvFBCToSysGrabFrame(&m_grabFrameParams);
    if (status == NVFBC_SUCCESS)
    {
      const int diffW = (m_grabInfo.dwWidth + 0x7F) >> 7;
      const int diffH = (m_grabInfo.dwHeight + 0x7F) >> 7;
      bool hasDiff = false;
      for (int y = 0; y < diffH && !hasDiff; ++y)
        for (int x = 0; x < diffW; ++x)
          if (m_diffMap[y * diffW + x])
          {
            hasDiff = true;
            break;
          }

      if (!hasDiff)
      {
        i = 0;
        continue;
      }
      break;
    }
    else
    {
      if (status == NVFBC_ERROR_DYNAMIC_DISABLE)
      {
        DEBUG_ERROR("NvFBC was disabled by someone else");
        return GRAB_STATUS_ERROR;
      }

      if (status == NVFBC_ERROR_INVALIDATED_SESSION)
      {
        DEBUG_WARN("Session was invalidated, attempting to restart");
        return GRAB_STATUS_REINIT;
      }

      if (i == 1)
      {
        DEBUG_ERROR("NvFBCToSysGrabFrame failed");
        return GRAB_STATUS_ERROR;
      }
    }
  }

  // if the capture size doesn't match the screen resolution then re-initialize to avoid
  // copying black/blank areas of the screen
  HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO monitorInfo;
  monitorInfo.cbSize = sizeof(MONITORINFO);
  unsigned int screenWidth, screenHeight;
  GetMonitorInfo(monitor, &monitorInfo);
  screenWidth  = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
  screenHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
  if (m_grabInfo.dwWidth != screenWidth || m_grabInfo.dwHeight != screenHeight)
  {
    DEBUG_INFO("Resolution change detected");
    return GRAB_STATUS_REINIT;
  }

  // turn off the cursor on the first frame as NvFBC is drawing it
  if (m_first)
    return GRAB_STATUS_OK | GRAB_STATUS_FRAME | GRAB_STATUS_CURSOR;
  else
    return GRAB_STATUS_OK | GRAB_STATUS_FRAME;
}

bool Capture::NvFBC::GetCursor(CursorInfo & cursor)
{
  cursor.hasShape = false;
  cursor.hasPos   = false;
  cursor.visible  = false;

  if (m_first)
  {
    m_first = false;
    return true;
  }

  return false;
}

void Capture::NvFBC::FreeCursor()
{
}

GrabStatus Capture::NvFBC::DiscardFrame()
{
  return GrabStatus();
}

enum GrabStatus NvFBC::GetFrame(struct FrameInfo & frame)
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

  frame.width  = m_grabInfo.dwWidth;
  frame.height = m_grabInfo.dwHeight;
  frame.stride = m_grabInfo.dwBufferWidth;
  frame.pitch  = m_grabInfo.dwBufferWidth * 4;

  memcpySSE((uint8_t*)frame.buffer, (uint8_t *)m_frameBuffer, frame.pitch * frame.height);
  return GRAB_STATUS_OK;
}

#endif// CONFIG_CAPTURE_NVFBC
