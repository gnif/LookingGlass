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

#include "transport/lgmp/CRecovery.h"

#include "transport/lgmp/CIVSHMEM.h"
#include "platform/CPlatformInfo.h"
#include "Atomic.h"
#include "CDebug.h"
#include "VersionInfo.h"

#include "common/KVMFR.h"
#include "common/KVMFRRecovery.h"

#include <lgmp/lgmp.h>

#include <Windows.h>
#include <string.h>

namespace
{
  static const uint64_t HELPER_TIMEOUT_MS = 30000;
  CSRWLock l_wireLock;

  uint64_t CreateSession(const void * memory, uint64_t previous)
  {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    uint64_t session = static_cast<uint64_t>(counter.QuadPart) ^
      (GetTickCount64() << 24) ^
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(memory)) ^
      (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
      GetCurrentThreadId();

    if (!session || session == previous)
      ++session;
    if (!session)
      ++session;
    return session;
  }
}

bool CRecovery::OwnsSession()
{
  if (Atomic::Load(m_data->header.ready) != KVMFR_R_READY)
    return false;

  const uint64_t session = m_data->header.session;
  Atomic::Fence();
  return session == m_session &&
    Atomic::Load(m_data->header.ready) == KVMFR_R_READY;
}

bool CRecovery::ReadRequest(
  KVMFRRRequest& source, KVMFRRRequest& result)
{
  for (unsigned i = 0; i < 4; ++i)
  {
    const uint32_t serial = Atomic::Load(source.serial);
    if (!serial || (serial & 1U))
      return false;

    const uint32_t type    = source.request;
    const uint64_t session = source.session;
    Atomic::Fence();
    if (Atomic::Load(source.serial) == serial)
    {
      result.serial  = serial;
      result.request = type;
      result.session = session;
      return true;
    }
  }

  return false;
}

bool CRecovery::ReadStatus(KVMFRRStatus& source, KVMFRRStatus& result)
{
  for (unsigned i = 0; i < 4; ++i)
  {
    const uint32_t serial = Atomic::Load(source.serial);
    if (!serial || (serial & 1U))
      continue;

    result.ackSerial  = source.ackSerial;
    result.ackRequest = source.ackRequest;
    result.state      = source.state;
    result.error      = source.error;
    result.session    = source.session;
    Atomic::Fence();
    if (Atomic::Load(source.serial) == serial)
    {
      result.serial = serial;
      return true;
    }
  }

  return false;
}

bool CRecovery::SerialNewer(uint32_t serial, uint32_t reference)
{
  const uint32_t difference = serial - reference;
  return difference && difference < 0x80000000U;
}

uint32_t CRecovery::NextTicket()
{
  return Atomic::Next(m_data->req.ticket, 2U);
}

void CRecovery::Publish(uint32_t serial, uint32_t request,
  uint32_t state, uint32_t error)
{
  const uint32_t writing = m_statusSerial | 1U;
  uint32_t published = writing + 1U;
  if (!published)
    published = KVMFR_R_REQ_FIRST;

  Atomic::Store(m_data->status.serial, writing);
  m_data->status.ackRequest = request;
  m_data->status.state      = state;
  m_data->status.error      = error;
  m_data->status.session    = m_session;
  m_data->status.ackSerial  = serial;
  Atomic::Store(m_data->status.serial, published);
  m_statusSerial = published;
}

bool CRecovery::Initialize(CIVSHMEM& ivshmem)
{
  CSRWExclusiveLock wireLock(l_wireLock);
  CSRWExclusiveLock lock(m_lock);
  if (m_data)
    return OwnsSession();

  m_data = static_cast<KVMFRR *>(ivshmem.GetRecoveryMem());
  if (!m_data)
  {
    DEBUG_ERROR("IVSHMEM is too small for the recovery region");
    return false;
  }

  const uint32_t oldReady = Atomic::Load(m_data->header.ready);
  const bool oldValid = oldReady == KVMFR_R_READY &&
    memcmp(m_data->header.magic, KVMFR_R_MAGIC,
      sizeof(m_data->header.magic)) == 0 &&
    m_data->header.abiVersion == KVMFR_R_VERSION &&
    m_data->header.structSize >= sizeof(KVMFRR) &&
    m_data->header.session != 0;
  const uint64_t oldSession = oldValid ? m_data->header.session : 0;

  uint32_t retainedRequest = KVMFR_R_REQ_NONE;
  if (oldValid)
  {
    KVMFRRStatus status = {};
    if (ReadStatus(m_data->status, status) &&
        status.session == oldSession)
    {
      if (status.ackRequest == KVMFR_R_REQ_RECOVERY &&
          (status.state == KVMFR_R_STATE_SWITCHING ||
           status.state == KVMFR_R_STATE_ACTIVE ||
           status.state == KVMFR_R_STATE_FAILED))
        retainedRequest = KVMFR_R_REQ_RECOVERY;
      else if (status.ackRequest == KVMFR_R_REQ_NORMAL &&
               (status.state == KVMFR_R_STATE_SWITCHING ||
                status.state == KVMFR_R_STATE_FAILED))
        retainedRequest = KVMFR_R_REQ_NORMAL;
    }
  }

  Atomic::Store(m_data->header.ready, 0);
  if (oldValid)
  {
    ZeroMemory(&m_data->header, sizeof(m_data->header));
    ZeroMemory(&m_data->info, sizeof(m_data->info));
    ZeroMemory(&m_data->status, sizeof(m_data->status));

    // Preserve the client-owned ticket and interrupted odd slots. Completed
    // requests belong to the old producer session and can now be reclaimed.
    for (unsigned i = 0; i < KVMFR_R_REQ_SLOTS; ++i)
    {
      KVMFRRRequest request = {};
      if (ReadRequest(m_data->requests[i], request))
        Atomic::CAS(m_data->requests[i].serial, request.serial, 0);
    }
  }
  else
    ZeroMemory(m_data, sizeof(*m_data));

  m_session = CreateSession(m_data, oldSession);

  memcpy(m_data->header.magic, KVMFR_R_MAGIC,
    sizeof(m_data->header.magic));
  m_data->header.abiVersion   = KVMFR_R_VERSION;
  m_data->header.structSize   = static_cast<uint16_t>(sizeof(*m_data));
  m_data->header.capabilities = KVMFR_R_CAP_DISPLAY;
  m_data->header.lgmpVersion  = LGMP_PROTOCOL_VERSION;
  m_data->header.kvmfrVersion = KVMFR_VERSION;
  m_data->header.session      = m_session;
  memcpy(m_data->header.uuid, CPlatformInfo::GetUUID(),
    sizeof(m_data->header.uuid));
  m_data->header.heartbeat    = 1;
  strncpy_s(m_data->info.version, sizeof(m_data->info.version),
    LG_VERSION_STR, _TRUNCATE);

  m_request    = retainedRequest == KVMFR_R_REQ_NONE ?
    KVMFR_R_REQ_NORMAL : retainedRequest;
  m_lastSerial = NextTicket();
  m_replay     = true;
  Publish(m_lastSerial, m_request,
    KVMFR_R_STATE_SWITCHING, KVMFR_R_ERR_NONE);

  m_nextHeartbeat = GetTickCount64() + KVMFR_R_HEARTBEAT_MS;
  Atomic::Store(m_data->header.ready, KVMFR_R_READY);

  DEBUG_INFO("Recovery channel initialized (session %llu%s)",
    (unsigned long long)m_session,
    retainedRequest != KVMFR_R_REQ_NONE ? ", request retained" : "");
  return true;
}

void CRecovery::Sync()
{
  CSRWExclusiveLock lock(m_lock);
  m_syncReady = true;
}

CRecovery::Request CRecovery::Process()
{
  Request result;
  CSRWExclusiveLock wireLock(l_wireLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_data || !OwnsSession())
    return result;

  const uint64_t now = GetTickCount64();
  if (now >= m_nextHeartbeat)
  {
    Atomic::Inc(m_data->header.heartbeat);
    m_nextHeartbeat = now + KVMFR_R_HEARTBEAT_MS;
  }

  KVMFRRRequest requests[KVMFR_R_REQ_SLOTS] = {};
  bool stable[KVMFR_R_REQ_SLOTS] = {};
  KVMFRRRequest request = {};
  bool haveRequest = false;
  for (unsigned i = 0; i < KVMFR_R_REQ_SLOTS; ++i)
  {
    stable[i] = ReadRequest(m_data->requests[i], requests[i]);
    if (!stable[i] || requests[i].session != m_session ||
        !SerialNewer(requests[i].serial, m_lastSerial))
      continue;

    if (!haveRequest || SerialNewer(requests[i].serial, request.serial))
    {
      request     = requests[i];
      haveRequest = true;
    }
  }

  if (haveRequest)
  {
    m_lastSerial = request.serial;

    if (request.session == m_session &&
        (request.request == KVMFR_R_REQ_NORMAL ||
         request.request == KVMFR_R_REQ_RECOVERY))
    {
      m_request  = request.request;
      m_replay   = m_request == KVMFR_R_REQ_NORMAL && !m_syncReady;
      m_waiting  = false;
      Publish(m_lastSerial, m_request,
        KVMFR_R_STATE_SWITCHING, KVMFR_R_ERR_NONE);

      if (m_replay)
        DEBUG_INFO("Deferring recovery request %u until monitor arrival",
          m_lastSerial);
      else
      {
        m_waiting  = true;
        m_deadline = now + HELPER_TIMEOUT_MS;

        result.session = m_session;
        result.serial  = m_lastSerial;
        result.valid   = true;
        result.active  = m_request == KVMFR_R_REQ_RECOVERY;
        DEBUG_INFO("Recovery mode request %u: %s", m_lastSerial,
          result.active ? "active" : "normal");
      }
    }
    else if (request.session == m_session)
    {
      m_request = request.request;
      m_replay  = false;
      m_waiting = false;
      Publish(m_lastSerial, m_request,
        KVMFR_R_STATE_FAILED, KVMFR_R_ERR_UNSUPPORTED);
      DEBUG_WARN("Ignoring invalid recovery request %u", m_lastSerial);
    }
  }
  else if (m_replay &&
           (m_request != KVMFR_R_REQ_NORMAL || m_syncReady))
  {
    m_replay   = false;
    m_waiting  = true;
    m_deadline = now + HELPER_TIMEOUT_MS;

    result.session = m_session;
    result.serial  = m_lastSerial;
    result.valid   = true;
    result.active  = m_request == KVMFR_R_REQ_RECOVERY;
    DEBUG_INFO("Synchronizing recovery helper mode: %s",
      result.active ? "active" : "normal");
  }

  // A stable slot cannot be reused until the producer clears it. Publish the
  // selected request's state first so its client cannot observe a reclaimed
  // slot without a corresponding acknowledgement.
  for (unsigned i = 0; i < KVMFR_R_REQ_SLOTS; ++i)
    if (stable[i])
      Atomic::CAS(m_data->requests[i].serial, requests[i].serial, 0);

  if (m_waiting && now >= m_deadline)
  {
    m_waiting = false;
    Publish(m_lastSerial, m_request,
      KVMFR_R_STATE_FAILED, KVMFR_R_ERR_HELPER_UNAVAILABLE);
    DEBUG_WARN("Recovery helper did not respond to request %u",
      m_lastSerial);
  }

  return result;
}

void CRecovery::SetStatus(uint64_t session, uint32_t serial, bool active,
  uint32_t state, uint32_t error)
{
  CSRWExclusiveLock wireLock(l_wireLock);
  CSRWExclusiveLock lock(m_lock);
  const bool expectedActive = m_request == KVMFR_R_REQ_RECOVERY;
  if (!m_data || !OwnsSession() || session != m_session ||
      serial != m_lastSerial ||
      active != expectedActive ||
      (state == KVMFR_R_STATE_ACTIVE && !active) ||
      (state == KVMFR_R_STATE_NORMAL && active))
  {
    DEBUG_WARN("Ignoring stale recovery helper status");
    return;
  }

  m_waiting = false;
  Publish(serial, m_request, state, error);
  DEBUG_INFO("Recovery request %u completed with state %u", serial, state);
}
