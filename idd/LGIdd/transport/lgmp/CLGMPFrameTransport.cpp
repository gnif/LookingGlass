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

#include "transport/lgmp/CLGMPFrameTransport.h"

#include "transport/lgmp/CIVSHMEM.h"
#include "transport/lgmp/CLGMPFrameCaps.h"
#include "transport/lgmp/CLGMPHost.h"
#include "CDebug.h"

#include <cstring>
#include <memory>
#include <new>

#pragma warning(push)
#pragma warning(disable: 4200)
struct LGMPBuffer
{
  volatile uint32_t wp;
  uint8_t data[0];
};
#pragma warning(pop)

static_assert(CAPTURE_PIPELINE_SLOTS == LGMP_Q_FRAME_LEN,
  "The capture pipeline must match the LGMP frame queue");
static_assert(CAPTURE_FRAME_BUFFERS == LGMP_Q_FRAME_BUFFER_LEN,
  "The capture buffer pool must match the LGMP frame buffers");
static_assert(CFrameScheduler::MAX_CLIENTS == LGMP_MAX_CLIENTS,
  "The scheduler must support every LGMP client");

static const struct LGMPQueueConfig FRAME_QUEUE_CONFIG =
{
  LGMP_Q_FRAME,       // queueID
  LGMP_Q_FRAME_LEN,   // numMessages
  1000                // subTimeout
};

static uint64_t FrameScheduleToken(
  const CFrameScheduler::Schedule& schedule)
{
  return static_cast<uint64_t>(schedule.epoch) << 32 |
    schedule.deliveryDeadlineSerial;
}

static bool FrameScheduleMatches(
  const CFrameScheduler::Schedule& a,
  const CFrameScheduler::Schedule& b)
{
  return a.clientID   == b.clientID   &&
         a.generation == b.generation &&
         a.epoch      == b.epoch;
}

CLGMPFrameTransport::CLGMPFrameTransport(
    CLGMPHost& host, CIVSHMEM& ivshmem) :
  m_host(host),
  m_ivshmem(ivshmem)
{
}

CLGMPFrameTransport::~CLGMPFrameTransport()
{
  DeInit();
}

bool CLGMPFrameTransport::Initialize()
{
  if (m_frameQueue)
  {
    for (PLGMPHostQueue queue : m_frameOwnerQueue)
      if (!queue)
        return false;
    return true;
  }

  LGMP_STATUS status = m_host.CreateQueue(
    FRAME_QUEUE_CONFIG, &m_frameQueue);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame): %s",
      lgmpStatusString(status));
    return false;
  }

  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    const struct LGMPQueueConfig config =
    {
      LGMP_Q_FRAME_OWNER + i, // queueID
      LGMP_Q_FRAME_LEN,       // numMessages
      1000                    // subTimeout
    };
    status = m_host.CreateQueue(config, &m_frameOwnerQueue[i]);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostQueueCreate Failed (Frame Owner %u): %s",
        i, lgmpStatusString(status));
      return false;
    }
  }

  return true;
}

void CLGMPFrameTransport::SealMemoryLayout()
{
  m_frameMemoryOffset =
    m_ivshmem.GetUsableSize() - m_host.Available();
}

bool CLGMPFrameTransport::Setup(size_t alignSize)
{
  // This may get called multiple times as frame buffers cannot be allocated
  // until the GPU-specific alignment is known.
  if (m_maxFrameSize)
    return true;

  m_alignSize = alignSize;

  if (!m_alignSize || (m_alignSize & (m_alignSize - 1)) ||
      m_alignSize < sizeof(KVMFRFrame) + sizeof(LGMPBuffer))
  {
    DEBUG_ERROR("Invalid frame buffer alignment: %llu",
      (unsigned long long)m_alignSize);
    return false;
  }

  const size_t available = m_host.Available();

  const size_t alignmentMask = m_alignSize - 1;
  const size_t alignedFrameMemoryOffset =
    (m_frameMemoryOffset + alignmentMask) & ~alignmentMask;
  const size_t alignmentPadding =
    alignedFrameMemoryOffset - m_frameMemoryOffset;
  if (available <= alignmentPadding)
  {
    DEBUG_ERROR("Insufficient shared memory for frame buffers");
    return false;
  }

  size_t frameAllocationSize =
    (available - alignmentPadding) / LGMP_Q_FRAME_BUFFER_LEN;
  frameAllocationSize &= ~alignmentMask;
  if (frameAllocationSize <= m_alignSize ||
      frameAllocationSize > UINT32_MAX)
  {
    DEBUG_ERROR("Invalid frame allocation size: %llu",
      (unsigned long long)frameAllocationSize);
    return false;
  }

  // The KVMFR frame header and frame-buffer write position occupy the first
  // alignment unit. Only the bytes after it are usable for pixel data.
  const size_t maxFrameSize = frameAllocationSize - m_alignSize;
  DEBUG_INFO("Max Frame Data Size: %u MiB",
    (unsigned int)(maxFrameSize / 1048576));

  LGMP_STATUS status;
  for (int i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    status = m_host.AllocateAligned(
      (uint32_t)frameAllocationSize, (uint32_t)m_alignSize,
      &m_frameMemory[i]);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAllocAligned Failed (Frame): %s",
        lgmpStatusString(status));
      return false;
    }

    m_frame[i] =
      static_cast<KVMFRFrame *>(lgmpHostMemPtr(m_frameMemory[i]));

    /**
     * Put the framebuffer on the border of the next page, this is to allow
     * for aligned DMA transfers by the receiver.
     */
    const size_t alignOffset = alignSize - sizeof(LGMPBuffer);
    m_frame[i]->offset = (uint32_t)alignOffset;
    m_frameBuffer[i] = reinterpret_cast<LGMPBuffer *>(
      reinterpret_cast<uint8_t *>(m_frame[i]) + alignOffset);
    m_frameInFlight[i].store(false, std::memory_order_release);
    m_frameCompleted[i] = false;
  }

  m_maxFrameSize = maxFrameSize;
  m_submittedFrameIndex.store(-1, std::memory_order_release);
  m_readyFrameIndex.store(-1, std::memory_order_release);
  m_deferredOwnerFrameIndex = -1;
  m_framePublishSequence = 0;
  m_frameReadySequence   = 0;
  memset(m_frameLastPublishSequence, 0,
    sizeof(m_frameLastPublishSequence));
  for (FrameDelivery& delivery : m_frameDelivery)
    delivery = {};
  for (OwnerDelivery& delivery : m_ownerDelivery)
    delivery = {};

  return true;
}

void CLGMPFrameTransport::DeInit()
{
  m_frameScheduler.Reset();

  {
    CSRWExclusiveLock lock(m_framePublishLock);
    m_submittedFrameIndex.store(-1, std::memory_order_release);
    m_readyFrameIndex.store(-1, std::memory_order_release);
    m_deferredOwnerFrameIndex = -1;
    m_framePublishSequence = 0;
    m_frameReadySequence   = 0;
    memset(m_frameLastPublishSequence, 0,
      sizeof(m_frameLastPublishSequence));
    memset(m_frameCompleted, 0, sizeof(m_frameCompleted));
    for (FrameDelivery& delivery : m_frameDelivery)
      delivery = {};
    for (OwnerDelivery& delivery : m_ownerDelivery)
      delivery = {};
  }

  for (int i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    m_frameInFlight[i].store(false, std::memory_order_release);
    lgmpHostMemFree(&m_frameMemory[i]);
    m_frame[i]       = nullptr;
    m_frameBuffer[i] = nullptr;
  }

  m_frameQueue = nullptr;
  memset(m_frameOwnerQueue, 0, sizeof(m_frameOwnerQueue));
}

std::shared_ptr<const FrameCaps> CLGMPFrameTransport::GetFrameCaps() const
{
  return std::shared_ptr<const FrameCaps>(new (std::nothrow)
    CLGMPFrameCaps(m_ivshmem.GetUsableSize(), m_frameMemoryOffset,
      LGMP_Q_FRAME_BUFFER_LEN));
}

CLGMPFrameTransport::SubscriberSnapshot
CLGMPFrameTransport::SnapshotSubscribers() const
{
  SubscriberSnapshot snapshot;
  snapshot.status = lgmpHostGetClientIDs(
    m_frameQueue, snapshot.clientIDs, &snapshot.clientCount);
  if (snapshot.status == LGMP_OK)
  {
    memcpy(snapshot.ownerClientIDs, snapshot.clientIDs,
      snapshot.clientCount * sizeof(*snapshot.ownerClientIDs));
    snapshot.ownerClientCount = snapshot.clientCount;
  }

  for (unsigned queueIndex = 0;
       snapshot.status == LGMP_OK && queueIndex < LGMP_Q_FRAME_LEN;
       ++queueIndex)
  {
    uint32_t queueClientIDs[LGMP_MAX_CLIENTS] = {};
    unsigned queueClientCount                = 0;
    snapshot.status = lgmpHostGetClientIDs(
      m_frameOwnerQueue[queueIndex], queueClientIDs, &queueClientCount);

    unsigned commonCount = 0;
    for (unsigned i = 0;
         snapshot.status == LGMP_OK && i < snapshot.ownerClientCount;
         ++i)
      for (unsigned candidate = 0;
           candidate < queueClientCount; ++candidate)
        if (snapshot.ownerClientIDs[i] == queueClientIDs[candidate])
        {
          snapshot.ownerClientIDs[commonCount++] =
            snapshot.ownerClientIDs[i];
          break;
        }
    snapshot.ownerClientCount = commonCount;
  }

  return snapshot;
}

void CLGMPFrameTransport::FinalizeSubscribers(
  const SubscriberSnapshot& snapshot, uint64_t now)
{
  if (snapshot.status == LGMP_OK)
    m_frameScheduler.UpdateSubscribers(
      snapshot.clientIDs, snapshot.clientCount,
      snapshot.ownerClientIDs, snapshot.ownerClientCount, now);
  else
    DEBUG_WARN("Failed to query LGMP frame subscribers: %s",
      lgmpStatusString(snapshot.status));

  m_frameScheduler.LogStatistics(now);

  if (lgmpHostQueueNewSubs(m_frameQueue))
    m_frameScheduler.NotifyPublisher();

  bool ownerSubscribed = false;
  for (unsigned queueIndex = 0;
       queueIndex < LGMP_Q_FRAME_LEN; ++queueIndex)
    ownerSubscribed |=
      lgmpHostQueueNewSubs(m_frameOwnerQueue[queueIndex]) != 0;
  if (ownerSubscribed)
    m_frameScheduler.RequestRepublish();

  ProcessFrameDeliveries();
}

bool CLGMPFrameTransport::UpdateSchedule(uint32_t sourceClientID,
  const FrameScheduleUpdate& schedule, uint64_t now)
{
  return m_frameScheduler.UpdateSchedule(sourceClientID, schedule, now);
}

CLGMPFrameTransport::SharedFramePostResult
CLGMPFrameTransport::PostSharedFrame(unsigned frameIndex,
  uint32_t excludeClientID, uint64_t now)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN ||
      lgmpHostQueuePending(m_frameQueue) != 0)
    return SHARED_FRAME_FAILED;

  uint32_t clientIDs[LGMP_MAX_CLIENTS] = {};
  unsigned clientCount                = 0;
  LGMP_STATUS status =
    lgmpHostGetClientIDs(m_frameQueue, clientIDs, &clientCount);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to query shared frame subscribers: %s",
      lgmpStatusString(status));
    return SHARED_FRAME_FAILED;
  }

  uint32_t recipients[LGMP_MAX_CLIENTS] = {};
  const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;
  const unsigned recipientCount =
    m_frameScheduler.GetSecondaryRecipients(
      clientIDs, clientCount, frameSerial, now, recipients);

  unsigned targetCount = 0;
  for (unsigned i = 0; i < recipientCount; ++i)
  {
    bool excluded = recipients[i] == excludeClientID;
    for (const OwnerDelivery& delivery : m_ownerDelivery)
      if (delivery.active && delivery.clientID == recipients[i])
      {
        excluded = true;
        break;
      }

    if (!excluded)
      recipients[targetCount++] = recipients[i];
  }

  if (!targetCount)
  {
    m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
    m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
    m_frameDelivery[frameIndex].sharedOwnerPending  = false;
    m_frameDelivery[frameIndex].sharedPending       = false;
    return SHARED_FRAME_IDLE;
  }

  unsigned postedCount = 0;
  status = lgmpHostQueuePostForClients(
    m_frameQueue, 0, m_frameMemory[frameIndex],
    recipients, targetCount, &postedCount);
  if (status != LGMP_OK)
  {
    if (status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish shared frame: %s",
        lgmpStatusString(status));
    return SHARED_FRAME_FAILED;
  }

  if (!postedCount)
  {
    m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
    m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
    m_frameDelivery[frameIndex].sharedOwnerPending  = false;
    m_frameDelivery[frameIndex].sharedPending       = false;
    return SHARED_FRAME_IDLE;
  }

  m_frameDelivery[frameIndex].sharedOwnerToken    = 0;
  m_frameDelivery[frameIndex].sharedOwnerClientID = 0;
  m_frameDelivery[frameIndex].sharedOwnerPending  = false;
  m_frameDelivery[frameIndex].sharedPending       = true;
  // LGMP returns only the number of matching subscribers. Account the full
  // snapshot: disappeared client IDs are harmless, while every surviving
  // target received this post.
  m_frameScheduler.FrameDelivered(
    recipients, targetCount, frameSerial, now);
  return SHARED_FRAME_POSTED;
}

bool CLGMPFrameTransport::PostSharedOwnerFrame(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN || !schedule.clientID ||
      m_frameDelivery[frameIndex].sharedOwnerPending ||
      lgmpHostQueuePending(m_frameQueue) >= LGMP_Q_FRAME_LEN)
    return false;

  unsigned recipientCount = 0;
  const LGMP_STATUS status = lgmpHostQueuePostForClients(
    m_frameQueue, FrameScheduleToken(schedule), m_frameMemory[frameIndex],
    &schedule.clientID, 1, &recipientCount);
  if (status != LGMP_OK || !recipientCount)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish shared owner frame: %s",
        lgmpStatusString(status));
    return false;
  }

  FrameDelivery& delivery      = m_frameDelivery[frameIndex];
  delivery.sharedOwnerToken    = FrameScheduleToken(schedule);
  delivery.sharedOwnerClientID = schedule.clientID;
  delivery.sharedOwnerPending  = true;
  delivery.sharedPending       = true;
  return true;
}

void CLGMPFrameTransport::ProcessFrameDeliveries()
{
  if (!m_frameQueue)
    return;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (!m_frameOwnerQueue[i])
      return;

  CSRWExclusiveLock lock(m_framePublishLock);

  bool released = false;
  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    if (m_frameDelivery[i].sharedOwnerPending &&
        !lgmpHostQueueMessagePending(
          m_frameQueue, m_frameMemory[i],
          m_frameDelivery[i].sharedOwnerToken))
    {
      m_frameDelivery[i].sharedOwnerPending = false;
      released = true;
    }

    if (m_frameDelivery[i].sharedPending &&
        !lgmpHostQueuePayloadPending(m_frameQueue, m_frameMemory[i]))
    {
      m_frameDelivery[i].sharedPending = false;
      released = true;
    }
  }

  for (unsigned queueIndex = 0;
       queueIndex < LGMP_Q_FRAME_LEN; ++queueIndex)
  {
    OwnerDelivery& owner = m_ownerDelivery[queueIndex];
    if (!owner.active ||
        lgmpHostQueuePayloadPending(
          m_frameOwnerQueue[queueIndex],
          m_frameMemory[owner.frameIndex]))
      continue;

    const unsigned frameIndex = owner.frameIndex;
    m_frameDelivery[frameIndex].ownerQueueMask &=
      ~(1U << queueIndex);
    owner = {};
    released = true;
  }
  lock.Unlock();

  if (released)
    m_frameScheduler.NotifyPublisher();
}

int CLGMPFrameTransport::FindAvailableOwnerQueue(
  unsigned preferredIndex) const
{
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
  {
    const unsigned queueIndex =
      (preferredIndex + i) % LGMP_Q_FRAME_LEN;
    if (!m_ownerDelivery[queueIndex].active &&
        m_frameOwnerQueue[queueIndex] &&
        lgmpHostQueuePending(m_frameOwnerQueue[queueIndex]) == 0)
      return static_cast<int>(queueIndex);
  }

  return -1;
}

unsigned CLGMPFrameTransport::CountOwnerDeliveries(
  uint32_t clientID) const
{
  unsigned count = 0;
  for (const OwnerDelivery& delivery : m_ownerDelivery)
    if (delivery.active && delivery.clientID == clientID)
      ++count;

  for (const FrameDelivery& delivery : m_frameDelivery)
    if (delivery.sharedOwnerPending &&
        delivery.sharedOwnerClientID == clientID)
      ++count;

  return count;
}

bool CLGMPFrameTransport::HasMatchingOwnerDelivery(
  uint32_t clientID, unsigned frameIndex, uint64_t token) const
{
  for (const OwnerDelivery& delivery : m_ownerDelivery)
    if (delivery.active                    &&
        delivery.clientID   == clientID    &&
        delivery.frameIndex == frameIndex  &&
        delivery.token      == token)
      return true;

  for (unsigned i = 0; i < LGMP_Q_FRAME_BUFFER_LEN; ++i)
  {
    const FrameDelivery& delivery = m_frameDelivery[i];
    if (i == frameIndex                              &&
        delivery.sharedOwnerPending                 &&
        delivery.sharedOwnerClientID == clientID    &&
        delivery.sharedOwnerToken    == token)
      return true;
  }

  return false;
}

bool CLGMPFrameTransport::FrameBufferReferenced(
  unsigned frameIndex) const
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return true;

  const FrameDelivery& delivery = m_frameDelivery[frameIndex];
  if (delivery.ownerQueueMask || delivery.sharedPending ||
      delivery.sharedOwnerPending ||
      lgmpHostQueuePayloadPending(
        m_frameQueue, m_frameMemory[frameIndex]))
    return true;

  for (unsigned queueIndex = 0;
       queueIndex < LGMP_Q_FRAME_LEN; ++queueIndex)
    if (lgmpHostQueuePayloadPending(
          m_frameOwnerQueue[queueIndex], m_frameMemory[frameIndex]))
      return true;

  return false;
}

int CLGMPFrameTransport::FindAvailableFrameBuffer(
  bool allowReady) const
{
  const LONG readyFrameIndex =
    m_readyFrameIndex.load(std::memory_order_acquire);
  int      available     = -1;
  uint64_t newestPublish = 0;
  for (unsigned frameIndex = 0;
       frameIndex < LGMP_Q_FRAME_BUFFER_LEN; ++frameIndex)
  {
    if (static_cast<LONG>(frameIndex) == readyFrameIndex ||
        m_frameInFlight[frameIndex].load(std::memory_order_acquire) ||
        FrameBufferReferenced(frameIndex))
      continue;

    if (available < 0 ||
        m_frameLastPublishSequence[frameIndex] > newestPublish)
    {
      available     = static_cast<int>(frameIndex);
      newestPublish = m_frameLastPublishSequence[frameIndex];
    }
  }

  if (available >= 0 || !allowReady || readyFrameIndex < 0 ||
      m_frameInFlight[readyFrameIndex].load(std::memory_order_acquire) ||
      FrameBufferReferenced(static_cast<unsigned>(readyFrameIndex)))
    return available;

  // A blocked owner can consume the other two buffers indefinitely. Once
  // the retained frame has no queue references it is safe to replace it with
  // a newer frame rather than waiting for the owner's LGMP timeout.
  return static_cast<int>(readyFrameIndex);
}

int CLGMPFrameTransport::FindNewestCompletedFrame(
  unsigned excludeFrameIndex) const
{
  int      newestFrame    = -1;
  uint64_t newestSequence = 0;
  for (unsigned frameIndex = 0;
       frameIndex < LGMP_Q_FRAME_BUFFER_LEN; ++frameIndex)
  {
    if (frameIndex == excludeFrameIndex || !m_frameCompleted[frameIndex] ||
        m_frameInFlight[frameIndex].load(std::memory_order_acquire))
      continue;

    if (newestFrame < 0 ||
        m_frameLastPublishSequence[frameIndex] > newestSequence)
    {
      newestFrame    = static_cast<int>(frameIndex);
      newestSequence = m_frameLastPublishSequence[frameIndex];
    }
  }

  return newestFrame;
}

bool CLGMPFrameTransport::FrameBufferAvailable(
  const CFrameScheduler::Schedule& schedule,
  bool allowReadyReplacement)
{
  if (!m_frameQueue)
    return false;
  for (unsigned i = 0; i < LGMP_Q_FRAME_LEN; ++i)
    if (!m_frameOwnerQueue[i])
      return false;

  CSRWSharedLock lock(m_framePublishLock);
  bool allowReady = false;
  // Pipeline one frame through each independent owner lane. Count the shared
  // fallback against the same limit so it cannot become a third delivery for
  // the same owner. Once both are occupied, a fully unreferenced buffer can
  // still retain a newer frame for secondary delivery and later republish.
  if (schedule.clientID)
  {
    const bool ownerBlocked =
      CountOwnerDeliveries(schedule.clientID) >= LGMP_Q_FRAME_LEN;
    const bool ownerQueuesBlocked = FindAvailableOwnerQueue(0) < 0;
    allowReady = allowReadyReplacement &&
      (ownerBlocked || ownerQueuesBlocked);
  }
  else if (lgmpHostQueuePending(m_frameQueue) != 0)
    return false;

  // With no owner delivery lane available, a copy can still replace an
  // unreferenced retained frame and be republished when a lane clears.
  const bool available = FindAvailableFrameBuffer(allowReady) >= 0;
  return available;
}

void CLGMPFrameTransport::ProcessDeliveries()
{
  if (!m_host.IsInitialized())
    return;

  const LGMP_STATUS status = m_host.Process();

  if (status != LGMP_OK && status != LGMP_ERR_CORRUPTED)
    DEBUG_ERROR("lgmpHostProcess Failed: %s", lgmpStatusString(status));

  if (status == LGMP_OK)
    ProcessFrameDeliveries();
}

bool CLGMPFrameTransport::GetPendingDeliveryTarget(uint64_t now,
  uint64_t& target)
{
  if (!m_frameQueue)
    return false;

  CSRWSharedLock lock(m_framePublishLock);
  const LONG frameIndex =
    m_readyFrameIndex.load(std::memory_order_acquire);
  if (frameIndex < 0 ||
      m_frameInFlight[frameIndex].load(std::memory_order_acquire) ||
      lgmpHostQueuePending(m_frameQueue) != 0)
    return false;

  uint32_t blockedClientIDs[LGMP_Q_FRAME_LEN] = {};
  unsigned blockedCount = 0;
  for (const OwnerDelivery& delivery : m_ownerDelivery)
    if (delivery.active)
      blockedClientIDs[blockedCount++] = delivery.clientID;

  const bool result = m_frameScheduler.GetSecondaryTarget(
    m_frame[frameIndex]->frameSerial, now,
    blockedClientIDs, blockedCount, target);
  return result;
}

bool CLGMPFrameTransport::RetryPendingDelivery(uint64_t now, bool& retry)
{
  retry = false;
  if (!m_frameQueue)
    return false;

  CSRWExclusiveLock lock(m_framePublishLock);
  const LONG frameIndex =
    m_readyFrameIndex.load(std::memory_order_acquire);
  if (frameIndex < 0 ||
      m_frameInFlight[frameIndex].load(std::memory_order_acquire) ||
      lgmpHostQueuePending(m_frameQueue) != 0)
    return false;

  const SharedFramePostResult result = PostSharedFrame(
    static_cast<unsigned>(frameIndex), 0, now);
  lock.Unlock();
  retry = result == SHARED_FRAME_FAILED;
  return result == SHARED_FRAME_POSTED;
}

SinkTarget CLGMPFrameTransport::PrepareFrameBuffer(
  unsigned pitch, const D12FrameFormat& srcFormat,
  const D12FrameFormat& dstFormat, const RECT * dirtyRects,
  unsigned nbDirtyRects, const CFrameScheduler::Schedule& schedule,
  bool allowReadyReplacement)
{
  SinkTarget result = {};

  const unsigned dataWidth = dstFormat.dataWidth ?
    dstFormat.dataWidth : (unsigned)dstFormat.desc.Width;
  const unsigned dataHeight = dstFormat.dataHeight ?
    dstFormat.dataHeight : dstFormat.desc.Height;

  if (dstFormat.format == FRAME_TYPE_INVALID)
  {
    DEBUG_ERROR("Unsupported frame format, skipping frame");
    return result;
  }

  CSRWExclusiveLock lock(m_framePublishLock);
  const bool ownerBlocked = schedule.clientID &&
    CountOwnerDeliveries(schedule.clientID) >= LGMP_Q_FRAME_LEN;
  const bool allowReady = allowReadyReplacement &&
    (ownerBlocked ||
      (schedule.clientID && FindAvailableOwnerQueue(0) < 0));
  const int availableFrameIndex =
    FindAvailableFrameBuffer(allowReady);
  bool expected = false;
  const bool acquired = availableFrameIndex >= 0 &&
    m_frameInFlight[availableFrameIndex].compare_exchange_strong(
      expected, true, std::memory_order_acq_rel);
  if (acquired)
  {
    const LONG readyFrameIndex =
      m_readyFrameIndex.load(std::memory_order_acquire);
    if (availableFrameIndex == readyFrameIndex)
      m_readyFrameIndex.store(
        FindNewestCompletedFrame(
          static_cast<unsigned>(availableFrameIndex)),
        std::memory_order_release);
    m_frameCompleted[availableFrameIndex] = false;
    m_frameDelivery[availableFrameIndex] = {};
  }
  const bool fullCopy = acquired &&
    (!m_frameLastPublishSequence[availableFrameIndex] ||
      m_framePublishSequence >
        m_frameLastPublishSequence[availableFrameIndex] + 1);
  lock.Unlock();
  if (!acquired)
    return result;
  const unsigned frameIndex = static_cast<unsigned>(availableFrameIndex);

  if (m_width       != dataWidth             ||
      m_height      != dataHeight            ||
      m_frameWidth  != dstFormat.width       ||
      m_frameHeight != dstFormat.height      ||
      m_pitch       != pitch                 ||
      m_format      != dstFormat.desc.Format ||
      m_frameType   != dstFormat.format)
  {
    m_width       = dataWidth;
    m_height      = dataHeight;
    m_frameWidth  = dstFormat.width;
    m_frameHeight = dstFormat.height;
    m_pitch       = pitch;
    m_format      = dstFormat.desc.Format;
    m_frameType   = dstFormat.format;
    ++m_formatVer;
  }

  // Detect HDR metadata changes that require a format version bump
  // so the client knows to re-apply the HDR image description.
  //
  // Use dstFormat so post-processing can propagate any metadata adjustments.
  if (dstFormat.hdr)
  {
    const bool metadataChanged =
      m_lastHDRMetadata != dstFormat.hdrMetadata ||
      (dstFormat.hdrMetadata &&
       (memcmp(m_lastHDRDisplayPrimary, dstFormat.displayPrimary,
          sizeof(m_lastHDRDisplayPrimary)) != 0 ||
        memcmp(m_lastHDRWhitePoint, dstFormat.whitePoint,
          sizeof(m_lastHDRWhitePoint)) != 0 ||
        m_lastHDRMaxDisplayLuminance !=
          dstFormat.maxDisplayLuminance ||
        m_lastHDRMinDisplayLuminance !=
          dstFormat.minDisplayLuminance ||
        m_lastHDRMaxContentLightLevel !=
          dstFormat.maxContentLightLevel ||
        m_lastHDRMaxFrameAverageLightLevel !=
          dstFormat.maxFrameAverageLightLevel));

    if (!m_lastHDRActive || metadataChanged ||
        m_lastSDRWhiteLevel != dstFormat.sdrWhiteLevel)
      ++m_formatVer;
  }
  else if (m_lastHDRActive)
  {
    // HDR was turned off.
    ++m_formatVer;
  }

  m_lastHDRActive   = dstFormat.hdr;
  m_lastHDRMetadata = dstFormat.hdrMetadata;
  memcpy(m_lastHDRDisplayPrimary, dstFormat.displayPrimary,
    sizeof(m_lastHDRDisplayPrimary));
  memcpy(m_lastHDRWhitePoint, dstFormat.whitePoint,
    sizeof(m_lastHDRWhitePoint));
  m_lastHDRMaxDisplayLuminance = dstFormat.maxDisplayLuminance;
  m_lastHDRMinDisplayLuminance = dstFormat.minDisplayLuminance;
  m_lastHDRMaxContentLightLevel = dstFormat.maxContentLightLevel;
  m_lastHDRMaxFrameAverageLightLevel =
    dstFormat.maxFrameAverageLightLevel;
  m_lastSDRWhiteLevel = dstFormat.sdrWhiteLevel;

  KVMFRFrame * fi = m_frame[frameIndex];

  const unsigned maxRows = (unsigned)(m_maxFrameSize / pitch);
  const int bpp = dstFormat.format == FRAME_TYPE_RGBA16F ? 8 : 4;
  KVMFRFrameFlags flags =
    (dstFormat.hdr         ? FRAME_FLAG_HDR          : 0) |
    (dstFormat.hdrPQ       ? FRAME_FLAG_HDR_PQ       : 0) |
    (dstFormat.hdrMetadata ? FRAME_FLAG_HDR_METADATA : 0);

  if (maxRows < dataHeight)
    flags |= FRAME_FLAG_TRUNCATED;

  fi->formatVer        = m_formatVer;
  if (!m_frameSerial)
    ++m_frameSerial;
  fi->frameSerial      = m_frameSerial++;
  fi->screenWidth      = srcFormat.width;
  fi->screenHeight     = srcFormat.height;
  fi->dataWidth        = dataWidth;
  fi->dataHeight       = min(maxRows, dataHeight);
  fi->frameWidth       = dstFormat.width;
  fi->frameHeight      = dstFormat.height;
  fi->stride           = pitch / bpp;
  fi->pitch            = pitch;
  // fi->offset is initialized at startup.
  fi->flags            = flags;
  fi->sdrWhiteLevel    = dstFormat.sdrWhiteLevel;

  fi->captureTime            = 0;
  fi->postProcessTime        = 0;
  fi->copyTime               = 0;
  fi->readyTime              = 0;
  fi->holdTime               = 0;
  fi->readyLeadTime          = 0;
  fi->timingSerial           = 0;
  fi->timingFlags            = 0;
  fi->scheduleGeneration     = 0;
  fi->scheduleEpoch          = 0;
  fi->scheduleDeadlineSerial = 0;
  InterlockedExchange((volatile LONG *)&fi->timingValid, 0);
  fi->rotation = FRAME_ROT_0;
  fi->type     = dstFormat.format;

  if (flags & FRAME_FLAG_HDR_METADATA)
  {
    memcpy(fi->hdrDisplayPrimary, dstFormat.displayPrimary,
      sizeof(fi->hdrDisplayPrimary));
    memcpy(fi->hdrWhitePoint, dstFormat.whitePoint,
      sizeof(fi->hdrWhitePoint));
    fi->hdrMaxDisplayLuminance = dstFormat.maxDisplayLuminance;
    fi->hdrMinDisplayLuminance = dstFormat.minDisplayLuminance;
    fi->hdrMaxContentLightLevel = dstFormat.maxContentLightLevel;
    fi->hdrMaxFrameAverageLightLevel =
      dstFormat.maxFrameAverageLightLevel;
  }
  else
  {
    memset(fi->hdrDisplayPrimary, 0, sizeof(fi->hdrDisplayPrimary));
    memset(fi->hdrWhitePoint, 0, sizeof(fi->hdrWhitePoint));
    fi->hdrMaxDisplayLuminance       = 0;
    fi->hdrMinDisplayLuminance       = 0;
    fi->hdrMaxContentLightLevel      = 0;
    fi->hdrMaxFrameAverageLightLevel = 0;
  }

  fi->damageRectsCount = 0;
  if (nbDirtyRects <= ARRAYSIZE(fi->damageRects))
  {
    fi->damageRectsCount = nbDirtyRects;
    for (unsigned i = 0; i < nbDirtyRects; ++i)
    {
      fi->damageRects[i].x      = dirtyRects[i].left;
      fi->damageRects[i].y      = dirtyRects[i].top;
      fi->damageRects[i].width  =
        dirtyRects[i].right - dirtyRects[i].left;
      fi->damageRects[i].height =
        dirtyRects[i].bottom - dirtyRects[i].top;
    }
  }

  LGMPBuffer * fb = m_frameBuffer[frameIndex];
  fb->wp = 0;

  result.slot       = frameIndex;
  result.mem        = fb->data;
  result.heapOffset = reinterpret_cast<uintptr_t>(fb->data) -
    reinterpret_cast<uintptr_t>(m_ivshmem.GetMem());
  result.capacity   = m_maxFrameSize;
  result.fullCopy   = fullCopy;
  if (fullCopy)
    fi->damageRectsCount = 0;
  return result;
}

bool CLGMPFrameTransport::PublishFrameBuffer(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule, bool& deliveredToOwner)
{
  deliveredToOwner = false;
  if (!m_frameQueue || frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return false;

  const uint64_t now = CFrameScheduler::Nanotime();
  CSRWExclusiveLock lock(m_framePublishLock);
  CFrameScheduler::Schedule currentSchedule = {};
  const bool scheduling =
    m_frameScheduler.GetSchedule(currentSchedule);
  if (scheduling != (schedule.clientID != 0) ||
      (scheduling && !FrameScheduleMatches(schedule, currentSchedule)))
    return false;

  KVMFRFrame * frame            = m_frame[frameIndex];
  frame->timingFlags            = 0;
  frame->scheduleGeneration     = schedule.generation;
  frame->scheduleEpoch          = schedule.epoch;
  frame->scheduleDeadlineSerial = schedule.deliveryDeadlineSerial;

  LGMP_STATUS status = LGMP_OK;
  bool published     = false;
  if (schedule.clientID)
  {
    if (CountOwnerDeliveries(schedule.clientID) >= LGMP_Q_FRAME_LEN)
    {
      // Both owner lanes still reference older frames. Retain the newest
      // frame locally and serve any unblocked secondary clients without
      // violating the lifetime of those outstanding LGMP payloads.
      PostSharedFrame(frameIndex, schedule.clientID, now);
      published = true;
    }
    else
    {
      const int ownerQueueIndex = FindAvailableOwnerQueue(frameIndex);
      if (ownerQueueIndex < 0)
      {
        published        = PostSharedOwnerFrame(frameIndex, schedule);
        deliveredToOwner = published;
        if (!published)
        {
          PostSharedFrame(frameIndex, schedule.clientID, now);
          published = true;
        }
      }
      else
      {
        unsigned recipientCount = 0;
        status = lgmpHostQueuePostForClients(
          m_frameOwnerQueue[ownerQueueIndex], FrameScheduleToken(schedule),
          m_frameMemory[frameIndex],
          &schedule.clientID, 1, &recipientCount);
        if (status == LGMP_OK && recipientCount)
        {
          const unsigned queueIndex =
            static_cast<unsigned>(ownerQueueIndex);
          OwnerDelivery& owner = m_ownerDelivery[queueIndex];
          owner.token           = FrameScheduleToken(schedule);
          owner.clientID        = schedule.clientID;
          owner.frameIndex      = frameIndex;
          owner.active          = true;

          FrameDelivery& delivery = m_frameDelivery[frameIndex];
          delivery.ownerQueueMask |= 1U << queueIndex;
          deliveredToOwner = true;
          published        = true;

          PostSharedFrame(frameIndex, schedule.clientID, now);
        }
      }
    }
  }
  else
  {
    published = PostSharedFrame(
      frameIndex, 0, now) != SHARED_FRAME_FAILED;
    deliveredToOwner = published;
  }

  if (published)
  {
    m_frameLastPublishSequence[frameIndex] = ++m_framePublishSequence;
    m_frameSchedule[frameIndex]   = schedule;
    m_frameDelivered[frameIndex]  = deliveredToOwner;
    m_deferredOwnerFrameIndex = schedule.clientID && !deliveredToOwner ?
      static_cast<LONG>(frameIndex) : -1;
    m_submittedFrameIndex.store(
      static_cast<LONG>(frameIndex), std::memory_order_release);
  }
  lock.Unlock();

  if (!published)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to publish frame: %s",
        lgmpStatusString(status));
    return false;
  }

  return true;
}

bool CLGMPFrameTransport::RepublishFrameBuffer(
  const CFrameScheduler::Schedule& schedule)
{
  if (!schedule.clientID)
    return false;

  CSRWExclusiveLock lock(m_framePublishLock);
  CFrameScheduler::Schedule currentSchedule = {};
  if (!m_frameScheduler.GetSchedule(currentSchedule) ||
      !FrameScheduleMatches(schedule, currentSchedule))
    return false;

  LONG frameIndex = m_deferredOwnerFrameIndex;
  if (frameIndex >= 0 &&
      !m_frameCompleted[frameIndex] &&
      !m_frameInFlight[frameIndex].load(std::memory_order_acquire))
  {
    m_deferredOwnerFrameIndex = -1;
    frameIndex = -1;
  }
  if (frameIndex < 0)
    frameIndex = m_readyFrameIndex.load(std::memory_order_acquire);
  if (frameIndex < 0 ||
      m_frameInFlight[frameIndex].load(std::memory_order_acquire))
    return false;

  CFrameScheduler::Schedule deliverySchedule = schedule;
  deliverySchedule.deliveryDeadlineSerial = 0;
  deliverySchedule.phaseEligible          = false;
  const uint64_t scheduleToken = FrameScheduleToken(deliverySchedule);
  const uint32_t frameSerial   = m_frame[frameIndex]->frameSerial;
  if (HasMatchingOwnerDelivery(schedule.clientID,
      static_cast<unsigned>(frameIndex), scheduleToken))
  {
    if (m_deferredOwnerFrameIndex == frameIndex)
      m_deferredOwnerFrameIndex = -1;
    lock.Unlock();
    m_frameScheduler.FrameRepublished(schedule, frameSerial);
    return true;
  }

  if (CountOwnerDeliveries(schedule.clientID) >= LGMP_Q_FRAME_LEN)
    return false;

  const int ownerQueueIndex =
    FindAvailableOwnerQueue(static_cast<unsigned>(frameIndex));
  if (ownerQueueIndex < 0)
  {
    const bool published = PostSharedOwnerFrame(
      static_cast<unsigned>(frameIndex), deliverySchedule);
    if (published && m_deferredOwnerFrameIndex == frameIndex)
      m_deferredOwnerFrameIndex = -1;
    lock.Unlock();
    if (published)
      m_frameScheduler.FrameRepublished(schedule, frameSerial);
    return published;
  }

  unsigned recipientCount = 0;
  const LGMP_STATUS status = lgmpHostQueuePostForClients(
    m_frameOwnerQueue[ownerQueueIndex], scheduleToken,
    m_frameMemory[frameIndex],
    &schedule.clientID, 1, &recipientCount);
  if (status == LGMP_OK && recipientCount)
  {
    const unsigned queueIndex =
      static_cast<unsigned>(ownerQueueIndex);
    OwnerDelivery& owner = m_ownerDelivery[queueIndex];
    owner.token           = scheduleToken;
    owner.clientID        = schedule.clientID;
    owner.frameIndex      = static_cast<unsigned>(frameIndex);
    owner.active          = true;

    FrameDelivery& delivery = m_frameDelivery[frameIndex];
    delivery.ownerQueueMask |= 1U << queueIndex;
    if (m_deferredOwnerFrameIndex == frameIndex)
      m_deferredOwnerFrameIndex = -1;
  }
  lock.Unlock();

  if (status != LGMP_OK || !recipientCount)
  {
    if (status != LGMP_OK && status != LGMP_ERR_QUEUE_FULL)
      DEBUG_ERROR("Failed to republish frame: %s",
        lgmpStatusString(status));
    return false;
  }

  m_frameScheduler.FrameRepublished(schedule, frameSerial);
  return true;
}

void CLGMPFrameTransport::CommitFrameBuffer(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule, bool periodic,
  bool deliveredToOwner)
{
  UNREFERENCED_PARAMETER(deliveredToOwner);

  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  const uint64_t now = CFrameScheduler::Nanotime();
  m_frameScheduler.FrameCommitted(
    schedule, m_frame[frameIndex]->frameSerial, now, periodic);
}

bool CLGMPFrameTransport::TryFrameSubmitted(unsigned frameIndex,
  const CFrameScheduler::Schedule& schedule)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return false;

  return m_frameScheduler.TryFrameSubmitted(
    schedule, m_frame[frameIndex]->frameSerial);
}

void CLGMPFrameTransport::ObserveFrame(uint64_t now)
{
  m_frameScheduler.ObserveFrame(now);
}

void CLGMPFrameTransport::ForceFrame()
{
  m_frameScheduler.ForceFrame();
}

bool CLGMPFrameTransport::GetPublishTarget(uint64_t now,
  uint64_t& target, CFrameScheduler::Schedule& schedule, bool& periodic,
  bool& republish)
{
  return m_frameScheduler.GetPublishTarget(
    now, target, schedule, periodic, republish);
}

void CLGMPFrameTransport::FrameMissed(
  const CFrameScheduler::Schedule& schedule, uint64_t now, bool periodic)
{
  m_frameScheduler.FrameMissed(schedule, now, periodic);
}

void CLGMPFrameTransport::FrameSuperseded()
{
  m_frameScheduler.FrameSuperseded();
}

void CLGMPFrameTransport::TryRecordFrameTiming(uint64_t duration)
{
  m_frameScheduler.TryRecordFrameTiming(duration);
}

void CLGMPFrameTransport::AbortFrameBuffer(unsigned frameIndex)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  CSRWExclusiveLock lock(m_framePublishLock);
  m_frameBuffer[frameIndex]->wp = 0;
  InterlockedExchange(
    (volatile LONG *)&m_frame[frameIndex]->timingValid, 0);
  m_frameCompleted[frameIndex] = false;
  if (m_deferredOwnerFrameIndex == static_cast<LONG>(frameIndex))
    m_deferredOwnerFrameIndex = -1;
  m_frameInFlight[frameIndex].store(false, std::memory_order_release);
}

void CLGMPFrameTransport::FailFrameBuffer(unsigned frameIndex)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  InterlockedExchange(
    (volatile LONG *)&m_frame[frameIndex]->timingValid, 0);
  FinalizeFrameBuffer(frameIndex);
  AbortFrameBuffer(frameIndex);
}

void CLGMPFrameTransport::CompleteFrameBuffer(
  unsigned frameIndex, FrameDone result)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  const bool succeeded = result == FrameDone::READY;
  CSRWExclusiveLock lock(m_framePublishLock);
  const CFrameScheduler::Schedule schedule = m_frameSchedule[frameIndex];
  const uint32_t frameSerial = m_frame[frameIndex]->frameSerial;
  const bool delivered = m_frameDelivered[frameIndex];
  const uint64_t sequence = m_frameLastPublishSequence[frameIndex];
  m_frameCompleted[frameIndex] = succeeded;
  if (!succeeded &&
      m_deferredOwnerFrameIndex == static_cast<LONG>(frameIndex))
    m_deferredOwnerFrameIndex = -1;
  if (succeeded)
  {
    // Completion callbacks may run out of order. Never replace a newer ready
    // frame with an older submission.
    const LONG readyFrameIndex =
      m_readyFrameIndex.load(std::memory_order_acquire);
    if (sequence &&
        (readyFrameIndex < 0 ||
          sequence > m_frameLastPublishSequence[readyFrameIndex]))
      m_readyFrameIndex.store(
        static_cast<LONG>(frameIndex), std::memory_order_release);
  }
  m_frameInFlight[frameIndex].store(false, std::memory_order_release);
  const bool newerThanReady =
    sequence && sequence > m_frameReadySequence;
  if (result == FrameDone::READY && newerThanReady)
    m_frameReadySequence = sequence;

  if (result == FrameDone::READY && newerThanReady)
  {
    if (delivered)
      m_frameScheduler.FramePublished(schedule, frameSerial);
    else
      m_frameScheduler.FrameRetained(schedule);
  }
  else if (result == FrameDone::FAILED && newerThanReady)
    m_frameScheduler.FrameFailed();
  else if (result == FrameDone::SUPERSEDED && newerThanReady)
    m_frameScheduler.FrameSuperseded();
}

void CLGMPFrameTransport::SetFrameTiming(unsigned frameIndex,
  uint64_t captureTime, uint64_t postProcessTime, uint64_t copyTime,
  uint64_t readyTime, uint64_t holdTime,
  const CFrameScheduler::Schedule& schedule, uint64_t completedAt)
{
  if (frameIndex >= LGMP_Q_FRAME_BUFFER_LEN)
    return;

  KVMFRFrame * frame = m_frame[frameIndex];
  const bool phaseValid = m_frameScheduler.TryFrameCompleted(
    schedule, frame->frameSerial, completedAt);

  frame->captureTime     = captureTime;
  frame->postProcessTime = postProcessTime;
  frame->copyTime        = copyTime;
  frame->readyTime       = readyTime;
  frame->holdTime        = holdTime;
  frame->readyLeadTime   = phaseValid && schedule.deadline >= completedAt ?
    schedule.deadline - completedAt : 0;
  frame->timingFlags  = phaseValid ?
    KVMFR_FRAME_TIMING_PHASE_VALID : 0;
  frame->timingSerial = frame->frameSerial;
  InterlockedExchange((volatile LONG *)&frame->timingValid, 1);
}

void CLGMPFrameTransport::WriteFrameBuffer(unsigned frameIndex, void * src,
  size_t offset, size_t len, bool setWritePos) const
{
  LGMPBuffer * fb = m_frameBuffer[frameIndex];

  memcpy(
    reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(fb->data) + offset),
    reinterpret_cast<void *>(
      reinterpret_cast<uintptr_t>(src) + offset),
    len);

  if (setWritePos)
    fb->wp = (uint32_t)(offset + len);
}

void CLGMPFrameTransport::WriteFrameBufferRows(unsigned frameIndex,
  void * src, size_t offset, size_t rowBytes, size_t pitch,
  unsigned rows) const
{
  LGMPBuffer * fb = m_frameBuffer[frameIndex];
  uint8_t * dst    = fb->data + offset;
  uint8_t * source = static_cast<uint8_t *>(src) + offset;
  for (unsigned row = 0; row < rows; ++row)
  {
    memcpy(dst, source, rowBytes);
    dst    += pitch;
    source += pitch;
  }
}

void CLGMPFrameTransport::FinalizeFrameBuffer(unsigned frameIndex) const
{
  const KVMFRFrame * frame = m_frame[frameIndex];
  LGMPBuffer       * fb    = m_frameBuffer[frameIndex];
  fb->wp = frame->dataHeight * frame->pitch;
}
