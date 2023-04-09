#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>
#include "CSwapChainProcessor.h"

class CIndirectMonitorContext
{
protected:
  IDDCX_MONITOR m_monitor;
  std::unique_ptr<CSwapChainProcessor> m_thread;

public:
  CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor);

  virtual ~CIndirectMonitorContext();
  
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();
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