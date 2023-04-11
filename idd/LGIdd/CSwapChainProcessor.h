#pragma once

#include "Direct3DDevice.h"
#include "CIndirectDeviceContext.h"

#include <Windows.h>
#include <wrl.h>
#include <IddCx.h>
#include <memory>

using namespace Microsoft::WRL;

#define STAGING_TEXTURES 3

class CSwapChainProcessor
{
private:
  CIndirectDeviceContext        * m_devContext;
  IDDCX_SWAPCHAIN                 m_hSwapChain;
  std::shared_ptr<Direct3DDevice> m_device;
  HANDLE                          m_newFrameEvent;

  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread[2];
  Wrappers::Event m_terminateEvent;

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  static DWORD CALLBACK _FrameThread(LPVOID arg);

  void SwapChainThread();
  void SwapChainThreadCore();
  void SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer);

  void FrameThread();

  struct StagingTexture
  {
    int         width;
    int         height;
    DXGI_FORMAT format;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  };

  StagingTexture m_cpuTex[STAGING_TEXTURES] = {};
  volatile LONG m_copyCount   = 0;
  volatile LONG m_contextLock = 0;
  int m_texRIndex = 0;
  int m_texWIndex = 0;

  bool SetupStagingTexture(StagingTexture & t, int width, int height, DXGI_FORMAT format);

public:
  CSwapChainProcessor(CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
  ~CSwapChainProcessor();
};