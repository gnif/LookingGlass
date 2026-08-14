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

#include <atomic>

class CSoftwareFrameProcessor final : public CFrameProcessor
{
private:
  struct CopyContext
  {
    FrameCopyBatch    batch;
    uint64_t          timingStart         = 0;
    std::atomic<bool> completionDone      = false;
    std::atomic<bool> completionSucceeded = false;
  };

  // The unindexed copy-slot API permits exactly one in-flight submission.
  CopyContext m_copy;

  static void CompletionFunction(
    CD3D12CommandSlot * slot, bool result, void * param1, void * param2);

public:
  CSoftwareFrameProcessor(IFrameTransport * transport,
    std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[CAPTURE_PIPELINE_SLOTS],
    CSRWLock * pipelineLock, HANDLE terminateEvent);

  bool Submit(const FrameSubmission& submission) override;
  bool HasReadyFrame() const override { return false; }
  bool Publish(const FramePlan&, uint64_t) override { return false; }
  bool UsesCadence() const override { return false; }
};
