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
#include <IddCx.h>

#include <atomic>
#include <memory>
#include <mutex>
#include "CIndirectDeviceContext.h"
#include "CSwapChainProcessor.h"

using namespace Microsoft::WRL;

class CIndirectMonitorContext
{
private:
  IDDCX_MONITOR m_monitor;

  // Guards the swap chain and device pointers. Assign and unassign can run
  // concurrently (an unassign triggered by the worker's WdfObjectDelete can
  // race the next assign), and shared_ptr copy/reset is not thread safe.
  SRWLOCK m_lock = SRWLOCK_INIT;

  // IddCx can issue a replacement assignment before an earlier assignment
  // has finished creating its devices. Serialize those expensive setup paths
  // while still allowing UnassignSwapChain to cancel the active one.
  std::mutex m_assignMutex;
  std::shared_ptr<CD3D11Device> m_dx11Device;
  std::shared_ptr<CD3D12Device> m_dx12Device;

  CIndirectDeviceContext * m_devContext;
  std::unique_ptr<CSwapChainProcessor> m_swapChain;

  // Incremented whenever the current assignment is replaced or unassigned.
  // Device creation is performed outside m_lock, so this lets an unassign
  // cancel that work before a processor is started on a stale swap chain.
  std::atomic<UINT64> m_assignmentGeneration = 0;

  void DetachSwapChain();

public:
  CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device);

  virtual ~CIndirectMonitorContext();
  
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();
  bool IsAssignmentCurrent(UINT64 generation) const
  {
    return m_assignmentGeneration.load(std::memory_order_acquire) == generation;
  }

  CIndirectDeviceContext * GetDeviceContext() { return m_devContext; }
};

struct CIndirectMonitorContextWrapper
{
  CIndirectMonitorContext* context;

  void Cleanup()
  {
    delete context;
    context = nullptr;
  }
};

WDF_DECLARE_CONTEXT_TYPE(CIndirectMonitorContextWrapper);
