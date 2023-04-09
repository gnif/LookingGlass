#pragma once

#include "Direct3DDevice.h"

#include <Windows.h>
#include <wrl.h>
#include <IddCx.h>
#include <memory>

class CSwapChainProcessor
{
private:
  IDDCX_SWAPCHAIN                 m_hSwapChain;
  std::shared_ptr<Direct3DDevice> m_device;
  HANDLE                          m_newFrameEvent;

  Microsoft::WRL::Wrappers::HandleT<
    Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_thread;
  Microsoft::WRL::Wrappers::Event  m_terminateEvent;

  static DWORD CALLBACK RunThread(LPVOID argument);

  void Run();
  void RunCore();

public:
  CSwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
  ~CSwapChainProcessor();
};