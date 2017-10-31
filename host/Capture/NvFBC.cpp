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
    m_hDLL(NULL)
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

    if (m_fnGetStatusEx(&status) != NVFBC_SUCCESS)
    {
      DEBUG_ERROR("Failed to get NvFBC status");
      DeInitialize();
      return false;
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

    // do not remove this
    #include "NvFBCSpecial.h"

    if (m_fnCreateEx(&params) != NVFBC_SUCCESS)
    {
      DEBUG_ERROR("Failed to create an instance of NvFBC");
      DeInitialize();
      return false;
    }

    m_maxCaptureWidth  = params.dwMaxDisplayWidth;
    m_maxCaptureHeight = params.dwMaxDisplayHeight;


    m_initialized = true;
    return true;
  }

  void NvFBC::DeInitialize()
  {
    m_fnCreateEx       = NULL;
    m_fnSetGlobalFlags = NULL;
    m_fnGetStatusEx    = NULL;
    m_fnEnable         = NULL;

    FreeLibrary(m_hDLL);
    m_hDLL = NULL;

    m_initialized = false;
  }

  FrameType NvFBC::GetFrameType()
  {
    return FRAME_TYPE_RGB;
  }

  FrameComp NvFBC::GetFrameCompression()
  {
    return FRAME_COMP_NONE;
  }

  size_t NvFBC::GetMaxFrameSize()
  {
    return 0;
  }

  bool NvFBC::GrabFrame(void * buffer, size_t bufferSize, size_t * outLen)
  {
    return false;
  }

};