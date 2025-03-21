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
  m_terminateEvent .Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_cursorDataEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_shapeBuffer = new BYTE[512 * 512 * 4];
}

CIndirectMonitorContext::~CIndirectMonitorContext()
{
  m_swapChain.reset();
  SetEvent(m_terminateEvent.Get());
  delete[] m_shapeBuffer;
}

void CIndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent)
{
  m_swapChain.reset();
  m_dx11Device = std::make_shared<CD3D11Device>(renderAdapter);
  if (FAILED(m_dx11Device->Init()))
  {
    WdfObjectDelete(swapChain);
    return;
  }

  UINT64 alignSize = CPlatformInfo::GetPageSize();
  m_dx12Device = std::make_shared<CD3D12Device>(renderAdapter);
  if (!m_dx12Device->Init(m_devContext->GetIVSHMEM(), alignSize))
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

  IDARG_IN_SETUP_HWCURSOR c = {};
  c.CursorInfo.Size                  = sizeof(c.CursorInfo);
  c.CursorInfo.AlphaCursorSupport    = TRUE;
  c.CursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
  c.CursorInfo.MaxX                  = 512;
  c.CursorInfo.MaxY                  = 512;
  c.hNewCursorDataAvailable          = m_cursorDataEvent.Get();
  NTSTATUS status = IddCxMonitorSetupHardwareCursor(m_monitor, &c);
  if (!NT_SUCCESS(status))
  {
    WdfObjectDelete(swapChain);
    DEBUG_ERROR_HR(status, "IddCxMonitorSetupHardwareCursor Failed");
    return;
  }

  m_swapChain.reset(new CSwapChainProcessor(m_devContext, swapChain, m_dx11Device, m_dx12Device, newFrameEvent));
  m_thread.Attach(CreateThread(nullptr, 0, _CursorThread, this, 0, nullptr));
}

void CIndirectMonitorContext::UnassignSwapChain()
{
  m_swapChain.reset();  
  m_dx11Device.reset();
  m_dx12Device.reset();
}

DWORD CALLBACK CIndirectMonitorContext::_CursorThread(LPVOID arg)
{
  reinterpret_cast<CIndirectMonitorContext*>(arg)->CursorThread();
  return 0;
}

void CIndirectMonitorContext::CursorThread()
{
  HRESULT hr = 0;

  for (;;)
  {
    HANDLE waitHandles[] =
    {
      m_cursorDataEvent.Get(),
      m_terminateEvent.Get()
    };
    DWORD waitResult = WaitForMultipleObjects(ARRAYSIZE(waitHandles), waitHandles, FALSE, 100);
    if (waitResult == WAIT_TIMEOUT)
      continue;
    else if (waitResult == WAIT_OBJECT_0 + 1)
      break;
    else if (waitResult != WAIT_OBJECT_0)
    {
      hr = HRESULT_FROM_WIN32(waitResult);
      DEBUG_ERROR_HR(hr, "WaitForMultipleObjects");
      return;
    }

    IDARG_IN_QUERY_HWCURSOR in  = {};
    in.LastShapeId            = m_lastShapeId;
    in.pShapeBuffer           = m_shapeBuffer;
    in.ShapeBufferSizeInBytes = 512 * 512 * 4;

    IDARG_OUT_QUERY_HWCURSOR out = {};
    NTSTATUS status = IddCxMonitorQueryHardwareCursor(m_monitor, &in, &out);
    if (FAILED(status))
    {
      DEBUG_ERROR_HR(status, "IddCxMonitorQueryHardwareCursor failed");
      return;
    }

    m_devContext->SendCursor(out, m_shapeBuffer);
  }
}