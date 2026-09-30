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

typedef void (*NetworkTrustDecisionFn)(void * opaque, bool trusted);
typedef bool (*NetworkTrustPromptFn)(void * opaque, const char * endpoint,
    const char * fingerprint, NetworkTrustDecisionFn decision,
    void * decisionOpaque);
typedef void (*NetworkTrustCancelFn)(void * opaque);
/* The packet view and payload are borrowed for the duration of the callback. */
typedef void (*NetworkPacketFn)(void * opaque,
    const LGNetPacketView * packet, bool datagram);
typedef void (*NetworkConnectionStateFn)(void * opaque, bool connected,
    LG_TransportStatus reason);

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

LG_TransportStatus networkConnectionSend(NetworkConnection * connection,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength);
