#include "CIndirectMonitorContext.h"
#include "Direct3DDevice.h"

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  m_thread.reset();
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  m_thread.reset();
  auto device = std::make_shared<Direct3DDevice>(renderAdapter);
  if (FAILED(device->Init()))
  {
    WdfObjectDelete(swapChain);
    return;
  }

  m_thread.reset(new CSwapChainProcessor(m_devContext, swapChain, device, newFrameEvent));
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  m_thread.reset();
}