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

#include <cstring>
#include <utility>

using namespace Microsoft::WRL;

static_assert(CAPTURE_PIPELINE_SLOTS == 2,
  "Software frame products assume two pipeline slots");

CSoftwareFrameProcessor::CSoftwareFrameProcessor(
    IFrameTransport * transport, std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[CAPTURE_PIPELINE_SLOTS],
    CSRWLock * pipelineLock, HANDLE terminateEvent) :
  CFrameProcessor(transport, std::move(dx12), postProcessors,
    pipelineLock, terminateEvent)
{
  m_productAvailableEvent.Attach(
    CreateEvent(nullptr, FALSE, FALSE, nullptr));
}

void CSoftwareFrameProcessor::SignalProductState(bool available)
{
  SetEvent(m_readyEvent.Get());
  if (available)
    SetEvent(m_productAvailableEvent.Get());
}

int CSoftwareFrameProcessor::AcquireProduct()
{
  CSRWExclusiveLock lock(m_productLock);
  for (unsigned i = 0; i < ARRAYSIZE(m_products); ++i)
    if (m_products[i].state == PRODUCT_FREE)
    {
      Product& product          = m_products[i];
      product.state             = PRODUCT_PREPARING;
      product.restoreState      = PRODUCT_FREE;
      product.completedState    = PRODUCT_FREE;
      product.sequence          = m_transport->NextContentSerial();
      product.sourceReady       = false;
      product.batchCommitted    = false;
      product.productNotified   = false;
      product.executeActive     = false;
      product.completionDone    = false;
      product.completionSucceeded = false;
      product.batch             = {};
      return static_cast<int>(i);
    }
  return -1;
}

int CSoftwareFrameProcessor::WaitForProduct()
{
  for (;;)
  {
    const int selected = AcquireProduct();
    if (selected >= 0)
      return selected;

    HANDLE handles[] =
    {
      m_terminateEvent,
      m_productAvailableEvent.Get(),
    };
    const DWORD result = WaitForMultipleObjects(
      ARRAYSIZE(handles), handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0)
      return -1;
    if (result != WAIT_OBJECT_0 + 1)
    {
      DEBUG_ERROR_HR(HRESULT_FROM_WIN32(GetLastError()),
        "Failed while waiting for a software frame product");
      return -2;
    }
  }
}

bool CSoftwareFrameProcessor::IsValid() const
{
  return CFrameProcessor::IsValid() && m_productAvailableEvent.Get();
}

void CSoftwareFrameProcessor::RestoreProduct(
  unsigned productIndex, ProductState state)
{
  if (productIndex >= ARRAYSIZE(m_products))
    return;

  {
    CSRWExclusiveLock lock(m_productLock);
    Product& product = m_products[productIndex];
    product.state             = state;
    product.restoreState      = PRODUCT_FREE;
    product.completedState    = PRODUCT_FREE;
    product.executeActive     = false;
    product.batch             = {};
  }
  SignalProductState(state == PRODUCT_FREE);
}

void CSoftwareFrameProcessor::MarkSourceReady(
  unsigned productIndex, uint64_t sequence)
{
  bool notify = false;
  {
    CSRWExclusiveLock lock(m_productLock);
    Product& product = m_products[productIndex];
    if (product.sequence != sequence)
      return;
    const bool retained = product.state == PRODUCT_RETAINED ||
      (product.state == PRODUCT_COMPLETING &&
       product.completionSucceeded &&
       product.completedState == PRODUCT_RETAINED);
    if (!retained)
      return;
    product.sourceReady = true;
    if (product.batchCommitted && !product.productNotified)
    {
      product.productNotified = true;
      notify = true;
    }
  }
  if (notify)
    m_transport->FrameProductReady(sequence);
}

void CSoftwareFrameProcessor::MarkBatchCommitted(
  unsigned productIndex, uint64_t sequence)
{
  bool notify = false;
  {
    CSRWExclusiveLock lock(m_productLock);
    Product& product = m_products[productIndex];
    if (product.sequence != sequence)
      return;
    const bool retained = product.state == PRODUCT_RETAINED ||
      (product.state == PRODUCT_COMPLETING &&
       product.completionSucceeded &&
       product.completedState == PRODUCT_RETAINED);
    product.batchCommitted = true;
    if (retained && product.sourceReady && !product.productNotified)
    {
      product.productNotified = true;
      notify = true;
    }
  }
  if (notify)
    m_transport->FrameProductReady(sequence);
}

void CSoftwareFrameProcessor::FinishProduct(
  unsigned productIndex, bool publishing, bool result)
{
  bool available;
  {
    CSRWExclusiveLock lock(m_productLock);
    Product& product = m_products[productIndex];
    const ProductState expected =
      publishing ? PRODUCT_PUBLISHING : PRODUCT_PREPARING;
    if (product.state != expected)
      return;

    bool newer = false;
    for (unsigned i = 0; i < ARRAYSIZE(m_products); ++i)
    {
      if (i == productIndex)
        continue;

      Product& current = m_products[i];
      const bool completing = current.state == PRODUCT_COMPLETING &&
        current.completionSucceeded &&
        current.completedState != PRODUCT_FREE;
      const bool complete = current.state == PRODUCT_READY ||
        current.state == PRODUCT_PUBLISHING ||
        current.state == PRODUCT_RETAINED || completing;
      if (complete && current.sequence > product.sequence)
        newer = true;
      else if (result && complete &&
          current.state != PRODUCT_PUBLISHING &&
          current.sequence < product.sequence)
      {
        if (current.state == PRODUCT_COMPLETING)
          current.completedState = PRODUCT_FREE;
        else
        {
          current.state = PRODUCT_FREE;
          SetEvent(m_productAvailableEvent.Get());
        }
      }
    }

    ProductState completedState;
    if (newer)
      completedState = PRODUCT_FREE;
    else if (!result)
      completedState = publishing ? product.restoreState : PRODUCT_FREE;
    else
      completedState = PRODUCT_RETAINED;
    product.completionDone      = true;
    product.completionSucceeded = result;
    if (product.executeActive)
    {
      product.state          = PRODUCT_COMPLETING;
      product.completedState = completedState;
    }
    else
    {
      product.state          = completedState;
      product.completedState = PRODUCT_FREE;
    }
    product.restoreState      = PRODUCT_FREE;

    available = m_products[productIndex].state == PRODUCT_FREE;
    if (!available)
      for (unsigned i = 0; i < ARRAYSIZE(m_products); ++i)
        if (i != productIndex &&
            m_products[i].state == PRODUCT_FREE)
        {
          available = true;
          break;
        }
  }
  SignalProductState(available);
}

bool CSoftwareFrameProcessor::EnsureProductResource(
  unsigned productIndex, size_t frameSize)
{
  Product& product = m_products[productIndex];

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width              = frameSize;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  if (product.resource &&
      CFrameProcessorUtil::ResourceDescMatches(
        product.resource->GetDesc(), desc, false))
    return true;

  product.resource.Reset();

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
  heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heapProps.CreationNodeMask     = 1;
  heapProps.VisibleNodeMask      = 1;

  const HRESULT hr = m_dx12->GetDevice()->CreateCommittedResource(
    &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
    D3D12_RESOURCE_STATE_COMMON, nullptr,
    IID_PPV_ARGS(&product.resource));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create retained software frame");
    return false;
  }

  static const WCHAR * names[] =
  {
    L"Software Frame 0",
    L"Software Frame 1",
  };
  product.resource->SetName(names[productIndex]);
  return true;
}

bool CSoftwareFrameProcessor::BeginExecute(unsigned productIndex,
  uint64_t sequence, ProductState state)
{
  CSRWExclusiveLock lock(m_productLock);
  Product& product = m_products[productIndex];
  if (product.sequence != sequence || product.state != state)
    return false;
  product.executeActive       = true;
  product.completionDone      = false;
  product.completionSucceeded = false;
  product.completedState      = PRODUCT_FREE;
  return true;
}

void CSoftwareFrameProcessor::EndExecute(unsigned productIndex,
  uint64_t sequence, bool& completed, bool& succeeded)
{
  bool available = false;
  completed = false;
  succeeded = false;
  {
    CSRWExclusiveLock lock(m_productLock);
    Product& product = m_products[productIndex];
    if (product.sequence != sequence)
      return;

    product.executeActive = false;
    completed = product.completionDone;
    succeeded = product.completionSucceeded;
    if (product.state == PRODUCT_COMPLETING)
    {
      product.state          = product.completedState;
      product.completedState = PRODUCT_FREE;
      available = product.state == PRODUCT_FREE;
    }
  }
  SignalProductState(available);
}

bool CSoftwareFrameProcessor::PrepareBatch(Product& product,
  unsigned productIndex, const FramePlan& plan, uint64_t copyStart)
{
  FramePlan currentPlan = {};
  {
    CSRWSharedLock lock(m_productLock);
    if (product.state == PRODUCT_READY ||
        product.state == PRODUCT_RETAINED)
    {
      currentPlan = plan;
      for (unsigned i = 0; i < currentPlan.count; ++i)
        currentPlan.targets[i].commitSchedule.phaseEligible = false;
    }
  }
  const FramePlan& batchPlan = currentPlan.count ? currentPlan : plan;

  PreparedFrameBatch prepared = {};
  if (!m_transport->PrepareFrameBatch(batchPlan, product.sequence,
        product.pitch, product.frameSize, product.srcFormat,
        product.dstFormat, product.dirtyRects, product.nbDirtyRects,
        true, prepared))
    return false;

  FrameCopyBatch& batch = product.batch;
  batch = {};
  batch.prepared       = prepared;
  batch.candidateIndex = productIndex;

  CPostProcessor& postProcessor = m_postProcessors[productIndex];
  const unsigned bytesPerPixel =
    product.dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  for (unsigned i = 0; i < prepared.count; ++i)
  {
    CFrameBufferResource * fbRes =
      m_frameBuffers.Get(prepared.targets[i], product.frameSize);
    batch.resources[i] = fbRes;
    if (!fbRes)
    {
      m_transport->AbortFrameBatch(prepared.token);
      batch = {};
      DEBUG_ERROR("Failed to get a software frame target resource");
      return false;
    }

    RECT copyDirtyRects[LG_MAX_DIRTY_RECTS * 2] = {};
    unsigned nbCopyDirtyRects = 0;
    const bool fullCopy = CFrameProcessorUtil::BuildCopyDamage(
      postProcessor, prepared.targets[i].fullCopy,
      product.previousDirtyRects, product.nbPreviousDirtyRects,
      product.dirtyRects, product.nbDirtyRects,
      product.dstFormat.width, product.dstFormat.height,
      copyDirtyRects, &nbCopyDirtyRects);
    fbRes->SetTiming(
      product.captureTime, product.postProcessStart, copyStart);
    fbRes->SetCandidateIndex(productIndex);
    fbRes->SetPostProcessSample(product.timingEffectIndex,
      product.timingToken, fullCopy);
    fbRes->SetCopyDamage(copyDirtyRects, nbCopyDirtyRects,
      fullCopy, product.pitch, bytesPerPixel);
    fbRes->ResetCompletion();
  }

  batch.accepted = m_transport->PublishFrameBatch(prepared.token);
  if (!batch.accepted)
  {
    batch = {};
    return false;
  }

  return true;
}

void CSoftwareFrameProcessor::CompleteBatch(CD3D12CommandSlot * slot,
  Product& product, unsigned productIndex, bool publishing, bool result)
{
  const FrameCopyBatch batch = product.batch;
  if (!batch.accepted)
    return;

  for (unsigned i = 0; i < batch.prepared.count; ++i)
    if ((batch.accepted & (1U << i)) && batch.resources[i])
      batch.resources[i]->MarkCompletion();

  if (!result)
  {
    return;
  }

  uint64_t gpuStart = 0;
  uint64_t gpuEnd   = 0;
  const bool gpuTimingValid = slot->GetGPUTimes(gpuStart, gpuEnd);
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
        m_transport->WriteFrameTarget(batch.prepared.token,
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
          m_transport->WriteFrameTargetRows(batch.prepared.token,
            i, fbRes->GetMap(), rowOffset, rowBytes, pitch,
            (unsigned)(rect->bottom - rect->top));
        }
      }
      stagedCopyTime = CFrameScheduler::Nanotime() - stagedCopyStart;
    }

    const uint64_t copyReady = CFrameScheduler::Nanotime();
    m_transport->FinalizeFrameTarget(batch.prepared.token, i);
    const uint64_t publishedAt = CFrameScheduler::Nanotime();

    uint64_t postProcessTime = 0;
    uint64_t copyTime        = 0;
    uint64_t readyTime       = 0;
    uint64_t holdTime        = 0;
    if (!publishing)
    {
      const uint64_t copyStart = fbRes->GetCopyStart();
      postProcessTime = copyStart >= product.postProcessStart ?
        copyStart - product.postProcessStart : 0;
      copyTime = copyReady >= copyStart ?
        copyReady - copyStart : 0;
      if (gpuTimingValid && gpuStart >= product.postProcessStart &&
          gpuEnd >= gpuStart && gpuEnd <= copyReady)
      {
        postProcessTime = gpuStart - product.postProcessStart;
        copyTime        = gpuEnd - gpuStart + stagedCopyTime;
      }
      const uint64_t elapsed = publishedAt >= product.postProcessStart ?
        publishedAt - product.postProcessStart : 0;
      const uint64_t measured = postProcessTime + copyTime;
      readyTime = elapsed > measured ? elapsed - measured : 0;
    }
    else
    {
      const uint64_t publishStart = fbRes->GetCopyStart();
      postProcessTime = product.prepareCopyStart >=
          product.postProcessStart ?
        product.prepareCopyStart - product.postProcessStart : 0;
      uint64_t prepareCopyTime = product.prepareReady >=
          product.prepareCopyStart ?
        product.prepareReady - product.prepareCopyStart : 0;
      if (product.prepareTimingValid &&
          product.prepareGPUStart >= product.postProcessStart &&
          product.prepareGPUEnd >= product.prepareGPUStart &&
          product.prepareGPUEnd <= product.prepareReady)
      {
        postProcessTime =
          product.prepareGPUStart - product.postProcessStart;
        prepareCopyTime =
          product.prepareGPUEnd - product.prepareGPUStart;
      }

      uint64_t publishCopyTime = copyReady >= publishStart ?
        copyReady - publishStart : 0;
      if (gpuTimingValid && gpuStart >= publishStart &&
          gpuEnd >= gpuStart && gpuEnd <= copyReady)
        publishCopyTime = gpuEnd - gpuStart + stagedCopyTime;
      copyTime = prepareCopyTime + publishCopyTime;

      const uint64_t prepareElapsed = product.prepareReady >=
          product.postProcessStart ?
        product.prepareReady - product.postProcessStart : 0;
      const uint64_t prepareMeasured = postProcessTime + prepareCopyTime;
      const uint64_t prepareWait = prepareElapsed > prepareMeasured ?
        prepareElapsed - prepareMeasured : 0;
      const uint64_t publishElapsed = publishedAt >= publishStart ?
        publishedAt - publishStart : 0;
      const uint64_t publishWait = publishElapsed > publishCopyTime ?
        publishElapsed - publishCopyTime : 0;
      readyTime = prepareWait + publishWait;
      holdTime = publishStart >= product.prepareReady ?
        publishStart - product.prepareReady : 0;
    }

    m_transport->SetFrameTargetTiming(batch.prepared.token, i,
      fbRes->GetCaptureTime(), postProcessTime, copyTime, readyTime,
      holdTime, publishedAt);
    m_transport->TryRecordFrameTiming(
      batch.prepared.token, i, publishedAt - fbRes->GetCopyStart());

    if (!timingRecorded && product.timingToken && product.timingStart)
    {
      uint64_t totalTime = 0;
      if (!publishing && copyReady >= product.timingStart)
        totalTime = copyReady - product.timingStart;
      else if (publishing &&
          product.prepareReady >= product.timingStart &&
          copyReady >= fbRes->GetCopyStart())
        totalTime = (product.prepareReady - product.timingStart) +
          (copyReady - fbRes->GetCopyStart());
      if (totalTime)
      {
        m_postProcessors[productIndex].RecordTiming(
          fbRes->GetTimingEffectIndex(), fbRes->GetTimingToken(),
          fbRes->IsFullCopy(), totalTime);
        timingRecorded = true;
      }
    }

    m_transport->CompleteFrameTarget(batch.prepared.token, i, true);
  }
}

void CSoftwareFrameProcessor::CompletionFunction(
  CD3D12CommandSlot * slot, bool result, void * param1, void * param2)
{
  auto processor = static_cast<CSoftwareFrameProcessor *>(param1);
  auto product   = static_cast<Product *>(param2);
  const unsigned productIndex =
    static_cast<unsigned>(product - processor->m_products);

  bool publishing;
  uint64_t sequence;
  {
    CSRWExclusiveLock lock(processor->m_productLock);
    if (product->state == PRODUCT_PUBLISHING)
      publishing = true;
    else if (product->state == PRODUCT_PREPARING)
      publishing = false;
    else
      return;
    sequence = product->sequence;
    if (!publishing)
    {
      product->prepareReady = CFrameScheduler::Nanotime();
      product->prepareTimingValid = result && slot->GetGPUTimes(
        product->prepareGPUStart, product->prepareGPUEnd);
    }
  }

  processor->CompleteBatch(
    slot, *product, productIndex, publishing, result);
  if (!result)
  {
    if (product->batch.accepted)
      processor->m_transport->FailFrameBatch(
        product->batch.prepared.token);
    processor->SetFullDamage();
    processor->m_transport->ForceFrame();
  }
  processor->FinishProduct(productIndex, publishing, result);
  if (result && !publishing)
    processor->MarkSourceReady(productIndex, sequence);
}

bool CSoftwareFrameProcessor::HasReadyFrame() const
{
  uint64_t completeSequence  = 0;
  uint64_t preparingSequence = 0;
  bool ready = false;
  {
    CSRWSharedLock lock(m_productLock);
    for (const Product& product : m_products)
    {
      if ((product.state == PRODUCT_READY ||
           (product.state == PRODUCT_RETAINED &&
            product.productNotified)) &&
          product.sequence > completeSequence)
        completeSequence = product.sequence;
      else if ((product.state == PRODUCT_PREPARING ||
          product.state == PRODUCT_COMPLETING) &&
          product.sequence > preparingSequence)
        preparingSequence = product.sequence;
    }
    if (completeSequence && preparingSequence <= completeSequence)
      for (const Product& product : m_products)
        if (product.state == PRODUCT_READY &&
            product.sequence == completeSequence)
        {
          ready = true;
          break;
        }
  }
  if (!completeSequence || preparingSequence > completeSequence)
    return false;
  return ready || m_transport->NeedsFrame();
}

bool CSoftwareFrameProcessor::Publish(
  const FramePlan& plan, uint64_t publishStart)
{
  CSRWSharedLock pipelineLock(*m_pipelineLock);

  int selected = -1;
  uint64_t newestSequence = 0;
  ProductState restoreState = PRODUCT_FREE;
  {
    CSRWExclusiveLock lock(m_productLock);
    for (unsigned i = 0; i < ARRAYSIZE(m_products); ++i)
      if (m_products[i].state == PRODUCT_READY &&
          (selected < 0 || m_products[i].sequence > newestSequence))
      {
        selected       = static_cast<int>(i);
        newestSequence = m_products[i].sequence;
      }

    if (selected < 0 && m_transport->NeedsFrame())
      for (unsigned i = 0; i < ARRAYSIZE(m_products); ++i)
        if (m_products[i].state == PRODUCT_RETAINED &&
            m_products[i].productNotified &&
            (selected < 0 || m_products[i].sequence > newestSequence))
        {
          selected       = static_cast<int>(i);
          newestSequence = m_products[i].sequence;
        }

    for (const Product& product : m_products)
      if ((product.state == PRODUCT_PREPARING ||
           product.state == PRODUCT_COMPLETING) &&
          product.sequence > newestSequence)
      {
        selected = -1;
        break;
      }

    if (selected >= 0)
    {
      Product& product = m_products[static_cast<unsigned>(selected)];
      restoreState         = product.state;
      product.restoreState = product.state;
      product.state        = PRODUCT_PUBLISHING;
      product.batch        = {};
    }
  }

  if (selected < 0)
    return false;
  const unsigned productIndex = static_cast<unsigned>(selected);
  Product& product = m_products[productIndex];
  const uint64_t sequence = product.sequence;

  const auto restoreProduct =
    [this, productIndex, sequence, restoreState]()
    {
      bool available = false;
      {
        CSRWExclusiveLock lock(m_productLock);
        Product& selected = m_products[productIndex];
        if (selected.state == PRODUCT_PUBLISHING &&
            selected.sequence == sequence)
        {
          selected.state        = restoreState;
          selected.restoreState = PRODUCT_FREE;
          selected.batch        = {};
          available = restoreState == PRODUCT_FREE;
        }
      }
      SignalProductState(available);
    };

  CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot(productIndex);
  if (!copySlot)
  {
    restoreProduct();
    return false;
  }

  if (!PrepareBatch(product, productIndex, plan, publishStart))
  {
    copySlot->Cancel();
    restoreProduct();
    return false;
  }

  const FrameBatchToken token = product.batch.prepared.token;
  copySlot->SetCompletionCallback(
    &CompletionFunction, this, &product);
  copySlot->BeginTiming();
  CPostProcessor& postProcessor = m_postProcessors[productIndex];
  for (unsigned i = 0; i < product.batch.prepared.count; ++i)
    if (product.batch.accepted & (1U << i))
    {
      CFrameBufferResource * fbRes = product.batch.resources[i];
      postProcessor.CopyFromCandidate(copySlot->GetGfxList(),
        fbRes->Get().Get(), product.resource.Get(),
        fbRes->GetCopyDirtyRects(), fbRes->GetCopyDirtyRectCount(),
        fbRes->IsFullCopy());
    }
  copySlot->EndTiming();

  if (!BeginExecute(productIndex, sequence, PRODUCT_PUBLISHING))
  {
    copySlot->Cancel();
    m_transport->FailFrameBatch(token);
    return false;
  }
  const bool executed = copySlot->Execute();
  const bool submittedWork = !executed && copySlot->HasSubmittedWork();
  bool completionDone;
  bool completionSucceeded;
  EndExecute(productIndex, sequence,
    completionDone, completionSucceeded);
  if (!executed)
  {
    if (submittedWork || (completionDone && completionSucceeded))
      m_transport->CommitFrameBatch(token);
    else if (!completionDone)
    {
      m_transport->FailFrameBatch(token);
      restoreProduct();
    }
    SetFullDamage();
    m_transport->ForceFrame();
    return false;
  }

  if (completionDone && !completionSucceeded)
    return false;
  m_transport->CommitFrameBatch(token);
  return true;
}

bool CSoftwareFrameProcessor::Submit(const FrameSubmission& submission)
{
  if (submission.noImageUpdate && !HasPendingDamage())
    return true;

  const int selected = submission.noImageUpdate ?
    WaitForProduct() : AcquireProduct();
  if (selected == -2)
    return false;
  if (selected < 0)
  {
    if (WaitForSingleObject(m_terminateEvent, 0) == WAIT_OBJECT_0)
      return true;
    if (!submission.noImageUpdate)
      m_transport->FrameSuperseded();
    return submission.noImageUpdate;
  }
  const unsigned productIndex = static_cast<unsigned>(selected);
  Product& product = m_products[productIndex];

  CSRWSharedLock pipelineLock(*m_pipelineLock);
  CPostProcessor& postProcessor = m_postProcessors[productIndex];
  const D12FrameFormat& dstFormat = postProcessor.GetOutputFormat();
  const unsigned pitch            = postProcessor.GetOutputPitch();
  const size_t frameSize          = postProcessor.GetOutputSize();
  if (!pitch || !frameSize ||
      frameSize > m_transport->GetMaxFrameSize())
  {
    RestoreProduct(productIndex, PRODUCT_FREE);
    DEBUG_ERROR("Software frame does not fit in primary frame memory");
    SetFullDamage();
    return false;
  }

  RECT currentDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbDirtyRects = 0;
  const bool hasDamage =
    TakePendingDamage(currentDirtyRects, &nbDirtyRects);
  CFrameProcessorUtil::ClipDirtyRects(currentDirtyRects,
    &nbDirtyRects, dstFormat.width, dstFormat.height);

  RECT previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
  unsigned nbPreviousDirtyRects = 0;
  GetPreviousDamage(previousDirtyRects, &nbPreviousDirtyRects);

  if (!EnsureProductResource(productIndex, frameSize))
  {
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    RestoreProduct(productIndex, PRODUCT_FREE);
    SetFullDamage();
    return false;
  }

  CD3D12CommandSlot * copySlot = m_dx12->GetCopySlot(productIndex);
  if (!copySlot)
  {
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    RestoreProduct(productIndex, PRODUCT_FREE);
    if (!submission.noImageUpdate)
      m_transport->FrameSuperseded();
    return true;
  }

  if (!submission.source->Signal() || !submission.source->Sync(*copySlot))
  {
    copySlot->Cancel();
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    RestoreProduct(productIndex, PRODUCT_FREE);
    SetFullDamage();
    return false;
  }

  product.srcFormat          = submission.sourceFormat;
  product.dstFormat          = dstFormat;
  product.nbDirtyRects       = nbDirtyRects;
  product.nbPreviousDirtyRects = nbPreviousDirtyRects;
  product.pitch              = pitch;
  product.frameSize          = frameSize;
  product.captureTime        = submission.captureTime;
  product.postProcessStart   = submission.postProcessStart;
  product.prepareCopyStart   = CFrameScheduler::Nanotime();
  product.prepareReady       = 0;
  product.prepareGPUStart    = 0;
  product.prepareGPUEnd      = 0;
  product.timingStart        = submission.timingToken ?
    CFrameScheduler::Nanotime() : 0;
  product.timingEffectIndex  = submission.timingEffectIndex;
  product.timingToken        = submission.timingToken;
  product.prepareTimingValid = false;
  if (nbDirtyRects)
    memcpy(product.dirtyRects, currentDirtyRects,
      nbDirtyRects * sizeof(*product.dirtyRects));
  if (nbPreviousDirtyRects)
    memcpy(product.previousDirtyRects, previousDirtyRects,
      nbPreviousDirtyRects * sizeof(*product.previousDirtyRects));

  FramePlan plan = {};
  const uint64_t now = CFrameScheduler::Nanotime();
  if (m_transport->GetImmediateFramePlan(now, plan))
    PrepareBatch(product, productIndex, plan,
      product.prepareCopyStart);

  const FrameBatchToken token = product.batch.prepared.token;
  const bool hasBatch = product.batch.accepted != 0;
  const uint64_t sequence = product.sequence;
  bool productValid;
  {
    CSRWExclusiveLock lock(m_productLock);
    productValid = product.state == PRODUCT_PREPARING &&
      product.sequence == sequence;
    if (productValid)
      product.batchCommitted = !hasBatch;
  }
  if (!productValid)
  {
    copySlot->Cancel();
    if (hasBatch)
      m_transport->FailFrameBatch(token);
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    return false;
  }
  copySlot->SetCompletionCallback(
    &CompletionFunction, this, &product);
  copySlot->BeginTiming();
  if (hasBatch)
    for (unsigned i = 0; i < product.batch.prepared.count; ++i)
      if (product.batch.accepted & (1U << i))
      {
        CFrameBufferResource * fbRes = product.batch.resources[i];
        postProcessor.CopyToFrameBuffer(copySlot->GetGfxList(),
          fbRes->Get().Get(), submission.source->GetRes().Get(),
          fbRes->GetCopyDirtyRects(), fbRes->GetCopyDirtyRectCount(),
          fbRes->IsFullCopy());
      }
  postProcessor.CopyToCandidate(copySlot->GetGfxList(),
    product.resource.Get(), submission.source->GetRes().Get());
  copySlot->EndTiming();

  if (!BeginExecute(productIndex, sequence, PRODUCT_PREPARING))
  {
    copySlot->Cancel();
    if (hasBatch)
      m_transport->FailFrameBatch(token);
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    return false;
  }
  const bool executed = copySlot->Execute();
  const bool submittedWork = !executed && copySlot->HasSubmittedWork();
  bool completionDone;
  bool completionSucceeded;
  EndExecute(productIndex, sequence,
    completionDone, completionSucceeded);
  if (!executed)
  {
    if (submittedWork || (completionDone && completionSucceeded))
    {
      if (hasBatch)
        m_transport->CommitFrameBatch(token);
      MarkBatchCommitted(productIndex, sequence);
      CommitDamage(currentDirtyRects, nbDirtyRects);
    }
    else if (!completionDone)
    {
      if (hasBatch)
        m_transport->FailFrameBatch(token);
      RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
      {
        CSRWExclusiveLock lock(m_productLock);
        if (product.state == PRODUCT_PREPARING &&
            product.sequence == sequence)
        {
          product.state        = PRODUCT_FREE;
          product.restoreState = PRODUCT_FREE;
          product.batch        = {};
        }
      }
      SignalProductState(true);
    }
    SetFullDamage();
    m_transport->ForceFrame();
    return false;
  }

  if (completionDone && !completionSucceeded)
  {
    RestorePendingDamage(currentDirtyRects, nbDirtyRects, hasDamage);
    return false;
  }
  if (hasBatch)
    m_transport->CommitFrameBatch(token);
  MarkBatchCommitted(productIndex, sequence);
  CommitDamage(currentDirtyRects, nbDirtyRects);
  return true;
}

void CSoftwareFrameProcessor::ResetProducts()
{
  {
    CSRWExclusiveLock lock(m_productLock);
    for (Product& product : m_products)
      product = {};
  }
  SignalProductState(true);
}

void CSoftwareFrameProcessor::Reset()
{
  ResetProducts();
  CFrameProcessor::Reset();
}

void CSoftwareFrameProcessor::ResetPipeline()
{
  ResetProducts();
  CFrameProcessor::Invalidate();
}
