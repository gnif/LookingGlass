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

#include "capture/CFrameProcessor.h"

class CHardwareFrameProcessor final : public CFrameProcessor
{
private:
  enum CandidateState
  {
    CANDIDATE_FREE,
    CANDIDATE_PREPARING,
    CANDIDATE_READY,
    CANDIDATE_PUBLISHING,
  };

  struct FrameCandidate
  {
    CandidateState         state               = CANDIDATE_FREE;
    ComPtr<ID3D12Resource> resource;
    D12FrameFormat         srcFormat           = {};
    D12FrameFormat         dstFormat           = {};
    RECT                   dirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned               nbDirtyRects        = 0;
    unsigned               pitch               = 0;
    size_t                 frameSize           = 0;
    uint64_t               sequence            = 0;
    uint64_t               captureTime         = 0;
    uint64_t               postProcessStart    = 0;
    uint64_t               prepareCopyStart    = 0;
    uint64_t               prepareReady        = 0;
    uint64_t               prepareGPUStart     = 0;
    uint64_t               prepareGPUEnd       = 0;
    uint64_t               timingStart         = 0;
    unsigned               timingEffectIndex   = 0;
    uint64_t               timingToken         = 0;
    bool                   prepareTimingValid  = false;
  };

  struct CandidateDamageTail
  {
    uint64_t ownerSequence = 0;
    RECT     dirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned nbDirtyRects  = 0;
    bool     hasDamage     = false;
    bool     active        = false;
  };

  FrameCandidate      m_candidates[LGMP_Q_FRAME_LEN];
  CandidateDamageTail m_candidateDamageTail[LGMP_Q_FRAME_LEN];
  mutable SRWLOCK     m_candidateLock      = SRWLOCK_INIT;
  SRWLOCK             m_copySubmitLock     = SRWLOCK_INIT;
  uint64_t            m_candidateSequence  = 0;
  bool                m_publishPending     = false;
  Wrappers::Event     m_candidateAvailableEvent;
  Wrappers::Event     m_copySubmitEvent;

  static void CandidateCompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);
  static void CompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);
  int AcquireCandidate(bool exclusiveSample, bool allowSupersede);
  void ReleaseCandidate(unsigned candidateIndex);
  bool EnsureCandidateResource(unsigned candidateIndex, size_t frameSize);
  void ResetCandidates();
  void SignalCandidateState();
  bool ExecuteCandidateCopy(CD3D12CommandSlot * copySlot);
  void AccumulateDamageLocked(
    const RECT dirtyRects[], unsigned count) override;
  void SetFullDamageLocked() override;

public:
  CHardwareFrameProcessor(CFrameTransport * transport,
    std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[LGMP_Q_FRAME_LEN],
    SRWLOCK * pipelineLock, HANDLE terminateEvent);

  bool IsValid() const override;
  bool Submit(const FrameSubmission& submission) override;
  bool HasReadyFrame() const override;
  bool Publish(const CFrameScheduler::Schedule& schedule,
    bool periodic, uint64_t publishStart) override;
  bool UsesCadence() const override { return true; }
  void Reset() override;
  void ResetPipeline() override;
};
