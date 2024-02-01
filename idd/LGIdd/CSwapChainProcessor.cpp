/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "CSwapChainProcessor.h"

#include <avrt.h>
#include "Debug.h"

#define LOCK(lock) \
  while (InterlockedCompareExchange((volatile LONG*)&(lock), 1, 0) != 0) {};

#define UNLOCK(lock) \
  InterlockedExchange((volatile LONG*)&(lock), 0);

#define LOCK_CONTEXT()   LOCK(m_contextLock);
#define UNLOCK_CONTEXT() UNLOCK(m_contextLock);
#define LOCK_ST(st)      LOCK((st).lock);
#define UNLOCK_ST(st)    UNLOCK((st).lock);

CSwapChainProcessor::CSwapChainProcessor(CIndirectDeviceContext* devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent) :
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_device(device),
  m_newFrameEvent(newFrameEvent)
{  
  m_terminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_thread[0].Attach(CreateThread(nullptr, 0, _SwapChainThread, this, 0, nullptr));
  m_thread[1].Attach(CreateThread(nullptr, 0, _FrameThread    , this, 0, nullptr));
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  SetEvent(m_terminateEvent.Get());
  if (m_thread[0].Get())
    WaitForSingleObject(m_thread[0].Get(), INFINITE);
  if (m_thread[1].Get())
    WaitForSingleObject(m_thread[1].Get(), INFINITE);

  for(int i = 0; i < STAGING_TEXTURES; ++i)
    if (m_cpuTex[i].map.pData)
    {
      m_device->m_context->Unmap(m_cpuTex[i].tex.Get(), 0);
      m_cpuTex[i].map.pData = nullptr;
    }
}

DWORD CALLBACK CSwapChainProcessor::_SwapChainThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->SwapChainThread();
  return 0;
}

void CSwapChainProcessor::SwapChainThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);

  DBGPRINT("Start");
  SwapChainThreadCore();

  WdfObjectDelete((WDFOBJECT)m_hSwapChain);
  m_hSwapChain = nullptr;

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

void CSwapChainProcessor::SwapChainThreadCore()
{  
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_device->m_device.As(&dxgiDevice);
  if (FAILED(hr))
  {
    DBGPRINT("Failed to get the dxgiDevice");
    return;
  }

  if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
  {
    DBGPRINT("Using IddCxSetRealtimeGPUPriority");
    IDARG_IN_SETREALTIMEGPUPRIORITY arg;
    arg.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSetRealtimeGPUPriority(m_hSwapChain, &arg)))
      DBGPRINT("Failed to set realtime GPU thread priority");
  }
  else
  {
    DBGPRINT("Using SetGPUThreadPriority");
    dxgiDevice->SetGPUThreadPriority(7);
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  LOCK_CONTEXT();
  hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
  UNLOCK_CONTEXT();

  if (FAILED(hr))
  {
    DBGPRINT("IddCxSwapChainSetDevice Failed (%08x)", hr);
    return;
  }

  UINT lastFrameNumber = 0;
  for (;;)
  {
    ComPtr<IDXGIResource> acquiredBuffer;
    IDARG_OUT_RELEASEANDACQUIREBUFFER buffer = {};

    LOCK_CONTEXT();
    hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);

    if (hr == E_PENDING)
    {
      UNLOCK_CONTEXT();
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
      if (buffer.MetaData.PresentationFrameNumber != lastFrameNumber)
      {
        lastFrameNumber = buffer.MetaData.PresentationFrameNumber;
        if (m_copyCount < STAGING_TEXTURES)
        {
          acquiredBuffer.Attach(buffer.MetaData.pSurface);
          SwapChainNewFrame(acquiredBuffer);
          acquiredBuffer.Reset();
        }        
      }

      hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
      UNLOCK_CONTEXT();
      if (FAILED(hr))
        break;
    }
    else
    {
      UNLOCK_CONTEXT();
      break;
    }
  }
}

void CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer)
{
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(acquiredBuffer->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture)))
  {
    DBGPRINT("Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    return;
  }

  D3D11_TEXTURE2D_DESC desc;
  texture->GetDesc(&desc);

  StagingTexture &st = m_cpuTex[m_texWIndex];
  if (st.map.pData)
  {
    m_device->m_context->Unmap(st.tex.Get(), 0);
    st.map.pData = nullptr;
  }

  if (!SetupStagingTexture(st, desc.Width, desc.Height, desc.Format))
    return;

  m_device->m_context->CopyResource(st.tex.Get(), texture.Get());

  InterlockedAdd(&m_copyCount, 1);
  if (++m_texWIndex == STAGING_TEXTURES)
    m_texWIndex = 0;
}

DWORD CALLBACK CSwapChainProcessor::_FrameThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor*>(arg)->FrameThread();
  return 0;
}

void CSwapChainProcessor::FrameThread()
{
  for(;;)
  {
    if (WaitForSingleObject(m_terminateEvent.Get(), 0) == WAIT_OBJECT_0)
      break;

    if (!m_copyCount)
    {
      Sleep(0);
      continue;
    }

    StagingTexture & st = m_cpuTex[m_texRIndex];
    LOCK(st);

    LOCK_CONTEXT();
    HRESULT status = m_device->m_context->Map(st.tex.Get(), 0, D3D11_MAP_READ,
      D3D11_MAP_FLAG_DO_NOT_WAIT, &st.map);
    UNLOCK_CONTEXT();

    if (FAILED(status))
    {
      UNLOCK(st);
      if (status == DXGI_ERROR_WAS_STILL_DRAWING)
        continue;

      DBGPRINT("Failed to map staging texture");

      InterlockedAdd(&m_copyCount, -1);
      if (++m_texRIndex == STAGING_TEXTURES)
        m_texRIndex = 0;

      continue;
    }

    m_devContext->SendFrame(st.width, st.height, st.map.RowPitch, st.format, st.map.pData);
    InterlockedAdd(&m_copyCount, -1);
    m_lastIndex = m_texRIndex;
    if (++m_texRIndex == STAGING_TEXTURES)
      m_texRIndex = 0;

    UNLOCK(st);
  }
}

bool CSwapChainProcessor::SetupStagingTexture(StagingTexture & st, int width, int height, DXGI_FORMAT format)
{
  if (st.width == width && st.height == height && st.format == format)
    return true;

  st.tex.Reset();
  st.width  = width;
  st.height = height;
  st.format = format;

  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width              = width;
  desc.Height             = height;
  desc.MipLevels          = 1;
  desc.ArraySize          = 1;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Usage              = D3D11_USAGE_STAGING;
  desc.Format             = format;
  desc.BindFlags          = 0;
  desc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags          = 0;

  if (FAILED(m_device->m_device->CreateTexture2D(&desc, nullptr, &st.tex)))
  {
    DBGPRINT("Failed to create staging texture");
    return false;
  }

  return true;
}

void CSwapChainProcessor::ResendLastFrame()
{
  LOCK_CONTEXT()
  StagingTexture & st = m_cpuTex[m_lastIndex];
  LOCK_ST(st);
  UNLOCK_CONTEXT();

  if (!st.map.pData)
  {
    UNLOCK_ST(st);
    return;
  }

  m_devContext->SendFrame(st.width, st.height, st.map.RowPitch, st.format, st.map.pData);
  UNLOCK_ST(st);
}