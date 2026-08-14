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

#include "capture/CSoftwareFrameProcessor.h"
#include "capture/CFrameProcessorUtil.h"
#include "transport/IFrameTransport.h"
#include "CSRWLock.h"
#include "CDebug.h"

#include <utility>

CSoftwareFrameProcessor::CSoftwareFrameProcessor(
    IFrameTransport * transport, std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[CAPTURE_PIPELINE_SLOTS],
    CSRWLock * pipelineLock, HANDLE terminateEvent) :
  CFrameProcessor(transport, std::move(dx12), postProcessors,
    pipelineLock, terminateEvent)
{
}

void CSoftwareFrameProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto processor = static_cast<CSoftwareFrameProcessor *>(param1);
  auto context   = static_cast<CopyContext *>(param2);
  const FrameCopyBatch batch = context->batch;

  CFrameBufferResource * fbRes =
    batch.accepted == 1U && batch.prepared.count == 1 ?
      batch.resources[0] : nullptr;
  if (fbRes)
    fbRes->MarkCompletion();
  const bool succeeded = result && fbRes;

  if (succeeded)
  {
    uint64_t stagedCopyTime = 0;
    if (fbRes->GetMap())
    {
      const uint64_t stagedCopyStart = CFrameScheduler::Nanotime();
      if (fbRes->IsFullCopy())
        processor->m_transport->WriteFrameTarget(batch.prepared.token,
          0, fbRes->GetMap(), 0, fbRes->GetFrameSize(), false);
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
            batch.prepared.token, 0, fbRes->GetMap(), rowOffset,
            rowBytes, pitch, (unsigned)(rect->bottom - rect->top));
        }
      }
      stagedCopyTime = CFrameScheduler::Nanotime() - stagedCopyStart;
    }

    uint64_t gpuStart = 0;
    uint64_t gpuEnd   = 0;
    const uint64_t copyReady = CFrameScheduler::Nanotime();
    const bool gpuTimingValid = slot->GetGPUTimes(gpuStart, gpuEnd);

    processor->m_transport->FinalizeFrameTarget(
      batch.prepared.token, 0);
    const uint64_t publishedAt = CFrameScheduler::Nanotime();
    const uint64_t postProcessStart = fbRes->GetPostProcessStart();
    const uint64_t copyStart        = fbRes->GetCopyStart();
    uint64_t postProcessTime = copyStart >= postProcessStart ?
      copyStart - postProcessStart : 0;
    uint64_t copyTime = copyReady >= copyStart ?
      copyReady - copyStart : 0;
    if (gpuTimingValid && gpuStart >= postProcessStart &&
        gpuEnd >= gpuStart && gpuEnd <= copyReady)
    {
      postProcessTime = gpuStart - postProcessStart;
      copyTime        = gpuEnd - gpuStart + stagedCopyTime;
    }

    const uint64_t elapsed = publishedAt >= postProcessStart ?
      publishedAt - postProcessStart : 0;
    const uint64_t measured  = postProcessTime + copyTime;
    const uint64_t readyTime = elapsed > measured ?
      elapsed - measured : 0;

    processor->m_transport->SetFrameTargetTiming(batch.prepared.token, 0,
      fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime, 0,
      publishedAt);
    processor->m_transport->TryRecordFrameTiming(
      batch.prepared.token, 0, publishedAt - copyStart);

    if (fbRes->GetTimingToken() && context->timingStart &&
        copyReady >= context->timingStart)
      processor->m_postProcessors[0].RecordTiming(
        fbRes->GetTimingEffectIndex(), fbRes->GetTimingToken(),
        fbRes->IsFullCopy(), copyReady - context->timingStart);

    processor->m_transport->CompleteFrameTarget(
      batch.prepared.token, 0, true);
  }
  else
  {
    if (batch.accepted)
      processor->m_transport->FailFrameBatch(batch.prepared.token);
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
  }

  context->completionSucceeded.store(
    succeeded, std::memory_order_relaxed);
  context->completionDone.store(true, std::memory_order_release);
}

bool CSoftwareFrameProcessor::Submit(const FrameSubmission& submission)
{
  if (submission.noImageUpdate && !HasPendingDamage())
    return true;

  CSRWSharedLock pipelineLock(*m_pipelineLock);
  CPostProcessor& postProcessor = m_postProcessors[0];
  const D12FrameFormat& dstFormat = postProcessor.GetOutputFormat();
  const unsigned pitch            = postProcessor.GetOutputPitch();
  const size_t frameSize          = postProcessor.GetOutputSize();
  if (!pitch || !frameSize ||
      frameSize > m_transport->GetMaxFrameSize())
  {
    DEBUG_ERROR("Software frame does not fit in primary frame memory");
    SetFullDamage();
    return false;
  }

  enum class BackpressureResult
  {
    RETRY,
    COMPLETE,
    FAILED,
  };
  const auto handleBackpressure = [this, &submission]()
  {
    if (!submission.noImageUpdate)
    {
      m_transport->FrameSuperseded();
      return BackpressureResult::COMPLETE;
    }

    const DWORD result = WaitForSingleObject(m_terminateEvent, 1);
    if (result == WAIT_TIMEOUT)
      return BackpressureResult::RETRY;
    if (result == WAIT_OBJECT_0)
      return BackpressureResult::COMPLETE;
    DEBUG_ERROR_HR(HRESULT_FROM_WIN32(GetLastError()),
      "Failed while waiting to retry software frame publication");
    return BackpressureResult::FAILED;
  };

  uint64_t contentSerial = 0;
  for (;;)
  {
    FramePlan plan = {};
    if (!m_transport->GetImmediatePrimaryFramePlan(
          CFrameScheduler::Nanotime(), plan))
    {
      const BackpressureResult result = handleBackpressure();
      if (result == BackpressureResult::RETRY)
        continue;
      return result == BackpressureResult::COMPLETE;
    }
    if (plan.count != 1 || !plan.targets[0].primary)
    {
      DEBUG_ERROR("Software capture received an invalid primary frame plan");
      SetFullDamage();
      return false;
    }

    CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot();
    if (!copySlot)
    {
      const BackpressureResult result = handleBackpressure();
      if (result == BackpressureResult::RETRY)
        continue;
      return result == BackpressureResult::COMPLETE;
    }
    if (!contentSerial)
      contentSerial = m_transport->NextContentSerial();

    RECT currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned nbDirtyRects = 0;
    const bool hasDamage =
      TakePendingDamage(currentDirtyRects, &nbDirtyRects);
    CFrameProcessorUtil::ClipDirtyRects(currentDirtyRects,
      &nbDirtyRects, dstFormat.width, dstFormat.height);

    RECT previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned nbPreviousDirtyRects = 0;
    GetPreviousDamage(previousDirtyRects, &nbPreviousDirtyRects);

    PreparedFrameBatch prepared = {};
    if (!m_transport->PrepareFrameBatch(plan, contentSerial,
          pitch, frameSize,
          submission.sourceFormat, dstFormat,
          currentDirtyRects, nbDirtyRects, submission.noImageUpdate,
          prepared))
    {
      copySlot->Cancel();
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      const BackpressureResult result = handleBackpressure();
      if (result == BackpressureResult::RETRY)
        continue;
      return result == BackpressureResult::COMPLETE;
    }
    if (prepared.count != 1 || !prepared.targets[0].direct)
    {
      copySlot->Cancel();
      m_transport->AbortFrameBatch(prepared.token);
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      DEBUG_ERROR("Software capture received an invalid primary frame target");
      SetFullDamage();
      return false;
    }

    CFrameBufferResource * fbRes =
      m_frameBuffers.Get(prepared.targets[0], frameSize);
    if (!fbRes)
    {
      copySlot->Cancel();
      m_transport->AbortFrameBatch(prepared.token);
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      DEBUG_ERROR("Failed to get the software frame target resource");
      SetFullDamage();
      return false;
    }

    if (!submission.source->Signal() ||
        !submission.source->Sync(*copySlot))
    {
      copySlot->Cancel();
      m_transport->AbortFrameBatch(prepared.token);
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      SetFullDamage();
      return false;
    }

    const uint64_t timingStart = submission.timingToken ?
      CFrameScheduler::Nanotime() : 0;
    RECT copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
    unsigned nbCopyDirtyRects = 0;
    const bool fullCopy = CFrameProcessorUtil::BuildCopyDamage(
      postProcessor, prepared.targets[0].fullCopy,
      previousDirtyRects, nbPreviousDirtyRects,
      currentDirtyRects, nbDirtyRects,
      dstFormat.width, dstFormat.height,
      copyDirtyRects, &nbCopyDirtyRects);

    const unsigned bytesPerPixel =
      dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
    const uint64_t copyStart = CFrameScheduler::Nanotime();
    fbRes->SetTiming(
      submission.captureTime, submission.postProcessStart, copyStart);
    fbRes->SetPostProcessSample(
      submission.timingEffectIndex, submission.timingToken, fullCopy);
    fbRes->SetCopyDamage(copyDirtyRects, nbCopyDirtyRects,
      fullCopy, pitch, bytesPerPixel);
    fbRes->ResetCompletion();

    copySlot->BeginTiming();
    postProcessor.CopyToFrameBuffer(copySlot->GetGfxList(),
      fbRes->Get().Get(), submission.source->GetRes().Get(),
      copyDirtyRects, nbCopyDirtyRects, fullCopy);
    copySlot->EndTiming();

    const uint32_t accepted =
      m_transport->PublishFrameBatch(prepared.token);
    if (accepted != 1U)
    {
      copySlot->Cancel();
      if (accepted)
        m_transport->FailFrameBatch(prepared.token);
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      const BackpressureResult result = handleBackpressure();
      if (result == BackpressureResult::RETRY)
        continue;
      return result == BackpressureResult::COMPLETE;
    }

    m_copy.batch              = {};
    m_copy.batch.prepared     = prepared;
    m_copy.batch.resources[0] = fbRes;
    m_copy.batch.accepted     = accepted;
    m_copy.timingStart        = timingStart;
    m_copy.completionSucceeded.store(false, std::memory_order_relaxed);
    m_copy.completionDone.store(false, std::memory_order_release);
    copySlot->SetCompletionCallback(
      &CompletionFunction, this, &m_copy);

    const FrameBatchToken token = prepared.token;
    const bool executed = copySlot->Execute();
    const bool submittedWork =
      !executed && copySlot->HasSubmittedWork();
    const bool completionDone =
      m_copy.completionDone.load(std::memory_order_acquire);
    const bool completionSucceeded = completionDone &&
      m_copy.completionSucceeded.load(std::memory_order_relaxed);
    if (!executed)
    {
      if (completionSucceeded ||
          (!completionDone && submittedWork))
      {
        m_transport->CommitFrameBatch(token);
        CommitDamage(currentDirtyRects, nbDirtyRects);
        RestorePendingDamage(
          currentDirtyRects, nbDirtyRects, hasDamage);
      }
      else
      {
        if (!completionDone)
          m_transport->FailFrameBatch(token);
        RestorePendingDamage(
          currentDirtyRects, nbDirtyRects, hasDamage);
        if (!completionDone)
        {
          SetFullDamage();
          m_transport->ForceFrame();
        }
      }
      return false;
    }

    if (completionDone && !completionSucceeded)
    {
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      return false;
    }

    m_transport->CommitFrameBatch(token);
    CommitDamage(currentDirtyRects, nbDirtyRects);
    return true;
  }
}
