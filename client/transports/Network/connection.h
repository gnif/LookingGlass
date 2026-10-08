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

#include "known_hosts.h"

#include "interface/transport.h"

#include <LGProtocol/NetworkProtocol.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct NetworkConnection NetworkConnection;

typedef enum NetworkPacketDelivery
{
  NETWORK_PACKET_DELIVERY_RELIABLE,
  NETWORK_PACKET_DELIVERY_DATAGRAM,
  NETWORK_PACKET_DELIVERY_BULK,
}
NetworkPacketDelivery;

typedef void (*NetworkTrustDecisionFn)(void * opaque, bool trusted);
typedef bool (*NetworkTrustPromptFn)(void * opaque, const char * endpoint,
    const char * fingerprint, NetworkTrustDecisionFn decision,
    void * decisionOpaque);
typedef void (*NetworkTrustCancelFn)(void * opaque);
/* The packet view and payload are borrowed for the duration of the callback. */
typedef void (*NetworkPacketFn)(void * opaque,
    const LGNetPacketView * packet, NetworkPacketDelivery delivery);
typedef void (*NetworkConnectionStateFn)(void * opaque, bool connected,
    LG_TransportStatus reason);
typedef void (*NetworkSendReadyFn)(void * opaque, LGNetService service,
    NetworkPacketDelivery delivery);
typedef void (*NetworkDatagramLostFn)(void * opaque, LGNetService service,
    uint64_t componentEpoch, uint64_t applicationSequence);

typedef struct NetworkConnectionParams
{
  const char               * address;
  uint16_t                   port;
  const char               * password;
  NetworkKnownHosts        * knownHosts;
  NetworkTrustPromptFn       trustPrompt;
  NetworkTrustCancelFn       trustCancel;
  NetworkPacketFn            packet;
  NetworkConnectionStateFn   state;
  NetworkSendReadyFn         sendReady;
  NetworkDatagramLostFn      datagramLost;
  const LGNetCapability    * capabilities;
  uint32_t                   capabilityCount;
  void                     * opaque;
}
NetworkConnectionParams;

bool networkConnectionCreate(NetworkConnection ** connection,
    const NetworkConnectionParams * params);
void networkConnectionDestroy(NetworkConnection ** connection);

LG_TransportStatus networkConnectionConnect(NetworkConnection * connection,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * cancelledOpaque);
void networkConnectionCancel(NetworkConnection * connection);
void networkConnectionDisconnect(NetworkConnection * connection);
bool networkConnectionValid(const NetworkConnection * connection);
bool networkConnectionServiceAvailable(NetworkConnection * connection,
    LGNetService service, LGNetCapabilityFlags required);
bool networkConnectionServiceVersion(NetworkConnection * connection,
    LGNetService service, uint16_t * version);
bool networkConnectionAwaitInputSend(NetworkConnection * connection,
    NetworkPacketDelivery delivery);

LG_TransportStatus networkConnectionSend(NetworkConnection * connection,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength);
LG_TransportStatus networkConnectionSendDatagram(
    NetworkConnection * connection, LGNetService service,
    uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength,
    uint64_t applicationSequence, bool discardIfBlocked);
LG_TransportStatus networkConnectionSendBulk(NetworkConnection * connection,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength);
