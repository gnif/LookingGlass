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

class CSoftwareFrameProcessor final : public CFrameProcessor
{
private:
  enum ProductState
  {
    PRODUCT_FREE,
    PRODUCT_PREPARING,
    PRODUCT_READY,
    PRODUCT_PUBLISHING,
    PRODUCT_RETAINED,
    PRODUCT_COMPLETING,
  };

  struct Product
  {
    ProductState           state        = PRODUCT_FREE;
    ProductState           restoreState = PRODUCT_FREE;
    ProductState           completedState = PRODUCT_FREE;
    ComPtr<ID3D12Resource> resource;
    D12FrameFormat         srcFormat    = {};
    D12FrameFormat         dstFormat    = {};
    RECT                   dirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned               nbDirtyRects = 0;
    RECT                   previousDirtyRects[LG_MAX_DIRTY_RECTS] = {};
    unsigned               nbPreviousDirtyRects = 0;
    unsigned               pitch        = 0;
    size_t                 frameSize    = 0;
    uint64_t               sequence     = 0;
    uint64_t               captureTime  = 0;
    uint64_t               postProcessStart = 0;
    uint64_t               prepareCopyStart = 0;
    uint64_t               prepareReady = 0;
    uint64_t               prepareGPUStart = 0;
    uint64_t               prepareGPUEnd = 0;
    uint64_t               timingStart  = 0;
    unsigned               timingEffectIndex = 0;
    uint64_t               timingToken  = 0;
    bool                   prepareTimingValid = false;
    bool                   sourceReady = false;
    bool                   batchCommitted = false;
    bool                   productNotified = false;
    bool                   executeActive = false;
    bool                   completionDone = false;
    bool                   completionSucceeded = false;
    FrameCopyBatch         batch        = {};
  };

  Product          m_products[CAPTURE_PIPELINE_SLOTS];
  mutable CSRWLock m_productLock;
  Wrappers::Event  m_productAvailableEvent;

  static void CompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);
  int AcquireProduct();
  int WaitForProduct();
  void RestoreProduct(unsigned productIndex, ProductState state);
  void FinishProduct(
    unsigned productIndex, bool publishing, bool result);
  void MarkSourceReady(unsigned productIndex, uint64_t sequence);
  void MarkBatchCommitted(unsigned productIndex, uint64_t sequence);
  bool EnsureProductResource(unsigned productIndex, size_t frameSize);
  bool BeginExecute(unsigned productIndex, uint64_t sequence,
    ProductState state);
  void EndExecute(unsigned productIndex, uint64_t sequence,
    bool& completed, bool& succeeded);
  bool PrepareBatch(Product& product, unsigned productIndex,
    const FramePlan& plan, uint64_t copyStart);
  void CompleteBatch(CD3D12CommandSlot * slot, Product& product,
    unsigned productIndex, bool publishing, bool result);
  void ResetProducts();
  void SignalProductState(bool available = false);

public:
  CSoftwareFrameProcessor(IFrameTransport * transport,
    std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[CAPTURE_PIPELINE_SLOTS],
    CSRWLock * pipelineLock, HANDLE terminateEvent);

  bool Submit(const FrameSubmission& submission) override;
  bool HasReadyFrame() const override;
  bool Publish(const FramePlan& plan, uint64_t publishStart) override;
  bool UsesCadence() const override { return false; }
  bool IsValid() const override;
  void Reset() override;
  void ResetPipeline() override;
};
