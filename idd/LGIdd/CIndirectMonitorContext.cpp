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
  UnassignSwapChain();

  // Build everything into locals so the members are never observed
  // half-constructed by a concurrent unassign, and so the processor (which
  // spawns a worker thread) is created outside the lock.
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
    CD3D12Device::InitResult r = dx12Device->Init(m_devContext->GetIVSHMEM(), alignSize);
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

  std::unique_ptr<CSwapChainProcessor> processor(new CSwapChainProcessor(
    m_monitor, m_devContext, swapChain, dx11Device, dx12Device, newFrameEvent));

  AcquireSRWLockExclusive(&m_lock);
  m_dx11Device = std::move(dx11Device);
  m_dx12Device = std::move(dx12Device);
  m_swapChain  = std::move(processor);
  ReleaseSRWLockExclusive(&m_lock);
}

void CIndirectMonitorContext::UnassignSwapChain()
{
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

  processor.reset();
  dx11Device.reset();
  dx12Device.reset();
}