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

#include "transport/network/CNetworkServer.h"

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
    "clients (%ls authentication, %ls video)",
    m_settings.bindAddress.c_str(),
    static_cast<unsigned>(m_settings.port),
    static_cast<unsigned>(m_settings.maxClients),
    m_settings.auth == LG_NET_AUTH_PASSWORD ? L"password" : L"none",
    m_settings.videoChroma ==
      static_cast<LGNetVideoChromaSubsampling>(LG_NET_VIDEO_CHROMA_420) ?
        L"4:2:0" : L"4:4:4");
  return OpenResult::SUCCESS;
}

bool CNetworkTransport::Initialize()
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_state == State::INITIALIZED || m_state == State::READY)
    return true;
  if (m_state != State::OPEN)
    return false;

  try
  {
    m_server.reset(
      new CNetworkServer(
        *this, m_audio, m_clipboard, m_video, m_settings));
  }
  catch (...)
  {
    DEBUG_ERROR("Failed to allocate the network server");
    return false;
  }
  if (!m_audio.AttachSender(*m_server))
  {
    DEBUG_ERROR("Failed to attach the network audio sender");
    m_server.reset();
    return false;
  }
  if (!m_clipboard.AttachSender(*m_server))
  {
    DEBUG_ERROR("Failed to attach the network clipboard sender");
    m_audio.DetachSender();
    m_server.reset();
    return false;
  }
  if (!m_video.AttachSender(*m_server))
  {
    DEBUG_ERROR("Failed to attach the network video sender");
    m_clipboard.DetachSender();
    m_audio.DetachSender();
    m_server.reset();
    return false;
  }
  if ((m_config.services & TRANSPORT_SERVICE_FRAME) &&
      !m_video.Start(m_settings.videoChroma))
  {
    DEBUG_ERROR("Failed to start the network video endpoint");
    m_video.DetachSender();
    m_clipboard.DetachSender();
    m_audio.DetachSender();
    m_server.reset();
    return false;
  }

  m_state = State::INITIALIZED;
  return true;
}

bool CNetworkTransport::Setup(size_t alignment)
{
  CNetworkServer * server;
  {
    CSRWExclusiveLock lock(m_lifecycleLock);
    if (m_state == State::READY)
      return m_alignment == alignment;
    if (m_state != State::INITIALIZED)
      return false;

    m_alignment = alignment;
    m_state     = State::READY;
    server      = m_server.get();
  }

  if (server && server->Start())
    return true;

  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_state == State::READY)
  {
    m_alignment = 0;
    m_state     = State::INITIALIZED;
  }
  return false;
}

ITransport::ProcessResult CNetworkTransport::Process(
  ITransportEvents& events)
{
  CNetworkServer * server;
  {
    CSRWSharedLock lock(m_lifecycleLock);
    if (m_state != State::INITIALIZED && m_state != State::READY)
      return m_state == State::STOPPED ?
        ProcessResult::FAILURE : ProcessResult::OK;
    server = m_server.get();
  }

  if (server)
    server->Process();

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
  CNetworkServer * server;
  {
    CSRWExclusiveLock lock(m_lifecycleLock);
    if (m_state == State::STOPPED)
      return;
    m_state = State::STOPPED;
    server  = m_server.get();
  }

  if (server)
    server->Stop();
  m_video.DetachSender();
  m_clipboard.DetachSender();
  m_audio.DetachSender();

  std::vector<SessionPtr> sessions;
  {
    CSRWExclusiveLock lock(m_sessionLock);
    sessions.swap(m_sessions);
  }
  for (const SessionPtr& session : sessions)
    session->Close();

  m_input.Stop();
  m_video.Stop();
  m_audio.Stop();
  m_clipboard.Stop();
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
  if (!(m_config.services & TRANSPORT_SERVICE_FRAME))
  {
    count = 0;
    return nullptr;
  }
  return m_video.Profiles(count);
}

CfgResult CNetworkTransport::Probe(const FrameCfg& cfg) const
{
  return (m_config.services & TRANSPORT_SERVICE_FRAME) ?
    m_video.Probe(cfg) : CfgResult::REJECTED;
}

CfgResult CNetworkTransport::Prepare(const FrameCfg& cfg)
{
  return (m_config.services & TRANSPORT_SERVICE_FRAME) ?
    m_video.Prepare(cfg) : CfgResult::REJECTED;
}

void CNetworkTransport::Commit()
{
  if (m_config.services & TRANSPORT_SERVICE_FRAME)
    m_video.Commit();
}

void CNetworkTransport::Abort()
{
  if (m_config.services & TRANSPORT_SERVICE_FRAME)
    m_video.Abort();
}

std::shared_ptr<const FrameCaps> CNetworkTransport::GetFrameCaps() const
{
  return (m_config.services & TRANSPORT_SERVICE_FRAME) ?
    m_video.GetFrameCaps() : std::shared_ptr<const FrameCaps>();
}

DirectFrameBufferMemory CNetworkTransport::GetDirectMemory() const
{
  return DirectFrameBufferMemory {};
}

IAudioSource * CNetworkTransport::Audio()
{
  return (m_config.services & TRANSPORT_SERVICE_AUDIO) ? &m_audio : nullptr;
}

IClipboardSource * CNetworkTransport::Clipboard()
{
  return (m_config.services & TRANSPORT_SERVICE_CLIPBOARD) ?
    &m_clipboard : nullptr;
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

ITexSink * CNetworkTransport::TexSink()
{
  return (m_config.services & TRANSPORT_SERVICE_FRAME) ? &m_video : nullptr;
}

bool CNetworkTransport::ServiceEnabled(LGNetService service) const
{
  switch (service)
  {
    case LG_NET_SERVICE_RECOVERY:
    case LG_NET_SERVICE_CURSOR:
    case LG_NET_SERVICE_CONTROL:
      return (m_config.services & TRANSPORT_SERVICE_CONTROL) != 0;

    case LG_NET_SERVICE_INPUT:
      return (m_config.services & TRANSPORT_SERVICE_INPUT) != 0;

    case LG_NET_SERVICE_AUDIO:
      return (m_config.services & TRANSPORT_SERVICE_AUDIO) != 0;

    case LG_NET_SERVICE_CLIPBOARD:
    case LG_NET_SERVICE_FILE:
      return (m_config.services & TRANSPORT_SERVICE_CLIPBOARD) != 0;

    case LG_NET_SERVICE_VIDEO:
      return (m_config.services & TRANSPORT_SERVICE_FRAME) != 0;

    case LG_NET_SERVICE_CORE:
    case LG_NET_SERVICE_USB:
      return false;
  }

  return false;
}

bool CNetworkTransport::ControlSnapshot(
  NetworkControlSnapshot& state, uint64_t knownRevision) const
{
  return m_control.Snapshot(state, knownRevision);
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
