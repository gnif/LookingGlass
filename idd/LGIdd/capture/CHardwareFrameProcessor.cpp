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

#include "capture/CHardwareFrameProcessor.h"
#include "capture/CFrameProcessorUtil.h"
#include "transport/IFrameTransport.h"
#include "CSRWLock.h"
#include "CDebug.h"

#include <cstring>
#include <utility>

using namespace Microsoft::WRL;

static_assert(CAPTURE_PIPELINE_SLOTS == 2,
  "IDD candidate pipeline assumes two slots");

class CPublishPending
{
private:
  CSRWLock& m_lock;
  bool    * m_pending;
  HANDLE    m_event;
  bool      m_active = true;

public:
  CPublishPending(CSRWLock& stateLock, bool * pending, HANDLE event) :
    m_lock(stateLock),
    m_pending(pending),
    m_event(event)
  {
    CSRWExclusiveLock guard(m_lock);
    *m_pending = true;
    ResetEvent(m_event);
  }

  ~CPublishPending()
  {
    Clear();
  }

  void Clear()
  {
    if (!m_active)
      return;

    {
      CSRWExclusiveLock lock(m_lock);
      *m_pending = false;
      SetEvent(m_event);
    }
    m_active = false;
  }
};

CHardwareFrameProcessor::CHardwareFrameProcessor(
    IFrameTransport * transport, std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[CAPTURE_PIPELINE_SLOTS],
    CSRWLock * pipelineLock, HANDLE terminateEvent) :
  CFrameProcessor(transport, std::move(dx12), postProcessors,
    pipelineLock, terminateEvent)
{
  m_candidateAvailableEvent.Attach(
    CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_copySubmitEvent.Attach(CreateEvent(nullptr, TRUE, TRUE, nullptr));
}

bool CHardwareFrameProcessor::IsValid() const
{
  return CFrameProcessor::IsValid() &&
    m_candidateAvailableEvent.Get() && m_copySubmitEvent.Get();
}

void CHardwareFrameProcessor::SignalCandidateState()
{
  SetEvent(m_readyEvent.Get());
  SetEvent(m_candidateAvailableEvent.Get());
}

void CHardwareFrameProcessor::SetFullDamageLocked()
{
  for (CandidateDamageTail& tail : m_candidateDamageTail)
    if (tail.active)
    {
      tail.hasDamage    = true;
      tail.nbDirtyRects = 0;
    }
}

void CHardwareFrameProcessor::AccumulateDamageLocked(
  const RECT dirtyRects[], unsigned count)
{
  for (CandidateDamageTail& tail : m_candidateDamageTail)
    if (tail.active)
      CFrameProcessorUtil::AccumulateDamage(
        tail.dirtyRects, &tail.nbDirtyRects, &tail.hasDamage,
        dirtyRects, count);
}

void CHardwareFrameProcessor::ResetCandidates()
{
  {
    CSRWExclusiveLock lock(m_candidateLock);
    for (FrameCandidate& candidate : m_candidates)
      candidate = {};
  }

  {
    CSRWExclusiveLock lock(m_damageLock);
    for (CandidateDamageTail& tail : m_candidateDamageTail)
      tail = {};
  }
  SignalCandidateState();
}

void CHardwareFrameProcessor::Reset()
{
  ResetCandidates();
  CFrameProcessor::Reset();
}

void CHardwareFrameProcessor::ResetPipeline()
{
  ResetCandidates();
  CFrameProcessor::Invalidate();
}

bool CHardwareFrameProcessor::HasReadyFrame() const
{
  bool ready = false;
  CSRWSharedLock lock(m_candidateLock);
  for (const FrameCandidate& candidate : m_candidates)
    if (candidate.state == CANDIDATE_READY)
    {
      ready = true;
      break;
    }
  return ready;
}

int CHardwareFrameProcessor::AcquireCandidate(
  bool exclusiveSample, bool allowSupersede)
{
  int      selected   = -1;
  uint64_t oldest     = UINT64_MAX;
  bool     superseded = false;
  bool     idle       = true;
  bool     publishing = false;

  {
    CSRWExclusiveLock lock(m_candidateLock);
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
    {
      if (m_candidates[i].state != CANDIDATE_FREE)
      {
        idle = false;
        if (m_candidates[i].state == CANDIDATE_PUBLISHING)
          publishing = true;
      }
      else if (selected < 0)
        selected = static_cast<int>(i);
    }

    if (exclusiveSample && !idle)
      selected = -1;

    unsigned readyCount = 0;
    for (const FrameCandidate& candidate : m_candidates)
      if (candidate.state == CANDIDATE_READY)
        ++readyCount;

    if (allowSupersede && !exclusiveSample && selected < 0 &&
        readyCount > (publishing ? 0U : 1U))
      for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
        if (m_candidates[i].state == CANDIDATE_READY &&
            m_candidates[i].sequence < oldest)
        {
          selected = static_cast<int>(i);
          oldest   = m_candidates[i].sequence;
        }

    if (selected >= 0)
    {
      FrameCandidate& candidate =
        m_candidates[static_cast<unsigned>(selected)];
      superseded         = candidate.state == CANDIDATE_READY;
      candidate.state    = CANDIDATE_PREPARING;
      candidate.sequence = ++m_candidateSequence;
    }
  }

  if (superseded)
    m_transport->FrameSuperseded();
  return selected;
}

void CHardwareFrameProcessor::ReleaseCandidate(unsigned candidateIndex)
{
  if (candidateIndex >= ARRAYSIZE(m_candidates))
    return;

  {
    CSRWExclusiveLock lock(m_candidateLock);
    m_candidates[candidateIndex].state = CANDIDATE_FREE;
  }
  SignalCandidateState();
}

bool CHardwareFrameProcessor::EnsureCandidateResource(
  unsigned candidateIndex, size_t frameSize)
{
  FrameCandidate& candidate = m_candidates[candidateIndex];

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width              = frameSize;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags              = D3D12_RESOURCE_FLAG_NONE;

  if (candidate.resource &&
      CFrameProcessorUtil::ResourceDescMatches(
        candidate.resource->GetDesc(), desc, false))
    return true;

  candidate.resource.Reset();

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask     = 1;
  heapProps.VisibleNodeMask      = 1;

  const HRESULT hr = m_dx12->GetDevice()->CreateCommittedResource(
    &heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
    nullptr, IID_PPV_ARGS(&candidate.resource));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create retained frame candidate");
    return false;
  }

  static const WCHAR * names[] =
  {
    L"Frame Candidate 0",
    L"Frame Candidate 1",
  };
  candidate.resource->SetName(names[candidateIndex]);
  return true;
}

bool CHardwareFrameProcessor::ExecuteCandidateCopy(
  CD3D12CommandSlot * copySlot)
{
  HANDLE waitHandles[] =
  {
    m_terminateEvent,
    m_copySubmitEvent.Get(),
  };

  for (;;)
  {
    {
      CSRWExclusiveLock lock(m_copySubmitLock);
      if (!m_publishPending)
        return copySlot->Execute();
    }

    const DWORD result = WaitForMultipleObjects(
      ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 + 1)
      continue;

    copySlot->Cancel();
    if (result != WAIT_OBJECT_0)
      DEBUG_ERROR_HR(HRESULT_FROM_WIN32(GetLastError()),
        "Failed while waiting to submit a frame candidate");
    return false;
  }
}

void CHardwareFrameProcessor::CandidateCompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto processor = static_cast<CHardwareFrameProcessor *>(param1);
  auto candidate = static_cast<FrameCandidate *>(param2);

  uint64_t gpuStart = 0;
  uint64_t gpuEnd   = 0;
  const bool timingValid = result && slot->GetGPUTimes(gpuStart, gpuEnd);

  bool forceFrame = false;
  {
    CSRWExclusiveLock lock(processor->m_candidateLock);
    if (candidate->state == CANDIDATE_PREPARING)
    {
      candidate->prepareReady       = CFrameScheduler::Nanotime();
      candidate->prepareGPUStart    = gpuStart;
      candidate->prepareGPUEnd      = gpuEnd;
      candidate->prepareTimingValid = timingValid;
      candidate->state              =
        result ? CANDIDATE_READY : CANDIDATE_FREE;
      forceFrame = result && candidate->timingToken != 0;
    }
  }

  if (!result)
  {
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
  }
  else if (forceFrame)
    processor->m_transport->ForceFrame();
  processor->SignalCandidateState();
}

void CHardwareFrameProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto processor = static_cast<CHardwareFrameProcessor *>(param1);
  auto fbRes     = static_cast<CFrameBufferResource *>(param2);
  const unsigned candidateIndex = fbRes->GetCandidateIndex();

  if (!result)
  {
    processor->m_transport->FailFrameBuffer(fbRes->GetFrameIndex());
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
    processor->ReleaseCandidate(candidateIndex);
    return;
  }

  uint64_t prepareCopyStart;
  uint64_t prepareReady;
  uint64_t prepareGPUStart;
  uint64_t prepareGPUEnd;
  uint64_t timingStart;
  bool     prepareTimingValid;
  {
    CSRWSharedLock lock(processor->m_candidateLock);
    const FrameCandidate& candidate =
      processor->m_candidates[candidateIndex];
    prepareCopyStart   = candidate.prepareCopyStart;
    prepareReady       = candidate.prepareReady;
    prepareGPUStart    = candidate.prepareGPUStart;
    prepareGPUEnd      = candidate.prepareGPUEnd;
    timingStart        = candidate.timingStart;
    prepareTimingValid = candidate.prepareTimingValid;
  }

  const uint64_t publishStart = fbRes->GetCopyStart();
  uint64_t       gpuCopyStart     = 0;
  uint64_t       gpuCopyEnd       = 0;
  uint64_t       indirectCopyTime = 0;
  if (processor->m_dx12->IsIndirectCopy())
  {
    const uint64_t indirectCopyStart = CFrameScheduler::Nanotime();
    processor->m_transport->WriteFrameBuffer(
      fbRes->GetFrameIndex(), fbRes->GetMap(), 0,
      fbRes->GetFrameSize(), false);
    indirectCopyTime = CFrameScheduler::Nanotime() - indirectCopyStart;
  }

  const bool gpuTimingValid =
    slot->GetGPUTimes(gpuCopyStart, gpuCopyEnd);
  const uint64_t copyReady = CFrameScheduler::Nanotime();

  const uint64_t postProcessStart = fbRes->GetPostProcessStart();
  uint64_t postProcessTime = prepareCopyStart - postProcessStart;
  uint64_t prepareCopyTime = prepareReady - prepareCopyStart;
  if (prepareTimingValid && prepareGPUStart >= postProcessStart &&
      prepareGPUEnd >= prepareGPUStart && prepareGPUEnd <= prepareReady)
  {
    postProcessTime = prepareGPUStart - postProcessStart;
    prepareCopyTime = prepareGPUEnd - prepareGPUStart;
  }

  uint64_t publishCopyTime = copyReady - publishStart;
  if (gpuTimingValid && gpuCopyStart >= publishStart &&
      gpuCopyEnd >= gpuCopyStart && gpuCopyEnd <= copyReady)
    publishCopyTime = gpuCopyEnd - gpuCopyStart + indirectCopyTime;

  const uint64_t copyTime = prepareCopyTime + publishCopyTime;

  processor->m_transport->FinalizeFrameBuffer(fbRes->GetFrameIndex());
  const uint64_t publishedAt = CFrameScheduler::Nanotime();
  const uint64_t prepareElapsed = prepareReady >= postProcessStart ?
    prepareReady - postProcessStart : 0;
  const uint64_t prepareMeasured = postProcessTime + prepareCopyTime;
  const uint64_t prepareReadyTime = prepareElapsed > prepareMeasured ?
    prepareElapsed - prepareMeasured : 0;
  const uint64_t publishElapsed = publishedAt >= publishStart ?
    publishedAt - publishStart : 0;
  const uint64_t publishReadyTime = publishElapsed > publishCopyTime ?
    publishElapsed - publishCopyTime : 0;
  const uint64_t readyTime = prepareReadyTime + publishReadyTime;
  const uint64_t holdTime = publishStart >= prepareReady ?
    publishStart - prepareReady : 0;

  processor->m_transport->SetFrameTiming(fbRes->GetFrameIndex(),
    fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime, holdTime,
    fbRes->GetSchedule(), publishedAt);
  processor->m_transport->TryRecordFrameTiming(publishedAt - publishStart);

  const uint64_t timingToken = fbRes->GetTimingToken();
  if (timingToken && timingStart && prepareReady >= timingStart &&
      copyReady >= publishStart)
  {
    const uint64_t totalTime =
      (prepareReady - timingStart) + (copyReady - publishStart);
    processor->m_postProcessors[candidateIndex].RecordTiming(
      fbRes->GetTimingEffectIndex(), timingToken,
      fbRes->IsFullCopy(), totalTime);
  }

  processor->m_transport->CompleteFrameBuffer(fbRes->GetFrameIndex(), true);
  processor->ReleaseCandidate(candidateIndex);
}

bool CHardwareFrameProcessor::Publish(
  const CFrameScheduler::Schedule& schedule, bool periodic,
  uint64_t publishStart)
{
  CPublishPending publishPending(
    m_copySubmitLock, &m_publishPending, m_copySubmitEvent.Get());
  CSRWSharedLock pipelineLock(*m_pipelineLock);

  int      selectedCandidate = -1;
  uint64_t newestSequence    = 0;

  {
    CSRWExclusiveLock lock(m_candidateLock);
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
      if (m_candidates[i].state == CANDIDATE_READY &&
          (selectedCandidate < 0 ||
            m_candidates[i].sequence > newestSequence))
      {
        selectedCandidate = static_cast<int>(i);
        newestSequence    = m_candidates[i].sequence;
      }

    if (selectedCandidate >= 0)
      m_candidates[static_cast<unsigned>(selectedCandidate)].state =
        CANDIDATE_PUBLISHING;
  }

  if (selectedCandidate < 0)
    return false;
  const unsigned candidateIndex =
    static_cast<unsigned>(selectedCandidate);

  const auto restoreCandidate = [this, candidateIndex]()
  {
    {
      CSRWExclusiveLock lock(m_candidateLock);
      if (m_candidates[candidateIndex].state == CANDIDATE_PUBLISHING)
        m_candidates[candidateIndex].state = CANDIDATE_READY;
    }
    SignalCandidateState();
  };

  bool candidateValid;
  {
    CSRWSharedLock lock(m_candidateLock);
    candidateValid =
      m_candidates[candidateIndex].state == CANDIDATE_PUBLISHING &&
      m_candidates[candidateIndex].resource.Get();
  }
  if (!candidateValid)
  {
    restoreCandidate();
    return false;
  }

  FrameCandidate& candidate        = m_candidates[candidateIndex];
  CPostProcessor& postProcessor    = m_postProcessors[candidateIndex];
  const uint64_t candidateSequence = candidate.sequence;

  auto buffer = m_transport->PrepareFrameBuffer(
    candidate.pitch, candidate.srcFormat, candidate.dstFormat,
    candidate.dirtyRects, candidate.nbDirtyRects, schedule);
  if (!buffer.mem)
  {
    restoreCandidate();
    return false;
  }

  CFrameBufferResource * fbRes =
    m_frameBuffers.Get(buffer, candidate.frameSize);
  if (!fbRes)
  {
    m_transport->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidate();
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    SetFullDamage();
    return false;
  }

  CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot(candidateIndex);
  if (!copySlot)
  {
    m_transport->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidate();
    DEBUG_ERROR("Failed to get a copy CommandSlot for publication");
    SetFullDamage();
    return false;
  }

  RECT     previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbPreviousDirtyRects                   = 0;
  GetPreviousDamage(previousDirtyRects, &nbPreviousDirtyRects);

  RECT     copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
  unsigned nbCopyDirtyRects                       = 0;
  const bool fullCopy = CFrameProcessorUtil::BuildCopyDamage(
    postProcessor, buffer.fullCopy,
    previousDirtyRects, nbPreviousDirtyRects,
    candidate.dirtyRects, candidate.nbDirtyRects,
    candidate.dstFormat.width, candidate.dstFormat.height,
    copyDirtyRects, &nbCopyDirtyRects);

  fbRes->SetTiming(
    candidate.captureTime, candidate.postProcessStart, publishStart);
  fbRes->SetCandidateIndex(candidateIndex);
  fbRes->SetPostProcessSample(
    candidate.timingEffectIndex, candidate.timingToken, fullCopy);
  copySlot->SetCompletionCallback(&CompletionFunction, this, fbRes);

  copySlot->BeginTiming();
  postProcessor.CopyFromCandidate(
    copySlot->GetGfxList(), fbRes->Get().Get(), candidate.resource.Get(),
    copyDirtyRects, nbCopyDirtyRects, fullCopy);
  copySlot->EndTiming();

  bool deliveredToOwner;
  if (!m_transport->PublishFrameBuffer(
        buffer.frameIndex, schedule, deliveredToOwner))
  {
    copySlot->Cancel();
    m_transport->AbortFrameBuffer(buffer.frameIndex);
    restoreCandidate();
    return false;
  }
  CFrameScheduler::Schedule frameSchedule = schedule;
  if (!deliveredToOwner ||
      !m_transport->TryFrameSubmitted(buffer.frameIndex, schedule))
    frameSchedule.phaseEligible = false;
  fbRes->SetSchedule(frameSchedule);

  {
    CSRWExclusiveLock lock(m_damageLock);
    if (candidate.nbDirtyRects)
      memcpy(m_previousDamage, candidate.dirtyRects,
        candidate.nbDirtyRects * sizeof(*m_previousDamage));
    m_previousDamageCount = candidate.nbDirtyRects;
    CandidateDamageTail& tail = m_candidateDamageTail[candidateIndex];
    if (tail.active && tail.ownerSequence == candidateSequence)
    {
      m_hasPendingDamage   = tail.hasDamage;
      m_pendingDamageCount = tail.nbDirtyRects;
      if (tail.hasDamage && tail.nbDirtyRects)
        memcpy(m_pendingDamage, tail.dirtyRects,
          tail.nbDirtyRects * sizeof(*m_pendingDamage));
      tail.ownerSequence = 0;
      tail.active        = false;
    }
  }

  const bool submitted = copySlot->Execute();
  publishPending.Clear();
  if (!submitted)
  {
    SetFullDamage();
    bool callbackPending;
    {
      CSRWSharedLock lock(m_candidateLock);
      callbackPending = candidate.state == CANDIDATE_PUBLISHING;
    }
    if (callbackPending && !copySlot->HasSubmittedWork())
    {
      m_transport->FailFrameBuffer(buffer.frameIndex);
      ReleaseCandidate(candidateIndex);
    }
    m_transport->ForceFrame();
    SignalCandidateState();
    return false;
  }

  m_transport->CommitFrameBuffer(
    buffer.frameIndex, schedule, periodic, deliveredToOwner);

  unsigned superseded = 0;
  {
    CSRWExclusiveLock lock(m_candidateLock);
    for (FrameCandidate& ready : m_candidates)
      if (ready.state == CANDIDATE_READY &&
          ready.sequence < candidateSequence)
      {
        ready.state = CANDIDATE_FREE;
        ++superseded;
      }
  }
  for (unsigned i = 0; i < superseded; ++i)
    m_transport->FrameSuperseded();
  SignalCandidateState();
  return true;
}

bool CHardwareFrameProcessor::Submit(const FrameSubmission& submission)
{
  int selectedCandidate = AcquireCandidate(
    submission.timingToken != 0, !submission.noImageUpdate);
  while (selectedCandidate < 0 && submission.noImageUpdate)
  {
    HANDLE waitHandles[] =
    {
      m_terminateEvent,
      m_candidateAvailableEvent.Get(),
    };
    const DWORD waitResult = WaitForMultipleObjects(
      ARRAYSIZE(waitHandles), waitHandles, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0)
      return true;
    if (waitResult != WAIT_OBJECT_0 + 1)
    {
      DEBUG_ERROR_HR(HRESULT_FROM_WIN32(GetLastError()),
        "Failed while waiting for a frame candidate");
      return false;
    }

    selectedCandidate = AcquireCandidate(
      submission.timingToken != 0, false);
  }
  if (selectedCandidate < 0)
  {
    m_transport->FrameSuperseded();
    return true;
  }
  const unsigned candidateIndex =
    static_cast<unsigned>(selectedCandidate);
  FrameCandidate& candidate = m_candidates[candidateIndex];

  CSRWSharedLock pipelineLock(*m_pipelineLock);
  CPostProcessor& postProcessor = m_postProcessors[candidateIndex];
  const D12FrameFormat& dstFormat = postProcessor.GetOutputFormat();

  RECT     currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects                          = 0;
  {
    CSRWExclusiveLock lock(m_damageLock);
    if (m_hasPendingDamage)
    {
      nbDirtyRects = m_pendingDamageCount;
      if (nbDirtyRects)
        memcpy(currentDirtyRects, m_pendingDamage,
          nbDirtyRects * sizeof(*currentDirtyRects));
    }
    CandidateDamageTail& tail = m_candidateDamageTail[candidateIndex];
    tail.ownerSequence = candidate.sequence;
    tail.nbDirtyRects  = 0;
    tail.hasDamage     = false;
    tail.active        = true;
  }

  CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot(candidateIndex);
  if (!copySlot)
  {
    ReleaseCandidate(candidateIndex);
    DEBUG_ERROR("Failed to get a copy CommandSlot");
    SetFullDamage();
    return false;
  }
  const uint64_t timingStart = submission.timingToken ?
    CFrameScheduler::Nanotime() : 0;

  ComPtr<ID3D12Resource> copySrcResource =
    submission.source->GetRes();
  CD3D12CommandSlot * computeSlot = nullptr;
  if (postProcessor.HasActiveEffects())
  {
    computeSlot = m_dx12->GetComputeSlot(candidateIndex);
    if (!computeSlot)
    {
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Failed to get a compute CommandSlot");
      SetFullDamage();
      return false;
    }
  }

  if (!submission.source->Signal())
  {
    if (computeSlot)
      computeSlot->Cancel();
    copySlot->Cancel();
    ReleaseCandidate(candidateIndex);
    SetFullDamage();
    return false;
  }

  if (computeSlot)
  {
    if (!submission.source->Sync(*computeSlot))
    {
      computeSlot->Cancel();
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      SetFullDamage();
      return false;
    }

    copySrcResource = postProcessor.Run(
      computeSlot->GetGfxList(), copySrcResource,
      currentDirtyRects, &nbDirtyRects);
    if (!copySrcResource)
    {
      computeSlot->Cancel();
      copySlot->Cancel();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Post processor returned no output resource");
      SetFullDamage();
      return false;
    }

    if (!computeSlot->Execute())
    {
      copySlot->Cancel();
      m_dx12->WaitForIdle();
      ReleaseCandidate(candidateIndex);
      SetFullDamage();
      return false;
    }

    if (!copySlot->WaitFor(*computeSlot))
    {
      copySlot->Cancel();
      m_dx12->WaitForIdle();
      ReleaseCandidate(candidateIndex);
      DEBUG_ERROR("Failed to queue compute synchronization");
      SetFullDamage();
      return false;
    }
  }
  else if (!submission.source->Sync(*copySlot))
  {
    copySlot->Cancel();
    ReleaseCandidate(candidateIndex);
    DEBUG_ERROR("Failed to queue source synchronization");
    SetFullDamage();
    return false;
  }

  CFrameProcessorUtil::ClipDirtyRects(
    currentDirtyRects, &nbDirtyRects,
    dstFormat.width, dstFormat.height);

  const size_t frameSize = postProcessor.GetOutputSize();
  if (!EnsureCandidateResource(candidateIndex, frameSize))
  {
    copySlot->Cancel();
    if (computeSlot)
      m_dx12->WaitForIdle();
    ReleaseCandidate(candidateIndex);
    SetFullDamage();
    return false;
  }

  candidate.srcFormat          = submission.sourceFormat;
  candidate.dstFormat          = dstFormat;
  candidate.nbDirtyRects       = nbDirtyRects;
  candidate.pitch              = postProcessor.GetOutputPitch();
  candidate.frameSize          = frameSize;
  candidate.captureTime        = submission.captureTime;
  candidate.postProcessStart   = submission.postProcessStart;
  candidate.prepareCopyStart   = CFrameScheduler::Nanotime();
  candidate.prepareReady       = 0;
  candidate.prepareGPUStart    = 0;
  candidate.prepareGPUEnd      = 0;
  candidate.timingStart        = timingStart;
  candidate.prepareTimingValid = false;
  if (nbDirtyRects)
    memcpy(candidate.dirtyRects, currentDirtyRects,
      nbDirtyRects * sizeof(*candidate.dirtyRects));
  candidate.timingEffectIndex = submission.timingEffectIndex;
  candidate.timingToken       = submission.timingToken;

  copySlot->SetCompletionCallback(
    &CandidateCompletionFunction, this, &candidate);
  copySlot->BeginTiming();
  postProcessor.CopyToCandidate(
    copySlot->GetGfxList(), candidate.resource.Get(),
    copySrcResource.Get());
  copySlot->EndTiming();

  if (!ExecuteCandidateCopy(copySlot))
  {
    if (!copySlot->HasSubmittedWork())
    {
      if (computeSlot)
        m_dx12->WaitForIdle();
      ReleaseCandidate(candidateIndex);
    }
    SetFullDamage();
    m_transport->ForceFrame();
    return false;
  }

  return true;
}
