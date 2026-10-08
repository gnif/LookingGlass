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

#include "interface/external_frame.h"
#include "interface/renderer.h"
#include "interface/transport.h"

#include <LGProtocol/NetworkServices.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct NetworkVideoDecoder NetworkVideoDecoder;

typedef enum NetworkVideoDecodeResult
{
  NETWORK_VIDEO_DECODE_OK,
  NETWORK_VIDEO_DECODE_NO_FRAME,
  NETWORK_VIDEO_DECODE_UNSUPPORTED,
  NETWORK_VIDEO_DECODE_CORRUPT,
  NETWORK_VIDEO_DECODE_ERROR,
}
NetworkVideoDecodeResult;

typedef struct NetworkVideoDecodedFrame
{
  LG_ExternalFrame         * externalFrame;
  LG_TransportFrameFormat    format;
  LG_TransportFrameTiming    timing;
  KVMFRFrameDamageRect       damageRects[LG_TRANSPORT_MAX_DAMAGE_RECTS];
  uint32_t                   damageRectsCount;
  uint64_t                   decodeTimeNs;
}
NetworkVideoDecodedFrame;

bool networkVideoDecoderCreate(NetworkVideoDecoder ** decoder);
void networkVideoDecoderDestroy(NetworkVideoDecoder ** decoder);

bool networkVideoDecoderAttach(NetworkVideoDecoder * decoder,
    const LG_RendererInterop * interop);
void networkVideoDecoderDetach(NetworkVideoDecoder * decoder);
void networkVideoDecoderReset(NetworkVideoDecoder * decoder);

NetworkVideoDecodeResult networkVideoDecoderConfigure(
    NetworkVideoDecoder * decoder, const LGNetVideoStreamConfig * config);
NetworkVideoDecodeResult networkVideoDecoderDecode(
    NetworkVideoDecoder * decoder, const LGNetVideoStreamConfig * config,
    const void * data, size_t size, uint32_t blockCount,
    bool update, bool complete, uint64_t baseFrameID, uint64_t frameID,
    NetworkVideoDecodedFrame * frame);

void networkVideoDecoderRelease(NetworkVideoDecodedFrame * frame);
