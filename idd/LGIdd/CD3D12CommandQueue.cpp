#include "CD3D12CommandQueue.h"
#include "CDebug.h"

bool CD3D12CommandQueue::Init(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type, const WCHAR* name)
{
  HRESULT hr;
  D3D12_COMMAND_QUEUE_DESC queueDesc = {};

  queueDesc.Type     = type;
  queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

  hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the CommandQueue (%ls)", name);
    return false;
  }
  m_queue->SetName(name);

  hr = device->CreateCommandAllocator(type, IID_PPV_ARGS(&m_allocator));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the CommandAllocator (%ls)", name);
    return false;
  }

  hr = device->CreateCommandList(0, type, m_allocator.Get(), NULL, IID_PPV_ARGS(&m_gfxList));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the Graphics CommandList (%ls)", name);
    return false;
  }
  m_gfxList->SetName(name);

  m_cmdList = m_gfxList;  
  if (!m_cmdList)
  {
    DBGPRINT_HR(hr, "Failed to get the CommandList (%ls)", name);
    return false;
  }

  hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to create the Fence (%ls)", name);
    return false;
  }

  m_event.Attach(CreateEvent(NULL, FALSE, FALSE, NULL));
  if (m_event.Get() == INVALID_HANDLE_VALUE)
  {
    DBGPRINT_HR(GetLastError(), "Failed to create the completion event (%ls)", name);
    return false;
  }

  m_name       = name;
  m_fenceValue = 0;
  DBGPRINT("Created CD3D12CommandQueue(%ls)", name);
  return true;
}

bool CD3D12CommandQueue::Execute()
{
  HRESULT hr = m_gfxList->Close();
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to close the command list (%ls)", m_name);
    return false;
  }

  ID3D12CommandList * lists[] = { m_cmdList.Get() };
  m_queue->ExecuteCommandLists(1, lists);

  m_queue->Signal(m_fence.Get(), ++m_fenceValue);
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to set the fence signal (%ls)", m_name);
    return false;
  }

  return true;
}

void CD3D12CommandQueue::Wait()
{
  if (m_fence->GetCompletedValue() >= m_fenceValue)
    return;

  m_fence->SetEventOnCompletion(m_fenceValue, m_event.Get());
  WaitForSingleObject(m_event.Get(), INFINITE);
}

bool CD3D12CommandQueue::Reset()
{
  HRESULT hr;
  
  hr = m_allocator->Reset();
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to reset the command allocator (%ls)", m_name);
    return false;
  }

  hr = m_gfxList->Reset(m_allocator.Get(), NULL);
  if (FAILED(hr))
  {
    DBGPRINT_HR(hr, "Failed to reset the graphics command list (%ls)", m_name);
    return false;
  }

  return true;
}


