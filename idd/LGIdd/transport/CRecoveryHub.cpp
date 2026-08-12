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

#include "transport/CRecoveryHub.h"

#include <Windows.h>

namespace
{
  uint64_t CreateSession(const void * owner)
  {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);

    uint64_t session = static_cast<uint64_t>(counter.QuadPart) ^
      (GetTickCount64() << 24) ^
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(owner)) ^
      (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
      GetCurrentThreadId();
    if (!session)
      ++session;
    return session;
  }
}

CRecoveryHub::CRecoveryHub() :
  m_session(CreateSession(this))
{
}

bool CRecoveryHub::SameSource(
  const SourceKey& left, const SourceKey& right)
{
  return left.backend == right.backend && left.epoch == right.epoch &&
    left.client == right.client && left.generation == right.generation;
}

bool CRecoveryHub::SameEpoch(
  const SourceKey& source, BackendId backend, uint32_t epoch)
{
  return source.backend == backend && source.epoch == epoch;
}

bool CRecoveryHub::SameRequest(const Request& request,
  const SourceKey& source, uint64_t session, uint32_t serial, bool active)
{
  return SameSource(request.source, source) && request.session == session &&
    request.serial == serial && request.active == active;
}

bool CRecoveryHub::SuccessMatches(RecoveryState state, bool active)
{
  return (state == RecoveryState::ACTIVE && active) ||
    (state == RecoveryState::NORMAL && !active);
}

bool CRecoveryHub::EpochAttachedLocked(const SourceKey& source) const
{
  for (const Epoch& epoch : m_epochs)
    if (epoch.backend == source.backend && epoch.epoch == source.epoch)
      return true;
  return false;
}

unsigned CRecoveryHub::FindSourceLocked(const SourceKey& source) const
{
  for (unsigned i = 0; i < MAX_REQUESTS; ++i)
    if (m_requests[i].state != SlotState::FREE &&
        SameSource(m_requests[i].source, source))
      return i;
  return MAX_REQUESTS;
}

unsigned CRecoveryHub::FindFreeLocked() const
{
  for (unsigned i = 0; i < MAX_REQUESTS; ++i)
    if (m_requests[i].state == SlotState::FREE)
      return i;
  return MAX_REQUESTS;
}

bool CRecoveryHub::ActionMatchesLocked(
  const RecoveryAction& action) const
{
  return m_operation.phase != OperationPhase::NONE &&
    action.route == m_operation.action.route &&
    action.session == m_operation.action.session &&
    action.serial == m_operation.action.serial &&
    action.active == m_operation.action.active;
}

uint64_t CRecoveryHub::NextNonzero(uint64_t& value)
{
  uint64_t result = value++;
  if (!result)
    result = value++;
  if (!value)
    ++value;
  return result;
}

uint32_t CRecoveryHub::NextSerial()
{
  uint32_t result = m_nextSerial;
  m_nextSerial += 2;
  if (!result)
  {
    result = 2;
    m_nextSerial = 4;
  }
  return result;
}

void CRecoveryHub::ClearRequestLocked(Request& request)
{
  request = Request {};
}

void CRecoveryHub::SetWaitingLocked(Request& request,
  const SourceKey& source, uint64_t session, uint32_t serial,
  bool active, uint64_t operation)
{
  request             = Request {};
  request.source      = source;
  request.session     = session;
  request.operation   = operation;
  request.sequence    = NextNonzero(m_nextSequence);
  request.serial      = serial;
  request.active      = active;
  request.state       = SlotState::WAITING;
}

void CRecoveryHub::SetReadyLocked(
  Request& request, RecoveryState state, uint32_t error)
{
  request.state  = SlotState::READY;
  request.result = state;
  request.error  = error;
}

void CRecoveryHub::FinishWaitersLocked(
  RecoveryState state, uint32_t error)
{
  for (Request& request : m_requests)
    if (request.state == SlotState::WAITING &&
        request.operation == m_operation.id)
      SetReadyLocked(request, state, error);
}

bool CRecoveryHub::Attach(
  BackendId backend, uint32_t epoch, bool& syncNow)
{
  syncNow = false;
  if (!backend || !epoch)
    return false;

  CSRWExclusiveLock lock(m_lock);
  for (const Epoch& item : m_epochs)
    if (item.backend == backend && item.epoch == epoch)
      return true;

  for (Epoch& item : m_epochs)
    if (!item.backend)
    {
      item.backend = backend;
      item.epoch   = epoch;
      item.synced  = m_monitorReady;
      syncNow      = m_monitorReady;
      return true;
    }
  return false;
}

void CRecoveryHub::Remove(BackendId backend, uint32_t epoch)
{
  if (!backend || !epoch)
    return;

  CSRWExclusiveLock lock(m_lock);
  for (Epoch& item : m_epochs)
    if (item.backend == backend && item.epoch == epoch)
      item = Epoch {};

  for (Request& request : m_requests)
    if (request.state != SlotState::FREE &&
        SameEpoch(request.source, backend, epoch))
      ClearRequestLocked(request);
}

RecoveryAdmission CRecoveryHub::Submit(const SourceKey& source,
  uint64_t session, uint32_t serial, bool active, uint64_t now,
  RecoveryAction& action, bool& dispatch)
{
  action   = RecoveryAction {};
  dispatch = false;

  RecoveryAdmission result;
  result.complete = true;
  result.state    = RecoveryState::FAILED;
  result.error    = ERROR_INVALID_PARAMETER;

  CSRWExclusiveLock lock(m_lock);
  if (!source.backend || !source.epoch || !session || !serial ||
      !EpochAttachedLocked(source))
    return result;

  unsigned index = FindSourceLocked(source);
  if (index != MAX_REQUESTS && SameRequest(
        m_requests[index], source, session, serial, active))
  {
    result.complete = false;
    result.error    = ERROR_SUCCESS;
    return result;
  }

  if (index != MAX_REQUESTS)
    ClearRequestLocked(m_requests[index]);
  else
    index = FindFreeLocked();

  if (m_operation.phase == OperationPhase::IN_FLIGHT)
  {
    if (active != m_operation.action.active)
    {
      result.error = ERROR_BUSY;
      return result;
    }
    if (index == MAX_REQUESTS)
    {
      result.error = ERROR_NOT_ENOUGH_QUOTA;
      return result;
    }

    SetWaitingLocked(m_requests[index], source, session, serial,
      active, m_operation.id);
    result.complete = false;
    result.error    = ERROR_SUCCESS;
    return result;
  }

  if (m_knownValid &&
      ((active && m_known == RecoveryState::ACTIVE) ||
       (!active && m_known == RecoveryState::NORMAL)))
  {
    result.state = m_known;
    result.error = ERROR_SUCCESS;
    return result;
  }

  if (index == MAX_REQUESTS)
  {
    result.error = ERROR_NOT_ENOUGH_QUOTA;
    return result;
  }

  m_operation = Operation {};
  m_operation.phase = OperationPhase::IN_FLIGHT;
  m_operation.id    = NextNonzero(m_nextOperation);
  m_operation.action.route    = NextNonzero(m_nextRoute);
  m_operation.action.session  = m_session;
  m_operation.action.serial   = NextSerial();
  m_operation.action.active   = active;
  m_operation.action.deadline = now + HELPER_TIMEOUT_MS;
  SetWaitingLocked(m_requests[index], source, session, serial,
    active, m_operation.id);

  action          = m_operation.action;
  dispatch        = true;
  result.complete = false;
  result.error    = ERROR_SUCCESS;
  return result;
}

bool CRecoveryHub::DispatchFailed(
  const RecoveryAction& action, uint32_t error)
{
  CSRWExclusiveLock lock(m_lock);
  if (!ActionMatchesLocked(action) ||
      m_operation.phase != OperationPhase::IN_FLIGHT)
    return false;

  FinishWaitersLocked(RecoveryState::FAILED,
    error ? error : RPC_S_SERVER_UNAVAILABLE);
  m_operation = Operation {};
  return true;
}

bool CRecoveryHub::Complete(const RecoveryAction& action,
  RecoveryState state, uint32_t error)
{
  CSRWExclusiveLock lock(m_lock);
  if (!ActionMatchesLocked(action))
    return false;

  if (!SuccessMatches(state, m_operation.action.active))
  {
    state = RecoveryState::FAILED;
    if (!error)
      error = ERROR_GEN_FAILURE;
    m_knownValid = false;
  }
  else
  {
    error        = ERROR_SUCCESS;
    m_known      = state;
    m_knownValid = true;
  }

  if (m_operation.phase == OperationPhase::LATCHED)
    return true;

  FinishWaitersLocked(state, error);
  m_operation.phase           = OperationPhase::LATCHED;
  m_operation.action.deadline = 0;
  return true;
}

bool CRecoveryHub::Tick(uint64_t now)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_operation.phase != OperationPhase::IN_FLIGHT ||
      now < m_operation.action.deadline)
    return false;

  FinishWaitersLocked(RecoveryState::FAILED, ERROR_TIMEOUT);
  m_knownValid                 = false;
  m_operation.phase           = OperationPhase::LATCHED;
  m_operation.action.deadline = 0;
  return true;
}

bool CRecoveryHub::TakeDelivery(
  BackendId backend, uint32_t epoch, Delivery& delivery)
{
  CSRWExclusiveLock lock(m_lock);
  unsigned selected = MAX_REQUESTS;
  uint64_t sequence = 0;
  for (unsigned i = 0; i < MAX_REQUESTS; ++i)
    if (m_requests[i].state == SlotState::READY &&
        SameEpoch(m_requests[i].source, backend, epoch) &&
        (selected == MAX_REQUESTS || m_requests[i].sequence < sequence))
    {
      selected = i;
      sequence = m_requests[i].sequence;
    }

  if (selected == MAX_REQUESTS)
    return false;

  const Request& request = m_requests[selected];
  delivery.source  = request.source;
  delivery.session = request.session;
  delivery.serial  = request.serial;
  delivery.active  = request.active;
  delivery.state   = request.result;
  delivery.error   = request.error;
  ClearRequestLocked(m_requests[selected]);
  return true;
}

bool CRecoveryHub::HasDelivery(
  BackendId backend, uint32_t epoch) const
{
  CSRWSharedLock lock(m_lock);
  for (const Request& request : m_requests)
    if (request.state == SlotState::READY &&
        SameEpoch(request.source, backend, epoch))
      return true;
  return false;
}

bool CRecoveryHub::MarkMonitorReady()
{
  CSRWExclusiveLock lock(m_lock);
  if (m_monitorReady)
    return false;
  m_monitorReady = true;
  return true;
}

bool CRecoveryHub::ClaimSync(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lock(m_lock);
  if (!m_monitorReady)
    return false;
  for (Epoch& item : m_epochs)
    if (item.backend == backend && item.epoch == epoch && !item.synced)
    {
      item.synced = true;
      return true;
    }
  return false;
}

bool CRecoveryHub::MonitorReady() const
{
  CSRWSharedLock lock(m_lock);
  return m_monitorReady;
}
