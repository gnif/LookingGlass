#pragma once
#include "ICapture.h"

#define W32_LEAN_AND_MEAN
#include <Windows.h>

#include <NvFBC\nvFBC.h>
#include <NvFBC\nvFBCToSys.h>

namespace Capture
{
  class NvFBC : public ICapture
  {
  public:
    NvFBC();
    ~NvFBC();

    bool Initialize();
    void DeInitialize();
    enum FrameType GetFrameType();
    enum FrameComp GetFrameCompression();
    size_t GetMaxFrameSize();
    bool GrabFrame(void * buffer, size_t bufferSize, size_t * outLen);

  private:
    bool m_initialized;
    HMODULE m_hDLL;

    NvFBC_CreateFunctionExType    m_fnCreateEx;
    NvFBC_SetGlobalFlagsType      m_fnSetGlobalFlags;
    NvFBC_GetStatusExFunctionType m_fnGetStatusEx;
    NvFBC_EnableFunctionType      m_fnEnable;

    DWORD m_maxCaptureWidth, m_maxCaptureHeight;
  };
};