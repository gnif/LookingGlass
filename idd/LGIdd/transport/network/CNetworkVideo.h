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

#include "transport/FrameCaps.h"
#include "transport/ITexSink.h"

#include <LGProtocol/NetworkProtocol.h>
#include <LGProtocol/NetworkServices.h>

#include <memory>
#include <stdint.h>
#include <vector>

struct NetworkVideoPeer
{
  uint32_t client     = 0;
  uint32_t generation = 0;
};

struct NetworkVideoPeerInfo
{
  uint16_t             serviceVersion     = 0;
  LGNetCapabilityFlags capabilityFlags    = 0;
  uint32_t             maxReliablePayload = 0;
  uint32_t             maxDatagramPayload = 0;
  uint32_t             maxBulkPayload     = 0;
};

enum class NetworkVideoDelivery
{
  RELIABLE,
  DATAGRAM,
  BULK,
};

enum class NetworkVideoSendResult
{
  ACCEPTED,
  BLOCKED,
  FAILED,
};

enum class NetworkVideoPacketResult
{
  HANDLED,
  IGNORED,
  INVALID,
};

class INetworkVideoSender
{
public:
  virtual ~INetworkVideoSender() = default;

  virtual bool VideoPeerInfo(
    const NetworkVideoPeer& peer, NetworkVideoPeerInfo& info) = 0;
  virtual NetworkVideoSendResult SendVideo(
    const NetworkVideoPeer& peer, NetworkVideoDelivery delivery,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    std::shared_ptr<const std::vector<uint8_t>> payload) = 0;
};

class CNetworkVideo final : public ITexSink
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  CNetworkVideo();
  ~CNetworkVideo() override;

  CNetworkVideo(const CNetworkVideo&)            = delete;
  CNetworkVideo& operator=(const CNetworkVideo&) = delete;

  bool AttachSender(INetworkVideoSender& sender);
  void DetachSender();

  bool Start(uint32_t videoBitrateMbps,
    LGNetVideoChromaSubsampling videoChroma);
  void Stop();
  void Process(uint64_t nowMs);

  bool PeerConnected(const NetworkVideoPeer& peer);
  void PeerDisconnected(const NetworkVideoPeer& peer);
  NetworkVideoPacketResult HandlePacket(const NetworkVideoPeer& peer,
    const LGNetPacketView& packet, NetworkVideoDelivery delivery);
  void SendReady(
    const NetworkVideoPeer& peer, NetworkVideoDelivery delivery);

  const FrameProfile * Profiles(unsigned& count) const;
  std::shared_ptr<const FrameCaps> GetFrameCaps() const;
  CfgResult Probe(const FrameCfg& cfg) const;
  CfgResult Prepare(const FrameCfg& cfg);
  void Commit();
  void Abort();

  PushResult Push(FrameIn frame, TexLease lease) noexcept override;
  PushResult Push(FrameIn frame, D11Lease lease) noexcept override;
};
