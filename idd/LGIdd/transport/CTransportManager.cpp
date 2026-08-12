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

#include "transport/CTransportManager.h"

#include "CDebug.h"

#include <Windows.h>
#include <new>

static const uint64_t RETRY_DELAY_MS = 500;

CTransportManager::~CTransportManager()
{
  Stop();
}

bool CTransportManager::Add(BackendId id, const char * name, bool required,
  bool primary, CreateFn create)
{
  if (!id || !name || !create || (primary && m_primary))
    return false;

  for (const auto& current : m_entries)
    if (current->id == id)
      return false;

  std::unique_ptr<Entry> entry(new (std::nothrow) Entry);
  if (!entry)
    return false;

  entry->id       = id;
  entry->name     = name;
  entry->required = required;
  entry->primary  = primary;
  entry->create   = create;

  Entry * raw = entry.get();
  m_entries.push_back(std::move(entry));
  if (primary)
    m_primary = raw;
  return true;
}

ITransport::OpenResult CTransportManager::OpenEntry(Entry& entry)
{
  if (!entry.transport)
    entry.transport = entry.create();
  if (!entry.transport)
  {
    entry.state = State::FAILED;
    return OpenResult::FAILURE;
  }

  const OpenResult result = entry.transport->Open();
  switch (result)
  {
    case OpenResult::SUCCESS:
      entry.state = State::OPEN;
      break;

    case OpenResult::RETRY:
      ScheduleRetry(entry);
      break;

    case OpenResult::FAILURE:
      entry.state = State::FAILED;
      break;
  }
  return result;
}

bool CTransportManager::InitializeEntry(Entry& entry)
{
  if (entry.state == State::INITIALIZED || entry.state == State::READY)
    return true;
  if (entry.state != State::OPEN || !entry.transport->Initialize())
  {
    entry.state = State::FAILED;
    return false;
  }

  entry.state = State::INITIALIZED;
  return true;
}

bool CTransportManager::SetupEntry(Entry& entry)
{
  if (entry.state == State::READY)
    return true;
  if (entry.state != State::INITIALIZED ||
      !entry.transport->Setup(m_alignment))
  {
    entry.state = State::FAILED;
    return false;
  }

  entry.state = State::READY;
  return true;
}

void CTransportManager::ScheduleRetry(Entry& entry)
{
  entry.state   = State::RETRY;
  entry.retryAt = GetTickCount64() + RETRY_DELAY_MS;
}

ITransport::OpenResult CTransportManager::Open()
{
  if (!m_primary)
    return OpenResult::FAILURE;

  OpenResult aggregate = OpenResult::SUCCESS;
  for (const auto& current : m_entries)
  {
    Entry& entry = *current;
    if (entry.state == State::OPEN ||
        entry.state == State::INITIALIZED || entry.state == State::READY)
      continue;

    const OpenResult result = OpenEntry(entry);
    if (!entry.required || result == OpenResult::SUCCESS)
      continue;
    if (result == OpenResult::FAILURE)
      return OpenResult::FAILURE;
    aggregate = OpenResult::RETRY;
  }
  return aggregate;
}

bool CTransportManager::Initialize()
{
  for (const auto& current : m_entries)
  {
    Entry& entry = *current;
    if (entry.state != State::OPEN)
      continue;
    if (!InitializeEntry(entry))
    {
      if (entry.required)
        return false;
      ScheduleRetry(entry);
    }
  }

  m_initialized = true;
  return m_primary &&
    (m_primary->state == State::INITIALIZED ||
     m_primary->state == State::READY);
}

bool CTransportManager::Setup(size_t alignment)
{
  if (!m_initialized || !m_primary)
    return false;

  m_alignment = alignment;
  if (!SetupEntry(*m_primary))
    return false;

  m_setup = true;
  return true;
}

void CTransportManager::RetryEntry(Entry& entry, uint64_t now)
{
  if (entry.state != State::RETRY || now < entry.retryAt)
    return;

  if (entry.primary && m_exposed)
    return;

  if (entry.transport)
    entry.transport->Stop();
  entry.transport.reset();
  ++entry.epoch;
  if (!entry.epoch)
    ++entry.epoch;

  if (OpenEntry(entry) != OpenResult::SUCCESS)
    return;
  if (m_initialized && !InitializeEntry(entry))
  {
    ScheduleRetry(entry);
    return;
  }
  if (m_setup && entry.primary && !SetupEntry(entry))
    ScheduleRetry(entry);
}

void CTransportManager::HandleProcessResult(
  Entry& entry, ProcessResult result)
{
  if (result == ProcessResult::OK)
    return;

  if (entry.primary && m_exposed)
  {
    DEBUG_WARN("Transport %s requested a restart while its frame interfaces "
      "are active", entry.name);
    return;
  }

  if (result == ProcessResult::RETRY || !entry.required)
  {
    ScheduleRetry(entry);
    return;
  }

  entry.state = State::FAILED;
}

ITransport::ProcessResult CTransportManager::Process(
  ITransportEvents& events)
{
  const uint64_t now = GetTickCount64();
  for (const auto& current : m_entries)
  {
    Entry& entry = *current;
    RetryEntry(entry, now);
    if (entry.state != State::INITIALIZED && entry.state != State::READY)
      continue;

    const ProcessResult result = entry.transport->Process(events);
    HandleProcessResult(entry, result);
  }
  return ProcessResult::OK;
}

void CTransportManager::Stop()
{
  for (auto current = m_entries.rbegin(); current != m_entries.rend();
       ++current)
  {
    Entry& entry = **current;
    if (entry.transport && entry.state != State::STOPPED)
      entry.transport->Stop();
    entry.state = State::STOPPED;
  }
}

void CTransportManager::SyncRecovery()
{
  for (const auto& current : m_entries)
    if (current->transport &&
        (current->state == State::INITIALIZED ||
         current->state == State::READY))
      current->transport->SyncRecovery();
}

void CTransportManager::RecoveryStatus(uint64_t session, uint32_t serial,
  bool active, Recovery state, uint32_t error)
{
  for (const auto& current : m_entries)
    if (current->transport &&
        (current->state == State::INITIALIZED ||
         current->state == State::READY))
      current->transport->RecoveryStatus(
        session, serial, active, state, error);
}

ITransport& CTransportManager::Primary()
{
  m_exposed = true;
  return *m_primary->transport;
}

FrameMemoryLimits CTransportManager::GetMemoryLimits() const
{
  return m_primary->transport->GetMemoryLimits();
}

DirectFrameBufferMemory CTransportManager::GetDirectMemory() const
{
  return m_primary->transport->GetDirectMemory();
}

IFrameTransport& CTransportManager::Frames()
{
  return Primary().Frames();
}

IControlTransport& CTransportManager::Control()
{
  return Primary().Control();
}

IInputTransport * CTransportManager::Input()
{
  return Primary().Input();
}
