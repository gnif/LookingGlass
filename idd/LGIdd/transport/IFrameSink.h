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

#include "postprocess/D12FrameFormat.h"
#include "capture/CFrameScheduler.h"
#include "transport/PreparedFrameBuffer.h"

#include <Windows.h>
#include <stddef.h>
#include <stdint.h>

class IFrameSink
{
public:
  virtual ~IFrameSink() = default;

  virtual size_t GetMaxFrameSize() const = 0;

  virtual bool FrameBufferAvailable(
    const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) = 0;
  virtual bool HasPublishedFrame() const = 0;
  virtual void ProcessDeliveries() = 0;
  virtual bool GetPendingDeliveryTarget(
    uint64_t now, uint64_t& target) = 0;
  virtual bool RetryPendingDelivery(uint64_t now, bool& retry) = 0;
  virtual SinkTarget PrepareFrameBuffer(unsigned pitch,
    const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
    const RECT * dirtyRects, unsigned nbDirtyRects,
    const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) = 0;
  virtual bool PublishFrameBuffer(unsigned slot,
    const CFrameScheduler::Schedule& schedule,
    bool& deliveredToOwner) = 0;
  virtual bool RepublishFrameBuffer(
    const CFrameScheduler::Schedule& schedule) = 0;
  virtual bool TryFrameSubmitted(unsigned slot,
    const CFrameScheduler::Schedule& schedule) = 0;
  virtual void CommitFrameBuffer(unsigned slot,
    const CFrameScheduler::Schedule& schedule, bool periodic,
    bool deliveredToOwner) = 0;
  virtual void AbortFrameBuffer(unsigned slot) = 0;
  virtual void FailFrameBuffer(unsigned slot) = 0;
  virtual void CompleteFrameBuffer(unsigned slot, bool succeeded) = 0;
  virtual void SetFrameTiming(unsigned slot, uint64_t captureTime,
    uint64_t postProcessTime, uint64_t copyTime, uint64_t readyTime,
    uint64_t holdTime, const CFrameScheduler::Schedule& schedule,
    uint64_t completedAt) = 0;
  virtual void WriteFrameBuffer(unsigned slot, void * src,
    size_t offset, size_t len, bool setWritePos) const = 0;
  virtual void WriteFrameBufferRows(unsigned slot, void * src,
    size_t offset, size_t rowBytes, size_t pitch,
    unsigned rows) const = 0;
  virtual void FinalizeFrameBuffer(unsigned slot) const = 0;

  virtual void ObserveFrame(uint64_t now) = 0;
  virtual void ForceFrame() = 0;
  virtual bool GetPublishTarget(uint64_t now, uint64_t& target,
    CFrameScheduler::Schedule& schedule, bool& periodic,
    bool& republish) = 0;
  virtual void FrameMissed(const CFrameScheduler::Schedule& schedule,
    uint64_t now, bool periodic) = 0;
  virtual void FrameSuperseded() = 0;
  virtual HANDLE GetFrameScheduleEvent() const = 0;
  virtual void SetFrameScheduleEvent(HANDLE event) = 0;
  virtual void TryRecordFrameTiming(uint64_t duration) = 0;
};
