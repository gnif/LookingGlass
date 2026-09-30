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

#include "transport/IAudioSource.h"

#include <LGProtocol/NetworkProtocol.h>

#include <memory>
#include <stdint.h>

struct NetworkAudioPeer
{
  uint32_t client     = 0;
  uint32_t generation = 0;
};

struct NetworkAudioPeerInfo
{
  uint16_t             serviceVersion       = 0;
  LGNetCapabilityFlags capabilityFlags      = 0;
  uint32_t             maxReliablePayload   = 0;
  uint32_t             maxDatagramPayload   = 0;
};

enum class NetworkAudioSendResult
{
  ACCEPTED,
  BLOCKED,
  FAILED,
};

enum class NetworkAudioPacketResult
{
  HANDLED,
  IGNORED,
  INVALID,
};

class INetworkAudioSender
{
public:
  virtual ~INetworkAudioSender() = default;

  virtual bool AudioPeerInfo(const NetworkAudioPeer& peer,
    NetworkAudioPeerInfo& info) = 0;
  virtual NetworkAudioSendResult SendAudio(
    const NetworkAudioPeer& peer, bool datagram, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength) = 0;
};

class CNetworkAudio final : public IAudioSource
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  CNetworkAudio();
  ~CNetworkAudio() override;

  CNetworkAudio(const CNetworkAudio&) = delete;
  CNetworkAudio& operator=(const CNetworkAudio&) = delete;

  bool AttachSender(INetworkAudioSender& sender);
  void DetachSender();

  bool PeerConnected(const NetworkAudioPeer& peer);
  void PeerDisconnected(const NetworkAudioPeer& peer);
  NetworkAudioPacketResult HandlePacket(const NetworkAudioPeer& peer,
    const LGNetPacketView& packet, bool datagram);
  void Process(uint64_t nowMs);
  void SendReady(const NetworkAudioPeer& peer,
    LGNetService service, bool datagram);

  bool Start(IAudioTarget& target) override;
  void Stop() override;
  void AudioState(const AudioEndpointStatus& state) override;
  AudioResult SendPlayback(const AudioPacket& packet,
    const uint8_t * data, size_t size) override;
  void CaptureReceiveReady() override;
};
