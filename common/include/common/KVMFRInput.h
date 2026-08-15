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

#ifndef _H_LG_COMMON_KVMFR_INPUT_
#define _H_LG_COMMON_KVMFR_INPUT_

#pragma once

#include "KVMFRStream.h"

#include <stddef.h>
#include <stdint.h>

#define KVMFR_INPUT_VERSION                 2
#define KVMFR_INPUT_STREAM_VERSION          1
#define KVMFR_INPUT_STREAM_ENDPOINT_COUNT   8
#define KVMFR_INPUT_STREAM_SLOT_COUNT       128
#define KVMFR_INPUT_STREAM_SLOT_SIZE        64
#define KVMFR_INPUT_MOUSE_BUTTON_COUNT      32
#define KVMFR_INPUT_MOUSE_ABSOLUTE_MAX      32767
#define KVMFR_INPUT_KEYBOARD_KEY_COUNT      6
#define KVMFR_INPUT_KEYBOARD_USAGE_MAX      0xe7

typedef uint32_t KVMFRInputMouseButtons;

/*
 * Bits 0..31 correspond to USB HID mouse button usages 1..32. Wheel motion
 * is carried separately and must not be represented as a button.
 */
typedef struct KVMFRInputMouseRelative
{
  KVMFRInputMouseButtons buttons;
  int32_t                deltaX;
  int32_t                deltaY;
  int32_t                wheel;
}
KVMFRInputMouseRelative;

typedef struct KVMFRInputMouseAbsolute
{
  KVMFRInputMouseButtons buttons;
  uint16_t               x;
  uint16_t               y;
  int32_t                wheel;
  uint32_t               reserved;
}
KVMFRInputMouseAbsolute;

typedef struct KVMFRInputKeyboard
{
  uint8_t modifiers;
  uint8_t keys[KVMFR_INPUT_KEYBOARD_KEY_COUNT];
  uint8_t reserved[9];
}
KVMFRInputKeyboard;

typedef union KVMFRInputPayload
{
  KVMFRInputMouseRelative mouseRelative;
  KVMFRInputMouseAbsolute mouseAbsolute;
  KVMFRInputKeyboard      keyboard;
  uint8_t                 reserved[16];
}
KVMFRInputPayload;

enum
{
  KVMFR_INPUT_MESSAGE_CLAIM          = 1,
  KVMFR_INPUT_MESSAGE_RELEASE        = 2,
  KVMFR_INPUT_MESSAGE_KEEPALIVE      = 3,
  KVMFR_INPUT_MESSAGE_RESET          = 4,
  KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE = 5,
  KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE = 6,
  KVMFR_INPUT_MESSAGE_KEYBOARD       = 7
};

typedef uint32_t KVMFRInputMessageType;

/*
 * One activation must remain on one ordered transport. CLAIM is sequence one,
 * reports, KEEPALIVE and RESET increment the sequence, and RELEASE is the
 * final record. A sender must start a new generation instead of moving an
 * active sequence between the queue fallback and a stream endpoint.
 */
typedef struct KVMFRInputMessage
{
  KVMFRInputMessageType type;
  // Non-zero client input activation generation.
  uint32_t              generation;
  // Non-zero ordered sequence within generation, beginning at one.
  uint32_t              sequence;
  // Must be zero.
  uint32_t              reserved;
  KVMFRInputPayload     payload;
}
KVMFRInputMessage;

enum
{
  KVMFR_INPUT_CAP_MOUSE_RELATIVE = 0x1,
  KVMFR_INPUT_CAP_MOUSE_ABSOLUTE = 0x2,
  KVMFR_INPUT_CAP_KEYBOARD       = 0x4
};

typedef uint32_t KVMFRInputCapabilityFlags;

enum
{
  KVMFR_INPUT_STATUS_AVAILABLE = 0x1,
  KVMFR_INPUT_STATUS_HAS_OWNER = 0x2,
};

typedef uint32_t KVMFRInputStatusFlags;

enum
{
  KVMFR_INPUT_TRANSPORT_QUEUE  = 0x1,
  KVMFR_INPUT_TRANSPORT_STREAM = 0x2,
};

typedef uint32_t KVMFRInputTransportFlags;

enum
{
  KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE = 0x1,
  KVMFR_INPUT_STREAM_ENDPOINT_BOUND     = 0x2,
};

typedef uint32_t KVMFRInputStreamEndpointFlags;

/*
 * A bound endpoint is usable only by boundClientID for bindingGeneration. The
 * host changes bindingGeneration before reusing an endpoint so stale writers
 * can never be accepted as a new client.
 */
typedef struct KVMFRInputStreamEndpoint
{
  KVMFRStreamDescriptor         stream;
  uint32_t                      boundClientID;
  uint32_t                      bindingGeneration;
  KVMFRInputStreamEndpointFlags flags;
  uint32_t                      reserved;
}
KVMFRInputStreamEndpoint;

typedef struct KVMFRInputStatus
{
  uint32_t                  version;
  KVMFRInputCapabilityFlags capabilities;
  KVMFRInputStatusFlags     flags;
  uint32_t                  generation;
  uint32_t                  ownerClientID;
  uint32_t                  ownerGeneration;
  // Input ownership lease duration in milliseconds.
  uint32_t                  lease;
  uint32_t                  maxButtons;
  KVMFRInputTransportFlags  transports;
  uint32_t                  streamVersion;
  uint32_t                  streamEndpointCount;
  uint32_t                  streamGeneration;
  uint32_t                  streamReserved[4];
  KVMFRInputStreamEndpoint  streamEndpoint[
    KVMFR_INPUT_STREAM_ENDPOINT_COUNT];
}
KVMFRInputStatus;

#if defined(__cplusplus)
static_assert(sizeof(KVMFRInputMouseRelative) == 16,
    "KVMFR relative mouse input layout changed");
static_assert(sizeof(KVMFRInputMouseAbsolute) == 16,
    "KVMFR absolute mouse input layout changed");
static_assert(sizeof(KVMFRInputKeyboard) == 16,
    "KVMFR keyboard input layout changed");
static_assert(sizeof(KVMFRInputPayload) == 16,
    "KVMFR input payload layout changed");
static_assert(offsetof(KVMFRInputMessage, payload) == 16,
    "KVMFR input message header layout changed");
static_assert(sizeof(KVMFRInputMessage) == 32,
    "KVMFR input message layout changed");
static_assert(sizeof(KVMFRInputStreamEndpoint) == 48,
    "KVMFR input stream endpoint layout changed");
static_assert(offsetof(KVMFRInputStatus, transports) == 32,
    "KVMFR input status transport layout changed");
static_assert(offsetof(KVMFRInputStatus, streamEndpoint) == 64,
    "KVMFR input stream discovery layout changed");
static_assert(sizeof(KVMFRInputStatus) == 448,
    "KVMFR input status layout changed");
static_assert(sizeof(KVMFRInputMessage) <= 64,
    "KVMFR input message must fit in one LGMP control message");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(KVMFRInputMouseRelative) == 16,
    "KVMFR relative mouse input layout changed");
_Static_assert(sizeof(KVMFRInputMouseAbsolute) == 16,
    "KVMFR absolute mouse input layout changed");
_Static_assert(sizeof(KVMFRInputKeyboard) == 16,
    "KVMFR keyboard input layout changed");
_Static_assert(sizeof(KVMFRInputPayload) == 16,
    "KVMFR input payload layout changed");
_Static_assert(offsetof(KVMFRInputMessage, payload) == 16,
    "KVMFR input message header layout changed");
_Static_assert(sizeof(KVMFRInputMessage) == 32,
    "KVMFR input message layout changed");
_Static_assert(sizeof(KVMFRInputStreamEndpoint) == 48,
    "KVMFR input stream endpoint layout changed");
_Static_assert(offsetof(KVMFRInputStatus, transports) == 32,
    "KVMFR input status transport layout changed");
_Static_assert(offsetof(KVMFRInputStatus, streamEndpoint) == 64,
    "KVMFR input stream discovery layout changed");
_Static_assert(sizeof(KVMFRInputStatus) == 448,
    "KVMFR input status layout changed");
_Static_assert(sizeof(KVMFRInputMessage) <= 64,
    "KVMFR input message must fit in one LGMP control message");
#endif

#endif
