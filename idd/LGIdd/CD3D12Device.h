#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <d3d12.h>

#include "CIVSHMEM.h"
#include "CD3D12CommandQueue.h"

using namespace Microsoft::WRL;

struct CD3D12Device
{
  private:
    LUID m_adapterLuid;
    bool m_debug;

    // static as this needs to persist if set
    static bool m_indirectCopy;

    ComPtr<ID3D12Debug6    > m_dxDebug;
    ComPtr<ID3D12InfoQueue1> m_infoQueue;
    DWORD                    m_callbackCookie;

    ComPtr<IDXGIFactory5> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device3> m_device;
    ComPtr<ID3D12Heap   > m_ivshmemHeap;

    CD3D12CommandQueue m_copyQueue[4];
    unsigned           m_copyQueueIndex = 0;
    CD3D12CommandQueue m_computeQueue;

    bool HeapTest();

  public:
    CD3D12Device(LUID adapterLUID);
    ~CD3D12Device() { DeInit(); }

    enum InitResult
    {
      RETRY,
      FAILURE,
      SUCCESS
    };

    InitResult Init(CIVSHMEM &ivshmem, UINT64 &alignSize);
    void DeInit();

    ComPtr<ID3D12Device3> GetDevice() { return m_device; }
    ComPtr<ID3D12Heap   > GetHeap() { return m_ivshmemHeap; }
    bool IsIndirectCopy() { return m_indirectCopy; }

    CD3D12CommandQueue * GetCopyQueue();
    CD3D12CommandQueue & GetComputeQueue() { return m_computeQueue; }
};