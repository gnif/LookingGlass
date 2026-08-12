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

#include "CSRWLock.h"
#include "transport/IFrameSink.h"
#include "transport/IFrameTransport.h"
#include "transport/ITransport.h"

#include <vector>

class CFrameHub final : public IFrameTransport
{
private:
  static const unsigned MAX_SLOTS = 1024;

  struct Slot
  {
    uint64_t serial             = 0;
    bool     committed          = false;
    bool     completionPending  = false;
    bool     completionSucceeded = false;
  };

  mutable CSRWLock m_lock;
  IFrameSink     * m_sink       = nullptr;
  BackendId        m_backend    = 0;
  uint32_t         m_epoch      = 0;
  uint64_t         m_nextSerial = 0;
  std::vector<Slot> m_slots;

  bool Valid(const FrameToken& token) const;
  void Invalidate(const FrameToken& token);

public:
  bool Bind(BackendId backend, uint32_t epoch, IFrameSink& sink);
  void Unbind(BackendId backend, uint32_t epoch);

  size_t GetMaxFrameSize() const override;
  bool FrameBufferAvailable(const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) override;
  bool HasPublishedFrame() const override;
  void ProcessDeliveries() override;
  bool GetPendingDeliveryTarget(uint64_t now, uint64_t& target) override;
  bool RetryPendingDelivery(uint64_t now, bool& retry) override;
  PreparedFrameBuffer PrepareFrameBuffer(unsigned pitch,
    const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
    const RECT * dirtyRects, unsigned nbDirtyRects,
    const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) override;
  bool PublishFrameBuffer(const FrameToken& token,
    const CFrameScheduler::Schedule& schedule,
    bool& deliveredToOwner) override;
  bool RepublishFrameBuffer(
    const CFrameScheduler::Schedule& schedule) override;
  bool TryFrameSubmitted(const FrameToken& token,
    const CFrameScheduler::Schedule& schedule) override;
  void CommitFrameBuffer(const FrameToken& token,
    const CFrameScheduler::Schedule& schedule, bool periodic,
    bool deliveredToOwner) override;
  void AbortFrameBuffer(const FrameToken& token) override;
  void FailFrameBuffer(const FrameToken& token) override;
  void CompleteFrameBuffer(
    const FrameToken& token, bool succeeded) override;
  void SetFrameTiming(const FrameToken& token, uint64_t captureTime,
    uint64_t postProcessTime, uint64_t copyTime, uint64_t readyTime,
    uint64_t holdTime, const CFrameScheduler::Schedule& schedule,
    uint64_t completedAt) override;
  void WriteFrameBuffer(const FrameToken& token, void * src,
    size_t offset, size_t len, bool setWritePos) const override;
  void WriteFrameBufferRows(const FrameToken& token, void * src,
    size_t offset, size_t rowBytes, size_t pitch,
    unsigned rows) const override;
  void FinalizeFrameBuffer(const FrameToken& token) const override;

  void ObserveFrame(uint64_t now) override;
  void ForceFrame() override;
  bool GetPublishTarget(uint64_t now, uint64_t& target,
    CFrameScheduler::Schedule& schedule, bool& periodic,
    bool& republish) override;
  void FrameMissed(const CFrameScheduler::Schedule& schedule,
    uint64_t now, bool periodic) override;
  void FrameSuperseded() override;
  HANDLE GetFrameScheduleEvent() const override;
  void TryRecordFrameTiming(
    const FrameToken& token, uint64_t duration) override;
};
