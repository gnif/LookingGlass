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

#include "interface/input.h"

#include <LGProtocol/NetworkProtocol.h>

#include <stdbool.h>

typedef struct NetworkInput NetworkInput;

bool networkInputCreate(NetworkInput ** input,
    NetworkConnection * connection);
void networkInputDestroy(NetworkInput ** input);

const LG_InputOps * networkInputGetOps(void);

void networkInputConnectionState(NetworkInput * input, bool connected);
void networkInputSendReady(NetworkInput * input,
    NetworkPacketDelivery delivery);
void networkInputDatagramLost(NetworkInput * input,
    uint64_t claimEpoch, uint64_t pointerSequence);
bool networkInputPacket(NetworkInput * input,
    const LGNetPacketView * packet, bool datagram);
