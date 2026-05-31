/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

CIndirectMonitorContext::CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device) :
  m_monitor(monitor),
  m_devContext(device)
{
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  UnassignSwapChain();
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
reInit:
  UnassignSwapChain();

  m_dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
  if (FAILED(m_dx11Device->Init()))
  {
    WdfObjectDelete(swapChain);
    return;
  }

  UINT64 alignSize = CPlatformInfo::GetPageSize();
  m_dx12Device = std::make_shared<CD3D12Device>(renderAdapter);
  switch (m_dx12Device->Init(m_devContext->GetIVSHMEM(), alignSize))
  {
    case CD3D12Device::SUCCESS:
      break;

    case CD3D12Device::FAILURE:
      WdfObjectDelete(swapChain);
      return;

    case CD3D12Device::RETRY:
      m_dx12Device.reset();
      m_dx11Device.reset();
      goto reInit;
  }
  
  if (!m_devContext->SetupLGMP(alignSize))
  {
    WdfObjectDelete(swapChain);
    DEBUG_ERROR("SetupLGMP failed");
    return;
  }

  m_swapChain.reset(new CSwapChainProcessor(m_monitor, m_devContext, swapChain, m_dx11Device, m_dx12Device, newFrameEvent));
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  m_swapChain.reset();  
  m_dx11Device.reset();
  m_dx12Device.reset();
}