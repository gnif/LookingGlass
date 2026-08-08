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

#include "ipc/CInputPipeServer.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "InputPipeProtocol.h"

#include <string.h>

CInputPipeServer g_inputPipeServer;

static constexpr DWORD WAIT_FIRST_OBJECT_VALUE = 0;

bool CInputPipeServer::Init()
{
  DeInit();

  m_state.store(0, std::memory_order_release);
  m_performanceFrequency.QuadPart = 0;
  if (!QueryPerformanceFrequency(&m_performanceFrequency))
    m_performanceFrequency.QuadPart = 0;
  m_lastStatistics = GetTickCount64();
  m_stopEvent  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_queueEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_stopEvent || !m_queueEvent)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGInput sender resources");
    DeInit();
    return false;
  }

  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create LGInput sender");
    DeInit();
    return false;
  }

  m_endpoint.SetHandler(this);
  if (!m_endpoint.Start(
      LG_INPUT_PIPE_NAME,
      CPipeEndpoint::Mode::Server,
      sizeof(LGInputPipeMessage)))
  {
    DeInit();
    return false;
  }
  return true;
}

void CInputPipeServer::DeInit()
{
  Invalidate(0, false);
  if (m_stopEvent)
    SetEvent(m_stopEvent);

  m_endpoint.Stop();

  if (m_thread)
  {
    WaitForSingleObject(m_thread, INFINITE);
    CloseHandle(m_thread);
    m_thread = nullptr;
  }

  if (m_queueEvent)
  {
    CloseHandle(m_queueEvent);
    m_queueEvent = nullptr;
  }
  if (m_stopEvent)
  {
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
  }

  CSRWExclusiveLock lock(&m_queueLock);
  m_queueHead       = 0;
  m_queueCount      = 0;
  m_mouseMode       = MouseMode::NONE;
  m_relativeButtons = 0;
  m_absoluteButtons = 0;

  m_statEnqueued          = 0;
  m_statRelativeCoalesced = 0;
  m_statAbsoluteCoalesced = 0;
  m_statResyncs           = 0;
  m_statResyncDiscarded   = 0;
  m_statQueueHighWater    = 0;
  m_statSent              = 0;
  m_statStale             = 0;
  m_statWriteFailed       = 0;
  m_statSlowWrites        = 0;
  m_statWriteTicks        = 0;
  m_statMaxWriteTicks     = 0;
}

bool CInputPipeServer::QueueLocked(
  LGInputPipeMessageType type,
  const KVMFRInputPayload& payload,
  bool pureMotion)
{
  if (m_queueCount)
  {
    const size_t tailIndex =
      (m_queueHead + m_queueCount - 1) % QUEUE_LENGTH;
    QueueItem& tail = m_queue[tailIndex];
    if (tail.type == type)
    {
      if (type == LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE &&
          m_queueCount >= MOTION_COALESCE_THRESHOLD &&
          tail.pureMotion && pureMotion &&
          tail.payload.mouseRelative.wheel == 0 &&
          payload.mouseRelative.wheel == 0 &&
          tail.payload.mouseRelative.buttons ==
            payload.mouseRelative.buttons)
      {
        const int64_t x = static_cast<int64_t>(
          tail.payload.mouseRelative.deltaX) +
          payload.mouseRelative.deltaX;
        const int64_t y = static_cast<int64_t>(
          tail.payload.mouseRelative.deltaY) +
          payload.mouseRelative.deltaY;
        if (x >= LG_INPUT_MOUSE_DELTA_MIN &&
            x <= LG_INPUT_MOUSE_DELTA_MAX &&
            y >= LG_INPUT_MOUSE_DELTA_MIN &&
            y <= LG_INPUT_MOUSE_DELTA_MAX)
        {
          tail.payload.mouseRelative.deltaX = static_cast<int32_t>(x);
          tail.payload.mouseRelative.deltaY = static_cast<int32_t>(y);
          ++m_statRelativeCoalesced;
          return true;
        }
      }
      else if (type == LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE &&
          tail.pureMotion && pureMotion &&
          tail.payload.mouseAbsolute.wheel == 0 &&
          payload.mouseAbsolute.wheel == 0 &&
          tail.payload.mouseAbsolute.buttons ==
            payload.mouseAbsolute.buttons)
      {
        tail.payload.mouseAbsolute.x = payload.mouseAbsolute.x;
        tail.payload.mouseAbsolute.y = payload.mouseAbsolute.y;
        ++m_statAbsoluteCoalesced;
        return true;
      }
    }
  }

  return QueueRawLocked(type, payload, pureMotion);
}

bool CInputPipeServer::QueueRawLocked(
  LGInputPipeMessageType type,
  const KVMFRInputPayload& payload,
  bool pureMotion)
{
  if (m_queueCount == QUEUE_LENGTH)
    return false;

  const size_t index =
    (m_queueHead + m_queueCount) % QUEUE_LENGTH;
  m_queue[index].type       = type;
  m_queue[index].state      =
    m_state.load(std::memory_order_relaxed);
  m_queue[index].payload    = payload;
  m_queue[index].pureMotion = pureMotion;
  ++m_queueCount;
  ++m_statEnqueued;
  if (m_queueCount > m_statQueueHighWater)
    m_statQueueHighWater = m_queueCount;
  SetEvent(m_queueEvent);
  return true;
}

bool CInputPipeServer::QueueResetLocked()
{
  KVMFRInputPayload payload = {};
  switch (m_mouseMode)
  {
    case MouseMode::RELATIVE_INPUT:
      if (m_relativeButtons && !QueueRawLocked(
          LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE, payload, false))
        return false;
      break;

    case MouseMode::ABSOLUTE_INPUT:
      if (m_absoluteButtons)
      {
        payload.mouseAbsolute.x = m_absoluteX;
        payload.mouseAbsolute.y = m_absoluteY;
        if (!QueueRawLocked(
            LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE, payload, false))
          return false;
      }
      break;

    case MouseMode::NONE:
      break;
  }

  payload = {};
  if (!QueueRawLocked(LG_INPUT_PIPE_MESSAGE_KEYBOARD, payload, false))
    return false;

  m_mouseMode       = MouseMode::NONE;
  m_relativeButtons = 0;
  m_absoluteButtons = 0;
  return true;
}

void CInputPipeServer::ResyncLocked()
{
  ++m_statResyncs;
  m_statResyncDiscarded += m_queueCount;
  m_queueHead  = 0;
  m_queueCount = 0;
  uint64_t state = m_state.load(std::memory_order_relaxed);
  while (state & 1)
  {
    if (m_state.compare_exchange_weak(
        state, state + 2, std::memory_order_acq_rel))
    {
      QueueResetLocked();
      return;
    }
  }
}

bool CInputPipeServer::SendMouseRelative(
  int32_t deltaX,
  int32_t deltaY,
  int32_t wheel,
  uint32_t buttons)
{
  if (deltaX < LG_INPUT_MOUSE_DELTA_MIN ||
      deltaX > LG_INPUT_MOUSE_DELTA_MAX ||
      deltaY < LG_INPUT_MOUSE_DELTA_MIN ||
      deltaY > LG_INPUT_MOUSE_DELTA_MAX ||
      wheel < LG_INPUT_MOUSE_WHEEL_MIN_TOTAL ||
      wheel > LG_INPUT_MOUSE_WHEEL_MAX ||
      !(m_state.load(std::memory_order_acquire) & 1))
    return false;

  KVMFRInputPayload payload = {};
  payload.mouseRelative.buttons = buttons;
  payload.mouseRelative.deltaX  = deltaX;
  payload.mouseRelative.deltaY  = deltaY;
  payload.mouseRelative.wheel   = wheel;

  CSRWExclusiveLock lock(&m_queueLock);
  const bool pureMotion = wheel == 0 && buttons == m_relativeButtons;
  const bool switching  = m_mouseMode == MouseMode::ABSOLUTE_INPUT;
  bool queued = (m_state.load(std::memory_order_relaxed) & 1) != 0;
  if (queued && switching && m_absoluteButtons)
  {
    KVMFRInputPayload neutral = {};
    neutral.mouseAbsolute.x = m_absoluteX;
    neutral.mouseAbsolute.y = m_absoluteY;
    queued = QueueRawLocked(
      LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE, neutral, false);
  }
  if (queued)
    queued = QueueLocked(
      LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE, payload, pureMotion);

  if (!queued)
    ResyncLocked();
  else
  {
    if (switching)
      m_absoluteButtons = 0;
    m_mouseMode       = MouseMode::RELATIVE_INPUT;
    m_relativeButtons = buttons;
  }
  return queued;
}

bool CInputPipeServer::SendMouseAbsolute(
  uint16_t x,
  uint16_t y,
  int32_t wheel,
  uint32_t buttons)
{
  if (x > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      y > LG_INPUT_MOUSE_ABSOLUTE_MAX ||
      wheel < LG_INPUT_MOUSE_WHEEL_MIN_TOTAL ||
      wheel > LG_INPUT_MOUSE_WHEEL_MAX ||
      !(m_state.load(std::memory_order_acquire) & 1))
    return false;

  KVMFRInputPayload payload = {};
  payload.mouseAbsolute.buttons = buttons;
  payload.mouseAbsolute.x       = x;
  payload.mouseAbsolute.y       = y;
  payload.mouseAbsolute.wheel   = wheel;

  CSRWExclusiveLock lock(&m_queueLock);
  const bool pureMotion = wheel == 0 && buttons == m_absoluteButtons;
  const bool switching  = m_mouseMode == MouseMode::RELATIVE_INPUT;
  bool queued = (m_state.load(std::memory_order_relaxed) & 1) != 0;
  if (queued && switching && m_relativeButtons)
  {
    const KVMFRInputPayload neutral = {};
    queued = QueueRawLocked(
      LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE, neutral, false);
  }
  if (queued)
    queued = QueueLocked(
      LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE, payload, pureMotion);

  if (!queued)
    ResyncLocked();
  else
  {
    if (switching)
      m_relativeButtons = 0;
    m_mouseMode       = MouseMode::ABSOLUTE_INPUT;
    m_absoluteX       = x;
    m_absoluteY       = y;
    m_absoluteButtons = buttons;
  }
  return queued;
}

bool CInputPipeServer::SendKeyboard(
  uint8_t modifiers,
  const uint8_t * keys)
{
  if (!keys || !(m_state.load(std::memory_order_acquire) & 1))
    return false;

  KVMFRInputPayload payload = {};
  payload.keyboard.modifiers = modifiers;
  for (size_t i = 0; i < LG_INPUT_KEYBOARD_KEY_COUNT; ++i)
  {
    if (keys[i] > LG_INPUT_KEYBOARD_USAGE_MAX)
      return false;
    payload.keyboard.keys[i] = keys[i];
  }

  CSRWExclusiveLock lock(&m_queueLock);
  const bool queued = (m_state.load(std::memory_order_relaxed) & 1) &&
    QueueLocked(LG_INPUT_PIPE_MESSAGE_KEYBOARD, payload, false);
  if (!queued)
    ResyncLocked();
  return queued;
}

bool CInputPipeServer::Reset()
{
  if (!(m_state.load(std::memory_order_acquire) & 1))
    return false;

  CSRWExclusiveLock lock(&m_queueLock);
  bool queued = (m_state.load(std::memory_order_relaxed) & 1) != 0;
  if (queued)
    queued = QueueResetLocked();
  if (!queued)
    ResyncLocked();
  return queued;
}

bool CInputPipeServer::Pop(QueueItem& item)
{
  CSRWExclusiveLock lock(&m_queueLock);
  if (!m_queueCount)
    return false;

  item = m_queue[m_queueHead];
  m_queueHead = (m_queueHead + 1) % QUEUE_LENGTH;
  --m_queueCount;
  if (m_queueCount)
    SetEvent(m_queueEvent);
  return true;
}

bool CInputPipeServer::Send(const QueueItem& item)
{
  LGInputPipeMessage message = {};
  message.magic       = LG_INPUT_PIPE_MAGIC;
  message.version     = LG_INPUT_PIPE_VERSION;
  message.type        = item.type;
  message.payloadSize = sizeof(KVMFRInputPayload);
  memcpy(message.payload, &item.payload, sizeof(item.payload));

  bool current;
  bool sent = true;
  bool timed = false;
  LARGE_INTEGER start = {};
  LARGE_INTEGER end   = {};
  {
    CSRWSharedLock lock(&m_connectionLock);
    const uint64_t state = m_state.load(std::memory_order_acquire);
    current = (state & 1) && item.state == state;
    if (current)
    {
      message.sequence = ++m_sequence;
      timed = QueryPerformanceCounter(&start) != FALSE;
      sent = m_endpoint.Send(&message, sizeof(message));
      timed = timed && QueryPerformanceCounter(&end) != FALSE;
    }
  }

  if (!current)
    ++m_statStale;
  else
  {
    if (timed)
    {
      const int64_t ticks = end.QuadPart - start.QuadPart;
      m_statWriteTicks += ticks;
      if (ticks > m_statMaxWriteTicks)
        m_statMaxWriteTicks = ticks;
      if (m_performanceFrequency.QuadPart &&
          ticks * 1000 >= m_performanceFrequency.QuadPart)
        ++m_statSlowWrites;
    }

    if (sent)
      ++m_statSent;
    else
      ++m_statWriteFailed;
  }

  if (!sent)
  {
    Invalidate(item.state, true);
    return false;
  }
  return current;
}

void CInputPipeServer::LogStatistics()
{
  const ULONGLONG now = GetTickCount64();
  if (now - m_lastStatistics < STATISTICS_INTERVAL_MS)
    return;

  uint64_t enqueued;
  uint64_t relativeCoalesced;
  uint64_t absoluteCoalesced;
  uint64_t resyncs;
  uint64_t resyncDiscarded;
  size_t   queueHighWater;
  {
    CSRWExclusiveLock lock(&m_queueLock);
    enqueued          = m_statEnqueued;
    relativeCoalesced = m_statRelativeCoalesced;
    absoluteCoalesced = m_statAbsoluteCoalesced;
    resyncs           = m_statResyncs;
    resyncDiscarded   = m_statResyncDiscarded;
    queueHighWater    = m_statQueueHighWater;

    m_statEnqueued          = 0;
    m_statRelativeCoalesced = 0;
    m_statAbsoluteCoalesced = 0;
    m_statResyncs           = 0;
    m_statResyncDiscarded   = 0;
    m_statQueueHighWater    = m_queueCount;
  }

  const uint64_t sent          = m_statSent;
  const uint64_t stale         = m_statStale;
  const uint64_t writeFailed   = m_statWriteFailed;
  const uint64_t slowWrites    = m_statSlowWrites;
  const int64_t  writeTicks    = m_statWriteTicks;
  const int64_t  maxWriteTicks = m_statMaxWriteTicks;

  m_statSent          = 0;
  m_statStale         = 0;
  m_statWriteFailed   = 0;
  m_statSlowWrites    = 0;
  m_statWriteTicks    = 0;
  m_statMaxWriteTicks = 0;
  m_lastStatistics    = now;

  if (!(enqueued || relativeCoalesced || absoluteCoalesced || resyncs ||
      resyncDiscarded || sent || stale || writeFailed || slowWrites))
    return;

  const double writeMs = m_performanceFrequency.QuadPart ?
    static_cast<double>(writeTicks) * 1000.0 /
      m_performanceFrequency.QuadPart : 0.0;
  const double maxWriteMs = m_performanceFrequency.QuadPart ?
    static_cast<double>(maxWriteTicks) * 1000.0 /
      m_performanceFrequency.QuadPart : 0.0;
  DEBUG_TRACE("LGInput pipe: %llu queued, %llu sent, %llu stale, "
    "%llu failed; %llu relative and %llu absolute coalesced, "
    "%llu resyncs discarded %llu, peak %zu; %.3f ms writes, "
    "%.3f ms max, %llu slow",
    static_cast<unsigned long long>(enqueued),
    static_cast<unsigned long long>(sent),
    static_cast<unsigned long long>(stale),
    static_cast<unsigned long long>(writeFailed),
    static_cast<unsigned long long>(relativeCoalesced),
    static_cast<unsigned long long>(absoluteCoalesced),
    static_cast<unsigned long long>(resyncs),
    static_cast<unsigned long long>(resyncDiscarded),
    queueHighWater,
    writeMs,
    maxWriteMs,
    static_cast<unsigned long long>(slowWrites));
}

void CInputPipeServer::Invalidate(uint64_t state, bool requireMatch)
{
  CSRWExclusiveLock connectionLock(&m_connectionLock);
  uint64_t current = m_state.load(std::memory_order_relaxed);
  for (;;)
  {
    if (!(current & 1) || (requireMatch && state != current))
      return;
    if (m_state.compare_exchange_weak(
        current, current + 1, std::memory_order_acq_rel))
      break;
  }

  CSRWExclusiveLock queueLock(&m_queueLock);
  m_queueHead  = 0;
  m_queueCount = 0;
}

DWORD WINAPI CInputPipeServer::ThreadProc(void * context)
{
  static_cast<CInputPipeServer *>(context)->Thread();
  return 0;
}

void CInputPipeServer::Thread()
{
  const HANDLE handles[] = { m_stopEvent, m_queueEvent };
  for (;;)
  {
    const DWORD wait = WaitForMultipleObjects(
      _countof(handles), handles, FALSE, INFINITE);
    if (wait == WAIT_FIRST_OBJECT_VALUE)
      break;
    if (wait != WAIT_FIRST_OBJECT_VALUE + 1)
    {
      DEBUG_ERROR_HR(GetLastError(), "LGInput sender wait failed");
      break;
    }

    QueueItem item = {};
    while (Pop(item))
    {
      const bool sent = Send(item);
      if (!sent)
        break;
    }
    LogStatistics();
  }

  Invalidate(0, false);
}

void CInputPipeServer::OnPipeConnected()
{
  CSRWExclusiveLock connectionLock(&m_connectionLock);
  CSRWExclusiveLock queueLock(&m_queueLock);

  uint64_t state = m_state.load(std::memory_order_relaxed);
  if (state & 1)
    ++state;
  ++state;
  m_sequence = 0;

  m_queueHead  = 0;
  m_queueCount = 0;
  m_mouseMode  = MouseMode::NONE;
  const bool reset = QueueResetLocked();
  for (size_t i = 0; i < m_queueCount; ++i)
  {
    const size_t index = (m_queueHead + i) % QUEUE_LENGTH;
    m_queue[index].state = state;
  }
  if (reset)
    m_state.store(state, std::memory_order_release);

  if (!reset)
    DEBUG_WARN("Failed to queue LGInput endpoint neutralization");
}

void CInputPipeServer::OnPipeDisconnected()
{
  Invalidate(0, false);
}

bool CInputPipeServer::OnPipeMessage(
  const void * message,
  size_t size)
{
  UNREFERENCED_PARAMETER(message);
  UNREFERENCED_PARAMETER(size);
  DEBUG_WARN("LGInput sent an unexpected message");
  return false;
}
