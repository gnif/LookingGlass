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

#include "CIndirectMonitorContext.h"
#include "CPlatformInfo.h"
#include "CDebug.h"
#include "CPipeServer.h"

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  UnassignSwapChain();
  m_devContext->OnMonitorDestroyed(m_monitor);
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  std::lock_guard<std::mutex> assignGuard(m_assignMutex);

  // Finish tearing down the previous assignment before reserving a generation
  // for the new one. Deleting the old processor can itself cause IddCx to
  // re-enter UnassignSwapChain, and that old callback must happen before the
  // new generation is established.
  DetachSwapChain();

  const UINT64 assignmentGeneration =
    m_assignmentGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

  // Build the devices into locals so the members are never observed
  // half-constructed and the expensive initialization stays outside m_lock.
  std::shared_ptr<CD3D11Device> dx11Device;
  std::shared_ptr<CD3D12Device> dx12Device;

  for (;;)
  {
    dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
    if (FAILED(dx11Device->Init()))
    {
      WdfObjectDelete(swapChain);
      return;
    }

    UINT64 alignSize = CPlatformInfo::GetPageSize();
    dx12Device = std::make_shared<CD3D12Device>(renderAdapter);
    CD3D12Device::InitResult r = dx12Device->Init(
      m_devContext->GetIVSHMEM(), alignSize, !dx11Device->IsSoftware());
    if (r == CD3D12Device::RETRY)
    {
      dx12Device.reset();
      dx11Device.reset();
      continue;
    }
    if (r == CD3D12Device::FAILURE)
    {
      WdfObjectDelete(swapChain);
      return;
    }

    if (!m_devContext->SetupLGMP(alignSize))
    {
      WdfObjectDelete(swapChain);
      DEBUG_ERROR("SetupLGMP failed");
      return;
    }
    break;
  }

  AcquireSRWLockExclusive(&m_lock);
  if (!IsAssignmentCurrent(assignmentGeneration))
  {
    ReleaseSRWLockExclusive(&m_lock);
    DEBUG_INFO("Swap chain assignment canceled before processor startup");
    return;
  }

  // Publish the assignment atomically with starting its worker. An unassign
  // now blocks on m_lock until m_swapChain exists, at which point it can
  // signal and join the processor normally.
  m_devContext->OnSwapChainAssigned();
  m_dx11Device = std::move(dx11Device);
  m_dx12Device = std::move(dx12Device);
  m_swapChain.reset(new CSwapChainProcessor(
    this, assignmentGeneration, m_monitor, m_devContext, swapChain,
    m_dx11Device, m_dx12Device, newFrameEvent));
  ReleaseSRWLockExclusive(&m_lock);
}

void CIndirectMonitorContext::DetachSwapChain()
{
  // Invalidate setup in progress before waiting for m_lock. This also lets a
  // worker about to call SetDevice observe an unassign whose callback is
  // blocked waiting for the processor to be published.
  m_assignmentGeneration.fetch_add(1, std::memory_order_acq_rel);

  // Detach under the lock, then destroy outside it. Destroying the processor
  // joins its worker thread, whose teardown (WdfObjectDelete) re-enters this
  // method on another thread - holding the lock across that would deadlock.
  std::unique_ptr<CSwapChainProcessor> processor;
  std::shared_ptr<CD3D11Device>        dx11Device;
  std::shared_ptr<CD3D12Device>        dx12Device;

  AcquireSRWLockExclusive(&m_lock);
  processor  = std::move(m_swapChain);
  dx11Device = std::move(m_dx11Device);
  dx12Device = std::move(m_dx12Device);
  ReleaseSRWLockExclusive(&m_lock);

  const bool hadSwapChain = !!processor;
  processor.reset();
  dx11Device.reset();
  dx12Device.reset();

  if (hadSwapChain)
    m_devContext->OnSwapChainReleased();
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  DetachSwapChain();
}
