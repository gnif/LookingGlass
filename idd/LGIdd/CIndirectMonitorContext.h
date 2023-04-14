#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>
#include "CIndirectDeviceContext.h"
#include "CSwapChainProcessor.h"

using namespace Microsoft::WRL;

class CIndirectMonitorContext
{
private:
  IDDCX_MONITOR m_monitor;
  CIndirectDeviceContext * m_devContext;
  std::unique_ptr<CSwapChainProcessor> m_swapChain;

  Wrappers::Event m_terminateEvent;
  Wrappers::Event m_cursorDataEvent;
  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread;
  BYTE * m_shapeBuffer;

  DWORD m_lastShapeId = 0;

  static DWORD CALLBACK _CursorThread(LPVOID arg);
  void CursorThread();

public:
  CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device);

  virtual ~CIndirectMonitorContext();
  
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();

  inline void ResendLastFrame()
  {
    if (m_swapChain)
      m_swapChain->ResendLastFrame();
  }
};

struct CIndirectMonitorContextWrapper
{
  CIndirectMonitorContext* context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CIndirectMonitorContextWrapper);