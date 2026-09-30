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

#ifndef _H_LG_COMMON_NETWORK_PROTOCOL_
#define _H_LG_COMMON_NETWORK_PROTOCOL_

#include "common/network/wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoded as the byte sequence "LGNW" by the little-endian wire writer. */
#define LG_NET_WIRE_MAGIC                     UINT32_C(0x574E474C)
#define LG_NET_WIRE_VERSION_MAJOR             1U
#define LG_NET_WIRE_VERSION_MINOR             0U
#define LG_NET_ENVELOPE_WIRE_SIZE             64U
#define LG_NET_MAX_PAYLOAD_LENGTH             (64U * 1024U * 1024U)
#define LG_NET_MESSAGE_VERSION_CURRENT        1U
#define LG_NET_MESSAGE_VERSION_INITIAL        1U
#define LG_NET_PROTOCOL_VERSION_CURRENT       1U
#define LG_NET_PROTOCOL_VERSION_MIN           1U
#define LG_NET_PROTOCOL_VERSION_MAX           LG_NET_PROTOCOL_VERSION_CURRENT
#define LG_NET_MAX_CAPABILITY_RECORDS         64U
#define LG_NET_HELLO_NONCE_LENGTH             32U
#define LG_NET_PASSWORD_SALT_MAX_LENGTH       64U
#define LG_NET_PASSWORD_CHALLENGE_MAX_LENGTH  64U
#define LG_NET_PASSWORD_NONCE_MAX_LENGTH      64U
#define LG_NET_PASSWORD_PROOF_MAX_LENGTH      64U
/* 19527 is 0x4c47 (ASCII "LG") and is currently IANA-unassigned. */
#define LG_NET_DEFAULT_PORT                   19527U
#define LG_NET_ALPN                           "looking-glass/1"
#define LG_NET_ALPN_LENGTH                    (sizeof(LG_NET_ALPN) - 1U)

#define LG_NET_CORE_VERSION_CURRENT       1U
#define LG_NET_CORE_VERSION_MIN           1U
#define LG_NET_CORE_VERSION_MAX           LG_NET_CORE_VERSION_CURRENT
#define LG_NET_RECOVERY_VERSION_CURRENT   1U
#define LG_NET_RECOVERY_VERSION_MIN       1U
#define LG_NET_RECOVERY_VERSION_MAX       LG_NET_RECOVERY_VERSION_CURRENT
#define LG_NET_VIDEO_VERSION_CURRENT      1U
#define LG_NET_VIDEO_VERSION_MIN          1U
#define LG_NET_VIDEO_VERSION_MAX          LG_NET_VIDEO_VERSION_CURRENT
#define LG_NET_CURSOR_VERSION_CURRENT     1U
#define LG_NET_CURSOR_VERSION_MIN         1U
#define LG_NET_CURSOR_VERSION_MAX         LG_NET_CURSOR_VERSION_CURRENT
#define LG_NET_INPUT_VERSION_CURRENT      1U
#define LG_NET_INPUT_VERSION_MIN          1U
#define LG_NET_INPUT_VERSION_MAX          LG_NET_INPUT_VERSION_CURRENT
#define LG_NET_AUDIO_VERSION_CURRENT      1U
#define LG_NET_AUDIO_VERSION_MIN          1U
#define LG_NET_AUDIO_VERSION_MAX          LG_NET_AUDIO_VERSION_CURRENT
#define LG_NET_CLIPBOARD_VERSION_CURRENT  1U
#define LG_NET_CLIPBOARD_VERSION_MIN      1U
#define LG_NET_CLIPBOARD_VERSION_MAX      LG_NET_CLIPBOARD_VERSION_CURRENT
#define LG_NET_FILE_VERSION_CURRENT       1U
#define LG_NET_FILE_VERSION_MIN           1U
#define LG_NET_FILE_VERSION_MAX           LG_NET_FILE_VERSION_CURRENT
/* Reserved now so a future USB implementation cannot collide with a service. */
#define LG_NET_USB_VERSION_RESERVED       1U

typedef uint16_t LGNetService;

enum
{
  LG_NET_SERVICE_CORE      = 1,
  LG_NET_SERVICE_RECOVERY  = 2,
  LG_NET_SERVICE_VIDEO     = 3,
  LG_NET_SERVICE_CURSOR    = 4,
  LG_NET_SERVICE_INPUT     = 5,
  LG_NET_SERVICE_AUDIO     = 6,
  LG_NET_SERVICE_CLIPBOARD = 7,
  LG_NET_SERVICE_FILE      = 8,
  LG_NET_SERVICE_USB       = 9,
};

typedef uint16_t LGNetCoreMessage;

enum
{
  LG_NET_CORE_MESSAGE_HELLO             = 1,
  LG_NET_CORE_MESSAGE_HELLO_ACK         = 2,
  LG_NET_CORE_MESSAGE_AUTH_CHALLENGE    = 3,
  LG_NET_CORE_MESSAGE_AUTH_RESPONSE     = 4,
  LG_NET_CORE_MESSAGE_AUTH_RESULT       = 5,
  LG_NET_CORE_MESSAGE_CAPABILITY        = 6,
  LG_NET_CORE_MESSAGE_CAPABILITIES_DONE = 7,
  LG_NET_CORE_MESSAGE_SESSION_INFO      = 8,
  LG_NET_CORE_MESSAGE_STATUS            = 9,
  LG_NET_CORE_MESSAGE_PING              = 10,
  LG_NET_CORE_MESSAGE_PONG              = 11,
  LG_NET_CORE_MESSAGE_GOODBYE           = 12,
  LG_NET_CORE_MESSAGE_ERROR             = 13,
};

typedef uint16_t LGNetRecoveryMessage;

enum
{
  LG_NET_RECOVERY_MESSAGE_GET_INFO = 1,
  LG_NET_RECOVERY_MESSAGE_INFO     = 2,
  LG_NET_RECOVERY_MESSAGE_REQUEST  = 3,
  LG_NET_RECOVERY_MESSAGE_STATUS   = 4,
};

typedef uint16_t LGNetVideoMessage;

enum
{
  LG_NET_VIDEO_MESSAGE_SUBSCRIBE        = 1,
  LG_NET_VIDEO_MESSAGE_UNSUBSCRIBE      = 2,
  LG_NET_VIDEO_MESSAGE_STREAM_CONFIG    = 3,
  LG_NET_VIDEO_MESSAGE_FRAME            = 4,
  LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT   = 5,
  LG_NET_VIDEO_MESSAGE_KEYFRAME_REQUEST = 6,
  LG_NET_VIDEO_MESSAGE_SCHEDULE         = 7,
  LG_NET_VIDEO_MESSAGE_FEEDBACK         = 8,
  LG_NET_VIDEO_MESSAGE_STATUS           = 9,
};

typedef uint16_t LGNetCursorMessage;

enum
{
  LG_NET_CURSOR_MESSAGE_STATE     = 1,
  LG_NET_CURSOR_MESSAGE_POSITION  = 2,
  LG_NET_CURSOR_MESSAGE_SHAPE     = 3,
  LG_NET_CURSOR_MESSAGE_TRANSFORM = 4,
  LG_NET_CURSOR_MESSAGE_STATUS    = 5,
};

typedef uint16_t LGNetInputMessage;

enum
{
  LG_NET_INPUT_MESSAGE_CLAIM          = 1,
  LG_NET_INPUT_MESSAGE_CLAIM_RESULT   = 2,
  LG_NET_INPUT_MESSAGE_KEEPALIVE      = 3,
  LG_NET_INPUT_MESSAGE_RELEASE        = 4,
  LG_NET_INPUT_MESSAGE_RESET          = 5,
  LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE = 6,
  LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE = 7,
  LG_NET_INPUT_MESSAGE_KEYBOARD       = 8,
  LG_NET_INPUT_MESSAGE_STATUS         = 9,
  LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS  = 10,
};

typedef uint16_t LGNetAudioMessage;

enum
{
  LG_NET_AUDIO_MESSAGE_SUBSCRIBE       = 1,
  LG_NET_AUDIO_MESSAGE_STATUS          = 2,
  LG_NET_AUDIO_MESSAGE_PLAYBACK_START  = 3,
  LG_NET_AUDIO_MESSAGE_PLAYBACK_STOP   = 4,
  LG_NET_AUDIO_MESSAGE_PLAYBACK_DATA   = 5,
  LG_NET_AUDIO_MESSAGE_PLAYBACK_VOLUME = 6,
  LG_NET_AUDIO_MESSAGE_PLAYBACK_MUTE   = 7,
  LG_NET_AUDIO_MESSAGE_CAPTURE_START   = 8,
  LG_NET_AUDIO_MESSAGE_CAPTURE_STOP    = 9,
  LG_NET_AUDIO_MESSAGE_CAPTURE_DATA    = 10,
  LG_NET_AUDIO_MESSAGE_CAPTURE_VOLUME  = 11,
  LG_NET_AUDIO_MESSAGE_CAPTURE_MUTE    = 12,
  LG_NET_AUDIO_MESSAGE_CLOCK_FEEDBACK  = 13,
  LG_NET_AUDIO_MESSAGE_KEEPALIVE       = 14,
  LG_NET_AUDIO_MESSAGE_RELEASE         = 15,
  LG_NET_AUDIO_MESSAGE_STATE_BARRIER   = 16,
  LG_NET_AUDIO_MESSAGE_STATE_ACK       = 17,
};

typedef uint16_t LGNetClipboardMessage;

enum
{
  LG_NET_CLIPBOARD_MESSAGE_CLAIM        = 1,
  LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT = 2,
  LG_NET_CLIPBOARD_MESSAGE_KEEPALIVE    = 3,
  LG_NET_CLIPBOARD_MESSAGE_RELEASE      = 4,
  LG_NET_CLIPBOARD_MESSAGE_OFFER        = 5,
  LG_NET_CLIPBOARD_MESSAGE_CLEAR        = 6,
  LG_NET_CLIPBOARD_MESSAGE_REQUEST      = 7,
  LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN   = 8,
  LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK   = 9,
  LG_NET_CLIPBOARD_MESSAGE_DATA_END     = 10,
  LG_NET_CLIPBOARD_MESSAGE_DATA_READY   = 11,
  LG_NET_CLIPBOARD_MESSAGE_CANCEL       = 12,
  LG_NET_CLIPBOARD_MESSAGE_STATUS       = 13,
};

typedef uint16_t LGNetFileMessage;

enum
{
  LG_NET_FILE_MESSAGE_OFFER        = 1,
  LG_NET_FILE_MESSAGE_ACQUIRE      = 2,
  LG_NET_FILE_MESSAGE_ACQUIRED     = 3,
  LG_NET_FILE_MESSAGE_RELEASE      = 4,
  LG_NET_FILE_MESSAGE_REQUEST      = 5,
  LG_NET_FILE_MESSAGE_ENTRY        = 6,
  LG_NET_FILE_MESSAGE_DATA_BEGIN   = 7,
  LG_NET_FILE_MESSAGE_DATA_CHUNK   = 8,
  LG_NET_FILE_MESSAGE_DATA_END     = 9,
  LG_NET_FILE_MESSAGE_DATA_READY   = 10,
  LG_NET_FILE_MESSAGE_CANCEL       = 11,
  LG_NET_FILE_MESSAGE_STATUS       = 12,
};

/* Message IDs are reserved for the future USB service; their payloads are not
 * defined by this protocol version. */
typedef uint16_t LGNetUSBMessage;

enum
{
  LG_NET_USB_MESSAGE_CAPABILITIES = 1,
  LG_NET_USB_MESSAGE_DEVICE       = 2,
  LG_NET_USB_MESSAGE_CLAIM        = 3,
  LG_NET_USB_MESSAGE_CONTROL      = 4,
  LG_NET_USB_MESSAGE_ENDPOINT     = 5,
  LG_NET_USB_MESSAGE_CANCEL       = 6,
  LG_NET_USB_MESSAGE_RESET        = 7,
  LG_NET_USB_MESSAGE_STATUS       = 8,
};

typedef uint32_t LGNetEnvelopeFlags;

enum
{
  LG_NET_ENVELOPE_RESPONSE     = 1U << 0,
  LG_NET_ENVELOPE_FINAL        = 1U << 1,
  LG_NET_ENVELOPE_RELIABLE     = 1U << 2,
  LG_NET_ENVELOPE_DATAGRAM     = 1U << 3,
  LG_NET_ENVELOPE_ACK_REQUIRED = 1U << 4,
  LG_NET_ENVELOPE_ERROR        = 1U << 5,
  LG_NET_ENVELOPE_EARLY_DATA   = 1U << 6,
};

typedef struct LGNetEnvelope
{
  uint16_t           wireMajor;
  uint16_t           wireMinor;
  uint16_t           headerSize;
  LGNetService       service;
  uint16_t           messageType;
  uint16_t           serviceVersion;
  uint16_t           messageVersion;
  LGNetEnvelopeFlags flags;
  uint32_t           payloadLength;
  uint64_t           sessionEpoch;
  uint64_t           componentEpoch;
  uint64_t           sequence;
  uint64_t           requestID;
}
LGNetEnvelope;

typedef struct LGNetPacketView
{
  LGNetEnvelope   envelope;
  const uint8_t * payload;
  size_t          payloadLength;
  size_t          wireSize;
}
LGNetPacketView;

typedef uint16_t LGNetRole;

enum
{
  LG_NET_ROLE_CLIENT = 1,
  LG_NET_ROLE_SERVER = 2,
};

typedef uint16_t LGNetAuthMode;

enum
{
  LG_NET_AUTH_NONE     = 0,
  LG_NET_AUTH_PASSWORD = 1,
};

#define LG_NET_AUTH_MODE_FLAG(mode) (UINT32_C(1) << (mode))
#define LG_NET_AUTH_MODE_DEFAULT     LG_NET_AUTH_NONE
#define LG_NET_AUTH_MODES_DEFAULT    LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_NONE)
#define LG_NET_AUTH_MODES_KNOWN      \
  (LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_NONE) | \
   LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_PASSWORD))

typedef struct LGNetHello
{
  LGNetRole role;
  uint16_t  protocolMin;
  uint16_t  protocolMax;
  uint32_t  authModes;
  uint32_t  maxPayloadLength;
  uint32_t  maxDatagramPayload;
  uint32_t  capabilityCount;
  uint8_t   nonce[LG_NET_HELLO_NONCE_LENGTH];
}
LGNetHello;

typedef uint32_t LGNetHelloAckFlags;

enum
{
  LG_NET_HELLO_ACK_RESUMED = 1U << 0,
};

typedef struct LGNetHelloAck
{
  uint16_t           protocolVersion;
  LGNetAuthMode      authMode;
  LGNetHelloAckFlags flags;
  uint32_t           maxPayloadLength;
  uint32_t           maxDatagramPayload;
  uint32_t           capabilityCount;
  uint64_t           sessionEpoch;
  uint8_t            nonce[LG_NET_HELLO_NONCE_LENGTH];
}
LGNetHelloAck;

#define LG_NET_HELLO_WIRE_SIZE      56U
#define LG_NET_HELLO_ACK_WIRE_SIZE  64U

typedef uint16_t LGNetCapabilityFlags;

enum
{
  LG_NET_CAPABILITY_REQUIRED     = 1U << 0,
  LG_NET_CAPABILITY_SELECTED     = 1U << 1,
  LG_NET_CAPABILITY_RELIABLE     = 1U << 2,
  LG_NET_CAPABILITY_DATAGRAM     = 1U << 3,
  LG_NET_CAPABILITY_BULK         = 1U << 4,
  LG_NET_CAPABILITY_MULTI_CLIENT = 1U << 5,
};

typedef struct LGNetCapability
{
  LGNetService         service;
  uint16_t             versionMin;
  uint16_t             versionMax;
  LGNetCapabilityFlags flags;
  uint64_t             features;
  uint32_t             maxPayloadLength;
  uint32_t             maxDatagramPayload;
  uint16_t             maxBidirectionalStreams;
  uint16_t             maxUnidirectionalStreams;
}
LGNetCapability;

#define LG_NET_CAPABILITY_WIRE_SIZE 32U

/* Password proofs are carried by this library but derived by the application.
 * A password must never be sent on the wire. The negotiated algorithm defines
 * the proof and transcript binding; cryptography is outside this wire codec. */
typedef uint16_t LGNetPasswordAlgorithm;

enum
{
  LG_NET_PASSWORD_PBKDF2_HMAC_SHA256 = 1,
};

typedef struct LGNetPasswordChallenge
{
  LGNetPasswordAlgorithm algorithm;
  uint32_t               rounds;
  uint16_t               saltLength;
  uint16_t               challengeLength;
  uint8_t                salt     [LG_NET_PASSWORD_SALT_MAX_LENGTH];
  uint8_t                challenge[LG_NET_PASSWORD_CHALLENGE_MAX_LENGTH];
}
LGNetPasswordChallenge;

typedef struct LGNetPasswordResponse
{
  LGNetPasswordAlgorithm algorithm;
  uint16_t               nonceLength;
  uint16_t               proofLength;
  uint8_t                nonce[LG_NET_PASSWORD_NONCE_MAX_LENGTH];
  uint8_t                proof[LG_NET_PASSWORD_PROOF_MAX_LENGTH];
}
LGNetPasswordResponse;

typedef uint16_t LGNetAuthResultCode;

enum
{
  LG_NET_AUTH_RESULT_ACCEPTED    = 1,
  LG_NET_AUTH_RESULT_REJECTED    = 2,
  LG_NET_AUTH_RESULT_UNSUPPORTED = 3,
  LG_NET_AUTH_RESULT_ERROR       = 4,
};

typedef struct LGNetAuthResult
{
  LGNetAuthResultCode result;
  uint16_t            proofLength;
  uint32_t            retryAfterMs;
  uint8_t             proof[LG_NET_PASSWORD_PROOF_MAX_LENGTH];
}
LGNetAuthResult;

#define LG_NET_PASSWORD_CHALLENGE_HEADER_SIZE 16U
#define LG_NET_PASSWORD_RESPONSE_HEADER_SIZE  12U
#define LG_NET_AUTH_RESULT_HEADER_SIZE        12U

void lgNetEnvelopeInit(LGNetEnvelope * envelope, LGNetService service,
  uint16_t messageType, uint16_t serviceVersion,
  uint16_t messageVersion);
bool lgNetEnvelopeEncode(void * data, size_t size,
  const LGNetEnvelope * envelope);
LGNetParseResult lgNetEnvelopeDecode(LGNetEnvelope * envelope,
  const void * data, size_t size);
bool lgNetPacketSize(const LGNetEnvelope * envelope, size_t * size);
bool lgNetPacketEncode(void * data, size_t size,
  const LGNetEnvelope * envelope, const void * payload);
LGNetParseResult lgNetPacketDecode(LGNetPacketView * packet,
  const void * data, size_t size);

bool lgNetServiceKnown(LGNetService service);
bool lgNetMessageKnown(LGNetService service, uint16_t messageType);

void lgNetHelloInit(LGNetHello * hello, LGNetRole role);
bool lgNetHelloValid(const LGNetHello * hello);
bool lgNetHelloEncode(void * data, size_t size, const LGNetHello * hello);
LGNetParseResult lgNetHelloDecode(LGNetHello * hello,
  const void * data, size_t size);

void lgNetHelloAckInit(LGNetHelloAck * hello, uint64_t sessionEpoch);
bool lgNetHelloAckValid(const LGNetHelloAck * hello);
bool lgNetHelloAckEncode(
  void * data, size_t size, const LGNetHelloAck * hello);
LGNetParseResult lgNetHelloAckDecode(LGNetHelloAck * hello,
  const void * data, size_t size);

bool lgNetCapabilityValid(const LGNetCapability * capability);
bool lgNetCapabilityEncode(void * data, size_t size,
  const LGNetCapability * capability);
LGNetParseResult lgNetCapabilityDecode(LGNetCapability * capability,
  const void * data, size_t size);

size_t lgNetPasswordChallengeSize(
  const LGNetPasswordChallenge * challenge);
bool lgNetPasswordChallengeValid(
  const LGNetPasswordChallenge * challenge);
bool lgNetPasswordChallengeEncode(void * data, size_t size,
  const LGNetPasswordChallenge * challenge);
LGNetParseResult lgNetPasswordChallengeDecode(
  LGNetPasswordChallenge * challenge, const void * data, size_t size);

size_t lgNetPasswordResponseSize(const LGNetPasswordResponse * response);
bool lgNetPasswordResponseValid(const LGNetPasswordResponse * response);
bool lgNetPasswordResponseEncode(void * data, size_t size,
  const LGNetPasswordResponse * response);
LGNetParseResult lgNetPasswordResponseDecode(
  LGNetPasswordResponse * response, const void * data, size_t size);

size_t lgNetAuthResultSize(const LGNetAuthResult * result);
bool lgNetAuthResultValid(const LGNetAuthResult * result);
bool lgNetAuthResultEncode(
  void * data, size_t size, const LGNetAuthResult * result);
LGNetParseResult lgNetAuthResultDecode(LGNetAuthResult * result,
  const void * data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
