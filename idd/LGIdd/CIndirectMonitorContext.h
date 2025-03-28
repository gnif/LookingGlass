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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <IddCx.h>

#include <memory>
#include "CIndirectDeviceContext.h"
#include "CSwapChainProcessor.h"

using namespace Microsoft::WRL;

class CIndirectMonitorContext
{
private:
  IDDCX_MONITOR m_monitor;

  std::shared_ptr<CD3D11Device> m_dx11Device;
  std::shared_ptr<CD3D12Device> m_dx12Device;

  CIndirectDeviceContext * m_devContext;
  std::unique_ptr<CSwapChainProcessor> m_swapChain;

  Wrappers::Event m_terminateEvent;
  Wrappers::Event m_cursorDataEvent;
  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread;
  BYTE * m_shapeBuffer;

  DWORD m_lastShapeId = 0;

  static DWORD CALLBACK _CursorThread(LPVOID arg);
  void CursorThread();

public:
  CIndirectMonitorContext(_In_ IDDCX_MONITOR monitor, CIndirectDeviceContext * device);

  virtual ~CIndirectMonitorContext();
  
  void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
  void UnassignSwapChain();

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