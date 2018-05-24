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

NvFBC::NvFBC() :
  m_options(NULL),
  m_optNoCrop(false),
  m_optNoWait(false),
  m_initialized(false),
  m_hDLL(NULL),
  m_nvFBC(NULL)
{
}

NvFBC::~NvFBC()
{
}

bool NvFBC::Initialize(CaptureOptions * options)
{
  if (m_initialized)
    DeInitialize();

  m_options = options;
  m_optNoCrop = false;
  for (CaptureOptions::const_iterator it = options->cbegin(); it != options->cend(); ++it)
  {
    if (_strcmpi(*it, "nocrop") == 0) { m_optNoCrop = false; continue; }
    if (_strcmpi(*it, "nowait") == 0) { m_optNoWait = true ; continue; }
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
  status.dwVersion    = NVFBC_STATUS_VER;
  status.dwAdapterIdx = 0;

  NVFBCRESULT ret = m_fnGetStatusEx(&status);
  if (ret != NVFBC_SUCCESS)
  {
    DEBUG_INFO("Attempting to enable NvFBC");
    if (m_fnEnable(NVFBC_STATE_ENABLE) == NVFBC_SUCCESS)
    {
      DEBUG_INFO("Success, attempting to get status again");
      ret = m_fnGetStatusEx(&status);
    }

    if (ret != NVFBC_SUCCESS)
    {
      DEBUG_ERROR("Failed to get NvFBC status");
      DeInitialize();
      return false;
    }
  }


  if (!status.bIsCapturePossible)
  {
    DEBUG_ERROR("Capture is not possible, unsupported device or driver");
    DeInitialize();
    return false;
  }

  if (!status.bCanCreateNow)
  {
    DEBUG_ERROR("Can not create an instance of NvFBC at this time");
    DeInitialize();
    return false;
  }

  NvFBCCreateParams params;
  ZeroMemory(&params, sizeof(NvFBCCreateParams));
  params.dwVersion       = NVFBC_CREATE_PARAMS_VER;
  params.dwInterfaceType = NVFBC_TO_SYS;
  params.pDevice         = NULL;
  params.dwAdapterIdx    = 0;

  if (m_fnCreateEx(&params) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("Failed to create an instance of NvFBC");
    DeInitialize();
    return false;
  }

  m_maxCaptureWidth = params.dwMaxDisplayWidth;
  m_maxCaptureHeight = params.dwMaxDisplayHeight;
  m_nvFBC = static_cast<NvFBCToSys *>(params.pNvFBC);

  NVFBC_TOSYS_SETUP_PARAMS setupParams;
  ZeroMemory(&setupParams, sizeof(NVFBC_TOSYS_SETUP_PARAMS));
  setupParams.dwVersion = NVFBC_TOSYS_SETUP_PARAMS_VER;
  setupParams.eMode = NVFBC_TOSYS_ARGB;
  setupParams.bWithHWCursor = TRUE;
  setupParams.bDiffMap = TRUE;
  setupParams.eDiffMapBlockSize = (NvU32)NVFBC_TOSYS_DIFFMAP_BLOCKSIZE_128X128;
  setupParams.ppBuffer = (void **)&m_frameBuffer;
  setupParams.ppDiffMap = (void **)&m_diffMap;

  if (m_nvFBC->NvFBCToSysSetUp(&setupParams) != NVFBC_SUCCESS)
  {
    DEBUG_ERROR("NvFBCToSysSetUp Failed");
    DeInitialize();
    return false;
  }

  // this is required according to NVidia sample code
  Sleep(100);

  ZeroMemory(&m_grabFrameParams, sizeof(NVFBC_TOSYS_GRAB_FRAME_PARAMS));
  ZeroMemory(&m_grabInfo, sizeof(NvFBCFrameGrabInfo));
  m_grabFrameParams.dwVersion = NVFBC_TOSYS_GRAB_FRAME_PARAMS_VER;
  m_grabFrameParams.dwFlags = m_optNoWait ? NVFBC_TOSYS_NOWAIT : NVFBC_TOSYS_WAIT_WITH_TIMEOUT;
  m_grabFrameParams.dwWaitTime = 1000;
  m_grabFrameParams.eGMode = NVFBC_TOSYS_SOURCEMODE_FULL;
  m_grabFrameParams.dwStartX = 0;
  m_grabFrameParams.dwStartY = 0;
  m_grabFrameParams.dwTargetWidth = 0;
  m_grabFrameParams.dwTargetHeight = 0;
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

  m_maxCaptureWidth = 0;
  m_maxCaptureHeight = 0;
  m_fnCreateEx = NULL;
  m_fnSetGlobalFlags = NULL;
  m_fnGetStatusEx = NULL;
  m_fnEnable = NULL;

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

  return FRAME_TYPE_ARGB;
}

size_t NvFBC::GetMaxFrameSize()
{
  if (!m_initialized)
    return false;

  return m_maxCaptureWidth * m_maxCaptureHeight * 4;
}

enum GrabStatus NvFBC::GrabFrame(struct FrameInfo & frame, struct CursorInfo & cursor)
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

  for(int i = 0; i < 2; ++i)
  {
    NVFBCRESULT status  = m_nvFBC->NvFBCToSysGrabFrame(&m_grabFrameParams);

    if (status == NVFBC_SUCCESS)
    {
      const int diffW = (m_grabInfo.dwWidth  + 0x7F) >> 7;
      const int diffH = (m_grabInfo.dwHeight + 0x7F) >> 7;
      bool hasDiff = false;
      for(int y = 0; y < diffH && !hasDiff; ++y)
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

      unsigned int dataWidth;
      unsigned int dataOffset;

      if (m_optNoCrop)
      {
        dataWidth    = m_grabInfo.dwWidth * 4;
        dataOffset   = 0;
        frame.width  = m_grabInfo.dwWidth;
        frame.height = m_grabInfo.dwHeight;
      }
      else
      {
        // get the actual resolution
        HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo;
        monitorInfo.cbSize = sizeof(MONITORINFO);

        unsigned int screenWidth, screenHeight;
        if (!GetMonitorInfo(monitor, &monitorInfo))
        {
          DEBUG_WARN("Failed to get monitor dimensions, assuming no cropping required");
          screenWidth  = m_grabInfo.dwWidth;
          screenHeight = m_grabInfo.dwHeight;;
        }
        else
        {
          screenWidth  = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
          screenHeight = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
        }

        // use the smaller or the two dimensions provided just to be sure we don't overflow the buffer
        const unsigned int realHeight = LG_MIN(m_grabInfo.dwHeight, screenHeight);
        const unsigned int realWidth  = LG_MIN(m_grabInfo.dwWidth , screenWidth );

        // calculate the new data width and offset to the start of the data
        dataWidth  = realWidth * 4;
        dataOffset =
          (((m_grabInfo.dwHeight - realHeight) >> 1)  * m_grabInfo.dwBufferWidth +
            ((m_grabInfo.dwWidth  - realWidth ) >> 1)) * 4;

        // update the frame size
        frame.width  = realWidth;
        frame.height = realHeight;
      }

      frame.stride = frame.width;
      frame.pitch  = dataWidth;
      uint8_t *src = (uint8_t *)m_frameBuffer + dataOffset;
      uint8_t *dst = (uint8_t *)frame.buffer;
      for(unsigned int y = 0; y < frame.height; ++y, dst += dataWidth, src += m_grabInfo.dwBufferWidth * 4)
        memcpySSE(dst, src, dataWidth);

      return GRAB_STATUS_OK;
    }

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
  }

  DEBUG_ERROR("Failed to grab frame");
  return GRAB_STATUS_ERROR;
}

#endif// CONFIG_CAPTURE_NVFBC
