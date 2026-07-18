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
    bool m_computeEnabled = false;

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

    InitResult Init(CIVSHMEM &ivshmem, UINT64 &alignSize,
      bool enableCompute);
    void DeInit();

    // Wait for all command queues to finish in-flight GPU work and run their
    // completion callbacks. Used at swap-chain teardown so no callback touches
    // resources we are about to release.
    void WaitForIdle();

    ComPtr<ID3D12Device3> GetDevice() { return m_device; }
    ComPtr<ID3D12Heap   > GetHeap() { return m_ivshmemHeap; }
    bool IsIndirectCopy() { return m_indirectCopy; }

    CD3D12CommandQueue * GetCopyQueue();
    CD3D12CommandQueue * GetComputeQueue();
};
