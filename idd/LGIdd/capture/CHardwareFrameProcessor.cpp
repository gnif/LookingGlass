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
    CSRWLock * pipelineLock, HANDLE terminateEvent, bool useCadence) :
  CFrameProcessor(transport, std::move(dx12), postProcessors,
    pipelineLock, terminateEvent),
  m_useCadence(useCadence)
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
  bool ready    = false;
  bool retained = false;
  {
    CSRWSharedLock lock(m_candidateLock);
    for (const FrameCandidate& candidate : m_candidates)
    {
      if (candidate.state == CANDIDATE_READY)
        ready = true;
      else if (candidate.state == CANDIDATE_RETAINED)
        retained = true;
    }
  }
  return ready || (retained && m_transport->NeedsFrame());
}

int CHardwareFrameProcessor::AcquireCandidate(
  bool exclusiveSample, bool allowSupersede)
{
  int      selected   = -1;
  uint64_t oldest     = UINT64_MAX;
  bool     superseded = false;
  bool     idle       = true;
  bool     publishing = false;
  bool     retained   = false;

  {
    CSRWExclusiveLock lock(m_candidateLock);
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
    {
      if (m_candidates[i].state == CANDIDATE_RETAINED)
      {
        retained = true;
        continue;
      }
      else if (m_candidates[i].state != CANDIDATE_FREE)
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
        readyCount > ((publishing || retained) ? 0U : 1U))
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
      candidate.sequence = m_transport->NextContentSerial();
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

void CHardwareFrameProcessor::RetainCandidate(unsigned candidateIndex)
{
  if (candidateIndex >= ARRAYSIZE(m_candidates))
    return;

  unsigned superseded = 0;
  {
    CSRWExclusiveLock lock(m_candidateLock);
    FrameCandidate& candidate = m_candidates[candidateIndex];
    if (candidate.state != CANDIDATE_PUBLISHING)
      return;

    bool newer = false;
    for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
      if (i != candidateIndex)
      {
        FrameCandidate& current = m_candidates[i];
        const bool complete = current.state == CANDIDATE_READY ||
          current.state == CANDIDATE_PUBLISHING ||
          current.state == CANDIDATE_RETAINED;
        if (complete && current.sequence > candidate.sequence)
          newer = true;
        else if (current.sequence < candidate.sequence &&
                 (current.state == CANDIDATE_READY ||
                  current.state == CANDIDATE_RETAINED))
        {
          if (current.state == CANDIDATE_READY)
            ++superseded;
          current.state = CANDIDATE_FREE;
        }
      }
    candidate.state = newer ? CANDIDATE_FREE : CANDIDATE_RETAINED;
  }
  for (unsigned i = 0; i < superseded; ++i)
    m_transport->FrameSuperseded();
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

  uint64_t readySequence = 0;
  bool forceFrame        = false;
  {
    CSRWExclusiveLock lock(processor->m_candidateLock);
    if (candidate->state == CANDIDATE_PREPARING)
    {
      candidate->prepareReady       = CFrameScheduler::Nanotime();
      candidate->prepareGPUStart    = gpuStart;
      candidate->prepareGPUEnd      = gpuEnd;
      candidate->prepareTimingValid = timingValid;
      if (result)
      {
        bool newer = false;
        for (FrameCandidate& current : processor->m_candidates)
          if (&current != candidate)
          {
            const bool complete = current.state == CANDIDATE_READY ||
              current.state == CANDIDATE_PUBLISHING ||
              current.state == CANDIDATE_RETAINED;
            if (complete && current.sequence > candidate->sequence)
              newer = true;
            else if (current.state == CANDIDATE_RETAINED)
              current.state = CANDIDATE_FREE;
          }
        candidate->state = newer ? CANDIDATE_FREE : CANDIDATE_READY;
      }
      else
        candidate->state = CANDIDATE_FREE;
      if (candidate->state == CANDIDATE_READY)
        readySequence = candidate->sequence;
      forceFrame = candidate->state == CANDIDATE_READY &&
        candidate->timingToken != 0;
    }
  }

  if (!result)
  {
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
  }
  else if (readySequence)
  {
    processor->m_transport->FrameProductReady(readySequence);
    if (forceFrame)
      processor->m_transport->ForceFrame();
  }
  processor->SignalCandidateState();
}

void CHardwareFrameProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto processor = static_cast<CHardwareFrameProcessor *>(param1);
  const FrameCopyBatch batch =
    *static_cast<FrameCopyBatch *>(param2);
  const unsigned candidateIndex = batch.candidateIndex;

  if (!result)
  {
    processor->m_transport->FailFrameBatch(batch.prepared.token);
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
    processor->RetainCandidate(candidateIndex);
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

  uint64_t publishStart = 0;
  for (unsigned i = 0; i < batch.prepared.count; ++i)
    if ((batch.accepted & (1U << i)) && batch.resources[i])
    {
      publishStart = batch.resources[i]->GetCopyStart();
      break;
    }
  uint64_t       gpuCopyStart     = 0;
  uint64_t       gpuCopyEnd       = 0;
  const bool gpuTimingValid =
    slot->GetGPUTimes(gpuCopyStart, gpuCopyEnd);
  bool timingRecorded = false;
  for (unsigned i = 0; i < batch.prepared.count; ++i)
  {
    if (!(batch.accepted & (1U << i)))
      continue;
    CFrameBufferResource * fbRes = batch.resources[i];
    if (!fbRes)
      continue;

    uint64_t stagedCopyTime = 0;
    if (fbRes->GetMap())
    {
      const uint64_t stagedCopyStart = CFrameScheduler::Nanotime();
      if (fbRes->IsFullCopy())
        processor->m_transport->WriteFrameTarget(batch.prepared.token,
          i, fbRes->GetMap(), 0, fbRes->GetFrameSize(), false);
      else
      {
        const unsigned pitch         = fbRes->GetCopyPitch();
        const unsigned bytesPerPixel = fbRes->GetCopyBytesPerPixel();
        const RECT * dirtyRects      = fbRes->GetCopyDirtyRects();
        const unsigned count         = fbRes->GetCopyDirtyRectCount();
        for (const RECT * rect = dirtyRects;
             rect < dirtyRects + count; ++rect)
        {
          const size_t rowOffset =
            (size_t)rect->top * pitch +
            (size_t)rect->left * bytesPerPixel;
          const size_t rowBytes =
            (size_t)(rect->right - rect->left) * bytesPerPixel;
          processor->m_transport->WriteFrameTargetRows(
            batch.prepared.token, i, fbRes->GetMap(), rowOffset,
            rowBytes, pitch, (unsigned)(rect->bottom - rect->top));
        }
      }
      stagedCopyTime = CFrameScheduler::Nanotime() - stagedCopyStart;
    }
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
      publishCopyTime = gpuCopyEnd - gpuCopyStart + stagedCopyTime;

    const uint64_t copyTime = prepareCopyTime + publishCopyTime;

    processor->m_transport->FinalizeFrameTarget(
      batch.prepared.token, i);
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

    processor->m_transport->SetFrameTargetTiming(batch.prepared.token, i,
      fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime,
      holdTime, publishedAt);
    processor->m_transport->TryRecordFrameTiming(
      batch.prepared.token, i, publishedAt - publishStart);

    const uint64_t timingToken = fbRes->GetTimingToken();
    if (!timingRecorded && timingToken && timingStart &&
        prepareReady >= timingStart && copyReady >= publishStart)
    {
      const uint64_t totalTime =
        (prepareReady - timingStart) + (copyReady - publishStart);
      processor->m_postProcessors[candidateIndex].RecordTiming(
        fbRes->GetTimingEffectIndex(), timingToken,
        fbRes->IsFullCopy(), totalTime);
      timingRecorded = true;
    }

    processor->m_transport->CompleteFrameTarget(
      batch.prepared.token, i, true);
  }
  processor->RetainCandidate(candidateIndex);
}

bool CHardwareFrameProcessor::Publish(
  const FramePlan& plan, uint64_t publishStart)
{
  CPublishPending publishPending(
    m_copySubmitLock, &m_publishPending, m_copySubmitEvent.Get());
  CSRWSharedLock pipelineLock(*m_pipelineLock);

  int      selectedCandidate = -1;
  uint64_t newestSequence    = 0;
  CandidateState selectedState = CANDIDATE_FREE;

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

    if (selectedCandidate < 0 && m_transport->NeedsFrame())
      for (unsigned i = 0; i < ARRAYSIZE(m_candidates); ++i)
        if (m_candidates[i].state == CANDIDATE_RETAINED &&
            (selectedCandidate < 0 ||
              m_candidates[i].sequence > newestSequence))
        {
          selectedCandidate = static_cast<int>(i);
          newestSequence    = m_candidates[i].sequence;
        }

    if (selectedCandidate >= 0)
    {
      FrameCandidate& selected =
        m_candidates[static_cast<unsigned>(selectedCandidate)];
      selectedState  = selected.state;
      selected.state = CANDIDATE_PUBLISHING;
    }
  }

  if (selectedCandidate < 0)
    return false;
  const unsigned candidateIndex =
    static_cast<unsigned>(selectedCandidate);

  const auto restoreCandidate =
    [this, candidateIndex, selectedState]()
  {
    {
      CSRWExclusiveLock lock(m_candidateLock);
      if (m_candidates[candidateIndex].state == CANDIDATE_PUBLISHING)
        m_candidates[candidateIndex].state = selectedState;
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

  PreparedFrameBatch prepared = {};
  if (!m_transport->PrepareFrameBatch(plan, candidate.sequence,
        candidate.pitch, candidate.frameSize, candidate.srcFormat,
        candidate.dstFormat, candidate.dirtyRects,
        candidate.nbDirtyRects, true, prepared))
  {
    restoreCandidate();
    return false;
  }

  CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot(candidateIndex);
  if (!copySlot)
  {
    m_transport->AbortFrameBatch(prepared.token);
    restoreCandidate();
    DEBUG_ERROR("Failed to get a copy CommandSlot for publication");
    SetFullDamage();
    return false;
  }

  RECT     previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbPreviousDirtyRects                   = 0;
  GetPreviousDamage(previousDirtyRects, &nbPreviousDirtyRects);

  FrameCopyBatch& batch = m_publishBatches[candidateIndex];
  batch = {};
  batch.prepared       = prepared;
  batch.candidateIndex = candidateIndex;
  const unsigned bytesPerPixel =
    candidate.dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  bool resourcesReady = true;
  for (unsigned i = 0; i < prepared.count; ++i)
  {
    CFrameBufferResource * fbRes =
      m_frameBuffers.Get(prepared.targets[i], candidate.frameSize);
    batch.resources[i] = fbRes;
    if (!fbRes)
    {
      resourcesReady = false;
      break;
    }

    RECT copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
    unsigned nbCopyDirtyRects = 0;
    const bool fullCopy = CFrameProcessorUtil::BuildCopyDamage(
      postProcessor, prepared.targets[i].fullCopy,
      previousDirtyRects, nbPreviousDirtyRects,
      candidate.dirtyRects, candidate.nbDirtyRects,
      candidate.dstFormat.width, candidate.dstFormat.height,
      copyDirtyRects, &nbCopyDirtyRects);
    fbRes->SetTiming(
      candidate.captureTime, candidate.postProcessStart, publishStart);
    fbRes->SetCandidateIndex(candidateIndex);
    fbRes->SetPostProcessSample(
      candidate.timingEffectIndex, candidate.timingToken, fullCopy);
    fbRes->SetCopyDamage(copyDirtyRects, nbCopyDirtyRects,
      fullCopy, candidate.pitch, bytesPerPixel);
  }
  if (!resourcesReady)
  {
    copySlot->Cancel();
    m_transport->FailFrameBatch(prepared.token);
    restoreCandidate();
    DEBUG_ERROR("Failed to get a CFrameBufferResource from the pool");
    SetFullDamage();
    return false;
  }

  const uint32_t accepted =
    m_transport->PublishFrameBatch(prepared.token);
  if (!accepted)
  {
    copySlot->Cancel();
    restoreCandidate();
    return false;
  }
  batch.accepted = accepted;

  copySlot->SetCompletionCallback(&CompletionFunction, this, &batch);
  copySlot->BeginTiming();
  for (unsigned i = 0; i < prepared.count; ++i)
    if (accepted & (1U << i))
    {
      CFrameBufferResource * fbRes = batch.resources[i];
      postProcessor.CopyFromCandidate(copySlot->GetGfxList(),
        fbRes->Get().Get(), candidate.resource.Get(),
        fbRes->GetCopyDirtyRects(), fbRes->GetCopyDirtyRectCount(),
        fbRes->IsFullCopy());
    }
  copySlot->EndTiming();

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
    const bool submittedWork = copySlot->HasSubmittedWork();
    bool callbackPending;
    {
      CSRWSharedLock lock(m_candidateLock);
      callbackPending = candidate.state == CANDIDATE_PUBLISHING;
    }
    if (submittedWork || !callbackPending)
      m_transport->CommitFrameBatch(prepared.token);
    else
    {
      m_transport->FailFrameBatch(prepared.token);
      RetainCandidate(candidateIndex);
    }
    m_transport->ForceFrame();
    SignalCandidateState();
    return false;
  }

  m_transport->CommitFrameBatch(prepared.token);

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
  const unsigned pitch = postProcessor.GetOutputPitch();
  if (!pitch || !frameSize || frameSize > m_transport->GetMaxFrameSize())
  {
    copySlot->Cancel();
    if (computeSlot)
      m_dx12->WaitForIdle();
    ReleaseCandidate(candidateIndex);
    DEBUG_ERROR("Processed frame does not fit in primary frame memory");
    SetFullDamage();
    return false;
  }

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
  candidate.pitch              = pitch;
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
