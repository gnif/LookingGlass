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

#include "transport/network/CNetworkTransport.h"

#include "CDebug.h"

#include <new>
#include <utility>

static constexpr unsigned EVENT_DRAIN_LIMIT = 64;

CNetworkTransport::CNetworkTransport(const TransportInstance& config) :
  m_config(config)
{
}

CNetworkTransport::~CNetworkTransport()
{
  Stop();
}

ITransport::OpenResult CNetworkTransport::Open()
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_state == State::STOPPED)
    return OpenResult::FAILURE;
  if (m_state != State::CONSTRUCTED)
    return OpenResult::SUCCESS;

  NetworkSettings settings;
  std::wstring    error;
  if (!ParseNetworkSettings(m_config.settings, settings, &error))
  {
    DEBUG_ERROR("Invalid network transport settings: %ls", error.c_str());
    return OpenResult::FAILURE;
  }

  m_settings = std::move(settings);
  m_state    = State::OPEN;
  DEBUG_INFO("Network transport configured on [%ls]:%u for up to %u "
    "clients (%ls authentication)",
    m_settings.bindAddress.c_str(),
    static_cast<unsigned>(m_settings.port),
    static_cast<unsigned>(m_settings.maxClients),
    m_settings.auth == LG_NET_AUTH_PASSWORD ? L"password" : L"none");
  return OpenResult::SUCCESS;
}

bool CNetworkTransport::Initialize()
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_state == State::INITIALIZED || m_state == State::READY)
    return true;
  if (m_state != State::OPEN)
    return false;

  m_state = State::INITIALIZED;
  if (m_config.services & TRANSPORT_SERVICE_FRAME)
    DEBUG_WARN("Network video is unavailable until the GPU codec and "
      "texture sink are attached");
  return true;
}

bool CNetworkTransport::Setup(size_t alignment)
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_state == State::READY)
    return m_alignment == alignment;
  if (m_state != State::INITIALIZED)
    return false;

  m_alignment = alignment;
  m_state     = State::READY;
  return true;
}

ITransport::ProcessResult CNetworkTransport::Process(
  ITransportEvents& events)
{
  {
    CSRWSharedLock lock(m_lifecycleLock);
    if (m_state != State::INITIALIZED && m_state != State::READY)
      return m_state == State::STOPPED ?
        ProcessResult::FAILURE : ProcessResult::OK;
  }

  NetworkEvent event;
  for (unsigned i = 0; i < EVENT_DRAIN_LIMIT && m_events.Take(event); ++i)
  {
    const SessionPtr session = FindSession(event.source);
    if (!session)
      continue;
    session->ProcessEvent(event, events);
  }
  return ProcessResult::OK;
}

void CNetworkTransport::Stop()
{
  {
    CSRWExclusiveLock lock(m_lifecycleLock);
    if (m_state == State::STOPPED)
      return;
    m_state = State::STOPPED;
  }

  std::vector<SessionPtr> sessions;
  {
    CSRWExclusiveLock lock(m_sessionLock);
    sessions.swap(m_sessions);
  }
  for (const SessionPtr& session : sessions)
    session->Close();

  m_input.Stop();
  m_control.SetControlEvents(nullptr, {});
  m_events.Reset();
}

void CNetworkTransport::RecoveryStatus(const SourceKey& source,
  uint64_t session, uint32_t serial, bool active,
  Recovery state, uint32_t error)
{
  const SessionPtr target = FindSession(source);
  if (!target)
    return;

  NetworkRecoveryStatus status;
  status.session = session;
  status.serial  = serial;
  status.active  = active;
  status.state   = state;
  status.error   = error;
  target->SetRecoveryStatus(status);
}

const FrameProfile * CNetworkTransport::Profiles(unsigned& count) const
{
  count = 0;
  return nullptr;
}

CfgResult CNetworkTransport::Probe(const FrameCfg& cfg) const
{
  UNREFERENCED_PARAMETER(cfg);
  return CfgResult::REJECTED;
}

CfgResult CNetworkTransport::Prepare(const FrameCfg& cfg)
{
  UNREFERENCED_PARAMETER(cfg);
  return CfgResult::REJECTED;
}

void CNetworkTransport::Commit()
{
}

void CNetworkTransport::Abort()
{
}

std::shared_ptr<const FrameCaps> CNetworkTransport::GetFrameCaps() const
{
  return std::shared_ptr<const FrameCaps>();
}

DirectFrameBufferMemory CNetworkTransport::GetDirectMemory() const
{
  return DirectFrameBufferMemory {};
}

IControlSink * CNetworkTransport::Control()
{
  return (m_config.services & TRANSPORT_SERVICE_CONTROL) ?
    &m_control : nullptr;
}

IInputSource * CNetworkTransport::Input()
{
  return (m_config.services & TRANSPORT_SERVICE_INPUT) ? &m_input : nullptr;
}

bool CNetworkTransport::ControlSnapshot(
  NetworkControlSnapshot& state) const
{
  return m_control.Snapshot(state);
}

std::shared_ptr<CNetworkSession> CNetworkTransport::CreateSession(
  uint32_t client, uint32_t generation)
{
  if (!client || !generation)
    return std::shared_ptr<CNetworkSession>();

  SessionPtr created;
  {
    CSRWSharedLock lifecycleLock(m_lifecycleLock);
    if (m_state != State::READY)
      return std::shared_ptr<CNetworkSession>();

    CSRWExclusiveLock lock(m_sessionLock);
    if (m_sessions.size() >= m_settings.maxClients)
      return std::shared_ptr<CNetworkSession>();
    for (const SessionPtr& existing : m_sessions)
    {
      const InputSourceId source = existing->Source();
      if (source.client == client && source.generation == generation)
        return std::shared_ptr<CNetworkSession>();
    }

    created.reset(
      new (std::nothrow) CNetworkSession(
        client, generation, m_events, m_input));
    if (!created)
      return std::shared_ptr<CNetworkSession>();
    m_sessions.push_back(created);
  }
  m_control.RequestReplay();
  return created;
}

void CNetworkTransport::RemoveSession(
  uint32_t client, uint32_t generation)
{
  SessionPtr removed;
  {
    CSRWExclusiveLock lock(m_sessionLock);
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it)
    {
      const InputSourceId source = (*it)->Source();
      if (source.client != client || source.generation != generation)
        continue;
      removed = *it;
      m_sessions.erase(it);
      break;
    }
  }
  if (removed)
    removed->Close();
}

std::shared_ptr<CNetworkSession> CNetworkTransport::FindSession(
  const SourceKey& source) const
{
  CSRWSharedLock lock(m_sessionLock);
  for (const SessionPtr& session : m_sessions)
  {
    const InputSourceId candidate = session->Source();
    if (candidate.client == source.client &&
        candidate.generation == source.generation)
      return session;
  }
  return std::shared_ptr<CNetworkSession>();
}
