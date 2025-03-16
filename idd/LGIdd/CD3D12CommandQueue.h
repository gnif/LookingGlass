#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <d3d12.h>

using namespace Microsoft::WRL;

class CD3D12CommandQueue
{
  private:
    const WCHAR * m_name = nullptr;

    ComPtr<ID3D12CommandQueue       > m_queue;
    ComPtr<ID3D12CommandAllocator   > m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_gfxList;
    ComPtr<ID3D12CommandList        > m_cmdList;
    ComPtr<ID3D12Fence              > m_fence;

    Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_event;
    UINT64 m_fenceValue = 0;

  public:
    bool Init(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type, const WCHAR * name);

    bool Execute();

    void Wait();

    bool Reset();

    ComPtr<ID3D12CommandQueue       > GetCmdQueue() { return m_queue;   }
    ComPtr<ID3D12GraphicsCommandList> GetGfxList()  { return m_gfxList; }
};

