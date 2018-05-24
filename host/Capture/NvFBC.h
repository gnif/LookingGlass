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

#pragma once
#include "ICapture.h"

#define W32_LEAN_AND_MEAN
#include <windows.h>

#include <NvFBC/nvFBC.h>
#include <NvFBC/nvFBCToSys.h>

namespace Capture
{
  class NvFBC : public ICapture
  {
  public:
    NvFBC();
    ~NvFBC();

    const char * GetName() { return "NvFBC"; }

    bool Initialize(CaptureOptions * options);
    void DeInitialize();
    bool ReInitialize()
    {
      DeInitialize();
      return Initialize(m_options);
    }
    enum FrameType GetFrameType();
    size_t GetMaxFrameSize();
    enum GrabStatus GrabFrame(struct FrameInfo & frame, struct CursorInfo & cursor);

  private:
    CaptureOptions * m_options;
    bool m_optNoCrop;
    bool m_optNoWait;

    bool m_initialized;
    HMODULE  m_hDLL;

    NvFBC_CreateFunctionExType    m_fnCreateEx;
    NvFBC_SetGlobalFlagsType      m_fnSetGlobalFlags;
    NvFBC_GetStatusExFunctionType m_fnGetStatusEx;
    NvFBC_EnableFunctionType      m_fnEnable;

    DWORD m_maxCaptureWidth, m_maxCaptureHeight;
    NvFBCToSys * m_nvFBC;
    uint8_t * m_frameBuffer;
    uint8_t * m_diffMap;
    NvFBCFrameGrabInfo m_grabInfo;
    NVFBC_TOSYS_GRAB_FRAME_PARAMS m_grabFrameParams;
  };
};

#endif //CONFIG_CAPTURE_NVFBC
