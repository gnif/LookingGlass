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

#include "CD3D11Device.h"
#include "CD3D12Device.h"
#include "CIndirectDeviceContext.h"
#include "CInteropResourcePool.h"
#include "CFrameBufferPool.h"
#include "CPostProcessor.h"

#include <Windows.h>
#include <wrl.h>
#include <IddCx.h>
#include <atomic>
#include <memory>

using namespace Microsoft::WRL;

#define STAGING_TEXTURES 3

class CIndirectMonitorContext;

class CSwapChainProcessor
{
private:
  CIndirectMonitorContext         * m_monitorContext;
  UINT64                            m_assignmentGeneration;
  IDDCX_MONITOR                   m_monitor;
  CIndirectDeviceContext        * m_devContext;
  IDDCX_SWAPCHAIN                 m_hSwapChain;
  std::shared_ptr<CD3D11Device>   m_dx11Device;
  std::shared_ptr<CD3D12Device>   m_dx12Device;
  HANDLE                          m_newFrameEvent;

  CInteropResourcePool m_resPool;
  CFrameBufferPool     m_fbPool;
  CPostProcessor       m_postProcessors[LGMP_Q_FRAME_LEN];

  enum CandidateState
  {
    CANDIDATE_FREE,
    CANDIDATE_PREPARING,
    CANDIDATE_READY,
    CANDIDATE_PUBLISHING,
    CANDIDATE_HELD,
  };

  struct FrameCandidate
  {
    CandidateState         state             = CANDIDATE_FREE;
    ComPtr<ID3D12Resource> resource;
    D12FrameFormat         srcFormat          = {};
    D12FrameFormat         dstFormat          = {};
    RECT                   dirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned               nbDirtyRects      = 0;
    unsigned               pitch             = 0;
    size_t                 frameSize         = 0;
    uint64_t               sequence          = 0;
    uint64_t               damageGeneration  = 0;
    uint64_t               captureTime       = 0;
    uint64_t               postProcessStart  = 0;
    uint64_t               prepareCopyStart  = 0;
    uint64_t               prepareReady      = 0;
    uint64_t               prepareGPUStart   = 0;
    uint64_t               prepareGPUEnd     = 0;
    unsigned               timingEffectIndex = 0;
    uint64_t               timingToken       = 0;
    bool                   prepareTimingValid = false;
  };

  FrameCandidate m_candidates[LGMP_Q_FRAME_LEN];
  SRWLOCK        m_candidateLock    = SRWLOCK_INIT;
  SRWLOCK        m_damageLock       = SRWLOCK_INIT;
  SRWLOCK        m_pipelineLock     = SRWLOCK_INIT;
  uint64_t       m_candidateSequence = 0;
  uint64_t       m_damageGeneration  = 0;

  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_thread[3];
  Wrappers::Event m_terminateEvent;
  Wrappers::Event m_candidateEvent;
  Wrappers::Event m_candidateAvailableEvent;
  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> m_publishTimer;

  Wrappers::Event m_cursorDataEvent;
  BYTE*           m_shapeBuffer;
  DWORD           m_lastShapeId = 0;
  std::atomic<UINT> m_sdrWhiteLevel { KVMFR_SDR_WHITE_LEVEL_DEFAULT };

  // Logical output-space damage from the previous published frame. The
  // shared-memory frame buffers alternate, so this must be copied along with
  // the current damage to bring the older target buffer up to date.
  RECT     m_dirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned m_nbDirtyRects = 0;

  // Source-space damage accumulated since the last published frame. Frames
  // can be dropped while the LGMP queue is full, but their damage must be
  // included in the next frame sent to the client. A count of zero represents
  // full-frame damage when m_hasPendingDamage is set.
  RECT     m_pendingDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned m_nbPendingDirtyRects = 0;
  bool     m_hasPendingDamage = true;

#ifdef HAS_IDDCX_110
  // The per-frame metadata stream can select the monitor default, provide a
  // replacement block, or retain the selection from the previous frame.
  bool                 m_useDefaultHDRMetadata = true;
  bool                 m_hasNewHDRMetadata     = false;
  IDDCX_HDR10_METADATA m_newHDRMetadata        = {};
#endif

  static DWORD CALLBACK _SwapChainThread(LPVOID arg);
  void SwapChainThread();
  bool SwapChainThreadCore();

  static DWORD CALLBACK _PublisherThread(LPVOID arg);
  void PublisherThread();
  bool PublishNewestCandidate(
    uint32_t scheduleGeneration, bool periodic, uint64_t publishStart);
  bool HasReadyCandidate();
  int AcquireCandidate();
  void ReleaseCandidate(unsigned candidateIndex);
  bool EnsureCandidateResource(unsigned candidateIndex,
    ID3D12Resource * source);
  void ResetCandidates();
  void SignalCandidateState();

  static DWORD CALLBACK _CursorThread(LPVOID arg);
  bool QueryHWCursor();
  void CursorThread();

  static void CompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);
  static void CandidateCompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);
  void AccumulateFrameDamage(const RECT * dirtyRects, unsigned nbDirtyRects);
  void SetFullPendingDamage();
#ifdef HAS_IDDCX_110
  void UpdateHDRMetadata(const IDDCX_METADATA2& metadata);
#endif
  bool GetContentHDRMetadata(D12FrameFormat& format) const;
  bool SwapChainNewFrame(ComPtr<IDXGIResource> acquiredBuffer,
    unsigned dirtyRectCount, unsigned moveRegionCount,
    DXGI_COLOR_SPACE_TYPE colorSpace, UINT sdrWhiteLevel,
    uint64_t captureStart);

public:
  CSwapChainProcessor(CIndirectMonitorContext * monitorContext, UINT64 assignmentGeneration,
    IDDCX_MONITOR monitor, CIndirectDeviceContext * devContext, IDDCX_SWAPCHAIN hSwapChain,
    std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, HANDLE newFrameEvent);
  ~CSwapChainProcessor();

  CIndirectDeviceContext * GetDevice() { return m_devContext; }
  std::shared_ptr<CD3D12Device> GetD3D12Device() { return m_dx12Device; }
};
