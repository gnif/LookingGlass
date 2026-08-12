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

#include <Windows.h>
#include <atomic>
#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
}

#include "common/KVMFR.h"
#include "capture/CFrameScheduler.h"
#include "capture/FramePipeline.h"
#include "transport/FrameCaps.h"
#include "transport/IFrameSink.h"

#include <memory>

class CIVSHMEM;
class CLGMPHost;
class CLGMPTransport;
struct LGMPBuffer;

class CLGMPFrameTransport final : public IFrameSink
{
private:
  friend class CLGMPTransport;

  struct SubscriberSnapshot
  {
    uint32_t    clientIDs     [LGMP_MAX_CLIENTS] = {};
    uint32_t    ownerClientIDs[LGMP_MAX_CLIENTS] = {};
    unsigned    clientCount                      = 0;
    unsigned    ownerClientCount                 = 0;
    LGMP_STATUS status                           = LGMP_OK;
  };

private:
  enum SharedFramePostResult
  {
    SHARED_FRAME_FAILED,
    SHARED_FRAME_IDLE,
    SHARED_FRAME_POSTED,
  };

  struct FrameDelivery
  {
    uint64_t sharedOwnerToken    = 0;
    unsigned ownerQueueMask      = 0;
    uint32_t sharedOwnerClientID = 0;
    bool     sharedOwnerPending  = false;
    bool     sharedPending       = false;
  };

  struct OwnerDelivery
  {
    uint64_t token      = 0;
    uint32_t clientID   = 0;
    unsigned frameIndex = 0;
    bool     active     = false;
  };

  CLGMPHost& m_host;
  CIVSHMEM&  m_ivshmem;

  PLGMPHostQueue m_frameQueue                        = nullptr;
  PLGMPHostQueue m_frameOwnerQueue[LGMP_Q_FRAME_LEN] = {};

  CFrameScheduler m_frameScheduler;

  size_t m_alignSize         = 0;
  size_t m_frameMemoryOffset = 0;
  size_t m_maxFrameSize      = 0;

  // LGMP publication precedes copy completion. Replay only completed frames;
  // the deferred index tracks the newest frame still owed to the owner.
  std::atomic<LONG> m_submittedFrameIndex     = -1;
  std::atomic<LONG> m_readyFrameIndex         = -1;
  LONG              m_deferredOwnerFrameIndex = -1;
  std::atomic<bool> m_frameInFlight[LGMP_Q_FRAME_BUFFER_LEN] = {};
  bool              m_frameCompleted[LGMP_Q_FRAME_BUFFER_LEN] = {};
  CSRWLock          m_framePublishLock;
  uint64_t          m_framePublishSequence = 0;
  uint64_t          m_frameReadySequence   = 0;
  uint64_t          m_frameLastPublishSequence[LGMP_Q_FRAME_BUFFER_LEN] = {};
  CFrameScheduler::Schedule
                    m_frameSchedule[LGMP_Q_FRAME_BUFFER_LEN] = {};
  bool              m_frameDelivered[LGMP_Q_FRAME_BUFFER_LEN] = {};

  FrameDelivery m_frameDelivery[LGMP_Q_FRAME_BUFFER_LEN] = {};
  OwnerDelivery m_ownerDelivery[LGMP_Q_FRAME_LEN]        = {};
  uint32_t      m_formatVer                              = 0;
  uint32_t      m_frameSerial                            = 0;
  PLGMPMemory   m_frameMemory[LGMP_Q_FRAME_BUFFER_LEN]   = {};
  KVMFRFrame  * m_frame      [LGMP_Q_FRAME_BUFFER_LEN]   = {};
  LGMPBuffer  * m_frameBuffer[LGMP_Q_FRAME_BUFFER_LEN]   = {};

  unsigned    m_width       = 0;
  unsigned    m_height      = 0;
  unsigned    m_frameWidth  = 0;
  unsigned    m_frameHeight = 0;
  unsigned    m_pitch       = 0;
  DXGI_FORMAT m_format      = DXGI_FORMAT_UNKNOWN;
  FrameType   m_frameType   = FRAME_TYPE_INVALID;

  // Previous HDR metadata used to detect changes for formatVer bumps.
  uint16_t m_lastHDRDisplayPrimary[3][2]      = {};
  uint16_t m_lastHDRWhitePoint[2]             = {};
  uint32_t m_lastHDRMaxDisplayLuminance       = 0;
  uint32_t m_lastHDRMinDisplayLuminance       = 0;
  uint32_t m_lastHDRMaxContentLightLevel      = 0;
  uint32_t m_lastHDRMaxFrameAverageLightLevel = 0;
  uint32_t m_lastSDRWhiteLevel                = 0;
  bool     m_lastHDRActive                    = false;
  bool     m_lastHDRMetadata                  = false;

  void ProcessFrameDeliveries();
  bool FrameBufferReferenced(unsigned frameIndex) const;
  int FindAvailableFrameBuffer(bool allowReady) const;
  int FindNewestCompletedFrame(unsigned excludeFrameIndex) const;
  int FindAvailableOwnerQueue(unsigned preferredIndex) const;
  unsigned CountOwnerDeliveries(uint32_t clientID) const;
  bool HasMatchingOwnerDelivery(uint32_t clientID, unsigned frameIndex,
    uint64_t token) const;
  SharedFramePostResult PostSharedFrame(unsigned frameIndex,
    uint32_t excludeClientID, uint64_t now);
  bool PostSharedOwnerFrame(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule);
  CLGMPFrameTransport(CLGMPHost& host, CIVSHMEM& ivshmem);
  bool Initialize();
  void SealMemoryLayout();
  bool Setup(size_t alignSize);
  void DeInit();
  std::shared_ptr<const FrameCaps> GetFrameCaps() const;
  SubscriberSnapshot SnapshotSubscribers() const;
  void FinalizeSubscribers(
    const SubscriberSnapshot& snapshot, uint64_t now);
  bool UpdateSchedule(uint32_t sourceClientID,
    const FrameScheduleUpdate& schedule, uint64_t now);

public:
  ~CLGMPFrameTransport() override;

  CLGMPFrameTransport(const CLGMPFrameTransport&) = delete;
  CLGMPFrameTransport& operator=(const CLGMPFrameTransport&) = delete;

  void SetFrameEvents(IFrameEvents *) override {}
  size_t GetMaxFrameSize() const override { return m_maxFrameSize; }

  bool FrameBufferAvailable(const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) override;
  bool HasPublishedFrame() const override
  {
    return m_readyFrameIndex.load(std::memory_order_acquire) >= 0;
  }
  void ProcessDeliveries() override;
  bool GetPendingDeliveryTarget(
    uint64_t now, uint64_t& target) override;
  bool RetryPendingDelivery(uint64_t now, bool& retry) override;
  SinkTarget PrepareFrameBuffer(unsigned pitch,
    const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
    const RECT * dirtyRects, unsigned nbDirtyRects,
    const CFrameScheduler::Schedule& schedule,
    bool allowReadyReplacement = true) override;
  bool PublishFrameBuffer(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule,
    bool& deliveredToOwner) override;
  bool RepublishFrameBuffer(
    const CFrameScheduler::Schedule& schedule) override;
  bool TryFrameSubmitted(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule) override;
  void CommitFrameBuffer(unsigned frameIndex,
    const CFrameScheduler::Schedule& schedule, bool periodic,
    bool deliveredToOwner) override;
  void AbortFrameBuffer(unsigned frameIndex) override;
  void FailFrameBuffer(unsigned frameIndex) override;
  FrameFill FrameFilled(const FrameToken&) override
  {
    return FrameFill::READY;
  }
  void CancelFrame(const FrameToken&) override {}
  void CompleteFrameBuffer(unsigned frameIndex, FrameDone result) override;
  void SetFrameTiming(unsigned frameIndex, uint64_t captureTime,
    uint64_t postProcessTime, uint64_t copyTime, uint64_t readyTime,
    uint64_t holdTime, const CFrameScheduler::Schedule& schedule,
    uint64_t completedAt) override;
  void WriteFrameBuffer(unsigned frameIndex, void * src, size_t offset,
    size_t len, bool setWritePos) const override;
  void WriteFrameBufferRows(unsigned frameIndex, void * src,
    size_t offset, size_t rowBytes, size_t pitch,
    unsigned rows) const override;
  void FinalizeFrameBuffer(unsigned frameIndex) const override;

  void ObserveFrame(uint64_t now) override;
  void ForceFrame() override;
  bool GetPublishTarget(uint64_t now, uint64_t& target,
    CFrameScheduler::Schedule& schedule, bool& periodic,
    bool& republish) override;
  void FrameMissed(const CFrameScheduler::Schedule& schedule,
    uint64_t now, bool periodic) override;
  void FrameSuperseded() override;
  HANDLE GetFrameScheduleEvent() const override
  {
    return m_frameScheduler.GetWakeEvent();
  }
  void SetFrameScheduleEvent(HANDLE event) override
  {
    m_frameScheduler.SetSharedWakeEvent(event);
  }
  void TryRecordFrameTiming(uint64_t duration) override;
};
