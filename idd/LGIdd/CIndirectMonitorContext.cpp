#include "CIndirectMonitorContext.h"
#include "Direct3DDevice.h"

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor) :
  m_monitor(monitor)
{
  OutputDebugStringA(__FUNCTION__);
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  OutputDebugStringA(__FUNCTION__);
  m_thread.reset();
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  OutputDebugStringA(__FUNCTION__);
  m_thread.reset();
  auto device = std::make_shared<Direct3DDevice>(renderAdapter);
  if (FAILED(device->Init()))
  {
    WdfObjectDelete(swapChain);
    return;
  }

  m_thread.reset(new CSwapChainProcessor(swapChain, device, newFrameEvent));
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  OutputDebugStringA(__FUNCTION__);
  m_thread.reset();
}