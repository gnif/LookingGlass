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

#include "d3d/CD3D11Device.h"
#include "d3d/CD3D12Device.h"
#include "display/IddCxCompat.h"
#include "d3d/CInteropResourcePool.h"
#include "capture/CFrameProcessor.h"
#include "common/KVMFR.h"
#include "postprocess/CPostProcessor.h"

#include <Windows.h>
#include <wrl.h>
#include <atomic>
#include <memory>

using namespace Microsoft::WRL;

class CMonitorContext;
class CDeviceContext;
class CFrameTransport;
class CLGMPControl;

class CSwapChainProcessor
{
private:
  CMonitorContext                * m_monitorContext;
  UINT64                           m_assignmentGeneration;
  IDDCX_MONITOR                    m_monitor;
  CDeviceContext                 * m_devContext;
  CFrameTransport                & m_transport;
  CLGMPControl                   & m_control;
  IDDCX_SWAPCHAIN                  m_hSwapChain;
  LUID                             m_renderAdapter;
  std::shared_ptr<CD3D11Device>    m_dx11Device;
  std::shared_ptr<CD3D12Device>    m_dx12Device;
  HANDLE                           m_newFrameEvent;

  CInteropResourcePool             m_resPool;
  CPostProcessor                   m_postProcessors[LGMP_Q_FRAME_LEN];
  std::unique_ptr<CFrameProcessor> m_frameProcessor;
  // Reconfiguration is exclusive while per-candidate recording is shared.
  SRWLOCK                          m_pipelineLock = SRWLOCK_INIT;

  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread[3];
  Wrappers::Event m_terminateEvent;
  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_publishTimer;

  Wrappers::Event m_cursorDataEvent;
  BYTE*           m_shapeBuffer;
  DWORD           m_lastShapeId = 0;
  std::atomic<UINT> m_sdrWhiteLevel { KVMFR_SDR_WHITE_LEVEL_DEFAULT };

#ifdef HAS_IDDCX_110
  // The per-frame metadata stream can select the monitor default, provide a
  // replacement block, or retain the selection from the previous frame.
  bool                 m_useDefaultHDRMetadata = true;
  bool                 m_hasNewHDRMetadata     = false;
  IDDCX_HDR10_METADATA m_newHDRMetadata        = {};
#endif

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  void SwapChainThread();
  void SwapChainThreadCore();
  bool InitializePipeline();

  static DWORD CALLBACK _PublisherThread(LPVOID arg);
  void PublisherThread();
  static DWORD CALLBACK _CursorThread(LPVOID arg);
  bool QueryHWCursor();
  void CursorThread();

#ifdef HAS_IDDCX_110
  void UpdateHDRMetadata(const IDDCX_METADATA2& metadata);
#endif
  bool GetContentHDRMetadata(D12FrameFormat& format) const;
  bool SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer,
    unsigned dirtyRectCount, unsigned moveRegionCount,
    DXGI_COLOR_SPACE_TYPE colorSpace, UINT sdrWhiteLevel,
    uint64_t captureStart, bool duplicateFrame);

public:
  CSwapChainProcessor(CMonitorContext * monitorContext,
    UINT64 assignmentGeneration, IDDCX_MONITOR monitor,
    CDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    LUID renderAdapter, std::shared_ptr<CD3D11Device> dx11Device,
    HANDLE newFrameEvent);
  ~CSwapChainProcessor();
  bool Start();
};
