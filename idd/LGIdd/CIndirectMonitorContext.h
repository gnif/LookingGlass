#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>
#include "CIndirectDeviceContext.h"
#include "CSwapChainProcessor.h"

class CIndirectMonitorContext
{
protected:
  IDDCX_MONITOR m_monitor;
  CIndirectDeviceContext * m_devContext;
  std::unique_ptr<CSwapChainProcessor> m_thread;

public:
  CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device);

  virtual ~CIndirectMonitorContext();
  
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();

  inline void ResendLastFrame()
  {
    if (m_thread)
      m_thread->ResendLastFrame();
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