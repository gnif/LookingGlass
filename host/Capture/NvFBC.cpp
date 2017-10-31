#include "NvFBC.h"

#include <string>

#include "common\debug.h"
#include "Util.h"

#ifdef _WIN64
#define NVFBC_LIBRARY_NAME "NvFBC64.dll"
#else
#define NVFBC_LIBRARY_NAME "NvFBC.dll"
#endif

namespace Capture
{

  NvFBC::NvFBC() :
    m_initialized(false),
    m_hDLL(NULL),
    m_nvFBC(NULL)
  {
  }

  NvFBC::~NvFBC()
  {
  }

  bool NvFBC::Initialize()
  {
    if (m_initialized)
      DeInitialize();

    std::string nvfbc = Util::GetSystemRoot() + "\\" + NVFBC_LIBRARY_NAME;
    m_hDLL = LoadLibraryA(nvfbc.c_str());
    if (!m_hDLL)
    {
      DEBUG_ERROR("Failed to load the NvFBC library: %d - %s", GetLastError(), nvfbc.c_str());
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
    setupParams.eMode = NVFBC_TOSYS_RGB;
    setupParams.bWithHWCursor = TRUE;
    setupParams.bDiffMap = FALSE;
    setupParams.ppBuffer = (void **)&m_frameBuffer;
    setupParams.ppDiffMap = NULL;

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
    m_grabFrameParams.dwFlags = NVFBC_TOSYS_NOFLAGS;
    m_grabFrameParams.dwStartX = 0;
    m_grabFrameParams.dwStartY = 0;
    m_grabFrameParams.eGMode = NVFBC_TOSYS_SOURCEMODE_SCALE;
    m_grabFrameParams.pNvFBCFrameGrabInfo = &m_grabInfo;

    if (!m_memcpy.Initialize())
    {
      DEBUG_ERROR("Failed to initialize MTMemcpy");
      DeInitialize();
      return false;
    }

    m_initialized = true;
    return true;
  }

  void NvFBC::DeInitialize()
  {
    m_memcpy.DeInitialize();

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

    return FRAME_TYPE_RGB;
  }

  FrameComp NvFBC::GetFrameCompression()
  {
    if (!m_initialized)
      return FRAME_COMP_NONE;

    return FRAME_COMP_NONE;
  }

  size_t NvFBC::GetMaxFrameSize()
  {
    if (!m_initialized)
      return false;

    return m_maxCaptureWidth * m_maxCaptureHeight * 3;
  }

  bool NvFBC::GrabFrame(struct FrameInfo & frame)
  {
    if (!m_initialized)
      return false;

    const HWND hDesktop = GetDesktopWindow();
    RECT desktop;
    GetWindowRect(hDesktop, &desktop);

    m_grabFrameParams.dwTargetWidth  = desktop.right;
    m_grabFrameParams.dwTargetHeight = desktop.bottom;
    for(int i = 0; i < 2; ++i)
    {
      NVFBCRESULT status = m_nvFBC->NvFBCToSysGrabFrame(&m_grabFrameParams);
      if (status == NVFBC_SUCCESS)
      {
        frame.width   = m_grabInfo.dwWidth;
        frame.height  = m_grabInfo.dwHeight;
        frame.stride  = m_grabInfo.dwBufferWidth;
        frame.outSize = m_grabInfo.dwBufferWidth * m_grabInfo.dwHeight * 3;
        if (!m_memcpy.Copy(frame.buffer, m_frameBuffer, frame.outSize))
        {
          DEBUG_ERROR("Memory copy failed");
          return false;
        }
        return true;
      }

      if (status == NVFBC_ERROR_DYNAMIC_DISABLE)
      {
        DEBUG_ERROR("NvFBC was disabled by someone else");
        return false;
      }

      if (status == NVFBC_ERROR_INVALIDATED_SESSION)
      {
        DEBUG_WARN("Session was invalidated, attempting to restart");
        DeInitialize();
        if (!Initialize())
        {
          DEBUG_ERROR("Failed to re-iniaialize");
          return false;
        }
      }
    }

    DEBUG_ERROR("Failed to grab frame");
    return false;
  }

};