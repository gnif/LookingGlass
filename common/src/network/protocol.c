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

#include "common/network/protocol.h"

#include <limits.h>
#include <string.h>

static const LGNetEnvelopeFlags ENVELOPE_FLAGS =
  LG_NET_ENVELOPE_RESPONSE     |
  LG_NET_ENVELOPE_FINAL        |
  LG_NET_ENVELOPE_RELIABLE     |
  LG_NET_ENVELOPE_DATAGRAM     |
  LG_NET_ENVELOPE_ACK_REQUIRED |
  LG_NET_ENVELOPE_ERROR        |
  LG_NET_ENVELOPE_EARLY_DATA;

static const LGNetCapabilityFlags CAPABILITY_FLAGS =
  LG_NET_CAPABILITY_REQUIRED     |
  LG_NET_CAPABILITY_SELECTED     |
  LG_NET_CAPABILITY_RELIABLE     |
  LG_NET_CAPABILITY_DATAGRAM     |
  LG_NET_CAPABILITY_BULK         |
  LG_NET_CAPABILITY_MULTI_CLIENT;

static bool envelopeValid(const LGNetEnvelope * envelope, bool encode)
{
  if (!envelope || envelope->wireMajor != LG_NET_WIRE_VERSION_MAJOR ||
      envelope->headerSize < LG_NET_ENVELOPE_WIRE_SIZE ||
      (encode && envelope->wireMinor != LG_NET_WIRE_VERSION_MINOR) ||
      (encode && envelope->headerSize != LG_NET_ENVELOPE_WIRE_SIZE) ||
      !envelope->service || !envelope->messageType ||
      !envelope->serviceVersion || !envelope->messageVersion ||
      envelope->payloadLength > LG_NET_MAX_PAYLOAD_LENGTH)
    return false;

  return true;
}

static bool authModeKnown(LGNetAuthMode mode)
{
  return mode == LG_NET_AUTH_NONE || mode == LG_NET_AUTH_PASSWORD;
}

static bool passwordAlgorithmKnown(LGNetPasswordAlgorithm algorithm)
{
  return algorithm == LG_NET_PASSWORD_PBKDF2_HMAC_SHA256;
}

static LGNetParseResult fixedDecodeResult(
    const LGNetReader * reader, size_t expected, size_t actual)
{
  if (!reader || !lgNetReaderValid(reader))
    return LG_NET_PARSE_INVALID_VALUE;
  if (actual != expected || lgNetReaderConsumed(reader) != expected)
    return LG_NET_PARSE_INVALID_LENGTH;
  return LG_NET_PARSE_OK;
}

void lgNetEnvelopeInit(LGNetEnvelope * envelope, LGNetService service,
    uint16_t messageType, uint16_t serviceVersion,
    uint16_t messageVersion)
{
  if (!envelope)
    return;

  memset(envelope, 0, sizeof(*envelope));
  envelope->wireMajor      = LG_NET_WIRE_VERSION_MAJOR;
  envelope->wireMinor      = LG_NET_WIRE_VERSION_MINOR;
  envelope->headerSize     = LG_NET_ENVELOPE_WIRE_SIZE;
  envelope->service        = service;
  envelope->messageType    = messageType;
  envelope->serviceVersion = serviceVersion;
  envelope->messageVersion = messageVersion;
}

bool lgNetEnvelopeEncode(void * data, size_t size,
    const LGNetEnvelope * envelope)
{
  if (!data || size < LG_NET_ENVELOPE_WIRE_SIZE ||
      !envelopeValid(envelope, true) ||
      envelope->flags & ~ENVELOPE_FLAGS)
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, LG_NET_WIRE_MAGIC)          &&
    lgNetWriterU16(&writer, envelope->wireMajor)        &&
    lgNetWriterU16(&writer, envelope->wireMinor)        &&
    lgNetWriterU16(&writer, envelope->headerSize)       &&
    lgNetWriterU16(&writer, envelope->service)          &&
    lgNetWriterU16(&writer, envelope->messageType)      &&
    lgNetWriterU16(&writer, envelope->serviceVersion)   &&
    lgNetWriterU16(&writer, envelope->messageVersion)   &&
    lgNetWriterU16(&writer, 0)                          &&
    lgNetWriterU32(&writer, envelope->flags)            &&
    lgNetWriterU32(&writer, envelope->payloadLength)    &&
    lgNetWriterU32(&writer, 0)                          &&
    lgNetWriterU64(&writer, envelope->sessionEpoch)     &&
    lgNetWriterU64(&writer, envelope->componentEpoch)   &&
    lgNetWriterU64(&writer, envelope->sequence)         &&
    lgNetWriterU64(&writer, envelope->requestID)        &&
    lgNetWriterSize(&writer) == LG_NET_ENVELOPE_WIRE_SIZE;
}

LGNetParseResult lgNetEnvelopeDecode(LGNetEnvelope * envelope,
    const void * data, size_t size)
{
  if (!envelope || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_ENVELOPE_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetEnvelope decoded;
  uint32_t      magic;
  LGNetReader   reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, LG_NET_ENVELOPE_WIRE_SIZE);
  if (!lgNetReaderU32(&reader, &magic)                    ||
      !lgNetReaderU16(&reader, &decoded.wireMajor)        ||
      !lgNetReaderU16(&reader, &decoded.wireMinor)        ||
      !lgNetReaderU16(&reader, &decoded.headerSize)       ||
      !lgNetReaderU16(&reader, &decoded.service)          ||
      !lgNetReaderU16(&reader, &decoded.messageType)      ||
      !lgNetReaderU16(&reader, &decoded.serviceVersion)   ||
      !lgNetReaderU16(&reader, &decoded.messageVersion)   ||
      !lgNetReaderZero(&reader, 2)                        ||
      !lgNetReaderU32(&reader, &decoded.flags)            ||
      !lgNetReaderU32(&reader, &decoded.payloadLength)    ||
      !lgNetReaderZero(&reader, 4)                        ||
      !lgNetReaderU64(&reader, &decoded.sessionEpoch)     ||
      !lgNetReaderU64(&reader, &decoded.componentEpoch)   ||
      !lgNetReaderU64(&reader, &decoded.sequence)         ||
      !lgNetReaderU64(&reader, &decoded.requestID))
    return LG_NET_PARSE_INVALID_HEADER;

  if (magic != LG_NET_WIRE_MAGIC)
    return LG_NET_PARSE_INVALID_MAGIC;
  if (decoded.wireMajor != LG_NET_WIRE_VERSION_MAJOR)
    return LG_NET_PARSE_INVALID_VERSION;
  if (decoded.headerSize < LG_NET_ENVELOPE_WIRE_SIZE)
    return LG_NET_PARSE_INVALID_HEADER;
  if (decoded.headerSize > size)
    return LG_NET_PARSE_TRUNCATED;
  if (!envelopeValid(&decoded, false) || decoded.flags & ~ENVELOPE_FLAGS)
    return LG_NET_PARSE_INVALID_VALUE;

  *envelope = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetPacketSize(const LGNetEnvelope * envelope, size_t * size)
{
  if (!size || !envelopeValid(envelope, false) ||
      envelope->headerSize > SIZE_MAX - envelope->payloadLength)
    return false;

  *size = (size_t)envelope->headerSize + envelope->payloadLength;
  return true;
}

bool lgNetPacketEncode(void * data, size_t size,
    const LGNetEnvelope * envelope, const void * payload)
{
  uint8_t header[LG_NET_ENVELOPE_WIRE_SIZE];
  size_t  wireSize;
  if (!data || !envelope || (envelope->payloadLength && !payload) ||
      !lgNetPacketSize(envelope, &wireSize) || wireSize > size ||
      !lgNetEnvelopeEncode(header, sizeof(header), envelope))
    return false;

  if (envelope->payloadLength)
    memmove((uint8_t *)data + envelope->headerSize, payload,
      envelope->payloadLength);
  memcpy(data, header, sizeof(header));
  return true;
}

LGNetParseResult lgNetPacketDecode(LGNetPacketView * packet,
    const void * data, size_t size)
{
  if (!packet)
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetPacketView decoded;
  memset(&decoded, 0, sizeof(decoded));
  const LGNetParseResult result = lgNetEnvelopeDecode(
    &decoded.envelope, data, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetPacketSize(&decoded.envelope, &decoded.wireSize))
    return LG_NET_PARSE_INVALID_LENGTH;
  if (decoded.wireSize > size)
    return LG_NET_PARSE_TRUNCATED;

  decoded.payload       = (const uint8_t *)data + decoded.envelope.headerSize;
  decoded.payloadLength = decoded.envelope.payloadLength;
  *packet               = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetServiceKnown(LGNetService service)
{
  return service >= LG_NET_SERVICE_CORE && service <= LG_NET_SERVICE_USB;
}

bool lgNetMessageKnown(LGNetService service, uint16_t messageType)
{
  if (!messageType)
    return false;

  switch (service)
  {
    case LG_NET_SERVICE_CORE:
      return messageType <= LG_NET_CORE_MESSAGE_ERROR;

    case LG_NET_SERVICE_RECOVERY:
      return messageType <= LG_NET_RECOVERY_MESSAGE_STATUS;

    case LG_NET_SERVICE_VIDEO:
      return messageType <= LG_NET_VIDEO_MESSAGE_STATUS;

    case LG_NET_SERVICE_CURSOR:
      return messageType <= LG_NET_CURSOR_MESSAGE_STATUS;

    case LG_NET_SERVICE_INPUT:
      return messageType <= LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS;

    case LG_NET_SERVICE_AUDIO:
      return messageType <= LG_NET_AUDIO_MESSAGE_STATE_ACK;

    case LG_NET_SERVICE_CLIPBOARD:
      return messageType <= LG_NET_CLIPBOARD_MESSAGE_STATUS;

    case LG_NET_SERVICE_FILE:
      return messageType <= LG_NET_FILE_MESSAGE_STATUS;

    case LG_NET_SERVICE_USB:
      return messageType <= LG_NET_USB_MESSAGE_STATUS;
  }

  return false;
}

void lgNetHelloInit(LGNetHello * hello, LGNetRole role)
{
  if (!hello)
    return;

  memset(hello, 0, sizeof(*hello));
  hello->role             = role;
  hello->protocolMin      = LG_NET_PROTOCOL_VERSION_MIN;
  hello->protocolMax      = LG_NET_PROTOCOL_VERSION_CURRENT;
  hello->authModes        = LG_NET_AUTH_MODES_DEFAULT;
  hello->maxPayloadLength = LG_NET_MAX_PAYLOAD_LENGTH;
}

bool lgNetHelloValid(const LGNetHello * hello)
{
  return hello &&
    (hello->role == LG_NET_ROLE_CLIENT || hello->role == LG_NET_ROLE_SERVER) &&
    hello->protocolMin && hello->protocolMin <= hello->protocolMax &&
    hello->authModes &&
    hello->maxPayloadLength &&
    hello->maxPayloadLength <= LG_NET_MAX_PAYLOAD_LENGTH &&
    hello->maxDatagramPayload <= hello->maxPayloadLength &&
    hello->capabilityCount <= LG_NET_MAX_CAPABILITY_RECORDS;
}

bool lgNetHelloEncode(void * data, size_t size, const LGNetHello * hello)
{
  if (!data || size < LG_NET_HELLO_WIRE_SIZE || !lgNetHelloValid(hello))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, hello->role)               &&
    lgNetWriterU16(&writer, hello->protocolMin)        &&
    lgNetWriterU16(&writer, hello->protocolMax)        &&
    lgNetWriterU16(&writer, 0)                         &&
    lgNetWriterU32(&writer, hello->authModes)          &&
    lgNetWriterU32(&writer, hello->maxPayloadLength)   &&
    lgNetWriterU32(&writer, hello->maxDatagramPayload) &&
    lgNetWriterU32(&writer, hello->capabilityCount)    &&
    lgNetWriterBytes(&writer, hello->nonce, sizeof(hello->nonce)) &&
    lgNetWriterSize(&writer) == LG_NET_HELLO_WIRE_SIZE;
}

LGNetParseResult lgNetHelloDecode(LGNetHello * hello,
    const void * data, size_t size)
{
  if (!hello || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_HELLO_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetHello  decoded;
  LGNetReader reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.role)               ||
      !lgNetReaderU16(&reader, &decoded.protocolMin)        ||
      !lgNetReaderU16(&reader, &decoded.protocolMax)        ||
      !lgNetReaderZero(&reader, 2)                          ||
      !lgNetReaderU32(&reader, &decoded.authModes)          ||
      !lgNetReaderU32(&reader, &decoded.maxPayloadLength)   ||
      !lgNetReaderU32(&reader, &decoded.maxDatagramPayload) ||
      !lgNetReaderU32(&reader, &decoded.capabilityCount)    ||
      !lgNetReaderBytes(&reader, decoded.nonce,
        sizeof(decoded.nonce)))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_HELLO_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetHelloValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *hello = decoded;
  return LG_NET_PARSE_OK;
}

void lgNetHelloAckInit(LGNetHelloAck * hello, uint64_t sessionEpoch)
{
  if (!hello)
    return;

  memset(hello, 0, sizeof(*hello));
  hello->protocolVersion  = LG_NET_PROTOCOL_VERSION_CURRENT;
  hello->authMode         = LG_NET_AUTH_MODE_DEFAULT;
  hello->maxPayloadLength = LG_NET_MAX_PAYLOAD_LENGTH;
  hello->sessionEpoch     = sessionEpoch;
}

bool lgNetHelloAckValid(const LGNetHelloAck * hello)
{
  return hello &&
    hello->protocolVersion >= LG_NET_PROTOCOL_VERSION_MIN &&
    hello->protocolVersion <= LG_NET_PROTOCOL_VERSION_MAX &&
    authModeKnown(hello->authMode) &&
    !(hello->flags & ~LG_NET_HELLO_ACK_RESUMED) &&
    hello->maxPayloadLength &&
    hello->maxPayloadLength <= LG_NET_MAX_PAYLOAD_LENGTH &&
    hello->maxDatagramPayload <= hello->maxPayloadLength &&
    hello->capabilityCount <= LG_NET_MAX_CAPABILITY_RECORDS &&
    hello->sessionEpoch;
}

bool lgNetHelloAckEncode(
    void * data, size_t size, const LGNetHelloAck * hello)
{
  if (!data || size < LG_NET_HELLO_ACK_WIRE_SIZE ||
      !lgNetHelloAckValid(hello))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, hello->protocolVersion)     &&
    lgNetWriterU16(&writer, hello->authMode)            &&
    lgNetWriterU32(&writer, hello->flags)               &&
    lgNetWriterU32(&writer, hello->maxPayloadLength)    &&
    lgNetWriterU32(&writer, hello->maxDatagramPayload)  &&
    lgNetWriterU32(&writer, hello->capabilityCount)     &&
    lgNetWriterU32(&writer, 0)                          &&
    lgNetWriterU64(&writer, hello->sessionEpoch)        &&
    lgNetWriterBytes(&writer, hello->nonce, sizeof(hello->nonce)) &&
    lgNetWriterSize(&writer) == LG_NET_HELLO_ACK_WIRE_SIZE;
}

LGNetParseResult lgNetHelloAckDecode(LGNetHelloAck * hello,
    const void * data, size_t size)
{
  if (!hello || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_HELLO_ACK_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetHelloAck decoded;
  LGNetReader   reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.protocolVersion)     ||
      !lgNetReaderU16(&reader, &decoded.authMode)            ||
      !lgNetReaderU32(&reader, &decoded.flags)               ||
      !lgNetReaderU32(&reader, &decoded.maxPayloadLength)    ||
      !lgNetReaderU32(&reader, &decoded.maxDatagramPayload)  ||
      !lgNetReaderU32(&reader, &decoded.capabilityCount)     ||
      !lgNetReaderZero(&reader, 4)                           ||
      !lgNetReaderU64(&reader, &decoded.sessionEpoch)        ||
      !lgNetReaderBytes(&reader, decoded.nonce,
        sizeof(decoded.nonce)))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_HELLO_ACK_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetHelloAckValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *hello = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetCapabilityValid(const LGNetCapability * capability)
{
  if (!capability || !capability->service ||
      !capability->versionMin ||
      capability->versionMin > capability->versionMax ||
      capability->flags & ~CAPABILITY_FLAGS ||
      !capability->maxPayloadLength ||
      capability->maxPayloadLength > LG_NET_MAX_PAYLOAD_LENGTH ||
      capability->maxDatagramPayload > capability->maxPayloadLength)
    return false;

  if ((capability->flags & LG_NET_CAPABILITY_SELECTED) &&
      capability->versionMin != capability->versionMax)
    return false;
  if (!!(capability->flags & LG_NET_CAPABILITY_DATAGRAM) !=
      !!capability->maxDatagramPayload)
    return false;
  return true;
}

bool lgNetCapabilityEncode(void * data, size_t size,
    const LGNetCapability * capability)
{
  if (!data || size < LG_NET_CAPABILITY_WIRE_SIZE ||
      !lgNetCapabilityValid(capability))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, capability->service)                  &&
    lgNetWriterU16(&writer, capability->versionMin)               &&
    lgNetWriterU16(&writer, capability->versionMax)               &&
    lgNetWriterU16(&writer, capability->flags)                    &&
    lgNetWriterU64(&writer, capability->features)                 &&
    lgNetWriterU32(&writer, capability->maxPayloadLength)         &&
    lgNetWriterU32(&writer, capability->maxDatagramPayload)       &&
    lgNetWriterU16(&writer, capability->maxBidirectionalStreams)  &&
    lgNetWriterU16(&writer, capability->maxUnidirectionalStreams) &&
    lgNetWriterU32(&writer, 0)                                    &&
    lgNetWriterSize(&writer) == LG_NET_CAPABILITY_WIRE_SIZE;
}

LGNetParseResult lgNetCapabilityDecode(LGNetCapability * capability,
    const void * data, size_t size)
{
  if (!capability || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CAPABILITY_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetCapability decoded;
  LGNetReader     reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.service)                  ||
      !lgNetReaderU16(&reader, &decoded.versionMin)               ||
      !lgNetReaderU16(&reader, &decoded.versionMax)               ||
      !lgNetReaderU16(&reader, &decoded.flags)                    ||
      !lgNetReaderU64(&reader, &decoded.features)                 ||
      !lgNetReaderU32(&reader, &decoded.maxPayloadLength)         ||
      !lgNetReaderU32(&reader, &decoded.maxDatagramPayload)       ||
      !lgNetReaderU16(&reader, &decoded.maxBidirectionalStreams)  ||
      !lgNetReaderU16(&reader, &decoded.maxUnidirectionalStreams) ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_CAPABILITY_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetCapabilityValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *capability = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetPasswordChallengeSize(
    const LGNetPasswordChallenge * challenge)
{
  if (!lgNetPasswordChallengeValid(challenge))
    return 0;
  return LG_NET_PASSWORD_CHALLENGE_HEADER_SIZE +
    challenge->saltLength + challenge->challengeLength;
}

bool lgNetPasswordChallengeValid(
    const LGNetPasswordChallenge * challenge)
{
  return challenge && passwordAlgorithmKnown(challenge->algorithm) &&
    challenge->rounds && challenge->saltLength &&
    challenge->saltLength <= LG_NET_PASSWORD_SALT_MAX_LENGTH &&
    challenge->challengeLength &&
    challenge->challengeLength <= LG_NET_PASSWORD_CHALLENGE_MAX_LENGTH;
}

bool lgNetPasswordChallengeEncode(void * data, size_t size,
    const LGNetPasswordChallenge * challenge)
{
  const size_t required = lgNetPasswordChallengeSize(challenge);
  if (!data || !required || size < required)
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, challenge->algorithm)       &&
    lgNetWriterU16(&writer, 0)                          &&
    lgNetWriterU32(&writer, challenge->rounds)          &&
    lgNetWriterU16(&writer, challenge->saltLength)      &&
    lgNetWriterU16(&writer, challenge->challengeLength) &&
    lgNetWriterU32(&writer, 0)                          &&
    lgNetWriterBytes(&writer, challenge->salt,
      challenge->saltLength)                              &&
    lgNetWriterBytes(&writer, challenge->challenge,
      challenge->challengeLength)                         &&
    lgNetWriterSize(&writer) == required;
}

LGNetParseResult lgNetPasswordChallengeDecode(
    LGNetPasswordChallenge * challenge, const void * data, size_t size)
{
  if (!challenge || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_PASSWORD_CHALLENGE_HEADER_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetPasswordChallenge decoded;
  LGNetReader            reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.algorithm)        ||
      !lgNetReaderZero(&reader, 2)                        ||
      !lgNetReaderU32(&reader, &decoded.rounds)           ||
      !lgNetReaderU16(&reader, &decoded.saltLength)       ||
      !lgNetReaderU16(&reader, &decoded.challengeLength)  ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  if (decoded.saltLength > LG_NET_PASSWORD_SALT_MAX_LENGTH ||
      decoded.challengeLength > LG_NET_PASSWORD_CHALLENGE_MAX_LENGTH)
    return LG_NET_PARSE_INVALID_LENGTH;

  const size_t expected = LG_NET_PASSWORD_CHALLENGE_HEADER_SIZE +
    decoded.saltLength + decoded.challengeLength;
  if (size < expected)
    return LG_NET_PARSE_TRUNCATED;
  if (size != expected ||
      !lgNetReaderBytes(&reader, decoded.salt, decoded.saltLength) ||
      !lgNetReaderBytes(&reader, decoded.challenge,
        decoded.challengeLength))
    return LG_NET_PARSE_INVALID_LENGTH;
  if (!lgNetPasswordChallengeValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *challenge = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetPasswordResponseSize(const LGNetPasswordResponse * response)
{
  if (!lgNetPasswordResponseValid(response))
    return 0;
  return LG_NET_PASSWORD_RESPONSE_HEADER_SIZE +
    response->nonceLength + response->proofLength;
}

bool lgNetPasswordResponseValid(const LGNetPasswordResponse * response)
{
  return response && passwordAlgorithmKnown(response->algorithm) &&
    response->nonceLength &&
    response->nonceLength <= LG_NET_PASSWORD_NONCE_MAX_LENGTH &&
    response->proofLength &&
    response->proofLength <= LG_NET_PASSWORD_PROOF_MAX_LENGTH;
}

bool lgNetPasswordResponseEncode(void * data, size_t size,
    const LGNetPasswordResponse * response)
{
  const size_t required = lgNetPasswordResponseSize(response);
  if (!data || !required || size < required)
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, response->algorithm)   &&
    lgNetWriterU16(&writer, response->nonceLength) &&
    lgNetWriterU16(&writer, response->proofLength) &&
    lgNetWriterU16(&writer, 0)                     &&
    lgNetWriterU32(&writer, 0)                     &&
    lgNetWriterBytes(&writer, response->nonce,
      response->nonceLength)                         &&
    lgNetWriterBytes(&writer, response->proof,
      response->proofLength)                         &&
    lgNetWriterSize(&writer) == required;
}

LGNetParseResult lgNetPasswordResponseDecode(
    LGNetPasswordResponse * response, const void * data, size_t size)
{
  if (!response || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_PASSWORD_RESPONSE_HEADER_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetPasswordResponse decoded;
  LGNetReader           reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.algorithm)   ||
      !lgNetReaderU16(&reader, &decoded.nonceLength) ||
      !lgNetReaderU16(&reader, &decoded.proofLength) ||
      !lgNetReaderZero(&reader, 2)                    ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  if (decoded.nonceLength > LG_NET_PASSWORD_NONCE_MAX_LENGTH ||
      decoded.proofLength > LG_NET_PASSWORD_PROOF_MAX_LENGTH)
    return LG_NET_PARSE_INVALID_LENGTH;

  const size_t expected = LG_NET_PASSWORD_RESPONSE_HEADER_SIZE +
    decoded.nonceLength + decoded.proofLength;
  if (size < expected)
    return LG_NET_PARSE_TRUNCATED;
  if (size != expected ||
      !lgNetReaderBytes(&reader, decoded.nonce, decoded.nonceLength) ||
      !lgNetReaderBytes(&reader, decoded.proof, decoded.proofLength))
    return LG_NET_PARSE_INVALID_LENGTH;
  if (!lgNetPasswordResponseValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *response = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetAuthResultSize(const LGNetAuthResult * result)
{
  if (!lgNetAuthResultValid(result))
    return 0;
  return LG_NET_AUTH_RESULT_HEADER_SIZE + result->proofLength;
}

bool lgNetAuthResultValid(const LGNetAuthResult * result)
{
  return result &&
    result->result >= LG_NET_AUTH_RESULT_ACCEPTED &&
    result->result <= LG_NET_AUTH_RESULT_ERROR &&
    result->proofLength <= LG_NET_PASSWORD_PROOF_MAX_LENGTH;
}

bool lgNetAuthResultEncode(
    void * data, size_t size, const LGNetAuthResult * result)
{
  const size_t required = lgNetAuthResultSize(result);
  if (!data || !required || size < required)
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, result->result)       &&
    lgNetWriterU16(&writer, result->proofLength)  &&
    lgNetWriterU32(&writer, result->retryAfterMs) &&
    lgNetWriterU32(&writer, 0)                    &&
    lgNetWriterBytes(&writer, result->proof,
      result->proofLength)                          &&
    lgNetWriterSize(&writer) == required;
}

LGNetParseResult lgNetAuthResultDecode(LGNetAuthResult * result,
    const void * data, size_t size)
{
  if (!result || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_AUTH_RESULT_HEADER_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetAuthResult decoded;
  LGNetReader     reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.result)       ||
      !lgNetReaderU16(&reader, &decoded.proofLength)  ||
      !lgNetReaderU32(&reader, &decoded.retryAfterMs) ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  if (decoded.proofLength > LG_NET_PASSWORD_PROOF_MAX_LENGTH)
    return LG_NET_PARSE_INVALID_LENGTH;

  const size_t expected = LG_NET_AUTH_RESULT_HEADER_SIZE +
    decoded.proofLength;
  if (size < expected)
    return LG_NET_PARSE_TRUNCATED;
  if (size != expected ||
      !lgNetReaderBytes(&reader, decoded.proof, decoded.proofLength))
    return LG_NET_PARSE_INVALID_LENGTH;
  if (!lgNetAuthResultValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *result = decoded;
  return LG_NET_PARSE_OK;
}
