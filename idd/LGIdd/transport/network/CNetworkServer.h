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

#include "transport/network/CNetworkAudio.h"
#include "transport/network/CNetworkClipboard.h"
#include "transport/network/CNetworkVideo.h"

#include <memory>

class CNetworkTransport;
struct NetworkSettings;

class CNetworkServer final :
  public INetworkAudioSender,
  public INetworkClipboardSender,
  public INetworkVideoSender
{
private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

public:
  CNetworkServer(CNetworkTransport& transport, CNetworkAudio& audio,
    CNetworkClipboard& clipboard, CNetworkVideo& video,
    const NetworkSettings& settings);
  ~CNetworkServer();

  CNetworkServer(const CNetworkServer&) = delete;
  CNetworkServer& operator=(const CNetworkServer&) = delete;

  bool Start();
  void Stop();
  void Process();

  bool AudioPeerInfo(const NetworkAudioPeer& peer,
    NetworkAudioPeerInfo& info) override;
  NetworkAudioSendResult SendAudio(
    const NetworkAudioPeer& peer, bool datagram, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength) override;

  bool ClipboardPeerInfo(const NetworkClipboardPeer& peer,
    NetworkClipboardPeerInfo& info) override;
  NetworkClipboardSendResult SendClipboard(
    const NetworkClipboardPeer& peer, bool bulk, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength) override;

  bool VideoPeerInfo(const NetworkVideoPeer& peer,
    NetworkVideoPeerInfo& info) override;
  NetworkVideoSendResult SendVideo(
    const NetworkVideoPeer& peer, NetworkVideoDelivery delivery,
    uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength) override;
};
