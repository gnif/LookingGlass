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

#include <ntstatus.h>

#include "transport/lgmp/CLGMPInputTransport.h"

#include <wudfwdm.h>

#include "config/CSettings.h"
#include "transport/lgmp/CLGMPHost.h"
#include "Atomic.h"
#include "CDebug.h"
#include "CSRWLock.h"
#include "Seq.h"

#include "common/KVMFRInput.h"
#include "common/LGMPConfig.h"

#include <avrt.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static const LGMPQueueConfig INPUT_QUEUE_CONFIG =
{
  LGMP_Q_INPUT,
  LGMP_Q_INPUT_LEN,
  1000,
};

static constexpr int32_t MAX_SPLIT_REPORTS = 4;
static constexpr int32_t MAX_MOUSE_DELTA =
  INT16_MAX * MAX_SPLIT_REPORTS;
static constexpr int32_t MIN_MOUSE_DELTA =
  INT16_MIN * MAX_SPLIT_REPORTS;
static constexpr int32_t MAX_MOUSE_WHEEL =
  INT8_MAX * MAX_SPLIT_REPORTS;
static constexpr int32_t MIN_MOUSE_WHEEL =
  -INT8_MAX * MAX_SPLIT_REPORTS;
static bool IsZero(const void * data, size_t size)
{
  const uint8_t * byte = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < size; ++i)
    if (byte[i])
      return false;
  return true;
}

static bool ArmPollTimer(HANDLE timer, uint32_t waitUs)
{
  LARGE_INTEGER due = {};
  due.QuadPart = -static_cast<LONGLONG>(waitUs) * 10;
  return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

static KVMFRStreamDescriptor ExportStreamDescriptor(
  const LGMPStreamDescriptor& source)
{
  KVMFRStreamDescriptor result = {};
  result.magic      = source.magic;
  result.version    = source.version;
  result.size       = source.size;
  result.offset     = source.offset;
  result.regionSize = source.regionSize;
  result.direction  = source.direction;
  result.policy     = source.policy;
  result.slotCount  = source.slotCount;
  result.slotSize   = source.slotSize;
  return result;
}

CLGMPInputTransport::~CLGMPInputTransport()
{
  DeInit();
}

bool CLGMPInputTransport::Initialize()
{
  if (m_queue)
    return true;

  LGMP_STATUS status = m_host.CreateQueue(
    INPUT_QUEUE_CONFIG, &m_queue);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Input): %s",
      lgmpStatusString(status));
    return false;
  }

  const LGMPStreamConfig streamConfig =
  {
    LGMP_STREAM_CLIENT_TO_HOST,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_INPUT_STREAM_SLOT_COUNT,
    KVMFR_INPUT_STREAM_SLOT_SIZE,
  };
  for (StreamEndpoint& endpoint : m_streamEndpoint)
  {
    status = m_host.CreateStream(streamConfig, &endpoint.stream);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostStreamNew Failed (Input): %s",
        lgmpStatusString(status));
      DeInit();
      return false;
    }
    lgmpHostStreamGetDescriptor(endpoint.stream, &endpoint.descriptor);
  }
  Seq::Inc(m_streamGeneration);

  for (PLGMPMemory& memory : m_statusMemory)
  {
    status = m_host.Allocate(sizeof(KVMFRInputStatus), &memory);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostMemAlloc Failed (Input Status): %s",
        lgmpStatusString(status));
      DeInit();
      return false;
    }
    memset(lgmpHostMemPtr(memory), 0, sizeof(KVMFRInputStatus));
  }

  {
    CSRWExclusiveLock lock(m_statusLock);
    m_statusDirty = true;
  }
  return true;
}

void CLGMPInputTransport::DeInit()
{
  Stop();
  for (PLGMPMemory& memory : m_statusMemory)
    lgmpHostMemFree(&memory);
  for (StreamEndpoint& endpoint : m_streamEndpoint)
  {
    lgmpHostStreamFree(&endpoint.stream);
    endpoint = {};
  }
  m_streamGeneration  = 0;
  m_streamDrainCursor = 0;
  m_queue = nullptr;
}

bool CLGMPInputTransport::ReconcileStreams(bool& changed)
{
  changed = false;
  uint32_t activeClientIDs[32] = {};
  unsigned activeClientCount = 0;
  const LGMP_STATUS clientsStatus = lgmpHostGetClientIDs(
    m_queue, activeClientIDs, &activeClientCount);
  if (clientsStatus != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostGetClientIDs Failed (Input): %s",
      lgmpStatusString(clientsStatus));
    return false;
  }

  CSRWExclusiveLock lock(m_streamLock);
  for (StreamEndpoint& endpoint : m_streamEndpoint)
  {
    if (!endpoint.clientID)
      continue;

    bool active = false;
    for (unsigned i = 0; i < activeClientCount; ++i)
      if (activeClientIDs[i] == endpoint.clientID)
      {
        active = true;
        break;
      }
    if (!active)
    {
      if (!endpoint.draining)
      {
        endpoint.draining = true;
        changed = true;
      }

      const LGMP_STATUS status = lgmpHostStreamUnbind(endpoint.stream);
      if (status == LGMP_ERR_STREAM_BUSY)
      {
        // A vanished peer may have left an active reservation. Keep this
        // endpoint draining; force-unbind is reserved for transport teardown.
        continue;
      }
      if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      {
        DEBUG_ERROR("lgmpHostStreamUnbind Failed (Input): %s",
          lgmpStatusString(status));
        return false;
      }
      endpoint.clientID = 0;
      endpoint.epoch    = 0;
      endpoint.draining = false;
      changed           = true;
      continue;
    }

    if (!endpoint.draining)
      continue;

    const LGMP_STATUS status = lgmpHostStreamUnbind(endpoint.stream);
    if (status == LGMP_ERR_STREAM_BUSY)
      continue;
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostStreamUnbind Failed (Input): %s",
        lgmpStatusString(status));
      return false;
    }
    endpoint.clientID = 0;
    endpoint.epoch    = 0;
    endpoint.draining = false;
    changed           = true;
  }

  for (unsigned i = 0; i < activeClientCount; ++i)
  {
    const uint32_t clientID = activeClientIDs[i];
    bool bound = false;
    for (const StreamEndpoint& endpoint : m_streamEndpoint)
      if (endpoint.clientID == clientID)
      {
        bound = true;
        break;
      }
    if (bound)
      continue;

    StreamEndpoint * available = nullptr;
    for (StreamEndpoint& endpoint : m_streamEndpoint)
      if (!endpoint.clientID)
      {
        available = &endpoint;
        break;
      }
    if (!available)
      continue;

    uint32_t epoch = 0;
    const LGMP_STATUS status = lgmpHostStreamBind(
      available->stream, clientID, &epoch);
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostStreamBind Failed (Input client %u): %s",
        clientID, lgmpStatusString(status));
      return false;
    }
    available->clientID = clientID;
    available->epoch    = epoch;
    available->draining = false;
    changed             = true;
  }

  if (changed)
    Seq::Inc(m_streamGeneration);
  return true;
}

void CLGMPInputTransport::ResetStreams()
{
  bool changed = false;
  {
    CSRWExclusiveLock lock(m_streamLock);
    for (StreamEndpoint& endpoint : m_streamEndpoint)
    {
      if (!endpoint.clientID)
        continue;
      const LGMP_STATUS status = lgmpHostStreamUnbind(endpoint.stream);
      if (status != LGMP_OK && status != LGMP_ERR_STREAM_BUSY)
        DEBUG_WARN("lgmpHostStreamUnbind Failed (Input): %s",
          lgmpStatusString(status));
      if (status == LGMP_OK)
      {
        endpoint.clientID = 0;
        endpoint.epoch    = 0;
        endpoint.draining = false;
      }
      else if (status == LGMP_ERR_STREAM_BUSY)
      {
        // Stop is restartable, so preserve outstanding records and finish
        // the graceful unbind after the transport starts again.
        endpoint.draining = true;
      }
      changed           = true;
    }
    if (changed)
      Seq::Inc(m_streamGeneration);
  }

  if (changed)
  {
    CSRWExclusiveLock lock(m_statusLock);
    m_statusDirty = true;
  }
}

InputSourceId CLGMPInputTransport::Owner() const
{
  InputSourceId source;
  source.client     = m_ownerClientID;
  source.generation = m_ownerGeneration;
  return source;
}

void CLGMPInputTransport::UpdateTargetState(
  const InputTargetState& state)
{
  CSRWExclusiveLock lock(m_statusLock);
  if (state.state            == m_targetState.state            &&
      state.available        == m_targetState.available        &&
      state.owned            == m_targetState.owned            &&
      state.owner.client     == m_targetState.owner.client     &&
      state.owner.generation == m_targetState.owner.generation)
    return;

  if (state.state != m_targetState.state)
  {
    Seq::Inc(m_endpointGeneration);
  }
  m_targetState = state;
  m_statusDirty = true;
}

bool CLGMPInputTransport::PublishStatus()
{
  if (!m_queue)
    return true;

  bool streamChanged = false;
  if (!ReconcileStreams(streamChanged))
    return false;

  CSRWSharedLock streamLock(m_streamLock);
  CSRWExclusiveLock lock(m_statusLock);

  if (lgmpHostQueueNewSubs(m_queue))
    m_statusDirty = true;
  if (streamChanged)
    m_statusDirty = true;
  if (!m_statusDirty || !lgmpHostQueueHasSubs(m_queue))
    return true;

  PLGMPMemory memory = nullptr;
  for (PLGMPMemory candidate : m_statusMemory)
    if (!lgmpHostQueuePayloadPending(m_queue, candidate))
    {
      memory = candidate;
      break;
    }
  if (!memory)
    return true;

  const bool available = m_targetState.available;
  KVMFRInputStatus status = {};
  status.version             = KVMFR_INPUT_VERSION;
  status.capabilities        = available ?
    KVMFR_INPUT_CAP_MOUSE_RELATIVE |
    KVMFR_INPUT_CAP_MOUSE_ABSOLUTE |
    KVMFR_INPUT_CAP_KEYBOARD : 0;
  status.flags               = available ?
    KVMFR_INPUT_STATUS_AVAILABLE : 0;
  if (m_targetState.owned)
  {
    status.flags           |= KVMFR_INPUT_STATUS_HAS_OWNER;
    status.ownerClientID    = m_targetState.owner.client;
    status.ownerGeneration  = m_targetState.owner.generation;
  }
  status.generation          = m_endpointGeneration;
  status.lease               = static_cast<uint32_t>(OWNER_LEASE_MS);
  status.maxButtons          = KVMFR_INPUT_MOUSE_BUTTON_COUNT;
  status.streamVersion       = KVMFR_INPUT_STREAM_VERSION;
  status.streamEndpointCount = KVMFR_INPUT_STREAM_ENDPOINT_COUNT;
  status.streamGeneration    = m_streamGeneration;
  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    const StreamEndpoint& endpoint = m_streamEndpoint[i];
    KVMFRInputStreamEndpoint& exported = status.streamEndpoint[i];
    exported.stream                    =
      ExportStreamDescriptor(endpoint.descriptor);
    exported.flags                     =
      KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE;
    if (endpoint.clientID && !endpoint.draining)
    {
      exported.boundClientID             = endpoint.clientID;
      exported.bindingGeneration         = endpoint.epoch;
      exported.flags |= KVMFR_INPUT_STREAM_ENDPOINT_BOUND;
    }
  }
  memcpy(lgmpHostMemPtr(memory), &status, sizeof(status));

  const uint32_t serial = Seq::Next(m_statusSerial);
  const LGMP_STATUS result = lgmpHostQueuePost(m_queue, serial, memory);
  if (result == LGMP_OK)
  {
    m_statusSerial = serial;
    m_statusDirty  = false;
  }
  else if (result != LGMP_ERR_QUEUE_FULL)
  {
    DEBUG_WARN("lgmpHostQueuePost Failed (Input Status): %s",
      lgmpStatusString(result));
    return false;
  }
  return true;
}

void CLGMPInputTransport::FlushStatus()
{
  if (Atomic::Load(m_statusFailed, std::memory_order_acquire) ||
      PublishStatus())
    return;

  Atomic::Store(m_statusFailed, true, std::memory_order_release);
  CSRWSharedLock lock(m_lifecycleLock);
  if (m_stopEvent)
    SetEvent(m_stopEvent);
}

bool CLGMPInputTransport::Start(IInputTarget& target)
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_thread)
  {
    const DWORD state = WaitForSingleObject(m_thread, 0);
    if (state == WAIT_TIMEOUT)
      return true;
    if (state != WAIT_OBJECT_0)
    {
      DEBUG_ERROR_HR(GetLastError(),
        "Failed to inspect LGMP input worker");
      return false;
    }

    CloseHandle(m_thread);
    CloseHandle(m_pollTimer);
    CloseHandle(m_stopEvent);
    m_thread    = nullptr;
    m_pollTimer = nullptr;
    m_stopEvent = nullptr;
    m_target    = nullptr;
    ResetStreams();
  }

  if (!m_queue)
    return false;

  m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_pollTimer = CreateWaitableTimerExW(nullptr, nullptr,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!m_pollTimer)
    m_pollTimer = CreateWaitableTimerExW(
      nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  if (!m_stopEvent || !m_pollTimer)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGMP input worker resources");
    if (m_pollTimer)
      CloseHandle(m_pollTimer);
    if (m_stopEvent)
      CloseHandle(m_stopEvent);
    m_pollTimer = nullptr;
    m_stopEvent = nullptr;
    return false;
  }

  m_target = &target;
  {
    CSRWExclusiveLock statusLock(m_statusLock);
    m_targetState = target.GetState({});
    Seq::Inc(m_endpointGeneration);
    m_statusDirty = true;
  }
  Atomic::Store(m_statusFailed, false, std::memory_order_release);
  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create LGMP input worker");
    m_target = nullptr;
    {
      CSRWExclusiveLock statusLock(m_statusLock);
      m_targetState = {};
      m_statusDirty = true;
    }
    CloseHandle(m_pollTimer);
    CloseHandle(m_stopEvent);
    m_pollTimer = nullptr;
    m_stopEvent = nullptr;
    return false;
  }

  return true;
}

void CLGMPInputTransport::Stop()
{
  CSRWExclusiveLock lock(m_lifecycleLock);

  if (m_stopEvent)
    SetEvent(m_stopEvent);
  if (m_thread)
    WaitForSingleObject(m_thread, INFINITE);

  if (m_thread)
  {
    CloseHandle(m_thread);
    m_thread = nullptr;
  }
  if (m_pollTimer)
  {
    CloseHandle(m_pollTimer);
    m_pollTimer = nullptr;
  }
  if (m_stopEvent)
  {
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
  }
  m_target = nullptr;
  m_ownerClientID    = 0;
  m_ownerGeneration  = 0;
  m_ownerSequence    = 0;
  m_ownerDeadline    = 0;
  ResetStreams();
  {
    CSRWExclusiveLock statusLock(m_statusLock);
    m_targetState = {};
    m_statusDirty = true;
  }
}

bool CLGMPInputTransport::IsOwner(
  uint32_t sourceClientID, uint32_t generation) const
{
  return m_ownerClientID == sourceClientID &&
    m_ownerGeneration == generation;
}

bool CLGMPInputTransport::Claim(
  uint32_t sourceClientID, const KVMFRInputMessage& message)
{
  if (message.sequence != 1)
  {
    ++m_statistics.sequenceErrors;
    return false;
  }
  if (!m_target)
  {
    ++m_statistics.deliveryFailures;
    return false;
  }

  InputSourceId source;
  source.client     = sourceClientID;
  source.generation = message.generation;
  const InputResult result = m_target->Claim(source);
  if (result != InputResult::ACCEPTED)
  {
    UpdateTargetState(m_target->GetState(source));
    ++m_statistics.deliveryFailures;
    return false;
  }

  m_ownerClientID    = sourceClientID;
  m_ownerGeneration  = message.generation;
  m_ownerSequence    = message.sequence;
  RenewLease();
  UpdateTargetState(m_target->GetState(source));
  ++m_statistics.claims;
  return true;
}

void CLGMPInputTransport::RenewLease()
{
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
}

void CLGMPInputTransport::ReleaseOwner(bool reset)
{
  if (!m_ownerClientID)
    return;

  if (m_target)
  {
    m_target->Release(Owner(), reset);
    UpdateTargetState(m_target->GetState({}));
  }

  m_ownerClientID    = 0;
  m_ownerGeneration  = 0;
  m_ownerSequence    = 0;
  m_ownerDeadline    = 0;
  ++m_statistics.releases;
}

void CLGMPInputTransport::CheckOwner()
{
  if (!m_target)
    return;

  if (m_ownerClientID)
  {
    bool found = false;
    bool retiring = false;
    {
      CSRWSharedLock lock(m_streamLock);
      for (const StreamEndpoint& endpoint : m_streamEndpoint)
        if (endpoint.clientID == m_ownerClientID)
        {
          found = true;
          retiring = endpoint.draining;
          break;
        }
    }
    if (!found || retiring)
    {
      ReleaseOwner(true);
      return;
    }
  }

  const InputTargetState state = m_target->GetState(Owner());
  UpdateTargetState(state);

  if (!m_ownerClientID)
    return;

  if (!state.available || !state.owned)
  {
    ReleaseOwner(false);
    return;
  }

  if (GetTickCount64() >= m_ownerDeadline)
    ReleaseOwner(true);
}

bool CLGMPInputTransport::ValidatePayload(
  const KVMFRInputMessage& message) const
{
  switch (message.type)
  {
    case KVMFR_INPUT_MESSAGE_CLAIM:
    case KVMFR_INPUT_MESSAGE_RELEASE:
    case KVMFR_INPUT_MESSAGE_KEEPALIVE:
    case KVMFR_INPUT_MESSAGE_RESET:
      return IsZero(&message.payload, sizeof(message.payload));

    case KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE:
      return message.payload.mouseRelative.deltaX >= MIN_MOUSE_DELTA &&
        message.payload.mouseRelative.deltaX <= MAX_MOUSE_DELTA &&
        message.payload.mouseRelative.deltaY >= MIN_MOUSE_DELTA &&
        message.payload.mouseRelative.deltaY <= MAX_MOUSE_DELTA &&
        message.payload.mouseRelative.wheel >= MIN_MOUSE_WHEEL &&
        message.payload.mouseRelative.wheel <= MAX_MOUSE_WHEEL;

    case KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE:
      return message.payload.mouseAbsolute.x <=
          KVMFR_INPUT_MOUSE_ABSOLUTE_MAX &&
        message.payload.mouseAbsolute.y <=
          KVMFR_INPUT_MOUSE_ABSOLUTE_MAX &&
        message.payload.mouseAbsolute.wheel >= MIN_MOUSE_WHEEL &&
        message.payload.mouseAbsolute.wheel <= MAX_MOUSE_WHEEL &&
        !message.payload.mouseAbsolute.reserved;

    case KVMFR_INPUT_MESSAGE_KEYBOARD:
      if (!IsZero(message.payload.keyboard.reserved,
            sizeof(message.payload.keyboard.reserved)))
        return false;
      for (size_t i = 0; i < KVMFR_INPUT_KEYBOARD_KEY_COUNT; ++i)
        if (message.payload.keyboard.keys[i] >
            KVMFR_INPUT_KEYBOARD_USAGE_MAX)
          return false;
      return true;

    default:
      return false;
  }
}

bool CLGMPInputTransport::ProcessMessage(
  uint32_t sourceClientID, const KVMFRInputMessage& message)
{
  const bool owner = IsOwner(sourceClientID, message.generation);
  InputSourceId source;
  source.client     = sourceClientID;
  source.generation = message.generation;

  if (!message.generation || !message.sequence || message.reserved ||
      !ValidatePayload(message))
  {
    ++m_statistics.malformedMessage;
    if (owner)
      ReleaseOwner(true);
    return false;
  }

  if (m_target)
    UpdateTargetState(m_target->GetState(
      m_ownerClientID ? Owner() : source));

  if (message.type == KVMFR_INPUT_MESSAGE_CLAIM)
  {
    if (m_ownerClientID)
    {
      if (!owner)
      {
        ++m_statistics.nonOwner;
        return true;
      }
      if (message.sequence == 1 && m_ownerSequence == 1)
        return true;

      ++m_statistics.sequenceErrors;
      ReleaseOwner(true);
      return false;
    }
    return Claim(sourceClientID, message);
  }

  if (!owner)
  {
    ++m_statistics.nonOwner;
    return true;
  }

  const uint32_t expectedSequence = Seq::Next(m_ownerSequence);
  if (message.sequence != expectedSequence)
  {
    ++m_statistics.sequenceErrors;
    ReleaseOwner(true);
    return false;
  }

  InputResult result = InputResult::STALE;
  bool inputReport = false;
  switch (message.type)
  {
    case KVMFR_INPUT_MESSAGE_RELEASE:
      ReleaseOwner(true);
      return true;

    case KVMFR_INPUT_MESSAGE_KEEPALIVE:
      result = m_target ? m_target->Touch(source) :
        InputResult::UNAVAILABLE;
      break;

    case KVMFR_INPUT_MESSAGE_RESET:
      result = m_target ? m_target->Reset(source) :
        InputResult::UNAVAILABLE;
      break;

    case KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE:
      inputReport = true;
      result = m_target ? m_target->SendMouseRelative(source,
        message.payload.mouseRelative.deltaX,
        message.payload.mouseRelative.deltaY,
        message.payload.mouseRelative.wheel,
        message.payload.mouseRelative.buttons) : InputResult::UNAVAILABLE;
      break;

    case KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE:
      inputReport = true;
      result = m_target ? m_target->SendMouseAbsolute(source,
        message.payload.mouseAbsolute.x,
        message.payload.mouseAbsolute.y,
        message.payload.mouseAbsolute.wheel,
        message.payload.mouseAbsolute.buttons) : InputResult::UNAVAILABLE;
      break;

    case KVMFR_INPUT_MESSAGE_KEYBOARD:
      inputReport = true;
      result = m_target ? m_target->SendKeyboard(source,
        message.payload.keyboard.modifiers,
        message.payload.keyboard.keys) : InputResult::UNAVAILABLE;
      break;

    default:
      break;
  }

  if (result != InputResult::ACCEPTED)
  {
    ++m_statistics.deliveryFailures;
    ReleaseOwner(true);
    return false;
  }

  UpdateTargetState(m_target->GetState(source));

  m_ownerSequence = message.sequence;
  RenewLease();
  if (inputReport)
    ++m_statistics.reports;
  return true;
}

bool CLGMPInputTransport::DrainStreamMessages(bool& received)
{
  received = false;
  unsigned count = 0;
  CSRWSharedLock lock(m_streamLock);
  for (; count < DRAIN_LIMIT; ++count)
  {
    StreamEndpoint * selected = nullptr;
    LGMPStreamBuffer buffer = {};
    for (unsigned scanned = 0;
        scanned < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++scanned)
    {
      const unsigned index = (m_streamDrainCursor + scanned) %
        KVMFR_INPUT_STREAM_ENDPOINT_COUNT;
      StreamEndpoint& endpoint = m_streamEndpoint[index];
      if (!endpoint.clientID)
        continue;

      const LGMP_STATUS status = lgmpHostStreamReadPeek(
        endpoint.stream, &buffer);
      if (status == LGMP_ERR_STREAM_EMPTY ||
          status == LGMP_ERR_STREAM_UNBOUND)
        continue;
      if (status != LGMP_OK)
      {
        DEBUG_ERROR("lgmpHostStreamReadPeek Failed (Input): %s",
          lgmpStatusString(status));
        return false;
      }

      selected = &endpoint;
      m_streamDrainCursor = (index + 1) %
        KVMFR_INPUT_STREAM_ENDPOINT_COUNT;
      break;
    }

    if (!selected)
      break;

    received = true;
    ++m_statistics.messages;
    // Retired endpoint generations are consumed only to finish their
    // graceful unbind; they must not reach a newly started input target.
    if (!selected->draining)
    {
      if (buffer.size != sizeof(KVMFRInputMessage))
      {
        DEBUG_WARN("Ignoring invalid KVMFR input stream message size");
        ++m_statistics.malformedSize;
        if (selected->clientID == m_ownerClientID)
          ReleaseOwner(true);
      }
      else
      {
        KVMFRInputMessage message = {};
        memcpy(&message, buffer.data, sizeof(message));
        ProcessMessage(selected->clientID, message);
      }
    }

    const LGMP_STATUS releaseStatus = lgmpHostStreamReadRelease(
      selected->stream, &buffer);
    if (releaseStatus != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostStreamReadRelease Failed (Input): %s",
        lgmpStatusString(releaseStatus));
      return false;
    }
  }

  if (count > m_statistics.maxDrain)
    m_statistics.maxDrain = count;
  if (count == DRAIN_LIMIT)
    ++m_statistics.drainLimit;
  return true;
}

void CLGMPInputTransport::LogStatistics(ULONGLONG now)
{
  if (!m_statistics.lastLog)
  {
    m_statistics.lastLog = now;
    return;
  }
  if (now - m_statistics.lastLog < LOG_INTERVAL_MS)
    return;

  const Statistics statistics = m_statistics;
  m_statistics = {};
  m_statistics.lastLog = now;

  if (!statistics.messages && !statistics.claims &&
      !statistics.releases && !statistics.deliveryFailures)
    return;

  if (!g_settings.ShouldLogStatistics())
    return;

  const double elapsed =
    static_cast<double>(now - statistics.lastLog) / 1000.0;
  DEBUG_TRACE("LGMP input host: %.1f msg/s, %llu reports, "
    "drain max %u, %llu limit; %llu bad size, %llu malformed, "
    "%llu sequence, %llu non-owner, %llu delivery failures; "
    "%llu claims, %llu releases",
    statistics.messages / elapsed,
    static_cast<unsigned long long>(statistics.reports),
    statistics.maxDrain,
    static_cast<unsigned long long>(statistics.drainLimit),
    static_cast<unsigned long long>(statistics.malformedSize),
    static_cast<unsigned long long>(statistics.malformedMessage),
    static_cast<unsigned long long>(statistics.sequenceErrors),
    static_cast<unsigned long long>(statistics.nonOwner),
    static_cast<unsigned long long>(statistics.deliveryFailures),
    static_cast<unsigned long long>(statistics.claims),
    static_cast<unsigned long long>(statistics.releases));
}

DWORD CALLBACK CLGMPInputTransport::ThreadProc(void * context)
{
  static_cast<CLGMPInputTransport *>(context)->Thread();
  return 0;
}

void CLGMPInputTransport::Thread()
{
  DWORD avTask = 0;
  HANDLE avTaskHandle =
    AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
  if (avTaskHandle &&
      !AvSetMmThreadPriority(avTaskHandle, AVRT_PRIORITY_HIGH))
    DEBUG_WARN("Failed to raise input MMCSS priority: %lu",
      GetLastError());

  LGMPStreamPollState streamPoll = {};
  LGMPStreamPollConfig pollConfig = {};
  pollConfig.spinCount = 64U;
  pollConfig.minWaitUs = 25U;
  pollConfig.maxWaitUs = 1000U;
  const LGMP_STATUS pollStatus = lgmpStreamPollInit(&streamPoll,
    pollConfig);
  if (pollStatus != LGMP_OK)
  {
    DEBUG_ERROR("Failed to initialize LGMP input polling: %s",
      lgmpStatusString(pollStatus));
    if (m_target)
      m_target->Failed();
    if (avTaskHandle)
      AvRevertMmThreadCharacteristics(avTaskHandle);
    return;
  }

  m_statistics = {};
  m_statistics.lastLog = GetTickCount64();
  const HANDLE waitHandles[] = { m_stopEvent, m_pollTimer };
  bool failed = false;
  for (;;)
  {
    CheckOwner();
    bool streamReceived = false;
    if (!DrainStreamMessages(streamReceived))
    {
      failed = true;
      break;
    }
    if (!PublishStatus())
    {
      Atomic::Store(m_statusFailed, true, std::memory_order_release);
      failed = true;
      break;
    }
    if (streamReceived)
      lgmpStreamPollActivity(&streamPoll);
    const ULONGLONG now = GetTickCount64();
    LogStatistics(now);

    const uint32_t waitUs = lgmpStreamPollIdle(&streamPoll);
    if (!waitUs)
      continue;
    if (!ArmPollTimer(m_pollTimer, waitUs))
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to arm LGMP input timer");
      if (WaitForSingleObject(m_stopEvent, 1) != WAIT_TIMEOUT)
        break;
      failed = true;
      break;
    }

    const DWORD wait = WaitForMultipleObjects(
      _countof(waitHandles), waitHandles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0)
    {
      failed = Atomic::Load(m_statusFailed, std::memory_order_acquire);
      break;
    }
    if (wait != WAIT_OBJECT_0 + 1)
    {
      DEBUG_ERROR_HR(GetLastError(), "LGMP input worker wait failed");
      failed = true;
      break;
    }
  }

  ReleaseOwner(true);
  UpdateTargetState({});
  PublishStatus();
  if (failed && m_target)
    m_target->Failed();
  if (avTaskHandle)
    AvRevertMmThreadCharacteristics(avTaskHandle);
}
