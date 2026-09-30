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

#pragma once

#include "CSRWLock.h"
#include "transport/ITransport.h"
#include "transport/network/CNetworkSession.h"
#include "transport/network/NetworkSettings.h"

#include <memory>
#include <vector>

class CNetworkServer;

// This provider owns transport-independent lifecycle and session state. It
// deliberately advertises no frame capabilities and no texture sink until a
// GPU codec backend is attached; it must not be mistaken for working media.
class CNetworkTransport final : public ITransport
{
private:
  using SessionPtr = std::shared_ptr<CNetworkSession>;

  enum class State
  {
    CONSTRUCTED,
    OPEN,
    INITIALIZED,
    READY,
    STOPPED,
  };

  TransportInstance               m_config;
  NetworkSettings                 m_settings;
  std::unique_ptr<CNetworkServer> m_server;

  mutable CSRWLock  m_lifecycleLock;
  State             m_state     = State::CONSTRUCTED;
  size_t            m_alignment = 0;

  CNetworkEventQueue  m_events;
  CNetworkInputSource m_input;
  CNetworkControlSink m_control;

  mutable CSRWLock        m_sessionLock;
  std::vector<SessionPtr> m_sessions;

  std::shared_ptr<CNetworkSession> FindSession(
    const SourceKey& source) const;

public:
  explicit CNetworkTransport(const TransportInstance& config);
  ~CNetworkTransport() override;

  CNetworkTransport(const CNetworkTransport&) = delete;
  CNetworkTransport& operator=(const CNetworkTransport&) = delete;

  OpenResult Open() override;
  bool Initialize() override;
  bool Setup(size_t alignment) override;
  ProcessResult Process(ITransportEvents& events) override;
  void Stop() override;
  void RecoveryStatus(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active,
    Recovery state, uint32_t error) override;

  const FrameProfile * Profiles(unsigned& count) const override;
  CfgResult Probe(const FrameCfg& cfg) const override;
  CfgResult Prepare(const FrameCfg& cfg) override;
  void Commit() override;
  void Abort() override;

  std::shared_ptr<const FrameCaps> GetFrameCaps() const override;
  DirectFrameBufferMemory GetDirectMemory() const override;
  IControlSink * Control() override;
  IInputSource * Input() override;

  const NetworkSettings& Settings() const { return m_settings; }
  bool ServiceEnabled(LGNetService service) const;
  bool ControlSnapshot(
    NetworkControlSnapshot& state, uint64_t knownRevision) const;

  // The QUIC provider calls these only after authenticating a peer. Keeping
  // the registry here makes Stop a barrier for session-originated callbacks.
  std::shared_ptr<CNetworkSession> CreateSession(
    uint32_t client, uint32_t generation);
  void RemoveSession(uint32_t client, uint32_t generation);
};
