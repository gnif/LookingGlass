#include "CD3D12Device.h"
#include "CDebug.h"

CD3D12Device::CD3D12Device(LUID adapterLuid) :
  m_adapterLuid(adapterLuid),
  m_debug(false),
  m_indirectCopy(false)
{
  if (m_debug)
  {
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&m_dxDebug));
    if (FAILED(hr))
    {
      DBGPRINT_HR(hr, "Failed to get the debug interface");
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

  DBGPRINT("category:%d severity:%d id:%d desc:%s",
    category,
    severity,
    id,
    description);
}

bool CD3D12Device::Init(CIVSHMEM &ivshmem, UINT64 &alignSize)
{
reInit:
  HRESULT hr;

  hr = CreateDXGIFactory2(m_debug ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_factory));  
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the DXGI factory");
    return false;
  }

  hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&m_adapter));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to enumerate the adapter");
    return false;
  }

  hr = D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the DirectX12 device");
    return false;
  }

  if (m_debug)
  {
    hr = m_device.As(&m_infoQueue);
    if (FAILED(hr))
    {
      DBGPRINT_HR(hr, "Failed to get the ID3D12InfoQueue1 interface");
      //non-fatal do not exit
    }
    else
      m_infoQueue->RegisterMessageCallback(
      _D3D12DebugCallback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, NULL, &m_callbackCookie);
  }

  if (!m_indirectCopy)
  {
    hr = m_device->OpenExistingHeapFromAddress(ivshmem.GetMem(), IID_PPV_ARGS(&m_ivshmemHeap));
    if (FAILED(hr))
    {
      DBGPRINT_HR(hr, "Failed to open IVSHMEM as a D3D12Heap");
      return false;
    }
    m_ivshmemHeap->SetName(L"IVSHMEM");
  
    D3D12_HEAP_DESC heapDesc = m_ivshmemHeap->GetDesc();
    alignSize = heapDesc.Alignment;

    // test that the heap is usable
    if (!HeapTest())
    {
      DBGPRINT("Unable to create resources in the IVSHMEM heap, falling back to indirect copy");

      // failure often results in the device being removed and we need to completely reinit when this occurs
      m_indirectCopy = true;
      m_device.Reset();
      m_adapter.Reset();
      m_factory.Reset();
      goto reInit;
    }

    DBGPRINT("Using IVSHMEM as a D3D12Heap");
  }

  if (!m_copyQueue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COPY, L"Copy"))
    return false;

  //if (!m_computeQueue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Compute"))
    //return false;

  DBGPRINT("Created CD3D12Device");
  return true;
}

void CD3D12Device::DeInit()
{
  if (m_debug && m_infoQueue)
    m_infoQueue->UnregisterMessageCallback(m_callbackCookie);
  m_infoQueue.Reset();
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
    DBGPRINT_HR(hr, "Failed to create the ivshmem ID3D12Resource");
    return false;
  }
  resource->SetName(L"HeapTest");

  /* the above may succeed even if there was a fault, as such we also need to check
   * check if the device was removed */
  hr = m_device->GetDeviceRemovedReason();
  if (hr != S_OK)
  {
    DBGPRINT_HR(hr, "Device Removed");
    return false;
  }

  return true;
}
