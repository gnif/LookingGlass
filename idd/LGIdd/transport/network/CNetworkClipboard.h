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

#include "transport/IClipboardSource.h"

#include <LGProtocol/NetworkProtocol.h>

#include <memory>
#include <stdint.h>

struct NetworkClipboardPeer
{
  uint32_t client     = 0;
  uint32_t generation = 0;
};

struct NetworkClipboardPeerInfo
{
  uint16_t             clipboardVersion         = 0;
  uint16_t             fileVersion              = 0;
  LGNetCapabilityFlags clipboardFlags           = 0;
  LGNetCapabilityFlags fileFlags                = 0;
  uint32_t             clipboardReliablePayload = 0;
  uint32_t             clipboardBulkPayload     = 0;
  uint32_t             fileReliablePayload      = 0;
  uint32_t             fileBulkPayload          = 0;
};

enum class NetworkClipboardSendResult
{
  ACCEPTED,
  BLOCKED,
  FAILED,
};

enum class NetworkClipboardPacketResult
{
  HANDLED,
  IGNORED,
  INVALID,
};

class INetworkClipboardSender
{
public:
  virtual ~INetworkClipboardSender() = default;

  virtual bool ClipboardPeerInfo(
    const NetworkClipboardPeer& peer, NetworkClipboardPeerInfo& info) = 0;
  virtual NetworkClipboardSendResult SendClipboard(
    const NetworkClipboardPeer& peer, bool bulk, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t componentEpoch,
    uint64_t requestID, const void * payload, uint32_t payloadLength) = 0;
};

class CNetworkClipboard final : public IClipboardSource
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  CNetworkClipboard();
  ~CNetworkClipboard() override;

  CNetworkClipboard(const CNetworkClipboard&)            = delete;
  CNetworkClipboard& operator=(const CNetworkClipboard&) = delete;

  bool AttachSender(INetworkClipboardSender& sender);
  void DetachSender();

  bool PeerConnected(const NetworkClipboardPeer& peer);
  void PeerDisconnected(const NetworkClipboardPeer& peer);
  NetworkClipboardPacketResult HandlePacket(
    const NetworkClipboardPeer& peer, const LGNetPacketView& packet, bool bulk);
  void Process(uint64_t nowMs);
  void SendReady(
    const NetworkClipboardPeer& peer, LGNetService service, bool bulk);

  bool Start(IClipboardTarget& target) override;
  void Stop() override;
  void ClipboardState(bool available, uint32_t generation) override;
  ClipboardChannelResult SendClipboard(
    const KVMFRClipboardMessage& record, const uint8_t * data) override;
  void ClipboardReset(uint32_t generation, uint32_t reason) override;
};
