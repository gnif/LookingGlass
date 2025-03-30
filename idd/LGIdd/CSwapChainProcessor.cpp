/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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
#include "CDebug.h"

CSwapChainProcessor::CSwapChainProcessor(CIndirectDeviceContext* devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, HANDLE newFrameEvent) :
  m_devContext(devContext),
  m_hSwapChain(hSwapChain),
  m_dx11Device(dx11Device),
  m_dx12Device(dx12Device),
  m_newFrameEvent(newFrameEvent)
{
  m_resPool.Init(dx11Device, dx12Device);
  m_fbPool.Init(this);

  m_terminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_thread[0].Attach(CreateThread(nullptr, 0, _SwapChainThread, this, 0, nullptr));
}

CSwapChainProcessor::~CSwapChainProcessor()
{
  SetEvent(m_terminateEvent.Get());
  if (m_thread[0].Get())
    WaitForSingleObject(m_thread[0].Get(), INFINITE);
  if (m_thread[1].Get())
    WaitForSingleObject(m_thread[1].Get(), INFINITE);

  m_resPool.Reset();
  m_fbPool.Reset();
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

  DEBUG_INFO("Start Thread");
  SwapChainThreadCore();

  WdfObjectDelete((WDFOBJECT)m_hSwapChain);
  m_hSwapChain = nullptr;

  AvRevertMmThreadCharacteristics(avTaskHandle);
}

void CSwapChainProcessor::SwapChainThreadCore()
{
  ComPtr<IDXGIDevice> dxgiDevice;
  HRESULT hr = m_dx11Device->GetDevice().As(&dxgiDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to get the dxgiDevice");
    return;
  }

  if (IDD_IS_FUNCTION_AVAILABLE(IddCxSetRealtimeGPUPriority))
  {
    DEBUG_INFO("Using IddCxSetRealtimeGPUPriority");
    IDARG_IN_SETREALTIMEGPUPRIORITY arg;
    arg.pDevice = dxgiDevice.Get();
    hr = IddCxSetRealtimeGPUPriority(m_hSwapChain, &arg);
    if (FAILED(hr))
      DEBUG_ERROR_HR(hr, "Failed to set realtime GPU thread priority");
  }
  else
  {
    DEBUG_INFO("Using SetGPUThreadPriority");
    dxgiDevice->SetGPUThreadPriority(7);
  }

  IDARG_IN_SWAPCHAINSETDEVICE setDevice = {};
  setDevice.pDevice = dxgiDevice.Get();

  hr = IddCxSwapChainSetDevice(m_hSwapChain, &setDevice);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "IddCxSwapChainSetDevice Failed");
    return;
  }

  UINT lastFrameNumber = 0;
  for (;;)
  {
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
      if (buffer.MetaData.PresentationFrameNumber != lastFrameNumber)
      {
        lastFrameNumber = buffer.MetaData.PresentationFrameNumber;
        SwapChainNewFrame(buffer.MetaData.pSurface);

        // report that all GPU processing for this frame has been queued
        hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
        if (FAILED(hr))
        {
          DEBUG_ERROR_HR(hr, "IddCxSwapChainFinishedProcessingFrame Failed");
          break;
        }

      }
    }
    else
    {
      break;
    }
  }
}

void CSwapChainProcessor::CompletionFunction(
  CD3D12CommandQueue * queue, bool result, void * param1, void * param2)
{
  UNREFERENCED_PARAMETER(queue);

  auto sc    = (CSwapChainProcessor *)param1;
  auto fbRes = (CFrameBufferResource*)param2;

  // fail gracefully
  if (!result)
  {
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
    return;
  }

  if (sc->m_dx12Device->IsIndirectCopy())
    sc->m_devContext->WriteFrameBuffer(
      fbRes->GetFrameIndex(),
      fbRes->GetMap(), 0, fbRes->GetFrameSize(), true);
  else
    sc->m_devContext->FinalizeFrameBuffer(fbRes->GetFrameIndex());
}

bool CSwapChainProcessor::SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer)
{
  ComPtr<ID3D11Texture2D> texture;
  HRESULT hr = acquiredBuffer.As(&texture);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the ID3D11Texture2D from the acquiredBuffer");
    return false;
  }

  CInteropResource * srcRes = m_resPool.Get(texture);
  if (!srcRes)
  {
    DEBUG_ERROR("Failed to get a CInteropResource from the pool");
    return false;
  }

  /**
   * Even though we have not performed any copy/draw operations we still need to
   * use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before the desktop duplication API has finished updating it.
   */
  srcRes->Signal();

  //FIXME: handle dirty rects
  srcRes->SetFullDamage();

  D3D12_RESOURCE_DESC desc = srcRes->GetRes()->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
  m_dx12Device->GetDevice()->GetCopyableFootprints(
    &desc,
    0,
    1,
    0,
    &layout,
    NULL, 
    NULL,
    NULL);

  auto buffer = m_devContext->PrepareFrameBuffer(
    (int)desc.Width,
    (int)desc.Height,
    (int)layout.Footprint.RowPitch,
    desc.Format);

  if (!buffer.mem)
    return false;

  CFrameBufferResource * fbRes = m_fbPool.Get(buffer,
    (size_t)layout.Footprint.RowPitch * desc.Height);

  if (!fbRes)
  {
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    return false;
  }

  auto copyQueue = m_dx12Device->GetCopyQueue();
  if (!copyQueue)
  {
    DEBUG_ERROR("Failed to get a CopyQueue");
    return false;
  }

  copyQueue->SetCompletionCallback(&CompletionFunction, this, fbRes);

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource        = srcRes->GetRes().Get();
  srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource       = fbRes->Get().Get();
  dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = layout;

  srcRes->Sync(*copyQueue);
  copyQueue->GetGfxList()->CopyTextureRegion(
    &dstLoc, 0, 0, 0, &srcLoc, NULL);
  copyQueue->Execute();

  return true;
}
