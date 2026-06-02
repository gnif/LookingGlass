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
#include <d3d12.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace Microsoft::WRL::Wrappers::HandleTraits;

class CD3D12CommandQueue
{
  private:
    const WCHAR * m_name = nullptr;

    ComPtr<ID3D12CommandQueue       > m_queue;
    ComPtr<ID3D12CommandAllocator   > m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_gfxList;
    ComPtr<ID3D12CommandList        > m_cmdList;
    ComPtr<ID3D12Fence              > m_fence;

    bool m_pending = false;
    HandleT<HANDLENullTraits> m_event;
    HANDLE m_waitHandle = INVALID_HANDLE_VALUE;
    UINT64 m_fenceValue = 0;
    bool m_needsReset = false;

    typedef void (*CompletionFunction)(CD3D12CommandQueue * queue,
      bool result, void * param1, void * param2);

    CompletionFunction   m_completionCallback = nullptr;
    void               * m_completionParams[2];
    bool                 m_completionResult = true;

    void OnCompletion()
    {
      if (m_completionCallback)
        m_completionCallback(
          this,
          m_completionResult,
          m_completionParams[0],
          m_completionParams[1]);
      m_pending = false;
    }

  public:
    ~CD3D12CommandQueue() { DeInit(); }

    enum CallbackMode
    {
      DISABLED, // no callbacks
      FAST,     // callback is expected to return almost immediately
      NORMAL    // normal callback
    };

    bool Init(ID3D12Device3 * device, D3D12_COMMAND_LIST_TYPE type, const WCHAR * name,
      CallbackMode callbackMode = DISABLED);

    void DeInit();

    void SetCompletionCallback(CompletionFunction fn, void * param1, void * param2)
    {
      m_completionCallback  = fn;
      m_completionParams[0] = param1;
      m_completionParams[1] = param2;
    }

    bool Reset();
    bool Execute();

    //void Wait();
    bool   IsReady () const { return !m_pending   ; }
    HANDLE GetEvent() const { return m_event.Get(); }

    ComPtr<ID3D12CommandQueue       > GetCmdQueue() { return m_queue;   }
    ComPtr<ID3D12GraphicsCommandList> GetGfxList()  { return m_gfxList; }
};
