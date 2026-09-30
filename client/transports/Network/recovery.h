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

#include "connection.h"

#include "interface/transport.h"

#include <LGProtocol/NetworkProtocol.h>

#include <stdbool.h>

typedef struct NetworkRecovery NetworkRecovery;

bool networkRecoveryCreate(NetworkRecovery ** recovery,
    NetworkConnection * connection);
void networkRecoveryDestroy(NetworkRecovery ** recovery);

void networkRecoveryConnectionState(NetworkRecovery * recovery,
    bool connected);
bool networkRecoveryPacket(NetworkRecovery * recovery,
    const LGNetPacketView * packet, bool datagram);

LG_TransportStatus networkRecoveryGetInfo(NetworkRecovery * recovery,
    LG_RecoveryInfo * info);
LG_TransportStatus networkRecoveryRequest(NetworkRecovery * recovery,
    LG_RecoveryRequest request, uint64_t * instance, uint32_t * serial);
