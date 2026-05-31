#include "CFrameBufferResource.h"
#include "CSwapChainProcessor.h"
#include "CDebug.h"

bool CFrameBufferResource::Init(CSwapChainProcessor * swapChain, unsigned frameIndex, uint8_t * base, size_t size)
{
  m_frameIndex = frameIndex;

  if (size > swapChain->GetDevice()->GetMaxFrameSize())
  {
    DEBUG_ERROR("Frame size of %lu is too large to fit in available shared ram");
    return false;
  }

  // nothing to do if the resource already exists and is large enough
  if (m_base == base && m_size >= size)
  {
    m_frameSize = size;
    return true;
  }

  Reset();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width              = size;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

  HRESULT hr;
  const WCHAR * resName;

  if (swapChain->GetD3D12Device()->IsIndirectCopy())
  {
    DEBUG_TRACE("Creating standard resource for %p", base);
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                 = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask     = 1;
    heapProps.VisibleNodeMask      = 1;

    hr = swapChain->GetD3D12Device()->GetDevice()->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_COPY_DEST,
      NULL,
      IID_PPV_ARGS(&m_res)
    );
    resName = L"STAGING";

    if (SUCCEEDED(hr))
    {
      D3D12_RANGE range = {0, 0};
      hr = m_res->Map(0, &range, &m_map);
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr, "Failed to map the resource");
        return false;
      }
    }
  }
  else
  {
    DEBUG_TRACE("Creating ivshmem resource for %p", base);
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags     = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

    hr = swapChain->GetD3D12Device()->GetDevice()->CreatePlacedResource(
      swapChain->GetD3D12Device()->GetHeap().Get(),
      (uintptr_t)base - (uintptr_t)swapChain->GetDevice()->GetIVSHMEM().GetMem(),
      &desc,
      D3D12_RESOURCE_STATE_COPY_DEST,
      NULL,
      IID_PPV_ARGS(&m_res)
    );
    resName = L"IVSHMEM";
  }

  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the FrameBuffer ID3D12Resource");
    return false;
  }

  m_res->SetName(resName);

  m_base      = base;
  m_size      = size;
  m_frameSize = size;
  m_valid     = true;
  return true;
}

void CFrameBufferResource::Reset()
{
  if (m_map)
  {
    m_res->Unmap(0, NULL);
    m_map = NULL;
  }

  m_base = nullptr;
  m_size = 0;
  m_res.Reset();
  m_valid = false;
}