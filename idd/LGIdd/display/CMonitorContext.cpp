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

#include "display/CMonitorContext.h"
#include "display/CDeviceContext.h"
#include "capture/CSwapChainProcessor.h"
#include "d3d/CD3D11Device.h"
#include "CDebug.h"

CMonitorContext::CMonitorContext(
    _In_ IDDCX_MONITOR monitor, CDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
}

CMonitorContext::~CMonitorContext()
{
  UnassignSwapChain();
  m_devContext->OnMonitorDestroyed(m_monitor);
}

NTSTATUS CMonitorContext::AssignSwapChain(
  IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  std::lock_guard<std::mutex> assignGuard(m_assignMutex);

  // Finish tearing down the previous assignment before reserving a generation
  // for the new one. Deleting the old processor can itself cause IddCx to
  // re-enter UnassignSwapChain, and that old callback must happen before the
  // new generation is established.
  DetachSwapChain();

  const UINT64 assignmentGeneration =
    m_assignmentGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;

  // Build the D3D11 device into a local so the member is never observed
  // half-constructed. The worker binds it before performing the expensive
  // D3D12, transport and post-processing initialization.
  auto dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
  const HRESULT initStatus = dx11Device->Init();
  if (FAILED(initStatus))
  {
    DEBUG_ERROR_HR(initStatus, "Failed to initialize D3D11 device");
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }

  AcquireSRWLockExclusive(&m_lock);
  if (!IsAssignmentCurrent(assignmentGeneration))
  {
    ReleaseSRWLockExclusive(&m_lock);
    DEBUG_INFO("Swap chain assignment canceled before processor startup");
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }

  // Publish the assignment atomically with starting its worker. An unassign
  // now blocks on m_lock until m_swapChain exists, at which point it can
  // signal and join the processor normally.
  m_devContext->OnSwapChainAssigned();
  m_dx11Device = std::move(dx11Device);
  m_swapChain.reset(new CSwapChainProcessor(
    this, assignmentGeneration, m_monitor, m_devContext, swapChain,
    renderAdapter, m_dx11Device, newFrameEvent));
  if (!m_swapChain->Start())
  {
    auto processor = std::move(m_swapChain);
    dx11Device      = std::move(m_dx11Device);
    ReleaseSRWLockExclusive(&m_lock);
    processor.reset();
    dx11Device.reset();
    m_devContext->OnSwapChainReleased();
    return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
  }
  ReleaseSRWLockExclusive(&m_lock);
  return STATUS_SUCCESS;
}

void CMonitorContext::DetachSwapChain()
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

  AcquireSRWLockExclusive(&m_lock);
  processor  = std::move(m_swapChain);
  dx11Device = std::move(m_dx11Device);
  ReleaseSRWLockExclusive(&m_lock);

  const bool hadSwapChain = !!processor;
  processor.reset();
  dx11Device.reset();

  if (hadSwapChain)
    m_devContext->OnSwapChainReleased();
}

void CMonitorContext::UnassignSwapChain()
{
  DetachSwapChain();
}
