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

#include "transport/lgmp/CLGMPInputTransport.h"

#include "input/IInputSink.h"
#include "transport/lgmp/CLGMPHost.h"
#include "CDebug.h"
#include "CSRWLock.h"

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

static bool ArmPollTimer(HANDLE timer, bool active)
{
  LARGE_INTEGER due = {};
  due.QuadPart = active ? -2500 : -10000;
  return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

CLGMPInputTransport::~CLGMPInputTransport()
{
  DeInit();
}

bool CLGMPInputTransport::Initialize()
{
  if (m_queue)
    return true;

  const LGMP_STATUS status = m_host.CreateQueue(
    INPUT_QUEUE_CONFIG, &m_queue);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Input): %s",
      lgmpStatusString(status));
    return false;
  }

  return true;
}

void CLGMPInputTransport::DeInit()
{
  Stop();
  m_queue = nullptr;
}

bool CLGMPInputTransport::Start(IInputSink& sink)
{
  CSRWExclusiveLock lock(&m_lifecycleLock);
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
    m_sink      = nullptr;
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

  m_sink = &sink;
  m_sinkGeneration = sink.GetGeneration();
  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to create LGMP input worker");
    m_sink = nullptr;
    m_sinkGeneration = 0;
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
  HANDLE thread;
  {
    CSRWExclusiveLock lock(&m_lifecycleLock);
    thread = m_thread;
    if (m_stopEvent)
      SetEvent(m_stopEvent);
  }

  if (thread)
    WaitForSingleObject(thread, INFINITE);

  CSRWExclusiveLock lock(&m_lifecycleLock);
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
  m_sink = nullptr;
  m_ownerClientID   = 0;
  m_ownerGeneration = 0;
  m_ownerSequence   = 0;
  m_ownerDeadline   = 0;
  m_sinkGeneration  = 0;
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
  if (message.sequence != 1 || !m_sink || !m_sink->IsAvailable())
    return false;

  const uint64_t sinkGeneration = m_sink->GetGeneration();
  if (sinkGeneration != m_sinkGeneration || !m_sink->Reset() ||
      !m_sink->IsAvailable() ||
      m_sink->GetGeneration() != sinkGeneration)
    return false;

  m_ownerClientID   = sourceClientID;
  m_ownerGeneration = message.generation;
  m_ownerSequence   = message.sequence;
  RenewLease();
  DEBUG_INFO("Input owner %u generation %u acquired",
    m_ownerClientID, m_ownerGeneration);
  return true;
}

void CLGMPInputTransport::RenewLease()
{
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
}

void CLGMPInputTransport::ReleaseOwner(
  bool reset, const char * reason)
{
  if (!m_ownerClientID)
    return;

  const uint32_t clientID   = m_ownerClientID;
  const uint32_t generation = m_ownerGeneration;
  if (reset && m_sink)
    m_sink->Reset();

  m_ownerClientID   = 0;
  m_ownerGeneration = 0;
  m_ownerSequence   = 0;
  m_ownerDeadline   = 0;
  DEBUG_INFO("Input owner %u generation %u released (%s)",
    clientID, generation, reason);
}

void CLGMPInputTransport::CheckOwner()
{
  if (!m_sink)
    return;

  const uint64_t generation = m_sink->GetGeneration();
  if (generation != m_sinkGeneration)
  {
    m_sinkGeneration = generation;
    ReleaseOwner(true, "input endpoint changed");
    return;
  }

  if (!m_ownerClientID)
    return;

  if (!m_sink->IsAvailable())
  {
    ReleaseOwner(true, "input unavailable");
    return;
  }

  if (GetTickCount64() >= m_ownerDeadline)
    ReleaseOwner(true, "lease expired");
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
  const uint64_t sinkGeneration = m_sink ?
    m_sink->GetGeneration() : m_sinkGeneration;
  if (sinkGeneration != m_sinkGeneration)
  {
    m_sinkGeneration = sinkGeneration;
    if (m_ownerClientID)
    {
      ReleaseOwner(true, "input endpoint changed");
      return false;
    }
  }

  if (!message.generation || !message.sequence || message.reserved ||
      !ValidatePayload(message))
  {
    if (owner)
      ReleaseOwner(true, "invalid input message");
    return false;
  }

  if (message.type == KVMFR_INPUT_MESSAGE_CLAIM)
  {
    if (m_ownerClientID)
    {
      if (!owner)
        return true;
      if (message.sequence == 1 && m_ownerSequence == 1)
        return true;

      ReleaseOwner(true, "sequence discontinuity");
      return false;
    }
    return Claim(sourceClientID, message);
  }

  if (!owner)
    return true;

  uint32_t expectedSequence = m_ownerSequence + 1;
  if (!expectedSequence)
    expectedSequence = 1;
  if (message.sequence != expectedSequence)
  {
    ReleaseOwner(true, "sequence discontinuity");
    return false;
  }

  bool accepted = false;
  switch (message.type)
  {
    case KVMFR_INPUT_MESSAGE_RELEASE:
      ReleaseOwner(true, "client release");
      return true;

    case KVMFR_INPUT_MESSAGE_KEEPALIVE:
      accepted = true;
      break;

    case KVMFR_INPUT_MESSAGE_RESET:
      accepted = m_sink && m_sink->Reset();
      break;

    case KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE:
      accepted = m_sink && m_sink->SendMouseRelative(
        message.payload.mouseRelative.deltaX,
        message.payload.mouseRelative.deltaY,
        message.payload.mouseRelative.wheel,
        message.payload.mouseRelative.buttons);
      break;

    case KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE:
      accepted = m_sink && m_sink->SendMouseAbsolute(
        message.payload.mouseAbsolute.x,
        message.payload.mouseAbsolute.y,
        message.payload.mouseAbsolute.wheel,
        message.payload.mouseAbsolute.buttons);
      break;

    case KVMFR_INPUT_MESSAGE_KEYBOARD:
      accepted = m_sink && m_sink->SendKeyboard(
        message.payload.keyboard.modifiers,
        message.payload.keyboard.keys);
      break;

    default:
      break;
  }

  if (!accepted)
  {
    ReleaseOwner(true, "input delivery failed");
    return false;
  }

  const uint64_t deliveredGeneration = m_sink->GetGeneration();
  if (deliveredGeneration != m_sinkGeneration)
  {
    m_sinkGeneration = deliveredGeneration;
    ReleaseOwner(true, "input endpoint changed");
    return false;
  }

  m_ownerSequence = message.sequence;
  RenewLease();
  return true;
}

bool CLGMPInputTransport::DrainMessages()
{
  bool received = false;
  for (unsigned count = 0; count < 256; ++count)
  {
    uint8_t data[LGMP_MSGS_SIZE] = {};
    size_t size = 0;
    uint32_t sourceClientID = 0;
    const LGMP_STATUS status = lgmpHostReadDataWithSource(
      m_queue, data, &size, &sourceClientID);
    if (status == LGMP_ERR_QUEUE_EMPTY)
      break;
    if (status != LGMP_OK)
    {
      DEBUG_ERROR("lgmpHostReadData Failed (Input): %s",
        lgmpStatusString(status));
      break;
    }

    received = true;
    if (size != sizeof(KVMFRInputMessage))
    {
      DEBUG_WARN("Ignoring invalid KVMFR input message size");
      if (sourceClientID == m_ownerClientID)
        ReleaseOwner(true, "invalid input message");
    }
    else
    {
      KVMFRInputMessage message = {};
      memcpy(&message, data, sizeof(message));
      ProcessMessage(sourceClientID, message);
    }

    lgmpHostAckData(m_queue);
  }
  return received;
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

  ULONGLONG activeUntil = 0;
  const HANDLE waitHandles[] = { m_stopEvent, m_pollTimer };
  for (;;)
  {
    CheckOwner();
    if (DrainMessages())
      activeUntil = GetTickCount64() + ACTIVE_POLL_MS;

    const bool active = GetTickCount64() < activeUntil;
    if (!ArmPollTimer(m_pollTimer, active))
    {
      DEBUG_ERROR_HR(GetLastError(), "Failed to arm LGMP input timer");
      if (WaitForSingleObject(m_stopEvent, 1) != WAIT_TIMEOUT)
        break;
      continue;
    }

    const DWORD wait = WaitForMultipleObjects(
      _countof(waitHandles), waitHandles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait != WAIT_OBJECT_0 + 1)
    {
      DEBUG_ERROR_HR(GetLastError(), "LGMP input worker wait failed");
      break;
    }
  }

  ReleaseOwner(true, "transport stopped");
  if (avTaskHandle)
    AvRevertMmThreadCharacteristics(avTaskHandle);
}
