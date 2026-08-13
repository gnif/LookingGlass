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

#include "transport/CControlHub.h"

#include "CDebug.h"
#include "Seq.h"

#include <algorithm>
#include <cstring>

CControlHub::CControlHub()
{
  m_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  if (!m_stopEvent)
    return;

  m_valid = true;
  for (unsigned i = 0; i < MAX_SINKS; ++i)
  {
    Sink& sink = m_sinks[i];
    sink.owner = this;
    sink.index = i;
    sink.wake  = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    sink.idle  = CreateEvent(nullptr, TRUE, TRUE, nullptr);
    if (!sink.wake || !sink.idle)
    {
      m_valid = false;
      break;
    }

    sink.thread = CreateThread(nullptr, 0, WorkerProc, &sink, 0, nullptr);
    if (!sink.thread)
    {
      m_valid = false;
      break;
    }
  }

  if (!m_valid)
  {
    SetEvent(m_stopEvent);
    for (Sink& sink : m_sinks)
      if (sink.wake)
        SetEvent(sink.wake);
  }
}

CControlHub::~CControlHub()
{
  for (Sink& sink : m_sinks)
  {
    BackendId backend;
    uint32_t epoch;
    {
      CSRWSharedLock lock(sink.lock);
      backend = sink.backend;
      epoch   = sink.epoch;
    }
    if (backend && epoch)
      Remove(backend, epoch);
  }

  if (m_stopEvent)
    SetEvent(m_stopEvent);
  for (Sink& sink : m_sinks)
    if (sink.wake)
      SetEvent(sink.wake);

  for (Sink& sink : m_sinks)
  {
    if (sink.thread)
      WaitForSingleObject(sink.thread, INFINITE);
    if (sink.thread)
      CloseHandle(sink.thread);
    if (sink.idle)
      CloseHandle(sink.idle);
    if (sink.wake)
      CloseHandle(sink.wake);
  }
  if (m_stopEvent)
    CloseHandle(m_stopEvent);
}

DWORD WINAPI CControlHub::WorkerProc(void * opaque)
{
  Sink * sink = static_cast<Sink *>(opaque);
  sink->owner->Worker(*sink);
  return 0;
}

bool CControlHub::TokenMatches(
  const Sink& sink, const ControlToken& token)
{
  return sink.backend == token.backend && sink.epoch == token.epoch;
}

bool CControlHub::Add(
  BackendId backend, uint32_t epoch, IControlSink& control)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (!backend || !epoch || !m_valid)
    return false;

  Sink * selected = nullptr;
  {
    CSRWExclusiveLock lock(m_listLock);
    for (Sink& sink : m_sinks)
    {
      CSRWSharedLock sinkLock(sink.lock);
      if ((sink.active || sink.reserved) &&
          sink.backend == backend && sink.epoch == epoch)
        return false;
    }

    for (Sink& sink : m_sinks)
    {
      CSRWExclusiveLock sinkLock(sink.lock);
      if (!sink.active && !sink.failed && !sink.reserved)
      {
        sink.reserved = true;
        sink.backend  = backend;
        sink.epoch    = epoch;
        selected      = &sink;
        break;
      }
    }
  }
  if (!selected)
    return false;

  const ControlToken token = { backend, epoch };
  {
    CSRWExclusiveLock lock(selected->lock);
    selected->target             = &control;
    selected->deliveredPosition  = 0;
    selected->deliveredShape     = 0;
    selected->deliveredTransform = 0;
    memset(selected->retryAt, 0, sizeof(selected->retryAt));
    selected->nextWork       = 0;
    selected->bindingSerial  = Seq::Next(selected->bindingSerial);
    selected->replaySerial   = Seq::Next(selected->replaySerial);
    selected->calling        = false;
    selected->active         = false;
    selected->failed         = false;
    selected->failurePending = false;
  }
  control.SetControlEvents(this, token);
  bool attached = false;
  {
    CSRWExclusiveLock lock(selected->lock);
    if (selected->target == &control && selected->backend == backend &&
        selected->epoch == epoch && selected->reserved && !selected->failed)
    {
      selected->replaySerial = Seq::Next(selected->replaySerial);
      selected->active       = true;
      selected->reserved     = false;
      attached               = true;
    }
  }
  if (!attached)
  {
    control.SetControlEvents(nullptr, {});
    SetEvent(selected->wake);
    WaitForSingleObject(selected->idle, INFINITE);
    CSRWExclusiveLock listLock(m_listLock);
    CSRWExclusiveLock sinkLock(selected->lock);
    if (selected->target == &control && selected->backend == backend &&
        selected->epoch == epoch)
    {
      selected->target   = nullptr;
      selected->backend  = 0;
      selected->epoch    = 0;
      selected->active   = false;
      selected->reserved = false;
      selected->failed   = false;
      selected->failurePending = false;
      selected->calling  = false;
      memset(selected->retryAt, 0, sizeof(selected->retryAt));
    }
    return false;
  }
  SetEvent(selected->wake);
  return true;
}

void CControlHub::Remove(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Sink * selected = nullptr;
  IControlSink * target = nullptr;
  {
    CSRWExclusiveLock listLock(m_listLock);
    for (Sink& sink : m_sinks)
    {
      CSRWExclusiveLock lock(sink.lock);
      if ((sink.active || sink.failed) &&
          sink.backend == backend && sink.epoch == epoch)
      {
        sink.active   = false;
        sink.failed   = false;
        sink.failurePending = false;
        sink.reserved = true;
        target        = sink.target;
        selected      = &sink;
        break;
      }
    }
  }
  if (!selected)
    return;

  SetEvent(selected->wake);
  WaitForSingleObject(selected->idle, INFINITE);
  if (target)
    target->SetControlEvents(nullptr, {});

  {
    CSRWExclusiveLock listLock(m_listLock);
    CSRWExclusiveLock sinkLock(selected->lock);
    if (selected->reserved && selected->backend == backend &&
        selected->epoch == epoch)
    {
      selected->target   = nullptr;
      selected->backend  = 0;
      selected->epoch    = 0;
      selected->reserved = false;
      selected->failed   = false;
      selected->failurePending = false;
      selected->calling  = false;
      memset(selected->retryAt, 0, sizeof(selected->retryAt));
    }
  }
}

bool CControlHub::TakeFailure(ControlToken& token)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWSharedLock listLock(m_listLock);
  for (Sink& sink : m_sinks)
  {
    CSRWExclusiveLock lock(sink.lock);
    if (!sink.failurePending)
      continue;
    token.backend = sink.backend;
    token.epoch   = sink.epoch;
    sink.failurePending = false;
    return true;
  }
  return false;
}

void CControlHub::OnControlReplay(const ControlToken& token)
{
  CSRWSharedLock listLock(m_listLock);
  for (Sink& sink : m_sinks)
  {
    CSRWExclusiveLock lock(sink.lock);
    if ((!sink.active && !sink.reserved) ||
        !TokenMatches(sink, token))
      continue;

    sink.deliveredPosition  = 0;
    sink.deliveredShape     = 0;
    sink.deliveredTransform = 0;
    sink.replaySerial       = Seq::Next(sink.replaySerial);
    memset(sink.retryAt, 0, sizeof(sink.retryAt));
    SetEvent(sink.wake);
    return;
  }
}

bool CControlHub::BeginWork(Sink& sink, Work& work, DWORD& wait)
{
  const uint64_t now = GetTickCount64();
  State state;
  {
    CSRWSharedLock lock(m_stateLock);
    state.transform         = m_state.transform;
    state.cursor            = m_state.cursor;
    state.cursorData        = m_state.cursorData;
    state.sdrWhiteLevel     = m_state.sdrWhiteLevel;
    state.positionRevision  = m_state.positionRevision;
    state.shapeRevision     = m_state.shapeRevision;
    state.transformRevision = m_state.transformRevision;
  }

  CSRWExclusiveLock lock(sink.lock);
  if (!sink.active || !sink.target)
  {
    wait = INFINITE;
    return false;
  }

  const bool pending[] =
  {
    state.positionRevision != sink.deliveredPosition,
    state.shapeRevision != sink.deliveredShape,
    state.transformRevision != sink.deliveredTransform,
  };

  unsigned selected = static_cast<unsigned>(WorkType::COUNT);
  uint64_t earliest = 0;
  for (unsigned offset = 0;
       offset < static_cast<unsigned>(WorkType::COUNT); ++offset)
  {
    const unsigned index =
      (sink.nextWork + offset) % static_cast<unsigned>(WorkType::COUNT);
    if (!pending[index])
      continue;
    if (!sink.retryAt[index] || sink.retryAt[index] <= now)
    {
      selected = index;
      break;
    }
    if (!earliest || sink.retryAt[index] < earliest)
      earliest = sink.retryAt[index];
  }

  if (selected == static_cast<unsigned>(WorkType::COUNT))
  {
    wait = earliest ? static_cast<DWORD>(std::min<uint64_t>(
      earliest - now, MAXDWORD - 1)) : INFINITE;
    return false;
  }

  sink.nextWork =
    (selected + 1) % static_cast<unsigned>(WorkType::COUNT);
  sink.calling = true;
  ResetEvent(sink.idle);

  work.type               = static_cast<WorkType>(selected);
  work.target             = sink.target;
  work.sdrWhiteLevel      = state.sdrWhiteLevel;
  work.positionRevision   = state.positionRevision;
  work.shapeRevision      = state.shapeRevision;
  work.transformRevision  = state.transformRevision;
  work.bindingSerial      = sink.bindingSerial;
  work.replaySerial       = sink.replaySerial;

  if (work.type == WorkType::TRANSFORM)
    work.transform = std::move(state.transform);
  else
  {
    work.cursor.IsCursorVisible = state.cursor.IsCursorVisible;
    work.cursor.X               = state.cursor.X;
    work.cursor.Y               = state.cursor.Y;
    if (work.type == WorkType::SHAPE)
    {
      work.cursor.IsCursorShapeUpdated = true;
      work.cursor.CursorShapeInfo = state.cursor.CursorShapeInfo;
      work.cursorData = std::move(state.cursorData);
    }
    else
      work.cursor.CursorShapeInfo.CursorType =
        IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED;
  }
  wait = INFINITE;
  return true;
}

bool CControlHub::CompleteWork(
  Sink& sink, const Work& work, ControlResult result)
{
  CSRWExclusiveLock lock(sink.lock);
  if (sink.target != work.target ||
      sink.bindingSerial != work.bindingSerial)
    return false;
  if (!sink.active)
  {
    sink.calling = false;
    SetEvent(sink.idle);
    return false;
  }

  if (result == ControlResult::FAILED)
  {
    sink.active         = false;
    sink.failed         = true;
    sink.failurePending = true;
    sink.calling        = false;
    SetEvent(sink.idle);
    return true;
  }

  if (sink.replaySerial != work.replaySerial)
  {
    sink.calling = false;
    SetEvent(sink.idle);
    return true;
  }

  const unsigned index = static_cast<unsigned>(work.type);
  if (result == ControlResult::RETRY)
    sink.retryAt[index] = GetTickCount64() + RETRY_MS;
  else
  {
    sink.retryAt[index] = 0;
    switch (work.type)
    {
      case WorkType::POSITION:
        sink.deliveredPosition = work.positionRevision;
        break;

      case WorkType::SHAPE:
        sink.deliveredPosition = work.positionRevision;
        sink.deliveredShape    = work.shapeRevision;
        break;

      case WorkType::TRANSFORM:
        sink.deliveredTransform = work.transformRevision;
        break;

      case WorkType::COUNT:
        break;
    }
  }

  sink.calling = false;
  SetEvent(sink.idle);

  return true;
}

void CControlHub::Worker(Sink& sink)
{
  DWORD wait = INFINITE;
  HANDLE events[] = { m_stopEvent, sink.wake };
  for (;;)
  {
    const DWORD status = WaitForMultipleObjects(2, events, FALSE, wait);
    if (status == WAIT_OBJECT_0 || status == WAIT_FAILED)
      return;

    Work work;
    if (!BeginWork(sink, work, wait))
      continue;

    ControlResult result;
    if (work.type == WorkType::TRANSFORM)
      result = work.target->SetColorTransform(std::move(work.transform));
    else
      result = work.target->SendCursor(work.cursor,
        !work.cursorData || work.cursorData->empty() ? nullptr :
          work.cursorData->data(),
        work.cursorData ? work.cursorData->size() : 0,
        work.sdrWhiteLevel);

    const bool completed = CompleteWork(sink, work, result);
    if (result == ControlResult::FAILED && completed)
      DEBUG_WARN("Control update delivery failed");
    if (completed)
    {
      wait = result == ControlResult::RETRY ? RETRY_MS : 0;
      if (result != ControlResult::RETRY)
        SetEvent(sink.wake);
    }
    else
      wait = INFINITE;
  }
}

void CControlHub::WakeAll(WorkType type)
{
  const unsigned index = static_cast<unsigned>(type);
  CSRWSharedLock listLock(m_listLock);
  for (Sink& sink : m_sinks)
  {
    CSRWExclusiveLock lock(sink.lock);
    if (!sink.active)
      continue;
    sink.retryAt[index] = 0;
    SetEvent(sink.wake);
  }
}

void CControlHub::SendCursor(const IDARG_OUT_QUERY_HWCURSOR& info,
  const BYTE * data, UINT sdrWhiteLevel)
{
  const bool shape = info.CursorShapeInfo.CursorType !=
    IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED;
  std::shared_ptr<std::vector<BYTE>> cursorData;
  if (shape)
  {
    if (info.CursorShapeInfo.Height &&
        info.CursorShapeInfo.Pitch >
          SIZE_MAX / info.CursorShapeInfo.Height)
    {
      DEBUG_ERROR("Pointer shape size overflow");
      return;
    }
    const size_t size = static_cast<size_t>(
      info.CursorShapeInfo.Height) * info.CursorShapeInfo.Pitch;
    if (size && !data)
    {
      DEBUG_ERROR("Missing pointer shape payload");
      return;
    }
    cursorData = std::make_shared<std::vector<BYTE>>(size);
    if (size)
      memcpy(cursorData->data(), data, size);
  }

  {
    CSRWExclusiveLock lock(m_stateLock);
    m_state.cursor.IsCursorVisible = info.IsCursorVisible;
    m_state.cursor.X               = info.X;
    m_state.cursor.Y               = info.Y;
    m_state.sdrWhiteLevel          = sdrWhiteLevel;
    m_state.positionRevision       = Seq::Next(m_state.positionRevision);

    if (shape)
    {
      m_state.cursor.IsCursorShapeUpdated = info.IsCursorShapeUpdated;
      m_state.cursor.CursorShapeInfo      = info.CursorShapeInfo;
      m_state.cursorData                  = std::move(cursorData);
      m_state.shapeRevision               = Seq::Next(m_state.shapeRevision);
    }
  }

  WakeAll(WorkType::POSITION);
  if (shape)
    WakeAll(WorkType::SHAPE);
}

void CControlHub::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  {
    CSRWExclusiveLock lock(m_stateLock);
    m_state.transform         = std::move(transform);
    m_state.transformRevision = Seq::Next(m_state.transformRevision);
  }
  WakeAll(WorkType::TRANSFORM);
}

std::shared_ptr<const D12ColorTransform>
CControlHub::GetColorTransform() const
{
  CSRWSharedLock lock(m_stateLock);
  return m_state.transform;
}
