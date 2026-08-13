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

#include "Atomic.h"
#include "CSRWLock.h"
#include "transport/IFrameSink.h"
#include "transport/IFrameTransport.h"
#include "transport/ITransport.h"

class CFrameHub final : public IFrameTransport, public IFrameEvents
{
private:
  struct Sink
  {
    struct ResourceLane
    {
      enum Phase
      {
        IDLE,
        FILLING,
        PENDING,
        CANCELING,
        COMPLETING,
      };

      uint8_t  * mem        = nullptr;
      uint64_t   heapOffset = 0;
      unsigned   localSlot  = 0;
      bool       direct     = false;
      bool       valid      = false;
      bool       busy       = false;

      FrameToken                 token       = {};
      CFrameScheduler::Schedule schedule    = {};
      uint64_t                   content     = 0;
      uint64_t                   captureTime = 0;
      uint64_t                   postProcessTime = 0;
      uint64_t                   copyTime    = 0;
      uint64_t                   readyTime   = 0;
      uint64_t                   holdTime    = 0;
      uint64_t                   filledAt    = 0;
      uint64_t                   workStart   = 0;
      unsigned                   pitch       = 0;
      unsigned                   width       = 0;
      unsigned                   height      = 0;
      DXGI_FORMAT                format      = DXGI_FORMAT_UNKNOWN;
      FrameType                  frameType   = FRAME_TYPE_INVALID;
      Phase                      phase       = IDLE;
      FrameDone                  pendingResult = FrameDone::FAILED;
      uint64_t                   pendingReadyAt = 0;
      bool                       timingValid = false;
      bool                       resultPending = false;
      bool                       callActive  = false;
      bool                       cancelRequested = false;
    };

    CSRWLock          callLock;
    CSRWLock          laneLock;
    IFrameSink      * target        = nullptr;
    HANDLE            drained       = nullptr;
    std::atomic<unsigned> outstanding = 0;
    std::atomic_bool   active        = false;
    std::atomic<BackendId> backend   = 0;
    std::atomic<uint32_t> epoch      = 0;
    bool               reserved      = false;
    bool               primary       = false;
    bool               needsFullCopy = true;
    uint64_t           lastContent   = 0;
    uint64_t           lastTerminalContent = 0;
    uint64_t           blockedContent = 0;
    size_t             blockedFrameSize = 0;
    bool               blockedAllocation = false;
    unsigned           pitch         = 0;
    unsigned           width         = 0;
    unsigned           height        = 0;
    DXGI_FORMAT        format        = DXGI_FORMAT_UNKNOWN;
    FrameType          frameType     = FRAME_TYPE_INVALID;
    ResourceLane       lanes[FRAME_SINK_BUFFERS] = {};
  };

  struct BatchTarget
  {
    Sink                    * sink       = nullptr;
    BackendId                 backend    = 0;
    uint32_t                  epoch      = 0;
    unsigned                  localSlot  = 0;
    unsigned                  resourceLane = 0;
    CFrameScheduler::Schedule schedule  = {};
    CFrameScheduler::Schedule deliverySchedule = {};
    uint64_t                  content    = 0;
    unsigned                  pitch      = 0;
    unsigned                  width      = 0;
    unsigned                  height     = 0;
    DXGI_FORMAT               format     = DXGI_FORMAT_UNKNOWN;
    FrameType                 frameType  = FRAME_TYPE_INVALID;
    bool                      periodic   = false;
    bool                      published  = false;
    bool                      delivered  = false;
    bool                      submitted  = false;
    bool                      committed  = false;
    bool                      completionPending   = false;
    bool                      completionSucceeded = false;
    bool                      releasePending = false;
    uint64_t                  captureTime = 0;
    uint64_t                  postProcessTime = 0;
    uint64_t                  copyTime    = 0;
    uint64_t                  readyTime   = 0;
    uint64_t                  holdTime    = 0;
    uint64_t                  filledAt    = 0;
    uint64_t                  workStart   = 0;
    bool                      timingValid = false;
    bool                      active      = false;
  };

  struct Batch
  {
    CSRWLock    lock;
    uint64_t    serial = 0;
    unsigned    count  = 0;
    bool        active = false;
    BatchTarget targets[FRAME_MAX_SINKS] = {};
  };

  struct SinkRef
  {
    Sink     * sink;
    unsigned   index;
    bool       primary;
  };

  mutable CSRWLock m_listLock;
  Sink             m_sinks[FRAME_MAX_SINKS];
  Batch            m_batches[FRAME_BATCHES];
  std::atomic<uint64_t> m_nextSerial = 0;
  std::atomic<uint64_t> m_nextContent = 0;
  std::atomic<uint64_t> m_newestContent = 0;
  HANDLE           m_wakeEvent      = nullptr;

  unsigned Snapshot(SinkRef refs[FRAME_MAX_SINKS]) const;
  bool BatchValid(const Batch& batch, const FrameBatchToken& token) const;
  void ReleaseTarget(Batch& batch, BatchTarget& target);
  bool DetachTarget(Batch& batch, BatchTarget& target,
    FrameToken& token);
  void FillLane(Sink& sink, unsigned laneIndex,
    const FrameToken& token);
  void CancelLane(Sink& sink, unsigned laneIndex,
    const FrameToken& token);
  void CompleteLane(Sink& sink, unsigned laneIndex,
    const FrameToken& token, FrameDone result, uint64_t readyAt);

public:
  CFrameHub();
  ~CFrameHub() override;

  CFrameHub(const CFrameHub&) = delete;
  CFrameHub& operator=(const CFrameHub&) = delete;

  bool Bind(BackendId backend, uint32_t epoch, bool primary,
    IFrameSink& sink);
  void Unbind(BackendId backend, uint32_t epoch);

  void OnFrameDone(const FrameToken& token, FrameDone result,
    uint64_t readyAt) override;

  size_t GetMaxFrameSize() const override;
  uint64_t NextContentSerial() override;
  void FrameProductReady(uint64_t contentSerial) override;
  bool NeedsFrame() const override;
  bool GetFramePlan(
    uint64_t now, bool productReady, FramePlan& plan) override;
  bool GetImmediateFramePlan(
    uint64_t now, FramePlan& plan) override;
  void MissFramePlan(const FramePlan& plan, uint64_t now) override;
  bool PrepareFrameBatch(const FramePlan& plan,
    uint64_t contentSerial, unsigned pitch, size_t frameSize,
    const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
    const RECT * dirtyRects, unsigned nbDirtyRects,
    bool allowReadyReplacement, PreparedFrameBatch& batch) override;
  uint32_t PublishFrameBatch(const FrameBatchToken& token) override;
  void CommitFrameBatch(const FrameBatchToken& token) override;
  void AbortFrameBatch(const FrameBatchToken& token) override;
  void FailFrameBatch(const FrameBatchToken& token) override;
  void WriteFrameTarget(const FrameBatchToken& token,
    unsigned target, void * src, size_t offset, size_t len,
    bool setWritePos) const override;
  void WriteFrameTargetRows(const FrameBatchToken& token,
    unsigned target, void * src, size_t offset, size_t rowBytes,
    size_t pitch, unsigned rows) const override;
  void FinalizeFrameTarget(const FrameBatchToken& token,
    unsigned target) const override;
  void SetFrameTargetTiming(const FrameBatchToken& token,
    unsigned target, uint64_t captureTime, uint64_t postProcessTime,
    uint64_t copyTime, uint64_t readyTime, uint64_t holdTime,
    uint64_t completedAt) override;
  void TryRecordFrameTiming(const FrameBatchToken& token,
    unsigned target, uint64_t duration) override;
  void CompleteFrameTarget(const FrameBatchToken& token,
    unsigned target, bool succeeded) override;
  void ObserveFrame(uint64_t now) override;
  void ForceFrame() override;
  void FrameSuperseded() override;
  HANDLE GetFrameScheduleEvent() const override { return m_wakeEvent; }
};
