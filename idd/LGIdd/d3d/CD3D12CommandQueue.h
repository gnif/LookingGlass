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

#include "Atomic.h"
#include "CSRWLock.h"

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <d3d12.h>
#include <stdint.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

class CD3D12CommandQueue;

class CD3D12CommandSlot
{
  friend class CD3D12CommandQueue;

  private:
    enum State
    {
      STATE_FREE,
      STATE_RECORDING,
      STATE_CANCELLING,
      STATE_SUBMITTED,
      STATE_COMPLETING,
      STATE_FAILED,
    };

    struct FenceWait
    {
      ComPtr<ID3D12Fence> fence;
      UINT64              value = 0;
    };

    static const UINT MAX_FENCE_WAITS = 2;

    const WCHAR          * m_name  = nullptr;
    CD3D12CommandQueue   * m_queue = nullptr;
    ComPtr<ID3D12CommandAllocator   > m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_gfxList;
    ComPtr<ID3D12CommandList        > m_cmdList;

    std::atomic<State>        m_state       = STATE_FREE;
    std::atomic<bool>         m_submitted   = false;
    HandleT<HANDLENullTraits> m_event;
    HandleT<HANDLENullTraits> m_availableEvent;
    HANDLE                    m_waitHandle  = INVALID_HANDLE_VALUE;
    bool                      m_needsReset  = false;
    UINT64                    m_fenceTarget = 0;

    FenceWait m_fenceWaits[MAX_FENCE_WAITS];
    UINT      m_fenceWaitCount = 0;

    typedef void (*CompletionFunction)(CD3D12CommandSlot * slot,
      bool result, void * param1, void * param2);

    CompletionFunction m_completionCallback  = nullptr;
    void             * m_completionParams[2] = {};
    bool               m_completionResult    = true;

    UINT   m_queryBase          = 0;
    bool   m_timingActive       = false;
    UINT64 m_timestampFrequency = 0;
    UINT64 m_calibrationGPU     = 0;
    UINT64 m_calibrationCPU     = 0;

    bool Init(ID3D12Device3 * device, CD3D12CommandQueue * queue,
      const WCHAR * name, UINT queryBase, ULONG waitFlags);
    void DeInit();
    void OnCompletion(bool timeout);

  public:
    enum CallbackMode
    {
      FAST,
      NORMAL,
    };

    ~CD3D12CommandSlot() { DeInit(); }

    bool Acquire();
    void Cancel();
    bool Execute();

    bool BeginTiming();
    void EndTiming();
    bool GetGPUTimes(uint64_t& start, uint64_t& end) const;

    bool WaitFor(ID3D12Fence * fence, UINT64 value);
    bool WaitFor(const CD3D12CommandSlot& slot);

    void SetCompletionCallback(CompletionFunction fn,
      void * param1, void * param2)
    {
      m_completionCallback  = fn;
      m_completionParams[0] = param1;
      m_completionParams[1] = param2;
    }

    bool IsIdle() const
    {
      const State state = Atomic::Load(m_state, std::memory_order_acquire);
      return state == STATE_FREE ||
        (state == STATE_FAILED &&
         !Atomic::Load(m_submitted, std::memory_order_acquire));
    }

    bool HasSubmittedWork() const
    {
      return Atomic::Load(m_submitted, std::memory_order_acquire);
    }

    ComPtr<ID3D12GraphicsCommandList> GetGfxList() { return m_gfxList; }
};

class CD3D12CommandQueue
{
  friend class CD3D12CommandSlot;

  private:
    static const UINT MAX_SLOTS = 2;

    const WCHAR * m_name = nullptr;

    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence       > m_fence;
    UINT64                     m_fenceValue     = 0;
    CSRWLock                   m_submitLock;
    std::atomic<bool>          m_failed         = false;

    CD3D12CommandSlot m_slots[MAX_SLOTS];
    UINT              m_slotCount = 0;

    ComPtr<ID3D12QueryHeap> m_timestampHeap;
    ComPtr<ID3D12Resource > m_timestampReadback;
    UINT64                * m_timestampMap    = nullptr;
    UINT64                  m_qpcFrequency    = 0;
    bool                    m_timingSupported = false;

    bool InitTiming(ID3D12Device3 * device, UINT slotCount);
    bool Submit(CD3D12CommandSlot& slot);
    bool SnapshotTiming(CD3D12CommandSlot& slot) const;
    bool GetGPUTimes(const CD3D12CommandSlot& slot,
      uint64_t& start, uint64_t& end) const;

  public:
    ~CD3D12CommandQueue() { DeInit(); }

    bool Init(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type,
      const WCHAR * name, CD3D12CommandSlot::CallbackMode callbackMode,
      UINT slotCount, bool enableTiming = false);
    void DeInit();

    CD3D12CommandSlot * Acquire(UINT slotIndex);
    CD3D12CommandSlot * Acquire();
    void WaitForIdle();
};
