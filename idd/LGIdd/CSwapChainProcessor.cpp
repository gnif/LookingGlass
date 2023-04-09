#include "CSwapChainProcessor.h"

#include <avrt.h>


CSwapChainProcessor::CSwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent) :
  m_hSwapChain(hSwapChain),
  m_device(device),
  m_newFrameEvent(newFrameEvent)
{
  OutputDebugStringA(__FUNCTION__);
  m_terminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_thread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  OutputDebugStringA(__FUNCTION__);
  SetEvent(m_terminateEvent.Get());
  if (m_thread.Get())
    WaitForSingleObject(m_thread.Get(), INFINITE);
}

DWORD CALLBACK CSwapChainProcessor::RunThread(LPVOID argument)
{
  OutputDebugStringA(__FUNCTION__);
  reinterpret_cast<CSwapChainProcessor*>(argument)->Run();
  return 0;
}

void CSwapChainProcessor::Run()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  RunCore();

  WdfObjectDelete((WDFOBJECT)m_hSwapChain);
  m_hSwapChain = nullptr;

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

void CSwapChainProcessor::RunCore()
{  
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_device->m_device.As(&dxgiDevice);
  if (FAILED(hr))
    return;

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
  if (FAILED(hr))
    return;

  for (;;)
  {
    Microsoft::WRL::ComPtr<IDXGIResource> acquiredBuffer;
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};
    hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);    

    if (hr == E_PENDING)
    {
      HANDLE waitHandles[] =
      {
        m_newFrameEvent,
        m_terminateEvent.Get()
      };
      DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 17);
      if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)
        continue;
      else if (waitResult == WAIT_OBJECT_0 + 1)
        break;
      else
      {
        hr = HRESULT_FROM_WIN32(waitResult);
        break;
      }
    }
    else if (SUCCEEDED(hr))
    {
      //acquiredBuffer.Attach(buffer.MetaData.pSurface);

      //TODO: process the frame

      //acquiredBuffer.Reset();
      hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
      if (FAILED(hr))
        break;
    }
    else
      break;
  }
}