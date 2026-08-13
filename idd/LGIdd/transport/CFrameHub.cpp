/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "transport/CFrameHub.h"

#include "Atomic.h"
#include "CDebug.h"

static const uint64_t RETRY_NS = 1000000ULL;

static void Earlier(uint64_t value, uint64_t& target)
{
  if (value && (!target || value < target))
    target = value;
}

static bool ContentAfter(uint64_t value, uint64_t current)
{
  return value && (!current ||
    static_cast<int64_t>(value - current) > 0);
}

static bool TokenMatches(const FrameToken& left, const FrameToken& right)
{
  return left.backend == right.backend &&
    left.epoch == right.epoch && left.slot == right.slot &&
    left.serial == right.serial;
}

CFrameHub::CFrameHub()
{
  m_wakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  for (Sink& sink : m_sinks)
    sink.drained = CreateEvent(nullptr, TRUE, TRUE, nullptr);
}

CFrameHub::~CFrameHub()
{
  for (Sink& sink : m_sinks)
  {
    const BackendId backend =
      Atomic::Load(sink.backend, std::memory_order_acquire);
    const uint32_t epoch =
      Atomic::Load(sink.epoch, std::memory_order_acquire);
    if (backend && epoch)
      Unbind(backend, epoch);
  }

  for (Sink& sink : m_sinks)
  {
    if (sink.drained)
      CloseHandle(sink.drained);
  }
  if (m_wakeEvent)
    CloseHandle(m_wakeEvent);
}

bool CFrameHub::Bind(BackendId backend, uint32_t epoch, bool primary,
  IFrameSink& target)
{
  if (!backend || !epoch || !m_wakeEvent)
    return false;

  Sink * selected = nullptr;
  {
    CSRWExclusiveLock lock(m_listLock);
    if (primary)
    {
      if (!Atomic::Load(
            m_sinks[0].active, std::memory_order_acquire) &&
          !m_sinks[0].reserved)
        selected = &m_sinks[0];
    }
    else
      for (unsigned i = 1; i < FRAME_MAX_SINKS; ++i)
        if (!Atomic::Load(
              m_sinks[i].active, std::memory_order_acquire) &&
            !m_sinks[i].reserved)
        {
          selected = &m_sinks[i];
          break;
        }

    if (!selected || !selected->drained)
      return false;
    selected->reserved = true;
  }

  {
    CSRWExclusiveLock lock(selected->callLock);
    selected->target        = &target;
    Atomic::Store(selected->backend, backend, std::memory_order_release);
    Atomic::Store(selected->epoch, epoch, std::memory_order_release);
    selected->primary       = primary;
    Atomic::Store(selected->outstanding, 0, std::memory_order_release);
    SetEvent(selected->drained);
  }
  {
    CSRWExclusiveLock lock(selected->laneLock);
    selected->needsFullCopy = true;
    selected->lastContent   = 0;
    selected->lastTerminalContent = 0;
    selected->blockedContent   = 0;
    selected->blockedFrameSize = 0;
    selected->blockedAllocation = false;
    selected->pitch         = 0;
    selected->width         = 0;
    selected->height        = 0;
    selected->format        = DXGI_FORMAT_UNKNOWN;
    selected->frameType     = FRAME_TYPE_INVALID;
    for (Sink::ResourceLane& lane : selected->lanes)
      lane = {};
  }

  target.SetFrameEvents(this);
  target.SetFrameScheduleEvent(m_wakeEvent);
  {
    CSRWExclusiveLock lock(m_listLock);
    Atomic::Store(selected->active, true, std::memory_order_release);
    selected->reserved = false;
  }
  target.ForceFrame();
  SetEvent(m_wakeEvent);
  return true;
}

void CFrameHub::Unbind(BackendId backend, uint32_t epoch)
{
  Sink * selected = nullptr;
  {
    CSRWExclusiveLock lock(m_listLock);
    for (Sink& sink : m_sinks)
      if (Atomic::Load(sink.active, std::memory_order_acquire) &&
          Atomic::Load(sink.backend, std::memory_order_acquire) == backend &&
          Atomic::Load(sink.epoch, std::memory_order_acquire) == epoch)
      {
        Atomic::Store(sink.active, false, std::memory_order_release);
        sink.reserved = true;
        selected = &sink;
        break;
      }
  }
  if (!selected)
    return;

  IFrameSink * target;
  {
    CSRWExclusiveLock lock(selected->callLock);
    target = selected->target;
  }
  if (target)
    target->SetFrameScheduleEvent(nullptr);

  FrameToken cancel[FRAME_SINK_BUFFERS] = {};
  unsigned cancelCount = 0;
  {
    CSRWExclusiveLock lock(selected->laneLock);
    for (Sink::ResourceLane& lane : selected->lanes)
    {
      if (lane.phase == Sink::ResourceLane::PENDING)
        cancel[cancelCount++] = lane.token;
      else if (lane.phase == Sink::ResourceLane::FILLING)
        lane.cancelRequested = true;
    }
  }
  for (unsigned i = 0; i < cancelCount; ++i)
    for (unsigned lane = 0; lane < FRAME_SINK_BUFFERS; ++lane)
    {
      CSRWSharedLock lock(selected->laneLock);
      if (!TokenMatches(selected->lanes[lane].token, cancel[i]))
        continue;
      lock.Unlock();
      CancelLane(*selected, lane, cancel[i]);
      break;
    }

  WaitForSingleObject(selected->drained, INFINITE);
  if (target)
    target->SetFrameEvents(nullptr);
  {
    CSRWExclusiveLock lock(selected->callLock);
    selected->target        = nullptr;
    Atomic::Store(selected->backend, 0, std::memory_order_release);
    Atomic::Store(selected->epoch, 0, std::memory_order_release);
    selected->primary       = false;
  }
  {
    CSRWExclusiveLock lock(selected->laneLock);
    selected->needsFullCopy = true;
    selected->lastContent   = 0;
    selected->lastTerminalContent = 0;
    selected->blockedContent   = 0;
    selected->blockedFrameSize = 0;
    selected->blockedAllocation = false;
    for (Sink::ResourceLane& lane : selected->lanes)
      lane = {};
  }
  {
    CSRWExclusiveLock lock(m_listLock);
    selected->reserved = false;
  }
  SetEvent(m_wakeEvent);
}

unsigned CFrameHub::Snapshot(SinkRef refs[FRAME_MAX_SINKS]) const
{
  unsigned count = 0;
  CSRWSharedLock lock(m_listLock);
  for (unsigned i = 0; i < FRAME_MAX_SINKS; ++i)
    if (Atomic::Load(m_sinks[i].active, std::memory_order_acquire))
      refs[count++] = {
        const_cast<Sink *>(&m_sinks[i]), i, m_sinks[i].primary };
  return count;
}

bool CFrameHub::BatchValid(
  const Batch& batch, const FrameBatchToken& token) const
{
  return token.serial && batch.active && batch.serial == token.serial;
}

void CFrameHub::ReleaseTarget(Batch& batch, BatchTarget& target)
{
  if (!target.active || target.releasePending)
    return;
  target.releasePending = true;
  if (target.resourceLane < FRAME_SINK_BUFFERS)
  {
    CSRWExclusiveLock lock(target.sink->laneLock);
    target.sink->lanes[target.resourceLane].busy = false;
  }
  if (Atomic::FetchSub(target.sink->outstanding,
        1, std::memory_order_acq_rel) == 1)
    SetEvent(target.sink->drained);

  target.active = false;

  for (unsigned i = 0; i < batch.count; ++i)
    if (batch.targets[i].active)
      return;
  batch.active = false;
}

bool CFrameHub::DetachTarget(Batch& batch, BatchTarget& target,
  FrameToken& token)
{
  if (!target.active || target.releasePending ||
      target.resourceLane >= FRAME_SINK_BUFFERS)
    return false;

  token.backend = target.backend;
  token.epoch   = target.epoch;
  token.slot    = target.localSlot;
  token.serial  = batch.serial;
  {
    CSRWExclusiveLock lock(target.sink->laneLock);
    Sink::ResourceLane& lane =
      target.sink->lanes[target.resourceLane];
    if (!lane.busy || lane.phase != Sink::ResourceLane::IDLE)
      return false;
    lane.token           = token;
    lane.schedule        = target.schedule;
    lane.content         = target.content;
    lane.captureTime     = target.captureTime;
    lane.postProcessTime = target.postProcessTime;
    lane.copyTime        = target.copyTime;
    lane.readyTime       = target.readyTime;
    lane.holdTime        = target.holdTime;
    lane.filledAt        = target.filledAt;
    lane.workStart       = target.workStart;
    lane.pitch           = target.pitch;
    lane.width           = target.width;
    lane.height          = target.height;
    lane.format          = target.format;
    lane.frameType       = target.frameType;
    lane.phase           = Sink::ResourceLane::FILLING;
    lane.timingValid     = target.timingValid;
    lane.resultPending   = false;
    lane.callActive      = false;
    lane.cancelRequested = false;
  }

  target.releasePending = true;
  target.active         = false;
  for (unsigned i = 0; i < batch.count; ++i)
    if (batch.targets[i].active)
      return true;
  batch.active = false;
  return true;
}

void CFrameHub::FillLane(Sink& sink, unsigned laneIndex,
  const FrameToken& token)
{
  if (laneIndex >= FRAME_SINK_BUFFERS)
    return;

  {
    CSRWExclusiveLock lock(sink.laneLock);
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (lane.phase != Sink::ResourceLane::FILLING ||
        !TokenMatches(lane.token, token))
      return;
    lane.callActive = true;
  }

  IFrameSink * target = sink.target;
  const FrameFill fill = target ? target->FrameFilled(token) :
    FrameFill::REJECTED;

  bool      terminal = false;
  bool      cancel   = false;
  FrameDone result   = FrameDone::FAILED;
  uint64_t  readyAt  = 0;
  {
    CSRWExclusiveLock lock(sink.laneLock);
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (lane.phase != Sink::ResourceLane::FILLING ||
        !TokenMatches(lane.token, token))
      return;
    lane.callActive = false;
    if (lane.resultPending)
    {
      terminal           = true;
      result             = lane.pendingResult;
      readyAt            = lane.pendingReadyAt;
      lane.resultPending = false;
    }
    else if (fill == FrameFill::READY)
    {
      terminal = true;
      result   = FrameDone::READY;
    }
    else if (fill == FrameFill::REJECTED)
      terminal = true;
    else
    {
      lane.phase = Sink::ResourceLane::PENDING;
      cancel = lane.cancelRequested ||
        !Atomic::Load(sink.active, std::memory_order_acquire);
    }
  }

  if (terminal)
    CompleteLane(sink, laneIndex, token, result, readyAt);
  else if (cancel)
    CancelLane(sink, laneIndex, token);
}

void CFrameHub::CancelLane(Sink& sink, unsigned laneIndex,
  const FrameToken& token)
{
  if (laneIndex >= FRAME_SINK_BUFFERS)
    return;

  {
    CSRWExclusiveLock lock(sink.laneLock);
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (lane.phase != Sink::ResourceLane::PENDING ||
        !TokenMatches(lane.token, token))
      return;
    lane.phase      = Sink::ResourceLane::CANCELING;
    lane.callActive = true;
  }

  IFrameSink * target = sink.target;
  if (target)
    target->CancelFrame(token);

  FrameDone result   = FrameDone::SUPERSEDED;
  uint64_t  readyAt  = 0;
  {
    CSRWExclusiveLock lock(sink.laneLock);
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (lane.phase != Sink::ResourceLane::CANCELING ||
        !TokenMatches(lane.token, token))
      return;
    lane.callActive = false;
    if (lane.resultPending)
    {
      result             = lane.pendingResult;
      readyAt            = lane.pendingReadyAt;
      lane.resultPending = false;
    }
  }
  CompleteLane(sink, laneIndex, token, result, readyAt);
}

void CFrameHub::CompleteLane(Sink& sink, unsigned laneIndex,
  const FrameToken& token, FrameDone result, uint64_t readyAt)
{
  if (laneIndex >= FRAME_SINK_BUFFERS)
    return;

  Sink::ResourceLane completed;
  {
    CSRWExclusiveLock lock(sink.laneLock);
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (!lane.busy || lane.phase == Sink::ResourceLane::IDLE ||
        lane.phase == Sink::ResourceLane::COMPLETING ||
        !TokenMatches(lane.token, token))
      return;
    lane.phase = Sink::ResourceLane::COMPLETING;
    completed  = lane;
  }

  if (!readyAt)
    readyAt = CFrameScheduler::Nanotime();
  IFrameSink * target = sink.target;
  if (target)
  {
    if (result == FrameDone::READY && completed.timingValid)
    {
      uint64_t readyTime = completed.readyTime;
      if (readyAt >= completed.filledAt)
        readyTime += readyAt - completed.filledAt;
      target->SetFrameTiming(completed.localSlot, completed.captureTime,
        completed.postProcessTime, completed.copyTime, readyTime,
        completed.holdTime, completed.schedule, readyAt);
      if (completed.workStart && readyAt >= completed.workStart)
        target->TryRecordFrameTiming(readyAt - completed.workStart);
    }
    target->CompleteFrameBuffer(completed.localSlot, result);
  }

  {
    CSRWExclusiveLock lock(sink.laneLock);
    const bool newestTerminal =
      ContentAfter(completed.content, sink.lastTerminalContent);
    if (newestTerminal)
      sink.lastTerminalContent = completed.content;
    if (result == FrameDone::READY &&
        (newestTerminal ||
         completed.content == sink.lastTerminalContent))
    {
      if (ContentAfter(completed.content, sink.lastContent))
      {
        sink.lastContent   = completed.content;
        sink.pitch         = completed.pitch;
        sink.width         = completed.width;
        sink.height        = completed.height;
        sink.format        = completed.format;
        sink.frameType     = completed.frameType;
        sink.needsFullCopy = false;
        sink.blockedContent   = 0;
        sink.blockedFrameSize = 0;
        sink.blockedAllocation = false;
      }
    }
    else if (newestTerminal)
      sink.needsFullCopy = true;
    Sink::ResourceLane& lane = sink.lanes[laneIndex];
    if (lane.phase == Sink::ResourceLane::COMPLETING &&
        TokenMatches(lane.token, token))
    {
      lane.token           = {};
      lane.schedule        = {};
      lane.content         = 0;
      lane.phase           = Sink::ResourceLane::IDLE;
      lane.timingValid     = false;
      lane.resultPending   = false;
      lane.callActive      = false;
      lane.cancelRequested = false;
      lane.busy            = false;
    }
  }
  if (Atomic::FetchSub(sink.outstanding,
        1, std::memory_order_acq_rel) == 1)
    SetEvent(sink.drained);
  SetEvent(m_wakeEvent);
}

void CFrameHub::OnFrameDone(const FrameToken& token, FrameDone result,
  uint64_t readyAt)
{
  if (!token.backend || !token.epoch || !token.serial)
    return;

  for (Sink& sink : m_sinks)
  {
    if (Atomic::Load(sink.backend, std::memory_order_acquire) !=
          token.backend ||
        Atomic::Load(sink.epoch, std::memory_order_acquire) != token.epoch)
      continue;

    unsigned laneIndex = FRAME_SINK_BUFFERS;
    {
      CSRWExclusiveLock lock(sink.laneLock);
      for (unsigned i = 0; i < FRAME_SINK_BUFFERS; ++i)
      {
        Sink::ResourceLane& lane = sink.lanes[i];
        if (!lane.busy || !TokenMatches(lane.token, token) ||
            lane.phase == Sink::ResourceLane::IDLE ||
            lane.phase == Sink::ResourceLane::COMPLETING)
          continue;
        if (lane.callActive)
        {
          if (!lane.resultPending)
          {
            lane.pendingResult  = result;
            lane.pendingReadyAt = readyAt;
            lane.resultPending  = true;
          }
          return;
        }
        if (lane.phase == Sink::ResourceLane::PENDING ||
            lane.phase == Sink::ResourceLane::CANCELING)
          laneIndex = i;
        break;
      }
    }
    if (laneIndex < FRAME_SINK_BUFFERS)
      CompleteLane(sink, laneIndex, token, result, readyAt);
    return;
  }
}

size_t CFrameHub::GetMaxFrameSize() const
{
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
    if (refs[i].primary)
    {
      CSRWSharedLock call(refs[i].sink->callLock);
      if (Atomic::Load(
            refs[i].sink->active, std::memory_order_acquire) &&
          refs[i].sink->target)
        return refs[i].sink->target->GetMaxFrameSize();
    }
  return 0;
}

uint64_t CFrameHub::NextContentSerial()
{
  return Atomic::Next(m_nextContent, std::memory_order_acq_rel);
}

void CFrameHub::FrameProductReady(uint64_t contentSerial)
{
  uint64_t newest =
    Atomic::Load(m_newestContent, std::memory_order_acquire);
  while (ContentAfter(contentSerial, newest) &&
         !Atomic::CASWeak(m_newestContent, newest, contentSerial,
           std::memory_order_acq_rel, std::memory_order_acquire))
  {
  }
  SetEvent(m_wakeEvent);
}

bool CFrameHub::NeedsFrame() const
{
  const uint64_t newest =
    Atomic::Load(m_newestContent, std::memory_order_acquire);
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    CSRWSharedLock call(refs[i].sink->callLock);
    if (!Atomic::Load(
          refs[i].sink->active, std::memory_order_acquire) ||
        !refs[i].sink->target)
      continue;
    const size_t maxFrameSize = refs[i].sink->target->GetMaxFrameSize();
    CSRWSharedLock state(refs[i].sink->laneLock);
    if (refs[i].sink->blockedContent == newest &&
        refs[i].sink->blockedFrameSize &&
        (refs[i].sink->blockedAllocation ||
         maxFrameSize < refs[i].sink->blockedFrameSize))
      continue;
    if (refs[i].sink->needsFullCopy ||
        ContentAfter(newest, refs[i].sink->lastContent))
      return true;
  }
  return false;
}

bool CFrameHub::GetFramePlan(
  uint64_t now, bool, FramePlan& plan)
{
  plan = {};
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    Sink& sink = *refs[i].sink;
    CSRWSharedLock call(sink.callLock);
    if (!Atomic::Load(sink.active, std::memory_order_acquire) ||
        !sink.target)
      continue;

    sink.target->ProcessDeliveries();
    uint64_t deliveryTarget = 0;
    if (sink.target->GetPendingDeliveryTarget(now, deliveryTarget))
    {
      if (deliveryTarget <= now)
      {
        bool retry = false;
        if (sink.target->RetryPendingDelivery(now, retry))
          plan.progressed = true;
        else if (retry)
          Earlier(now + RETRY_NS, plan.nextWake);
      }
      else
        Earlier(deliveryTarget, plan.nextWake);
    }

    uint64_t target = 0;
    CFrameScheduler::Schedule schedule = {};
    bool periodic  = false;
    bool republish = false;
    if (!sink.target->GetPublishTarget(
          now, target, schedule, periodic, republish))
      continue;

    if (target > now)
    {
      Earlier(target, plan.nextWake);
      continue;
    }

    if (republish && sink.target->HasPublishedFrame())
    {
      if (sink.target->RepublishFrameBuffer(schedule))
      {
        plan.progressed = true;
        continue;
      }
      else
      {
        Earlier(now + RETRY_NS, plan.nextWake);
        continue;
      }
    }

    if (plan.count == FRAME_MAX_SINKS)
      continue;
    FramePlanTarget& request = plan.targets[plan.count++];
    request.sink     = refs[i].index;
    request.backend  = Atomic::Load(
      sink.backend, std::memory_order_acquire);
    request.epoch    = Atomic::Load(
      sink.epoch, std::memory_order_acquire);
    request.schedule = schedule;
    request.commitSchedule = schedule;
    request.periodic = periodic;
    request.primary  = sink.primary;
  }
  return plan.count != 0;
}

bool CFrameHub::GetImmediateFramePlan(uint64_t now, FramePlan& plan)
{
  plan = {};
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    Sink& sink = *refs[i].sink;
    CSRWSharedLock call(sink.callLock);
    if (!Atomic::Load(sink.active, std::memory_order_acquire) ||
        !sink.target)
      continue;

    sink.target->ProcessDeliveries();
    uint64_t deliveryTarget = 0;
    if (sink.target->GetPendingDeliveryTarget(now, deliveryTarget) &&
        deliveryTarget <= now)
    {
      bool retry = false;
      if (sink.target->RetryPendingDelivery(now, retry))
        plan.progressed = true;
    }

    uint64_t target = 0;
    CFrameScheduler::Schedule schedule = {};
    bool periodic  = false;
    bool republish = false;
    sink.target->GetPublishTarget(
      now, target, schedule, periodic, republish);

    if (plan.count == FRAME_MAX_SINKS)
      continue;
    FramePlanTarget& request = plan.targets[plan.count++];
    request.sink           = refs[i].index;
    request.backend        = Atomic::Load(
      sink.backend, std::memory_order_acquire);
    request.epoch          = Atomic::Load(
      sink.epoch, std::memory_order_acquire);
    request.schedule       = schedule;
    request.schedule.deliveryDeadlineSerial = 0;
    request.schedule.phaseEligible          = false;
    request.commitSchedule = schedule;
    request.periodic       = false;
    request.primary        = sink.primary;
  }
  return plan.count != 0;
}

void CFrameHub::MissFramePlan(const FramePlan& plan, uint64_t now)
{
  for (unsigned i = 0; i < plan.count; ++i)
  {
    const FramePlanTarget& request = plan.targets[i];
    if (!request.periodic ||
        !request.schedule.deliveryDeadlineSerial ||
        request.schedule.deadline > now ||
        request.sink >= FRAME_MAX_SINKS)
      continue;
    Sink& sink = m_sinks[request.sink];
    CSRWSharedLock call(sink.callLock);
    if (Atomic::Load(sink.active, std::memory_order_acquire) &&
        sink.target &&
        Atomic::Load(sink.backend, std::memory_order_acquire) ==
          request.backend &&
        Atomic::Load(sink.epoch, std::memory_order_acquire) ==
          request.epoch)
      sink.target->FrameMissed(
        request.commitSchedule, now, request.periodic);
  }
}

bool CFrameHub::PrepareFrameBatch(const FramePlan& plan,
  uint64_t contentSerial, unsigned pitch, size_t frameSize,
  const D12FrameFormat& srcFormat, const D12FrameFormat& dstFormat,
  const RECT * dirtyRects, unsigned nbDirtyRects,
  bool allowReadyReplacement, PreparedFrameBatch& prepared)
{
  prepared = {};
  if (!contentSerial || !pitch || !frameSize)
    return false;

  Batch * batch = nullptr;
  unsigned batchSlot = 0;
  for (; batchSlot < FRAME_BATCHES; ++batchSlot)
  {
    Batch& candidate = m_batches[batchSlot];
    CSRWExclusiveLock lock = CSRWExclusiveLock::Try(candidate.lock);
    if (!lock || candidate.active)
      continue;
    candidate.active = true;
    candidate.count  = 0;
    candidate.serial = Atomic::Next(
      m_nextSerial, std::memory_order_acq_rel);
    for (BatchTarget& target : candidate.targets)
      target = {};
    prepared.token = {
      static_cast<uint32_t>(batchSlot), candidate.serial };
    batch = &candidate;
    lock.Unlock();
    break;
  }
  if (!batch)
    return false;

  CSRWExclusiveLock batchLock(batch->lock);
  for (unsigned i = 0; i < plan.count &&
       batch->count < FRAME_MAX_SINKS; ++i)
  {
    const FramePlanTarget& request = plan.targets[i];
    if (request.sink >= FRAME_MAX_SINKS)
      continue;
    Sink& sink = m_sinks[request.sink];
    CSRWExclusiveLock call(sink.callLock);
    if (!Atomic::Load(sink.active, std::memory_order_acquire) ||
        !sink.target ||
        Atomic::Load(sink.backend, std::memory_order_acquire) !=
          request.backend ||
        Atomic::Load(sink.epoch, std::memory_order_acquire) !=
          request.epoch ||
        !sink.target->FrameBufferAvailable(
          request.schedule, allowReadyReplacement))
      continue;

    const size_t maxFrameSize = sink.target->GetMaxFrameSize();
    if (!maxFrameSize || frameSize > maxFrameSize)
    {
      CSRWExclusiveLock lock(sink.laneLock);
      sink.blockedContent    = contentSerial;
      sink.blockedFrameSize  = frameSize;
      sink.blockedAllocation = false;
      continue;
    }

    bool forceFull;
    {
      CSRWSharedLock lock(sink.laneLock);
      const bool layoutChanged = sink.pitch != pitch ||
        sink.width != dstFormat.width ||
        sink.height != dstFormat.height ||
        sink.format != dstFormat.desc.Format ||
        sink.frameType != dstFormat.format;
      forceFull = sink.needsFullCopy || layoutChanged ||
        sink.lastContent + 1 != contentSerial;
    }
    const RECT * damage = forceFull ? nullptr : dirtyRects;
    const unsigned damageCount = forceFull ? 0 : nbDirtyRects;
    SinkTarget result = sink.target->PrepareFrameBuffer(
      pitch, srcFormat, dstFormat, damage, damageCount,
      request.schedule, allowReadyReplacement);
    if (!result.mem || result.capacity < frameSize)
    {
      if (result.mem)
      {
        {
          CSRWExclusiveLock lock(sink.laneLock);
          sink.blockedContent    = contentSerial;
          sink.blockedFrameSize  = frameSize;
          sink.blockedAllocation = true;
        }
        sink.target->AbortFrameBuffer(result.slot);
      }
      continue;
    }

    {
      CSRWExclusiveLock lock(sink.laneLock);
      unsigned resourceLane = FRAME_SINK_BUFFERS;
      for (unsigned lane = 0; lane < FRAME_SINK_BUFFERS; ++lane)
      {
        const Sink::ResourceLane& candidate = sink.lanes[lane];
        if (!candidate.busy && candidate.valid &&
            candidate.mem == result.mem &&
            candidate.heapOffset == result.heapOffset &&
            candidate.localSlot == result.slot &&
            candidate.direct == request.primary)
        {
          resourceLane = lane;
          break;
        }
      }
      if (resourceLane == FRAME_SINK_BUFFERS)
        for (unsigned lane = 0; lane < FRAME_SINK_BUFFERS; ++lane)
          if (!sink.lanes[lane].busy && !sink.lanes[lane].valid)
          {
            resourceLane = lane;
            break;
          }
      if (resourceLane == FRAME_SINK_BUFFERS)
        for (unsigned lane = 0; lane < FRAME_SINK_BUFFERS; ++lane)
          if (!sink.lanes[lane].busy)
          {
            resourceLane = lane;
            break;
          }
      if (resourceLane == FRAME_SINK_BUFFERS)
      {
        lock.Unlock();
        sink.target->AbortFrameBuffer(result.slot);
        continue;
      }

      Sink::ResourceLane& lane = sink.lanes[resourceLane];
      lane.mem        = result.mem;
      lane.heapOffset = result.heapOffset;
      lane.localSlot  = result.slot;
      lane.direct     = request.primary;
      lane.valid      = true;
      lane.busy       = true;
      lane.phase      = Sink::ResourceLane::IDLE;

      unsigned index = batch->count;
      if (sink.primary && index)
      {
        for (unsigned move = index; move != 0; --move)
        {
          batch->targets[move] = batch->targets[move - 1];
          prepared.targets[move] = prepared.targets[move - 1];
        }
        index = 0;
      }
      ++batch->count;
      BatchTarget& target = batch->targets[index];
      target.sink       = &sink;
      target.backend    = request.backend;
      target.epoch      = request.epoch;
      target.localSlot  = result.slot;
      target.resourceLane = resourceLane;
      target.schedule   = request.commitSchedule;
      target.deliverySchedule = request.schedule;
      target.content    = contentSerial;
      target.pitch      = pitch;
      target.width      = dstFormat.width;
      target.height     = dstFormat.height;
      target.format     = dstFormat.desc.Format;
      target.frameType  = dstFormat.format;
      target.periodic   = request.periodic;
      target.active     = true;
      if (Atomic::FetchAdd(sink.outstanding,
            1, std::memory_order_acq_rel) == 0)
        ResetEvent(sink.drained);

      PreparedFrameBuffer& output = prepared.targets[index];
      output.token.backend = request.backend;
      output.token.epoch  = request.epoch;
      output.token.slot   = result.slot;
      output.token.serial = batch->serial;
      output.resourceSlot =
        request.sink * FRAME_SINK_BUFFERS + resourceLane;
      output.mem          = result.mem;
      output.heapOffset   = result.heapOffset;
      output.capacity     = result.capacity;
      output.direct       = request.primary;
      output.fullCopy     = forceFull || result.fullCopy;
    }
  }
  prepared.count = batch->count;
  if (!batch->count)
  {
    batch->active = false;
    prepared = {};
    return false;
  }
  return true;
}

uint32_t CFrameHub::PublishFrameBatch(const FrameBatchToken& token)
{
  if (token.slot >= FRAME_BATCHES)
    return 0;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token))
    return 0;

  uint32_t accepted = 0;
  for (unsigned i = 0; i < batch.count; ++i)
  {
    BatchTarget& target = batch.targets[i];
    if (!target.active)
      continue;
    CSRWExclusiveLock call(target.sink->callLock);
    bool delivered = false;
    const bool valid = target.sink->target &&
      Atomic::Load(target.sink->backend, std::memory_order_acquire) ==
        target.backend &&
      Atomic::Load(target.sink->epoch, std::memory_order_acquire) ==
        target.epoch;
    if (!valid || !target.sink->target->PublishFrameBuffer(
          target.localSlot, target.deliverySchedule, delivered))
    {
      if (valid)
        target.sink->target->AbortFrameBuffer(target.localSlot);
      {
        CSRWExclusiveLock state(target.sink->laneLock);
        target.sink->needsFullCopy = true;
      }
      ReleaseTarget(batch, target);
      continue;
    }

    target.published = true;
    target.delivered = delivered;
    target.submitted = delivered &&
      target.sink->target->TryFrameSubmitted(
        target.localSlot, target.deliverySchedule);
    if (!target.submitted)
      target.schedule.phaseEligible = false;
    accepted |= 1U << i;
  }
  return accepted;
}

void CFrameHub::CommitFrameBatch(const FrameBatchToken& token)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  struct FillRequest
  {
    Sink       * sink;
    unsigned     lane;
    FrameToken  token;
    bool        succeeded;
  } fill[FRAME_MAX_SINKS] = {};
  unsigned fillCount = 0;
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token))
    return;
  for (unsigned i = 0; i < batch.count; ++i)
  {
    BatchTarget& target = batch.targets[i];
    if (!target.active || !target.published)
      continue;
    {
      CSRWExclusiveLock call(target.sink->callLock);
      if (target.sink->target &&
          Atomic::Load(target.sink->backend,
            std::memory_order_acquire) == target.backend &&
          Atomic::Load(target.sink->epoch,
            std::memory_order_acquire) == target.epoch)
        target.sink->target->CommitFrameBuffer(target.localSlot,
          target.schedule, target.periodic, target.delivered);
    }
    target.committed = true;
    if (target.completionPending)
    {
      FrameToken frameToken;
      const unsigned lane = target.resourceLane;
      Sink * sink = target.sink;
      if (DetachTarget(batch, target, frameToken))
        fill[fillCount++] = {
          sink, lane, frameToken, target.completionSucceeded };
    }
  }
  lock.Unlock();
  for (unsigned i = 0; i < fillCount; ++i)
    if (fill[i].succeeded)
      FillLane(*fill[i].sink, fill[i].lane, fill[i].token);
    else
      CompleteLane(*fill[i].sink, fill[i].lane, fill[i].token,
        FrameDone::FAILED, CFrameScheduler::Nanotime());
}

void CFrameHub::AbortFrameBatch(const FrameBatchToken& token)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token))
    return;
  for (unsigned i = 0; i < batch.count; ++i)
  {
    BatchTarget& target = batch.targets[i];
    if (!target.active)
      continue;
    CSRWExclusiveLock call(target.sink->callLock);
    if (target.sink->target &&
        Atomic::Load(target.sink->backend,
          std::memory_order_acquire) == target.backend &&
        Atomic::Load(target.sink->epoch,
          std::memory_order_acquire) == target.epoch)
      target.sink->target->AbortFrameBuffer(target.localSlot);
    {
      CSRWExclusiveLock state(target.sink->laneLock);
      target.sink->needsFullCopy = true;
    }
    ReleaseTarget(batch, target);
  }
}

void CFrameHub::FailFrameBatch(const FrameBatchToken& token)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token))
    return;
  for (unsigned i = 0; i < batch.count; ++i)
  {
    BatchTarget& target = batch.targets[i];
    if (!target.active)
      continue;
    CSRWExclusiveLock call(target.sink->callLock);
    if (target.sink->target &&
        Atomic::Load(target.sink->backend,
          std::memory_order_acquire) == target.backend &&
        Atomic::Load(target.sink->epoch,
          std::memory_order_acquire) == target.epoch)
    {
      if (target.published)
        target.sink->target->FailFrameBuffer(target.localSlot);
      else
        target.sink->target->AbortFrameBuffer(target.localSlot);
    }
    {
      CSRWExclusiveLock state(target.sink->laneLock);
      target.sink->needsFullCopy = true;
    }
    ReleaseTarget(batch, target);
  }
}

void CFrameHub::WriteFrameTarget(const FrameBatchToken& token,
  unsigned index, void * src, size_t offset, size_t len,
  bool setWritePos) const
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = const_cast<Batch&>(m_batches[token.slot]);
  CSRWSharedLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  CSRWSharedLock call(target.sink->callLock);
  if (target.sink->target &&
      Atomic::Load(target.sink->backend, std::memory_order_acquire) ==
        target.backend &&
      Atomic::Load(target.sink->epoch, std::memory_order_acquire) ==
        target.epoch)
    target.sink->target->WriteFrameBuffer(
      target.localSlot, src, offset, len, setWritePos);
}

void CFrameHub::WriteFrameTargetRows(const FrameBatchToken& token,
  unsigned index, void * src, size_t offset, size_t rowBytes,
  size_t pitch, unsigned rows) const
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = const_cast<Batch&>(m_batches[token.slot]);
  CSRWSharedLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  CSRWSharedLock call(target.sink->callLock);
  if (target.sink->target &&
      Atomic::Load(target.sink->backend, std::memory_order_acquire) ==
        target.backend &&
      Atomic::Load(target.sink->epoch, std::memory_order_acquire) ==
        target.epoch)
    target.sink->target->WriteFrameBufferRows(target.localSlot, src,
      offset, rowBytes, pitch, rows);
}

void CFrameHub::FinalizeFrameTarget(
  const FrameBatchToken& token, unsigned index) const
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = const_cast<Batch&>(m_batches[token.slot]);
  CSRWSharedLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  CSRWSharedLock call(target.sink->callLock);
  if (target.sink->target &&
      Atomic::Load(target.sink->backend, std::memory_order_acquire) ==
        target.backend &&
      Atomic::Load(target.sink->epoch, std::memory_order_acquire) ==
        target.epoch)
    target.sink->target->FinalizeFrameBuffer(target.localSlot);
}

void CFrameHub::SetFrameTargetTiming(const FrameBatchToken& token,
  unsigned index, uint64_t captureTime, uint64_t postProcessTime,
  uint64_t copyTime, uint64_t readyTime, uint64_t holdTime,
  uint64_t completedAt)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  target.captureTime     = captureTime;
  target.postProcessTime = postProcessTime;
  target.copyTime        = copyTime;
  target.readyTime       = readyTime;
  target.holdTime        = holdTime;
  target.filledAt        = completedAt;
  target.timingValid     = true;
}

void CFrameHub::TryRecordFrameTiming(const FrameBatchToken& token,
  unsigned index, uint64_t duration)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  if (target.filledAt >= duration)
    target.workStart = target.filledAt - duration;
}

void CFrameHub::CompleteFrameTarget(const FrameBatchToken& token,
  unsigned index, bool succeeded)
{
  if (token.slot >= FRAME_BATCHES)
    return;
  Batch& batch = m_batches[token.slot];
  CSRWExclusiveLock lock(batch.lock);
  if (!BatchValid(batch, token) || index >= batch.count ||
      !batch.targets[index].active)
    return;
  BatchTarget& target = batch.targets[index];
  if (!target.committed)
  {
    target.completionPending   = true;
    target.completionSucceeded = succeeded;
    return;
  }
  FrameToken frameToken;
  const unsigned lane = target.resourceLane;
  Sink * sink = target.sink;
  if (!DetachTarget(batch, target, frameToken))
    return;
  lock.Unlock();
  if (succeeded)
    FillLane(*sink, lane, frameToken);
  else
    CompleteLane(*sink, lane, frameToken, FrameDone::FAILED,
      CFrameScheduler::Nanotime());
}

void CFrameHub::ObserveFrame(uint64_t now)
{
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    CSRWSharedLock call(refs[i].sink->callLock);
    if (Atomic::Load(
          refs[i].sink->active, std::memory_order_acquire) &&
        refs[i].sink->target)
      refs[i].sink->target->ObserveFrame(now);
  }
}

void CFrameHub::ForceFrame()
{
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    CSRWSharedLock call(refs[i].sink->callLock);
    if (Atomic::Load(
          refs[i].sink->active, std::memory_order_acquire) &&
        refs[i].sink->target)
      refs[i].sink->target->ForceFrame();
  }
}

void CFrameHub::FrameSuperseded()
{
  SinkRef refs[FRAME_MAX_SINKS];
  const unsigned count = Snapshot(refs);
  for (unsigned i = 0; i < count; ++i)
  {
    CSRWSharedLock call(refs[i].sink->callLock);
    if (Atomic::Load(
          refs[i].sink->active, std::memory_order_acquire) &&
        refs[i].sink->target)
      refs[i].sink->target->FrameSuperseded();
  }
}
