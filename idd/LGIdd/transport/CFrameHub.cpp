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

#include "transport/CFrameHub.h"

bool CFrameHub::Bind(
  BackendId backend, uint32_t epoch, IFrameSink& sink)
{
  if (!backend || !epoch)
    return false;

  CSRWExclusiveLock lock(m_lock);
  if (m_sink)
    return false;
  m_sink    = &sink;
  m_backend = backend;
  m_epoch   = epoch;
  m_slots.clear();
  return true;
}

void CFrameHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_backend != backend || m_epoch != epoch)
    return;
  m_sink    = nullptr;
  m_backend = 0;
  m_epoch   = 0;
  m_slots.clear();
}

bool CFrameHub::Valid(const FrameToken& token) const
{
  return m_sink && token.sink == m_backend && token.epoch == m_epoch &&
    token.slot < m_slots.size() && token.serial &&
    m_slots[token.slot].serial == token.serial;
}

void CFrameHub::Invalidate(const FrameToken& token)
{
  if (Valid(token))
    m_slots[token.slot] = {};
}

size_t CFrameHub::GetMaxFrameSize() const
{
  CSRWSharedLock lock(m_lock);
  return m_sink ? m_sink->GetMaxFrameSize() : 0;
}

bool CFrameHub::FrameBufferAvailable(
  const CFrameScheduler::Schedule& schedule, bool allowReadyReplacement)
{
  CSRWSharedLock lock(m_lock);
  return m_sink &&
    m_sink->FrameBufferAvailable(schedule, allowReadyReplacement);
}

bool CFrameHub::HasPublishedFrame() const
{
  CSRWSharedLock lock(m_lock);
  return m_sink && m_sink->HasPublishedFrame();
}

void CFrameHub::ProcessDeliveries()
{
  CSRWSharedLock lock(m_lock);
  if (m_sink)
    m_sink->ProcessDeliveries();
}

bool CFrameHub::GetPendingDeliveryTarget(uint64_t now, uint64_t& target)
{
  CSRWSharedLock lock(m_lock);
  return m_sink && m_sink->GetPendingDeliveryTarget(now, target);
}

bool CFrameHub::RetryPendingDelivery(uint64_t now, bool& retry)
{
  CSRWSharedLock lock(m_lock);
  return m_sink && m_sink->RetryPendingDelivery(now, retry);
}

PreparedFrameBuffer CFrameHub::PrepareFrameBuffer(unsigned pitch,
  const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
  const RECT * dirtyRects, unsigned nbDirtyRects,
  const CFrameScheduler::Schedule& schedule, bool allowReadyReplacement)
{
  PreparedFrameBuffer result = {};
  CSRWExclusiveLock lock(m_lock);
  if (!m_sink)
    return result;

  const SinkTarget target = m_sink->PrepareFrameBuffer(
    pitch, srcFormat, dstFormat, dirtyRects, nbDirtyRects, schedule,
    allowReadyReplacement);
  if (!target.mem || target.slot >= MAX_SLOTS)
  {
    if (target.mem)
      m_sink->AbortFrameBuffer(target.slot);
    return result;
  }

  if (target.slot >= m_slots.size())
    m_slots.resize(target.slot + 1);
  if (++m_nextSerial == 0)
    ++m_nextSerial;
  m_slots[target.slot].serial = m_nextSerial;

  result.token.sink   = m_backend;
  result.token.epoch  = m_epoch;
  result.token.slot   = target.slot;
  result.token.serial = m_nextSerial;
  result.resourceSlot = target.slot;
  result.mem          = target.mem;
  result.heapOffset   = target.heapOffset;
  result.fullCopy     = target.fullCopy;
  return result;
}

bool CFrameHub::PublishFrameBuffer(const FrameToken& token,
  const CFrameScheduler::Schedule& schedule, bool& deliveredToOwner)
{
  CSRWSharedLock lock(m_lock);
  return Valid(token) &&
    m_sink->PublishFrameBuffer(token.slot, schedule, deliveredToOwner);
}

bool CFrameHub::RepublishFrameBuffer(
  const CFrameScheduler::Schedule& schedule)
{
  CSRWSharedLock lock(m_lock);
  return m_sink && m_sink->RepublishFrameBuffer(schedule);
}

bool CFrameHub::TryFrameSubmitted(const FrameToken& token,
  const CFrameScheduler::Schedule& schedule)
{
  CSRWSharedLock lock(m_lock);
  return Valid(token) &&
    m_sink->TryFrameSubmitted(token.slot, schedule);
}

void CFrameHub::CommitFrameBuffer(const FrameToken& token,
  const CFrameScheduler::Schedule& schedule, bool periodic,
  bool deliveredToOwner)
{
  CSRWExclusiveLock lock(m_lock);
  if (Valid(token))
  {
    m_sink->CommitFrameBuffer(
      token.slot, schedule, periodic, deliveredToOwner);
    Slot& slot     = m_slots[token.slot];
    slot.committed = true;
    if (slot.completionPending)
    {
      m_sink->CompleteFrameBuffer(
        token.slot, slot.completionSucceeded);
      Invalidate(token);
    }
  }
}

void CFrameHub::AbortFrameBuffer(const FrameToken& token)
{
  CSRWExclusiveLock lock(m_lock);
  if (Valid(token))
  {
    m_sink->AbortFrameBuffer(token.slot);
    Invalidate(token);
  }
}

void CFrameHub::FailFrameBuffer(const FrameToken& token)
{
  CSRWExclusiveLock lock(m_lock);
  if (Valid(token))
  {
    m_sink->FailFrameBuffer(token.slot);
    Invalidate(token);
  }
}

void CFrameHub::CompleteFrameBuffer(
  const FrameToken& token, bool succeeded)
{
  CSRWExclusiveLock lock(m_lock);
  if (Valid(token))
  {
    Slot& slot = m_slots[token.slot];
    if (slot.committed)
    {
      m_sink->CompleteFrameBuffer(token.slot, succeeded);
      Invalidate(token);
    }
    else
    {
      slot.completionPending   = true;
      slot.completionSucceeded = succeeded;
    }
  }
}

void CFrameHub::SetFrameTiming(const FrameToken& token,
  uint64_t captureTime, uint64_t postProcessTime, uint64_t copyTime,
  uint64_t readyTime, uint64_t holdTime,
  const CFrameScheduler::Schedule& schedule, uint64_t completedAt)
{
  CSRWSharedLock lock(m_lock);
  if (Valid(token))
    m_sink->SetFrameTiming(token.slot, captureTime, postProcessTime,
      copyTime, readyTime, holdTime, schedule, completedAt);
}

void CFrameHub::WriteFrameBuffer(const FrameToken& token, void * src,
  size_t offset, size_t len, bool setWritePos) const
{
  CSRWSharedLock lock(m_lock);
  if (Valid(token))
    m_sink->WriteFrameBuffer(
      token.slot, src, offset, len, setWritePos);
}

void CFrameHub::WriteFrameBufferRows(const FrameToken& token, void * src,
  size_t offset, size_t rowBytes, size_t pitch, unsigned rows) const
{
  CSRWSharedLock lock(m_lock);
  if (Valid(token))
    m_sink->WriteFrameBufferRows(
      token.slot, src, offset, rowBytes, pitch, rows);
}

void CFrameHub::FinalizeFrameBuffer(const FrameToken& token) const
{
  CSRWSharedLock lock(m_lock);
  if (Valid(token))
    m_sink->FinalizeFrameBuffer(token.slot);
}

void CFrameHub::ObserveFrame(uint64_t now)
{
  CSRWSharedLock lock(m_lock);
  if (m_sink)
    m_sink->ObserveFrame(now);
}

void CFrameHub::ForceFrame()
{
  CSRWSharedLock lock(m_lock);
  if (m_sink)
    m_sink->ForceFrame();
}

bool CFrameHub::GetPublishTarget(uint64_t now, uint64_t& target,
  CFrameScheduler::Schedule& schedule, bool& periodic, bool& republish)
{
  CSRWSharedLock lock(m_lock);
  return m_sink && m_sink->GetPublishTarget(
    now, target, schedule, periodic, republish);
}

void CFrameHub::FrameMissed(const CFrameScheduler::Schedule& schedule,
  uint64_t now, bool periodic)
{
  CSRWSharedLock lock(m_lock);
  if (m_sink)
    m_sink->FrameMissed(schedule, now, periodic);
}

void CFrameHub::FrameSuperseded()
{
  CSRWSharedLock lock(m_lock);
  if (m_sink)
    m_sink->FrameSuperseded();
}

HANDLE CFrameHub::GetFrameScheduleEvent() const
{
  CSRWSharedLock lock(m_lock);
  return m_sink ? m_sink->GetFrameScheduleEvent() : nullptr;
}

void CFrameHub::TryRecordFrameTiming(
  const FrameToken& token, uint64_t duration)
{
  CSRWSharedLock lock(m_lock);
  if (Valid(token))
    m_sink->TryRecordFrameTiming(duration);
}
