/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "CD3D12Device.h"
#include "CDebug.h"

bool CD3D12Device::m_indirectCopy = false;

CD3D12Device::CD3D12Device(LUID adapterLuid) :
  m_adapterLuid(adapterLuid),
  m_debug(false)
{
  if (m_debug)
  {
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&m_dxDebug));
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to get the debug interface");
      return;
    }

    m_dxDebug->EnableDebugLayer();
    m_dxDebug->SetEnableGPUBasedValidation(TRUE);
    m_dxDebug->SetEnableSynchronizedCommandQueueValidation(TRUE);
    m_dxDebug->SetForceLegacyBarrierValidation(TRUE);
  }
}

static void CALLBACK _D3D12DebugCallback(
  D3D12_MESSAGE_CATEGORY category,
  D3D12_MESSAGE_SEVERITY severity,
  D3D12_MESSAGE_ID       id,
  LPCSTR                 description,
  void                  *context
)
{
  (void)context;

  DEBUG_INFO("category:%d severity:%d id:%d desc:%s",
    category,
    severity,
    id,
    description);
}

CD3D12Device::InitResult CD3D12Device::Init(CIVSHMEM &ivshmem,
  UINT64 &alignSize, bool enableCompute)
{
  HRESULT hr;

  m_computeEnabled = enableCompute;

  hr = CreateDXGIFactory2(m_debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_factory));  
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the DXGI factory");
    return InitResult::FAILURE;
  }

  hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&m_adapter));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to enumerate the adapter");
    return InitResult::FAILURE;
  }

  hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the DirectX12 device");
    return InitResult::FAILURE;
  }

  if (m_debug)
  {
    hr = m_device.As(&m_infoQueue);
    if (FAILED(hr))
    {
      DEBUG_WARN_HR(hr, "Failed to get the ID3D12InfoQueue1 interface");
      //non-fatal do not exit
    }
    else
    {
      m_infoQueue->RegisterMessageCallback(
        _D3D12DebugCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &m_callbackCookie);
    }
  }

  if (!m_indirectCopy)
  {
    hr = m_device->OpenExistingHeapFromAddress(ivshmem.GetMem(), IID_PPV_ARGS(&m_ivshmemHeap));
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to open IVSHMEM as a D3D12Heap");
      m_indirectCopy = true;
      return InitResult::RETRY;
    }
    m_ivshmemHeap->SetName(L"IVSHMEM");
  
    D3D12_HEAP_DESC heapDesc = m_ivshmemHeap->GetDesc();
    alignSize = heapDesc.Alignment;
    m_ivshmemTextureSupported =
      (heapDesc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) &&
      !(heapDesc.Flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES);

    // test that the heap is usable
    if (!HeapTest())
    {
      DEBUG_WARN("Unable to create resources in the IVSHMEM heap, falling back to indirect copy");

      // failure often results in the device being removed and we need to completely reinit when this occurs
      m_indirectCopy = true;
      return InitResult::RETRY;
    }

    DEBUG_INFO("Using IVSHMEM as a D3D12Heap");
    if (!m_ivshmemTextureSupported)
      DEBUG_WARN("IVSHMEM heap does not support placed textures");
  }

  if (!m_copyQueue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COPY,
      L"Copy", m_indirectCopy ? CD3D12CommandSlot::NORMAL :
                               CD3D12CommandSlot::FAST, 2, true))
    return InitResult::FAILURE;

  if (m_computeEnabled &&
      !m_computeQueue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE,
        L"Compute", CD3D12CommandSlot::FAST, 2))
    return InitResult::FAILURE;

  DEBUG_INFO("Created CD3D12Device");
  return InitResult::SUCCESS;
}

void CD3D12Device::DeInit()
{
  if (m_debug && m_infoQueue)
    m_infoQueue->UnregisterMessageCallback(m_callbackCookie);
  m_infoQueue.Reset();
}

void CD3D12Device::WaitForIdle()
{
  m_copyQueue.WaitForIdle();
  if (m_computeEnabled)
    m_computeQueue.WaitForIdle();
}

bool CD3D12Device::HeapTest()
{
  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Alignment          = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  desc.Width              = 1048576;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

  HRESULT hr;

  ComPtr<ID3D12Resource> resource;
  hr = m_device->CreatePlacedResource(
    m_ivshmemHeap.Get(),
    0,
    &desc,
    D3D12_RESOURCE_STATE_COPY_DEST,
    NULL,
    IID_PPV_ARGS(&resource));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the ivshmem ID3D12Resource");
    return false;
  }
  resource->SetName(L"HeapTest");

  /* the above may succeed even if there was a fault, as such we also need to check
   * check if the device was removed */
  hr = m_device->GetDeviceRemovedReason();
  if (hr != S_OK)
  {
    DEBUG_ERROR_HR(hr, "Device Removed");
    return false;
  }

  return true;
}

CD3D12CommandSlot * CD3D12Device::GetCopySlot(unsigned frameIndex)
{
  return m_copyQueue.Acquire(frameIndex);
}

CD3D12CommandSlot * CD3D12Device::GetCopySlot()
{
  return m_copyQueue.Acquire();
}

CD3D12CommandSlot * CD3D12Device::GetComputeSlot(unsigned frameIndex)
{
  if (!m_computeEnabled)
  {
    DEBUG_ERROR("Compute queue requested while compute processing is disabled");
    return nullptr;
  }

  return m_computeQueue.Acquire(frameIndex);
}
