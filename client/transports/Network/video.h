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
#include <stdint.h>

typedef struct NetworkVideo NetworkVideo;

typedef void (*NetworkVideoStatusFn)(void * opaque,
    const LG_VideoComponentStatus * status);

bool networkVideoCreate(NetworkVideo ** video,
    NetworkConnection * connection);
void networkVideoDestroy(NetworkVideo ** video);

void networkVideoConnectionState(NetworkVideo * video, bool connected);
void networkVideoProcess(NetworkVideo * video);
void networkVideoSendReady(NetworkVideo * video, bool datagram);

bool networkVideoPacket(NetworkVideo * video,
    const LGNetPacketView * packet);
bool networkVideoDatagram(NetworkVideo * video,
    const LGNetPacketView * packet);

void networkVideoSetStatusListener(NetworkVideo * video,
    NetworkVideoStatusFn callback, void * opaque);
LG_VideoComponentStatus networkVideoGetStatus(NetworkVideo * video);

bool networkVideoAttachRenderer(NetworkVideo * video,
    const LG_RendererInterop * interop);
void networkVideoDetachRenderer(NetworkVideo * video);

LG_TransportStatus networkVideoNextFrame(NetworkVideo * video, bool useDMA,
    LG_TransportFrame * frame);
void networkVideoGetFrameTiming(NetworkVideo * video,
    const LG_TransportFrame * frame, LG_TransportFrameTiming * timing);
void networkVideoReleaseFrame(NetworkVideo * video,
    LG_TransportFrame * frame);
void networkVideoCancelFrameWait(NetworkVideo * video);
void networkVideoStopFrame(NetworkVideo * video);
