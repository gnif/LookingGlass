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

#include "transport/network/CNetworkSession.h"

#include <new>
#include <utility>

namespace
{
  uint64_t NextRevision(uint64_t revision)
  {
    ++revision;
    if (!revision)
      ++revision;
    return revision;
  }

  LGNetControlStatusCode ControlStatusCode(InteractionResult result)
  {
    switch (result)
    {
      case InteractionResult::ACCEPTED:
        return LG_NET_CONTROL_STATUS_APPLIED;

      case InteractionResult::BUSY:
        return LG_NET_CONTROL_STATUS_BUSY;

      case InteractionResult::UNAVAILABLE:
        return LG_NET_CONTROL_STATUS_UNSUPPORTED;

      case InteractionResult::STALE:
        return LG_NET_CONTROL_STATUS_STALE;

      case InteractionResult::REJECTED:
        return LG_NET_CONTROL_STATUS_INVALID;

      case InteractionResult::FAILED:
        return LG_NET_CONTROL_STATUS_ERROR;
    }

    return LG_NET_CONTROL_STATUS_ERROR;
  }
}

bool CNetworkEventQueue::Push(const NetworkEvent& event)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_count == CAPACITY)
    return false;

  const unsigned tail = (m_head + m_count) % CAPACITY;
  m_events[tail] = event;
  ++m_count;
  return true;
}

bool CNetworkEventQueue::Take(NetworkEvent& event)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_count)
    return false;

  event            = m_events[m_head];
  m_events[m_head] = {};
  m_head           = (m_head + 1) % CAPACITY;
  --m_count;
  return true;
}

void CNetworkEventQueue::Reset()
{
  CSRWExclusiveLock lock(m_lock);
  for (NetworkEvent& event : m_events)
    event = {};
  m_head  = 0;
  m_count = 0;
}

bool CNetworkInputSource::Start(IInputTarget& target)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_target && m_target != &target)
    return false;
  m_target = &target;
  return true;
}

void CNetworkInputSource::Stop()
{
  CSRWExclusiveLock lock(m_lock);
  m_target = nullptr;
}

InputTargetState CNetworkInputSource::GetState(
  const InputSourceId& source) const
{
  CSRWSharedLock lock(m_lock);
  return m_target ? m_target->GetState(source) : InputTargetState {};
}

InputResult CNetworkInputSource::Claim(const InputSourceId& source)
{
  CSRWSharedLock lock(m_lock);
  return m_target ?
    m_target->Claim(source) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::Touch(const InputSourceId& source)
{
  CSRWSharedLock lock(m_lock);
  return m_target ?
    m_target->Touch(source) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::Release(
  const InputSourceId& source, bool reset)
{
  CSRWSharedLock lock(m_lock);
  return m_target ?
    m_target->Release(source, reset) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::SendMouseRelative(
  const InputSourceId& source, int32_t deltaX, int32_t deltaY,
  int32_t wheel, uint32_t buttons)
{
  CSRWSharedLock lock(m_lock);
  return m_target ? m_target->SendMouseRelative(
    source, deltaX, deltaY, wheel, buttons) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::SendMouseAbsolute(
  const InputSourceId& source, uint16_t x, uint16_t y,
  int32_t wheel, uint32_t buttons)
{
  CSRWSharedLock lock(m_lock);
  return m_target ? m_target->SendMouseAbsolute(
    source, x, y, wheel, buttons) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::SendKeyboard(
  const InputSourceId& source, uint8_t modifiers, const uint8_t * keys)
{
  CSRWSharedLock lock(m_lock);
  return m_target ? m_target->SendKeyboard(
    source, modifiers, keys) : InputResult::UNAVAILABLE;
}

InputResult CNetworkInputSource::Reset(const InputSourceId& source)
{
  CSRWSharedLock lock(m_lock);
  return m_target ?
    m_target->Reset(source) : InputResult::UNAVAILABLE;
}

void CNetworkControlSink::SetControlEvents(
  IControlEvents * events, const ControlToken& token)
{
  CSRWExclusiveLock lock(m_lock);
  m_events = events;
  m_token  = events ? token : ControlToken {};
}

ControlResult CNetworkControlSink::SendCursor(
  const IDARG_OUT_QUERY_HWCURSOR& info, const BYTE * data,
  size_t size, UINT sdrWhiteLevel)
{
  const bool shape = info.CursorShapeInfo.CursorType !=
    IDDCX_CURSOR_SHAPE_TYPE_UNINITIALIZED;
  if (shape && info.CursorShapeInfo.CursorType !=
        IDDCX_CURSOR_SHAPE_TYPE_ALPHA &&
      info.CursorShapeInfo.CursorType !=
        IDDCX_CURSOR_SHAPE_TYPE_MASKED_COLOR)
    return ControlResult::FAILED;
  if (shape && info.CursorShapeInfo.Height &&
      info.CursorShapeInfo.Pitch >
        SIZE_MAX / info.CursorShapeInfo.Height)
    return ControlResult::FAILED;
  const size_t shapeSize = shape ? static_cast<size_t>(
    info.CursorShapeInfo.Height) * info.CursorShapeInfo.Pitch : 0;
  if (size != shapeSize || (size && !data) ||
      shapeSize > LG_NET_CURSOR_MAX_SHAPE_LENGTH ||
      (shape && (!info.CursorShapeInfo.Width ||
        info.CursorShapeInfo.Width > LG_NET_CURSOR_MAX_WIDTH ||
        !info.CursorShapeInfo.Height ||
        info.CursorShapeInfo.Height > LG_NET_CURSOR_MAX_HEIGHT ||
        info.CursorShapeInfo.XHot >= info.CursorShapeInfo.Width ||
        info.CursorShapeInfo.YHot >= info.CursorShapeInfo.Height ||
        info.CursorShapeInfo.Pitch < info.CursorShapeInfo.Width * 4U)))
    return ControlResult::FAILED;

  std::vector<BYTE> cursorData;
  try
  {
    if (shapeSize)
      cursorData.assign(data, data + size);
  }
  catch (...)
  {
    return ControlResult::FAILED;
  }

  CSRWExclusiveLock lock(m_lock);
  if (!m_events)
    return ControlResult::RETRY;
  const UINT whiteLevel = sdrWhiteLevel ?
    sdrWhiteLevel : LG_SDR_WHITE_LEVEL_DEFAULT;
  if (m_state.sdrWhiteLevel != whiteLevel)
  {
    m_state.sdrWhiteLevel = whiteLevel;
    m_state.colorRevision = NextRevision(m_state.colorRevision);
  }
  m_state.cursor.IsCursorVisible = info.IsCursorVisible;
  m_state.cursor.X               = info.X;
  m_state.cursor.Y               = info.Y;
  m_state.cursorValid            = true;
  m_state.positionRevision       = NextRevision(m_state.positionRevision);
  if (shape)
  {
    m_state.cursor.IsCursorShapeUpdated = info.IsCursorShapeUpdated;
    m_state.cursor.CursorShapeInfo      = info.CursorShapeInfo;
    m_state.cursorData                  = std::move(cursorData);
    m_state.shapeValid                  = true;
    m_state.shapeRevision               = NextRevision(m_state.shapeRevision);
  }
  m_state.stateRevision          = NextRevision(m_state.stateRevision);
  return ControlResult::APPLIED;
}

ControlResult CNetworkControlSink::SetColorTransform(
  std::shared_ptr<const D12ColorTransform> transform)
{
  std::shared_ptr<const D12ColorTransform> copy;
  try
  {
    if (transform)
      copy = std::make_shared<D12ColorTransform>(*transform);
  }
  catch (...)
  {
    return ControlResult::FAILED;
  }

  CSRWExclusiveLock lock(m_lock);
  if (!m_events)
    return ControlResult::RETRY;
  m_state.transform      = std::move(copy);
  m_state.transformValid = true;
  m_state.colorRevision  = NextRevision(m_state.colorRevision);
  m_state.stateRevision  = NextRevision(m_state.stateRevision);
  return ControlResult::APPLIED;
}

void CNetworkControlSink::RequestReplay()
{
  CSRWSharedLock lock(m_lock);
  if (m_events)
    m_events->OnControlReplay(m_token);
}

bool CNetworkControlSink::Snapshot(
  NetworkControlSnapshot& state, uint64_t knownRevision) const
{
  CSRWSharedLock lock(m_lock);
  if (!m_events || !m_state.stateRevision ||
      m_state.stateRevision == knownRevision)
    return false;
  try
  {
    state = m_state;
  }
  catch (...)
  {
    return false;
  }
  return true;
}

CNetworkSession::CNetworkSession(uint32_t client, uint32_t generation,
  CNetworkEventQueue& events, CNetworkInputSource& input) :
  m_events(events), m_input(input)
{
  m_source.client     = client;
  m_source.generation = generation;
}

CNetworkSession::~CNetworkSession()
{
  Close();
}

SourceKey CNetworkSession::EventSource() const
{
  SourceKey source;
  source.client     = m_source.client;
  source.generation = m_source.generation;
  return source;
}

bool CNetworkSession::IsOpen() const
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open;
}

void CNetworkSession::Close()
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (!m_open)
    return;
  m_open = false;
  m_input.Release(m_source, true);
}

bool CNetworkSession::SetCursorPosition(
  uint64_t controlID, int32_t x, int32_t y)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (!m_open || !controlID)
    return false;

  NetworkEvent event;
  event.type      = NetworkEventType::SET_CURSOR_POSITION;
  event.source    = EventSource();
  event.controlID = controlID;
  event.x         = x;
  event.y         = y;
  return m_events.Push(event);
}

bool CNetworkSession::SetResolution(
  uint64_t controlID, uint32_t width, uint32_t height)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (!m_open || !controlID || !width || !height)
    return false;

  NetworkEvent event;
  event.type      = NetworkEventType::SET_RESOLUTION;
  event.source    = EventSource();
  event.controlID = controlID;
  event.width     = width;
  event.height    = height;
  return m_events.Push(event);
}

bool CNetworkSession::SetFrameSchedule(
  uint64_t controlID, const FrameScheduleUpdate& schedule)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (!m_open || !controlID)
    return false;

  NetworkEvent event;
  event.type      = NetworkEventType::SET_FRAME_SCHEDULE;
  event.source    = EventSource();
  event.schedule  = schedule;
  event.controlID = controlID;
  return m_events.Push(event);
}

bool CNetworkSession::RequestRecovery(
  uint64_t session, uint32_t serial, bool active)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (!m_open || !session || !serial)
    return false;

  NetworkEvent event;
  event.type    = NetworkEventType::RECOVERY_REQUEST;
  event.source  = EventSource();
  event.session = session;
  event.serial  = serial;
  event.active  = active;
  return m_events.Push(event);
}

void CNetworkSession::ProcessEvent(
  const NetworkEvent& event, ITransportEvents& events)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (!m_open || event.source.client != m_source.client ||
      event.source.generation != m_source.generation)
    return;

  switch (event.type)
  {
    case NetworkEventType::SET_CURSOR_POSITION:
    {
      NetworkControlStatus status;
      status.controlID = event.controlID;
      status.status    = ControlStatusCode(
        events.OnSetCursorPos(event.source, event.x, event.y));
      SetControlStatus(status);
      break;
    }

    case NetworkEventType::SET_RESOLUTION:
    {
      NetworkControlStatus status;
      status.controlID = event.controlID;
      status.status    = ControlStatusCode(events.OnSetResolution(
        event.source, event.width, event.height));
      SetControlStatus(status);
      break;
    }

    case NetworkEventType::SET_FRAME_SCHEDULE:
    {
      NetworkControlStatus status;
      status.controlID = event.controlID;
      status.status    = LG_NET_CONTROL_STATUS_UNSUPPORTED;
      SetControlStatus(status);
      break;
    }

    case NetworkEventType::RECOVERY_REQUEST:
    {
      const RecoveryAdmission admission = events.OnRecoveryRequest(
        event.source, event.session, event.serial, event.active);
      if (!admission.complete)
        break;

      NetworkRecoveryStatus status;
      status.session = event.session;
      status.serial  = event.serial;
      status.active  = event.active;
      status.state   = admission.state;
      status.error   = admission.error;
      SetRecoveryStatus(status);
      break;
    }
  }
}

void CNetworkSession::SetRecoveryStatus(
  const NetworkRecoveryStatus& status)
{
  CSRWExclusiveLock lock(m_recoveryLock);
  if (m_recoveryCount == RECOVERY_CAPACITY)
  {
    m_recovery[m_recoveryHead] = {};
    m_recoveryHead             =
      (m_recoveryHead + 1) % RECOVERY_CAPACITY;
    --m_recoveryCount;
  }

  const unsigned tail =
    (m_recoveryHead + m_recoveryCount) % RECOVERY_CAPACITY;
  m_recovery[tail] = status;
  ++m_recoveryCount;
}

bool CNetworkSession::TakeRecoveryStatus(NetworkRecoveryStatus& status)
{
  CSRWExclusiveLock lock(m_recoveryLock);
  if (!m_recoveryCount)
    return false;

  status                     = m_recovery[m_recoveryHead];
  m_recovery[m_recoveryHead] = {};
  m_recoveryHead             =
    (m_recoveryHead + 1) % RECOVERY_CAPACITY;
  --m_recoveryCount;
  return true;
}

void CNetworkSession::SetControlStatus(
  const NetworkControlStatus& status)
{
  CSRWExclusiveLock lock(m_controlLock);
  if (m_controlCount == CONTROL_CAPACITY)
  {
    m_control[m_controlHead] = {};
    m_controlHead            = (m_controlHead + 1) % CONTROL_CAPACITY;
    --m_controlCount;
  }

  const unsigned tail =
    (m_controlHead + m_controlCount) % CONTROL_CAPACITY;
  m_control[tail] = status;
  ++m_controlCount;
}

bool CNetworkSession::TakeControlStatus(NetworkControlStatus& status)
{
  CSRWExclusiveLock lock(m_controlLock);
  if (!m_controlCount)
    return false;

  status                   = m_control[m_controlHead];
  m_control[m_controlHead] = {};
  m_controlHead            = (m_controlHead + 1) % CONTROL_CAPACITY;
  --m_controlCount;
  return true;
}

InputTargetState CNetworkSession::InputState() const
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ? m_input.GetState(m_source) : InputTargetState {};
}

InputResult CNetworkSession::ClaimInput()
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ?
    m_input.Claim(m_source) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::TouchInput()
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ?
    m_input.Touch(m_source) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::ReleaseInput(bool reset)
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ?
    m_input.Release(m_source, reset) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::SendMouseRelative(
  int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons)
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ? m_input.SendMouseRelative(
    m_source, deltaX, deltaY, wheel, buttons) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::SendMouseAbsolute(
  uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons)
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ? m_input.SendMouseAbsolute(
    m_source, x, y, wheel, buttons) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::SendKeyboard(
  uint8_t modifiers, const uint8_t * keys)
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ? m_input.SendKeyboard(
    m_source, modifiers, keys) : InputResult::UNAVAILABLE;
}

InputResult CNetworkSession::ResetInput()
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_open ?
    m_input.Reset(m_source) : InputResult::UNAVAILABLE;
}
