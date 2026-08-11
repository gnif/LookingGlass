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
#include "capture/FramePipeline.h"
#include "CSRWLock.h"
#include "transport/IFrameTransport.h"
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
  auto fbRes     = static_cast<CFrameBufferResource *>(param2);
  fbRes->MarkCompletion();

  if (!result)
  {
    processor->m_transport->FailFrameBuffer(fbRes->GetFrameIndex());
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
    return;
  }

  uint64_t indirectCopyTime = 0;
  if (processor->m_dx12->IsIndirectCopy())
  {
    const uint64_t indirectCopyStart = CFrameScheduler::Nanotime();
    if (fbRes->IsFullCopy())
      processor->m_transport->WriteFrameBuffer(fbRes->GetFrameIndex(),
        fbRes->GetMap(), 0, fbRes->GetFrameSize(), false);
    else
    {
      const unsigned pitch         = fbRes->GetCopyPitch();
      const unsigned bytesPerPixel = fbRes->GetCopyBytesPerPixel();
      const RECT * dirtyRects      = fbRes->GetCopyDirtyRects();
      const unsigned count         = fbRes->GetCopyDirtyRectCount();
      for (const RECT * rect = dirtyRects; rect < dirtyRects + count; ++rect)
      {
        const size_t rowOffset =
          (size_t)rect->top * pitch +
          (size_t)rect->left * bytesPerPixel;
        const size_t rowBytes =
          (size_t)(rect->right - rect->left) * bytesPerPixel;
        processor->m_transport->WriteFrameBufferRows(fbRes->GetFrameIndex(),
          fbRes->GetMap(), rowOffset, rowBytes, pitch,
          (unsigned)(rect->bottom - rect->top));
      }
    }
    indirectCopyTime = CFrameScheduler::Nanotime() - indirectCopyStart;
  }

  uint64_t       gpuStart       = 0;
  uint64_t       gpuEnd         = 0;
  const uint64_t copyReady      = CFrameScheduler::Nanotime();
  const bool     gpuTimingValid = slot->GetGPUTimes(gpuStart, gpuEnd);

  processor->m_transport->FinalizeFrameBuffer(fbRes->GetFrameIndex());
  const uint64_t publishedAt      = CFrameScheduler::Nanotime();
  const uint64_t postProcessStart = fbRes->GetPostProcessStart();
  const uint64_t copyStart        = fbRes->GetCopyStart();
  uint64_t       postProcessTime  = copyStart >= postProcessStart ?
    copyStart - postProcessStart : 0;
  uint64_t       copyTime         = copyReady >= copyStart ?
    copyReady - copyStart : 0;
  if (gpuTimingValid && gpuStart >= postProcessStart &&
      gpuEnd >= gpuStart && gpuEnd <= copyReady)
  {
    postProcessTime = gpuStart - postProcessStart;
    copyTime        = gpuEnd - gpuStart + indirectCopyTime;
  }

  const uint64_t elapsed   = publishedAt >= postProcessStart ?
    publishedAt - postProcessStart : 0;
  const uint64_t measured  = postProcessTime + copyTime;
  const uint64_t readyTime = elapsed > measured ? elapsed - measured : 0;

  processor->m_transport->SetFrameTiming(fbRes->GetFrameIndex(),
    fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime, 0,
    fbRes->GetSchedule(), publishedAt);
  processor->m_transport->CompleteFrameBuffer(fbRes->GetFrameIndex(), true);
}

bool CSoftwareFrameProcessor::Submit(const FrameSubmission& submission)
{
  CSRWSharedLock pipelineLock(*m_pipelineLock);
  CPostProcessor&       postProcessor = m_postProcessors[0];
  const D12FrameFormat& dstFormat     = postProcessor.GetOutputFormat();

  // A copyable footprint describes a buffer layout, not the physical layout
  // of a row-major texture. Use that explicit buffer layout so the pitch sent
  // through KVMFR always matches the bytes written into transport memory.
  const unsigned pitch     = postProcessor.GetOutputPitch();
  const size_t   frameSize = postProcessor.GetOutputSize();

  if (!pitch || !frameSize || frameSize > m_transport->GetMaxFrameSize())
  {
    DEBUG_ERROR("Software frame does not fit in transport memory");
    SetFullDamage();
    return false;
  }

  if (submission.noImageUpdate && !HasPendingDamage())
    return true;

  for (;;)
  {
    CFrameScheduler::Schedule commitSchedule   = {};
    CFrameScheduler::Schedule deliverySchedule = {};
    PreparedFrameBuffer        buffer           = {};
    CD3D12CommandSlot        * copySlot         = nullptr;
    RECT     currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned nbDirtyRects                          = 0;
    bool     hasDamage                             = false;

    uint64_t ignoredTarget    = 0;
    bool     ignoredPeriodic  = false;
    bool     ignoredRepublish = false;
    m_transport->GetPublishTarget(CFrameScheduler::Nanotime(),
      ignoredTarget, commitSchedule, ignoredPeriodic, ignoredRepublish);
    deliverySchedule = commitSchedule;
    deliverySchedule.deliveryDeadlineSerial = 0;
    deliverySchedule.phaseEligible          = false;

    m_transport->ProcessDeliveries();
    if (!m_transport->FrameBufferAvailable(
          deliverySchedule, submission.noImageUpdate))
    {
      if (!submission.noImageUpdate)
      {
        m_transport->FrameSuperseded();
        return true;
      }

      if (WaitForSingleObject(m_terminateEvent, 1) == WAIT_OBJECT_0)
        return true;
      continue;
    }

    copySlot = m_dx12->GetCopySlot();
    if (!copySlot)
    {
      if (!submission.noImageUpdate)
      {
        m_transport->FrameSuperseded();
        return true;
      }

      if (WaitForSingleObject(m_terminateEvent, 1) == WAIT_OBJECT_0)
        return true;
      continue;
    }

    hasDamage = TakePendingDamage(currentDirtyRects, &nbDirtyRects);
    CFrameProcessorUtil::ClipDirtyRects(
      currentDirtyRects, &nbDirtyRects,
      dstFormat.width, dstFormat.height);
    buffer = m_transport->PrepareFrameBuffer(
      pitch, submission.sourceFormat, dstFormat,
      currentDirtyRects, nbDirtyRects, deliverySchedule,
      submission.noImageUpdate);
    if (!buffer.mem)
    {
      copySlot->Cancel();
      RestorePendingDamage(
        currentDirtyRects, nbDirtyRects, hasDamage);
      if (!submission.noImageUpdate)
      {
        m_transport->FrameSuperseded();
        return true;
      }

      if (WaitForSingleObject(m_terminateEvent, 1) == WAIT_OBJECT_0)
        return true;
      continue;
    }

    CFrameBufferResource * fbRes =
      m_frameBuffers.Get(buffer, frameSize);
    if (!fbRes)
    {
      copySlot->Cancel();
      m_transport->AbortFrameBuffer(buffer.frameIndex);
      RestorePendingDamage(
        currentDirtyRects, nbDirtyRects, hasDamage);
      DEBUG_ERROR("Failed to get a framebuffer for software capture");
      SetFullDamage();
      return false;
    }

    if (!submission.source->Signal() ||
        !submission.source->Sync(*copySlot))
    {
      copySlot->Cancel();
      m_transport->AbortFrameBuffer(buffer.frameIndex);
      RestorePendingDamage(
        currentDirtyRects, nbDirtyRects, hasDamage);
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
      currentDirtyRects, nbDirtyRects,
      dstFormat.width, dstFormat.height,
      copyDirtyRects, &nbCopyDirtyRects);

    const unsigned bytesPerPixel =
      dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
    const uint64_t copyStart = CFrameScheduler::Nanotime();
    fbRes->SetTiming(submission.captureTime,
      submission.postProcessStart, copyStart);
    fbRes->SetSchedule(deliverySchedule);
    fbRes->SetCopyDamage(copyDirtyRects, nbCopyDirtyRects,
      fullCopy, pitch, bytesPerPixel);
    fbRes->ResetCompletion();
    copySlot->SetCompletionCallback(
      &CompletionFunction, this, fbRes);
    copySlot->BeginTiming();
    postProcessor.CopyToFrameBuffer(copySlot->GetGfxList(),
      fbRes->Get().Get(), submission.source->GetRes().Get(),
      copyDirtyRects, nbCopyDirtyRects, fullCopy);
    copySlot->EndTiming();

    bool deliveredToOwner;
    if (!m_transport->PublishFrameBuffer(
          buffer.frameIndex, deliverySchedule, deliveredToOwner))
    {
      copySlot->Cancel();
      m_transport->AbortFrameBuffer(buffer.frameIndex);
      RestorePendingDamage(
        currentDirtyRects, nbDirtyRects, hasDamage);
      if (!submission.noImageUpdate)
      {
        m_transport->FrameSuperseded();
        return true;
      }

      if (WaitForSingleObject(m_terminateEvent, 1) == WAIT_OBJECT_0)
        return true;
      continue;
    }

    CommitDamage(currentDirtyRects, nbDirtyRects);
    if (!copySlot->Execute())
    {
      const bool submittedWork     = copySlot->HasSubmittedWork();
      const bool completionHandled = fbRes->CompletionHandled();
      if (!submittedWork && !completionHandled)
        m_transport->FailFrameBuffer(buffer.frameIndex);
      RestorePendingDamage(
        currentDirtyRects, nbDirtyRects, hasDamage);
      if (!submittedWork && !completionHandled)
      {
        SetFullDamage();
        m_transport->ForceFrame();
      }
      return false;
    }

    m_transport->CommitFrameBuffer(
      buffer.frameIndex, commitSchedule, false, deliveredToOwner);
    return true;
  }
}
