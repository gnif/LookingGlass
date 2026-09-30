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

#include "connection.h"
#include "auth_crypto.h"
#include "fingerprint.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"

#include <LGProtocol/NetworkAuth.h>
#include <LGProtocol/NetworkPacket.h>
#include <LGProtocol/NetworkRoute.h>
#include <LGProtocol/NetworkSequence.h>
#include <LGProtocol/NetworkStream.h>

#include <msquic.h>

#if !defined(QUIC_API_VERSION_2)
#error "The network transport requires MsQuic API version 2"
#endif

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_CONNECT_POLL_MS               50U
#define NETWORK_HANDSHAKE_TIMEOUT_MS          5000U
#define NETWORK_IDLE_TIMEOUT_MS               10000U
#define NETWORK_KEEPALIVE_INTERVAL_MS         1000U
#define NETWORK_INITIAL_RTT_MS                1U
#define NETWORK_MAX_PEER_STREAMS              16U
#define NETWORK_MAX_CONTROL_PAYLOAD           (1024U * 1024U)
#define NETWORK_MAX_CONTROL_BUFFER            \
  (2U * (LG_NET_ENVELOPE_WIRE_SIZE + NETWORK_MAX_CONTROL_PAYLOAD))
#define NETWORK_MAX_DATAGRAM_PAYLOAD          1136U
#define NETWORK_HELLO_REQUEST_ID              UINT64_C(1)
#define NETWORK_SEQUENCE_CONNECTION_EPOCH     UINT64_C(1)
#define NETWORK_AUTH_PBKDF2_ROUNDS_MIN        10000U
#define NETWORK_AUTH_PBKDF2_ROUNDS_MAX        1000000U
#define NETWORK_AUTH_SALT_LENGTH_MIN          16U
#define NETWORK_AUTH_CHALLENGE_LENGTH_MIN     16U
#define NETWORK_AUTH_RESPONSE_NONCE_LENGTH    32U

_Static_assert(NETWORK_AUTH_SHA256_SIZE == LG_NET_PASSWORD_SHA256_LENGTH,
    "Network authentication SHA-256 sizes differ");

struct NetworkSend
{
  QUIC_BUFFER buffer;
  uint8_t     data[];
};

struct NetworkConnection
{
  const QUIC_API_TABLE * api;
  HQUIC                  registration;
  HQUIC                  configuration;
  HQUIC                  connection;
  HQUIC                  controlStream;

  LGEvent * connectEvent;
  LGEvent * closeEvent;
  LG_Lock   lock;
  LG_Lock   stateCallbackLock;

  atomic_bool stopping;
  atomic_bool sessionValid;

  char     * address;
  uint16_t   port;
  char     * password;
  char     * endpoint;

  NetworkKnownHosts        * knownHosts;
  NetworkTrustPromptFn       trustPrompt;
  NetworkTrustCancelFn       trustCancel;
  NetworkPacketFn            packet;
  NetworkConnectionStateFn   state;
  void                     * opaque;

  LGNetCapability  offeredCapabilities[LG_NET_SERVICE_USB + 1U];
  uint32_t         offeredCapabilityCount;
  uint16_t         offeredServices;

  bool                certificatePending;
  char                pendingFingerprint[NETWORK_FINGERPRINT_TEXT_SIZE];

  LG_TransportStatus   connectStatus;
  bool                 connecting;
  LG_TransportSession  session;
  uint64_t             sessionEpoch;
  uint64_t             sendSequence;
  uint64_t             datagramSendSequence;
  uint64_t             receiveSequence;
  LGNetSequenceTracker datagramReceive;
  uint16_t             protocolVersion;
  LGNetAuthMode        authMode;
  bool                 helloAcknowledged;
  bool                 authenticationComplete;
  bool                 authChallengeReceived;
  bool                 authResponseSent;
  bool                 authResultReceived;
  bool                 authKeyValid;
  bool                 capabilitiesComplete;
  bool                 connectFailed;
  uint32_t             expectedCapabilities;
  uint32_t             receivedCapabilities;
  uint16_t             receivedServices;
  uint16_t             reliableServices;
  uint16_t             datagramServices;
  uint16_t             serviceVersions[LG_NET_SERVICE_USB + 1U];
  uint32_t             maxPayloadLength;
  uint32_t             maxDatagramPayload;
  uint16_t             maxDatagramSendLength;
  bool                 datagramSendEnabled;
  uint32_t             serviceMaxPayload[LG_NET_SERVICE_USB + 1U];
  uint32_t             serviceMaxDatagram[LG_NET_SERVICE_USB + 1U];

  uint8_t                clientHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  uint8_t                serverHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  LGNetPasswordChallenge authChallenge;
  LGNetPasswordResponse  authResponse;
  uint8_t                authKey[LG_NET_PASSWORD_SHA256_LENGTH];

  LGNetStreamParser   receiveParser;
  uint8_t           * receiveData;
  uint8_t           * receivePayload;
};

static QUIC_STATUS QUIC_API networkConnectionCallback(HQUIC handle,
    void * opaque, QUIC_CONNECTION_EVENT * event);
static QUIC_STATUS QUIC_API networkStreamCallback(HQUIC handle,
    void * opaque, QUIC_STREAM_EVENT * event);
static QUIC_STATUS QUIC_API networkRejectedStreamCallback(HQUIC handle,
    void * opaque, QUIC_STREAM_EVENT * event);

static char * networkDuplicate(const char * value)
{
  if (!value)
    return NULL;

  const size_t   length = strlen(value) + 1U;
  char         * result = malloc(length);
  if (result)
    memcpy(result, value, length);
  return result;
}

static char * networkEndpoint(const char * address, uint16_t port)
{
  const size_t   length   = strlen(address) + 10U;
  char         * endpoint = malloc(length);
  if (!endpoint)
    return NULL;

  snprintf(endpoint, length, "[%s]:%u", address, port);
  return endpoint;
}

static bool networkCapabilityOfferValid(
    const LGNetCapability * capability)
{
  const LGNetServiceVersionInfo * version = capability ?
    lgNetServiceVersionInfo(capability->service) : NULL;
  if (!lgNetCapabilityValid(capability) ||
      !version ||
      capability->service <= LG_NET_SERVICE_CORE ||
      capability->service >= LG_NET_SERVICE_USB ||
      capability->flags & LG_NET_CAPABILITY_SELECTED ||
      capability->versionMin < version->minimum ||
      capability->versionMax > version->maximum ||
      !(capability->flags & (LG_NET_CAPABILITY_RELIABLE |
        LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_BULK)))
    return false;

  return true;
}

static bool networkBytesNonzero(const uint8_t * data, size_t size)
{
  if (!data)
    return false;

  uint8_t value = 0;
  for (size_t index = 0; index < size; ++index)
    value |= data[index];
  return value != 0;
}

static void networkConnectionClearAuthProofNL(NetworkConnection * this)
{
  networkAuthSecureZero(
      this->clientHelloNonce, sizeof(this->clientHelloNonce));
  networkAuthSecureZero(
      this->serverHelloNonce, sizeof(this->serverHelloNonce));
  networkAuthSecureZero(&this->authChallenge, sizeof(this->authChallenge));
  networkAuthSecureZero(&this->authResponse, sizeof(this->authResponse));
  networkAuthSecureZero(this->authKey, sizeof(this->authKey));
  this->authChallengeReceived  = false;
  this->authResponseSent       = false;
  this->authResultReceived     = false;
  this->authKeyValid           = false;
}

static void networkConnectionSetState(NetworkConnection * this,
    bool connected, LG_TransportStatus reason)
{
  LG_LOCK(this->stateCallbackLock);
  atomic_store_explicit(
      &this->sessionValid, connected, memory_order_release);
  if (this->state)
    this->state(this->opaque, connected, reason);
  LG_UNLOCK(this->stateCallbackLock);
}

static void networkConnectionFail(NetworkConnection * this,
    LG_TransportStatus status)
{
  const bool         wasStopping = atomic_exchange_explicit(
      &this->stopping, true, memory_order_acq_rel);
  LG_TransportStatus reason;

  LG_LOCK(this->lock);
  if (this->connecting && !this->connectFailed)
  {
    this->connectStatus = wasStopping ? LG_TRANSPORT_DISCONNECTED : status;
    this->connectFailed = true;
  }
  reason = this->connectStatus == LG_TRANSPORT_OK ?
    status : this->connectStatus;
  networkConnectionClearAuthProofNL(this);
  LG_UNLOCK(this->lock);

  networkConnectionSetState(this, false, reason);
  lgSignalEvent(this->connectEvent);
}

static bool networkConnectionSendPacket(NetworkConnection * this,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength, bool allowConnecting)
{
  LGNetEnvelope routeEnvelope;
  lgNetEnvelopeInit(&routeEnvelope, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  if (payloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
      (payloadLength && !payload) ||
      (flags &
        (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_EARLY_DATA)) ||
      !lgNetEnvelopeRouteValid(&routeEnvelope, LG_NET_ROLE_CLIENT,
        LG_NET_ROUTE_DELIVERY_RELIABLE) ||
      atomic_load_explicit(&this->stopping, memory_order_acquire))
    return false;

  const size_t         packetSize = LG_NET_ENVELOPE_WIRE_SIZE +
    payloadLength;
  struct NetworkSend * send       = malloc(sizeof(*send) + packetSize);
  if (!send)
    return false;

  LGNetPacketParameters parameters;
  lgNetPacketParametersInit(&parameters, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  parameters.flags          = flags | LG_NET_ENVELOPE_RELIABLE;
  parameters.componentEpoch = componentEpoch;
  parameters.requestID      = requestID;

  LG_LOCK(this->lock);
  HQUIC          stream           = this->controlStream;
  const bool     serviceReady     =
    allowConnecting || service == LG_NET_SERVICE_CORE ||
    ((this->receivedServices & (UINT16_C(1) << service)) &&
      (this->reliableServices & (UINT16_C(1) << service)) &&
      this->serviceVersions[service] == serviceVersion);
  const uint32_t maxPayloadLength = allowConnecting ?
    NETWORK_MAX_CONTROL_PAYLOAD :
    service == LG_NET_SERVICE_CORE ?
      this->maxPayloadLength : this->serviceMaxPayload[service];
  if (!stream || (!allowConnecting &&
        !atomic_load_explicit(&this->sessionValid, memory_order_acquire)) ||
      !serviceReady || payloadLength > maxPayloadLength ||
      this->sendSequence == UINT64_MAX)
  {
    LG_UNLOCK(this->lock);
    free(send);
    return false;
  }

  parameters.sessionEpoch = this->sessionEpoch;
  parameters.sequence     = ++this->sendSequence;
  size_t wireSize;
  if (!lgNetPacketConstruct(send->data, packetSize, &wireSize,
        &parameters, payload, payloadLength) || wireSize != packetSize)
  {
    --this->sendSequence;
    LG_UNLOCK(this->lock);
    free(send);
    return false;
  }

  send->buffer.Buffer = send->data;
  send->buffer.Length = (uint32_t)packetSize;

  const QUIC_STATUS status = this->api->StreamSend(stream,
      &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
  if (QUIC_FAILED(status))
    --this->sendSequence;
  LG_UNLOCK(this->lock);

  if (QUIC_FAILED(status))
  {
    free(send);
    return false;
  }
  return true;
}

static bool networkConnectionSendHello(NetworkConnection * this)
{
  LGNetHello hello;
  lgNetHelloInit(&hello, LG_NET_ROLE_CLIENT);
  hello.maxPayloadLength   = NETWORK_MAX_CONTROL_PAYLOAD;
  hello.maxDatagramPayload = NETWORK_MAX_DATAGRAM_PAYLOAD;
  hello.capabilityCount    = this->offeredCapabilityCount;
  hello.authModes          = this->password && *this->password ?
    LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_PASSWORD) :
    LG_NET_AUTH_MODE_FLAG(LG_NET_AUTH_NONE);

  uint8_t payload[LG_NET_HELLO_WIRE_SIZE];
  bool    result = false;
  if (!networkAuthRandomBytes(hello.nonce, sizeof(hello.nonce)) ||
      !networkBytesNonzero(hello.nonce, sizeof(hello.nonce)) ||
      !lgNetHelloEncode(payload, sizeof(payload), &hello))
    goto cleanup;

  LG_LOCK(this->lock);
  memcpy(this->clientHelloNonce, hello.nonce, sizeof(hello.nonce));
  LG_UNLOCK(this->lock);

  if (!networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
      LG_NET_CORE_MESSAGE_HELLO, LG_NET_CORE_VERSION_CURRENT,
      LG_NET_ENVELOPE_FINAL, 0, NETWORK_HELLO_REQUEST_ID,
      payload, sizeof(payload), true))
    goto cleanup;

  for (LGNetService service = LG_NET_SERVICE_RECOVERY;
       service < LG_NET_SERVICE_USB; ++service)
  {
    if (!(this->offeredServices & (UINT16_C(1) << service)))
      continue;

    uint8_t capabilityPayload[LG_NET_CAPABILITY_WIRE_SIZE];
    if (!lgNetCapabilityEncode(capabilityPayload,
          sizeof(capabilityPayload), &this->offeredCapabilities[service]) ||
        !networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_CAPABILITY, LG_NET_CORE_VERSION_CURRENT,
          LG_NET_ENVELOPE_FINAL, 0, NETWORK_HELLO_REQUEST_ID,
          capabilityPayload, sizeof(capabilityPayload), true))
      goto cleanup;
  }

  result = networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
      LG_NET_CORE_MESSAGE_CAPABILITIES_DONE,
      LG_NET_CORE_VERSION_CURRENT, LG_NET_ENVELOPE_FINAL,
      0, NETWORK_HELLO_REQUEST_ID, NULL, 0, true);

cleanup:
  networkAuthSecureZero(payload, sizeof(payload));
  networkAuthSecureZero(&hello, sizeof(hello));
  if (!result)
  {
    LG_LOCK(this->lock);
    networkConnectionClearAuthProofNL(this);
    LG_UNLOCK(this->lock);
  }
  return result;
}

static void networkConnectionReady(NetworkConnection * this)
{
  LG_LOCK(this->stateCallbackLock);
  LG_LOCK(this->lock);
  if (!this->connecting || this->connectStatus == LG_TRANSPORT_OK ||
      atomic_load_explicit(&this->stopping, memory_order_acquire))
  {
    LG_UNLOCK(this->lock);
    LG_UNLOCK(this->stateCallbackLock);
    return;
  }

  this->connectStatus = LG_TRANSPORT_OK;
  atomic_store_explicit(&this->sessionValid, true, memory_order_release);
  LG_UNLOCK(this->lock);

  if (this->state)
    this->state(this->opaque, true, LG_TRANSPORT_OK);
  LG_UNLOCK(this->stateCallbackLock);
  lgSignalEvent(this->connectEvent);
}

static bool networkConnectionHandleCapability(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  LGNetCapability capability;
  if (lgNetCapabilityDecode(&capability,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  if (capability.service <= LG_NET_SERVICE_CORE ||
      capability.service >= LG_NET_SERVICE_USB)
    return false;

  LG_LOCK(this->lock);
  const uint16_t             serviceMask   =
    UINT16_C(1) << capability.service;
  const LGNetCapability      offered       =
    this->offeredCapabilities[capability.service];
  const LGNetCapabilityFlags selectedFlags =
    capability.flags &
    (LG_NET_CAPABILITY_RELIABLE | LG_NET_CAPABILITY_DATAGRAM |
      LG_NET_CAPABILITY_BULK | LG_NET_CAPABILITY_MULTI_CLIENT);
  const LGNetCapabilityFlags offeredFlags  =
    offered.flags &
    (LG_NET_CAPABILITY_RELIABLE | LG_NET_CAPABILITY_DATAGRAM |
      LG_NET_CAPABILITY_BULK | LG_NET_CAPABILITY_MULTI_CLIENT);
  if (!this->helloAcknowledged || this->capabilitiesComplete ||
      this->receivedCapabilities >= this->expectedCapabilities ||
      !(this->offeredServices & serviceMask) ||
      !(capability.flags & LG_NET_CAPABILITY_SELECTED) ||
      capability.flags & LG_NET_CAPABILITY_REQUIRED ||
      !(selectedFlags & LG_NET_CAPABILITY_RELIABLE) ||
      selectedFlags & ~offeredFlags ||
      capability.features & ~offered.features ||
      capability.versionMin < offered.versionMin ||
      capability.versionMin > offered.versionMax ||
      capability.maxPayloadLength > offered.maxPayloadLength ||
      capability.maxPayloadLength > this->maxPayloadLength ||
      capability.maxDatagramPayload > offered.maxDatagramPayload ||
      capability.maxDatagramPayload > this->maxDatagramPayload ||
      capability.maxBidirectionalStreams >
        offered.maxBidirectionalStreams ||
      capability.maxUnidirectionalStreams >
        offered.maxUnidirectionalStreams ||
      (this->receivedServices & serviceMask))
  {
    LG_UNLOCK(this->lock);
    return false;
  }

  this->receivedServices |= serviceMask;
  if (capability.flags & LG_NET_CAPABILITY_RELIABLE)
    this->reliableServices |= UINT16_C(1) << capability.service;
  if (capability.flags & LG_NET_CAPABILITY_DATAGRAM)
    this->datagramServices |= UINT16_C(1) << capability.service;
  this->serviceVersions[capability.service]    = capability.versionMin;
  this->serviceMaxPayload[capability.service]  =
    capability.maxPayloadLength;
  this->serviceMaxDatagram[capability.service] =
    capability.maxDatagramPayload;
  ++this->receivedCapabilities;
  LG_UNLOCK(this->lock);
  return true;
}

static bool networkConnectionRequiredCapabilitiesSelected(
    const NetworkConnection * this)
{
  for (LGNetService service = LG_NET_SERVICE_RECOVERY;
       service < LG_NET_SERVICE_USB; ++service)
  {
    const uint16_t serviceMask = UINT16_C(1) << service;
    if ((this->offeredServices & serviceMask) &&
        (this->offeredCapabilities[service].flags &
          LG_NET_CAPABILITY_REQUIRED) &&
        !(this->receivedServices & serviceMask))
      return false;
  }
  return true;
}

static bool networkConnectionReadyToActivateNL(
    const NetworkConnection * this)
{
  return this->helloAcknowledged &&
    this->authenticationComplete &&
    this->capabilitiesComplete &&
    this->receivedCapabilities == this->expectedCapabilities &&
    networkConnectionRequiredCapabilitiesSelected(this);
}

static void networkConnectionSetSessionFeatures(NetworkConnection * this)
{
  const uint16_t services =
    this->receivedServices & this->reliableServices;
  if (services & (UINT16_C(1) << LG_NET_SERVICE_INPUT))
    this->session.features |= LG_TRANSPORT_FEATURE_INPUT;
  if (services & (UINT16_C(1) << LG_NET_SERVICE_CLIPBOARD))
    this->session.features |= LG_TRANSPORT_FEATURE_CLIPBOARD;
  if (services & (UINT16_C(1) << LG_NET_SERVICE_AUDIO))
    this->session.features |= LG_TRANSPORT_FEATURE_AUDIO;
}

static bool networkConnectionHandleAuthChallenge(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  const LGNetEnvelopeFlags responseFlags =
    LG_NET_ENVELOPE_RESPONSE |
    LG_NET_ENVELOPE_FINAL    |
    LG_NET_ENVELOPE_RELIABLE;
  LGNetPasswordChallenge challenge;
  if (packet->envelope.flags != responseFlags ||
      packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
      packet->envelope.componentEpoch != 0 ||
      packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
      lgNetPasswordChallengeDecode(&challenge,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      challenge.algorithm != LG_NET_PASSWORD_PBKDF2_HMAC_SHA256 ||
      challenge.rounds < NETWORK_AUTH_PBKDF2_ROUNDS_MIN ||
      challenge.rounds > NETWORK_AUTH_PBKDF2_ROUNDS_MAX ||
      challenge.saltLength < NETWORK_AUTH_SALT_LENGTH_MIN ||
      challenge.challengeLength < NETWORK_AUTH_CHALLENGE_LENGTH_MIN)
    return false;

  uint16_t protocolVersion;
  uint64_t sessionEpoch;
  uint8_t  clientHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  uint8_t  serverHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  LG_LOCK(this->lock);
  const bool validState = this->password && *this->password &&
    this->helloAcknowledged &&
    this->authMode == LG_NET_AUTH_PASSWORD &&
    !this->authenticationComplete &&
    !this->authChallengeReceived &&
    !this->authResponseSent &&
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    !atomic_load_explicit(&this->stopping, memory_order_acquire);
  if (validState)
  {
    this->authChallengeReceived = true;
    protocolVersion             = this->protocolVersion;
    sessionEpoch                = this->sessionEpoch;
    memcpy(clientHelloNonce, this->clientHelloNonce,
        sizeof(clientHelloNonce));
    memcpy(serverHelloNonce, this->serverHelloNonce,
        sizeof(serverHelloNonce));
  }
  LG_UNLOCK(this->lock);
  if (!validState)
    return false;

  LGNetPasswordResponse response;
  uint8_t               key[LG_NET_PASSWORD_SHA256_LENGTH] = { 0 };
  uint8_t               transcript
    [LG_NET_PASSWORD_CLIENT_PROOF_TRANSCRIPT_MAX_SIZE] = { 0 };
  uint8_t               payload[LG_NET_PASSWORD_RESPONSE_HEADER_SIZE +
    LG_NET_PASSWORD_NONCE_MAX_LENGTH +
    LG_NET_PASSWORD_PROOF_MAX_LENGTH] = { 0 };
  memset(&response, 0, sizeof(response));
  response.algorithm   = LG_NET_PASSWORD_PBKDF2_HMAC_SHA256;
  response.nonceLength = NETWORK_AUTH_RESPONSE_NONCE_LENGTH;
  response.proofLength = LG_NET_PASSWORD_SHA256_LENGTH;

  LGNetPasswordProofContext context =
  {
    .protocolVersion = protocolVersion,
    .sessionEpoch    = sessionEpoch,
    .helloNonce      = clientHelloNonce,
    .helloAckNonce   = serverHelloNonce,
    .challenge       = &challenge,
    .response        = &response,
  };
  size_t transcriptSize = 0;
  size_t payloadSize    = 0;
  bool   stored         = false;
  bool   sent           = false;
  if (!networkAuthRandomBytes(response.nonce, response.nonceLength) ||
      !networkBytesNonzero(response.nonce, response.nonceLength) ||
      !networkAuthPBKDF2HMACSHA256(this->password,
        strlen(this->password), challenge.salt, challenge.saltLength,
        challenge.rounds, key, sizeof(key)) ||
      !lgNetPasswordClientProofTranscriptSize(&context, &transcriptSize) ||
      transcriptSize > sizeof(transcript) ||
      !lgNetPasswordClientProofTranscriptEncode(
        transcript, transcriptSize, &context) ||
      !networkAuthHMACSHA256(key, sizeof(key), transcript,
        transcriptSize, response.proof))
    goto cleanup;

  payloadSize = lgNetPasswordResponseSize(&response);
  if (!payloadSize || payloadSize > sizeof(payload) ||
      !lgNetPasswordResponseEncode(payload, payloadSize, &response))
    goto cleanup;

  LG_LOCK(this->lock);
  stored = this->authChallengeReceived &&
    !this->authResponseSent &&
    !this->authenticationComplete &&
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    !atomic_load_explicit(&this->stopping, memory_order_acquire);
  if (stored)
  {
    this->authChallenge = challenge;
    this->authResponse  = response;
    memcpy(this->authKey, key, sizeof(key));
    this->authResponseSent = true;
    this->authKeyValid     = true;
  }
  LG_UNLOCK(this->lock);
  if (!stored)
    goto cleanup;

  sent = networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
      LG_NET_CORE_MESSAGE_AUTH_RESPONSE, LG_NET_CORE_VERSION_CURRENT,
      LG_NET_ENVELOPE_FINAL, 0, NETWORK_HELLO_REQUEST_ID,
      payload, (uint32_t)payloadSize, true);

cleanup:
  if (stored && !sent)
  {
    LG_LOCK(this->lock);
    networkConnectionClearAuthProofNL(this);
    LG_UNLOCK(this->lock);
  }
  networkAuthSecureZero(payload, sizeof(payload));
  networkAuthSecureZero(transcript, sizeof(transcript));
  networkAuthSecureZero(key, sizeof(key));
  networkAuthSecureZero(&response, sizeof(response));
  networkAuthSecureZero(&challenge, sizeof(challenge));
  networkAuthSecureZero(clientHelloNonce, sizeof(clientHelloNonce));
  networkAuthSecureZero(serverHelloNonce, sizeof(serverHelloNonce));
  return sent;
}

static bool networkConnectionHandleAuthResult(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  const LGNetEnvelopeFlags responseFlags =
    LG_NET_ENVELOPE_RESPONSE |
    LG_NET_ENVELOPE_FINAL    |
    LG_NET_ENVELOPE_RELIABLE;
  LGNetAuthResult result;
  if (packet->envelope.flags != responseFlags ||
      packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
      packet->envelope.componentEpoch != 0 ||
      packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
      lgNetAuthResultDecode(&result,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  if (result.result != LG_NET_AUTH_RESULT_ACCEPTED)
  {
    DEBUG_ERROR("Network password authentication was rejected "
        "(result %u, retry after %u ms)",
        (unsigned)result.result, (unsigned)result.retryAfterMs);
    networkAuthSecureZero(&result, sizeof(result));
    return false;
  }
  if (result.retryAfterMs ||
      result.proofLength != LG_NET_PASSWORD_SHA256_LENGTH)
  {
    networkAuthSecureZero(&result, sizeof(result));
    return false;
  }

  uint16_t               protocolVersion;
  uint64_t               sessionEpoch;
  uint8_t                clientHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  uint8_t                serverHelloNonce[LG_NET_HELLO_NONCE_LENGTH];
  LGNetPasswordChallenge challenge;
  LGNetPasswordResponse  response;
  uint8_t                key[LG_NET_PASSWORD_SHA256_LENGTH];
  LG_LOCK(this->lock);
  const bool validState = this->helloAcknowledged &&
    this->authMode == LG_NET_AUTH_PASSWORD &&
    !this->authenticationComplete &&
    this->authChallengeReceived &&
    this->authResponseSent &&
    !this->authResultReceived &&
    this->authKeyValid &&
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    !atomic_load_explicit(&this->stopping, memory_order_acquire);
  if (validState)
  {
    this->authResultReceived = true;
    protocolVersion          = this->protocolVersion;
    sessionEpoch             = this->sessionEpoch;
    memcpy(clientHelloNonce, this->clientHelloNonce,
        sizeof(clientHelloNonce));
    memcpy(serverHelloNonce, this->serverHelloNonce,
        sizeof(serverHelloNonce));
    challenge = this->authChallenge;
    response  = this->authResponse;
    memcpy(key, this->authKey, sizeof(key));
  }
  LG_UNLOCK(this->lock);
  if (!validState)
  {
    networkAuthSecureZero(&result, sizeof(result));
    return false;
  }

  LGNetPasswordProofContext context =
  {
    .protocolVersion = protocolVersion,
    .sessionEpoch    = sessionEpoch,
    .helloNonce      = clientHelloNonce,
    .helloAckNonce   = serverHelloNonce,
    .challenge       = &challenge,
    .response        = &response,
  };
  uint8_t transcript
    [LG_NET_PASSWORD_SERVER_PROOF_TRANSCRIPT_MAX_SIZE] = { 0 };
  uint8_t expectedProof[LG_NET_PASSWORD_SHA256_LENGTH] = { 0 };
  size_t  transcriptSize                               = 0;
  bool    verified                                     = false;
  if (lgNetPasswordServerProofTranscriptSize(&context, &transcriptSize) &&
      transcriptSize <= sizeof(transcript) &&
      lgNetPasswordServerProofTranscriptEncode(
        transcript, transcriptSize, &context) &&
      networkAuthHMACSHA256(key, sizeof(key), transcript,
        transcriptSize, expectedProof))
    verified = networkAuthConstantTimeEqual(
        expectedProof, result.proof, sizeof(expectedProof));

  bool ready      = false;
  bool stateValid = false;
  LG_LOCK(this->lock);
  stateValid = this->authResultReceived &&
    !this->authenticationComplete &&
    this->authResponseSent &&
    this->authKeyValid &&
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    !atomic_load_explicit(&this->stopping, memory_order_acquire);
  if (verified && stateValid)
  {
    this->authenticationComplete = true;
    ready                        =
      networkConnectionReadyToActivateNL(this);
    if (ready)
      networkConnectionSetSessionFeatures(this);
  }
  networkConnectionClearAuthProofNL(this);
  LG_UNLOCK(this->lock);

  if (!verified)
    DEBUG_ERROR("Network server password proof validation failed");
  if (ready)
    networkConnectionReady(this);

  networkAuthSecureZero(expectedProof, sizeof(expectedProof));
  networkAuthSecureZero(transcript, sizeof(transcript));
  networkAuthSecureZero(key, sizeof(key));
  networkAuthSecureZero(&response, sizeof(response));
  networkAuthSecureZero(&challenge, sizeof(challenge));
  networkAuthSecureZero(clientHelloNonce, sizeof(clientHelloNonce));
  networkAuthSecureZero(serverHelloNonce, sizeof(serverHelloNonce));
  networkAuthSecureZero(&result, sizeof(result));
  return verified && stateValid;
}

static bool networkConnectionHandleCore(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  const LGNetEnvelopeFlags responseFlags =
    LG_NET_ENVELOPE_RESPONSE |
    LG_NET_ENVELOPE_FINAL    |
    LG_NET_ENVELOPE_RELIABLE;

  uint64_t sessionEpoch;
  LG_LOCK(this->lock);
  sessionEpoch = this->sessionEpoch;
  LG_UNLOCK(this->lock);

  switch (packet->envelope.messageType)
  {
    case LG_NET_CORE_MESSAGE_HELLO_ACK:
    {
      LGNetHelloAck acknowledgement;
      const LGNetAuthMode expectedAuthMode =
        this->password && *this->password ?
          LG_NET_AUTH_PASSWORD : LG_NET_AUTH_NONE;
      if (packet->envelope.flags != responseFlags ||
          packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
          packet->envelope.componentEpoch != 0 ||
          packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
          lgNetHelloAckDecode(&acknowledgement,
            packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
          acknowledgement.protocolVersion !=
            LG_NET_PROTOCOL_VERSION_CURRENT ||
          acknowledgement.authMode != expectedAuthMode ||
          acknowledgement.flags ||
          acknowledgement.capabilityCount >
            this->offeredCapabilityCount ||
          acknowledgement.maxPayloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
          acknowledgement.maxDatagramPayload >
            NETWORK_MAX_DATAGRAM_PAYLOAD ||
          !networkBytesNonzero(
            acknowledgement.nonce, sizeof(acknowledgement.nonce)) ||
          packet->envelope.sessionEpoch != acknowledgement.sessionEpoch)
        return false;

      LG_LOCK(this->lock);
      if (this->helloAcknowledged ||
          !networkBytesNonzero(
            this->clientHelloNonce, sizeof(this->clientHelloNonce)))
      {
        LG_UNLOCK(this->lock);
        return false;
      }

      this->helloAcknowledged      = true;
      this->authenticationComplete =
        acknowledgement.authMode == LG_NET_AUTH_NONE;
      this->expectedCapabilities   = acknowledgement.capabilityCount;
      this->protocolVersion        = acknowledgement.protocolVersion;
      this->authMode               = acknowledgement.authMode;
      this->sessionEpoch           = acknowledgement.sessionEpoch;
      this->maxPayloadLength       = acknowledgement.maxPayloadLength;
      this->maxDatagramPayload     = acknowledgement.maxDatagramPayload;
      memcpy(this->serverHelloNonce, acknowledgement.nonce,
          sizeof(this->serverHelloNonce));
      const bool sequenceReady = lgNetSequenceTrackerInit(
          &this->datagramReceive, acknowledgement.sessionEpoch,
          NETWORK_SEQUENCE_CONNECTION_EPOCH, 64, false);
      if (this->authenticationComplete)
        networkConnectionClearAuthProofNL(this);
      LG_UNLOCK(this->lock);
      return sequenceReady;
    }

    case LG_NET_CORE_MESSAGE_CAPABILITY:
      if (packet->envelope.flags != responseFlags ||
          packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
          packet->envelope.componentEpoch != 0 ||
          packet->envelope.sessionEpoch != sessionEpoch)
        return false;
      return networkConnectionHandleCapability(this, packet);

    case LG_NET_CORE_MESSAGE_CAPABILITIES_DONE:
    {
      if (packet->envelope.flags != responseFlags ||
          packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
          packet->envelope.componentEpoch != 0 ||
          packet->envelope.sessionEpoch != sessionEpoch ||
          packet->payloadLength)
        return false;

      bool ready;
      LG_LOCK(this->lock);
      const bool valid = !this->capabilitiesComplete &&
        this->helloAcknowledged &&
        this->receivedCapabilities == this->expectedCapabilities &&
        networkConnectionRequiredCapabilitiesSelected(this);
      this->capabilitiesComplete = valid;
      ready                      =
        valid && networkConnectionReadyToActivateNL(this);
      if (ready)
        networkConnectionSetSessionFeatures(this);
      LG_UNLOCK(this->lock);
      if (ready)
        networkConnectionReady(this);
      return valid;
    }

    case LG_NET_CORE_MESSAGE_AUTH_CHALLENGE:
      return networkConnectionHandleAuthChallenge(this, packet);

    case LG_NET_CORE_MESSAGE_AUTH_RESULT:
      return networkConnectionHandleAuthResult(this, packet);

    default:
      return false;
  }
}

static bool networkConnectionHandlePacket(NetworkConnection * this,
    const LGNetPacketView * packet, bool datagram)
{
  if (packet->envelope.service == LG_NET_SERVICE_CORE)
    return networkConnectionHandleCore(this, packet);

  if (!atomic_load_explicit(&this->sessionValid, memory_order_acquire))
    return false;

  LG_LOCK(this->lock);
  const bool sessionValid =
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    (this->receivedServices &
      (UINT16_C(1) << packet->envelope.service)) &&
    (this->reliableServices &
      (UINT16_C(1) << packet->envelope.service)) &&
    packet->envelope.serviceVersion ==
      this->serviceVersions[packet->envelope.service] &&
    packet->payloadLength <=
      this->serviceMaxPayload[packet->envelope.service];
  LG_UNLOCK(this->lock);
  if (!sessionValid)
    return false;

  if (this->packet)
    this->packet(this->opaque, packet, datagram);
  return true;
}

static bool networkConnectionValidateControl(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  uint32_t maxPayloadLength;
  LG_LOCK(this->lock);
  maxPayloadLength = this->maxPayloadLength;
  LG_UNLOCK(this->lock);

  return packet->envelope.headerSize == LG_NET_ENVELOPE_WIRE_SIZE &&
    packet->payloadLength <= maxPayloadLength &&
    packet->envelope.messageVersion == LG_NET_MESSAGE_VERSION_CURRENT &&
    (packet->envelope.flags &
      (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL)) ==
        (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL) &&
    !(packet->envelope.flags &
      (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_EARLY_DATA)) &&
    lgNetEnvelopeRouteValid(&packet->envelope, LG_NET_ROLE_SERVER,
      LG_NET_ROUTE_DELIVERY_RELIABLE);
}

static bool networkConnectionDecodeDatagram(NetworkConnection * this,
    const QUIC_BUFFER * buffer, LGNetPacketView * packet)
{
  if (!buffer || !buffer->Buffer || !packet ||
      lgNetPacketDecode(packet, buffer->Buffer,
        buffer->Length) != LG_NET_PARSE_OK ||
      packet->wireSize != buffer->Length ||
      packet->envelope.service == LG_NET_SERVICE_CORE ||
      !lgNetEnvelopeRouteValid(&packet->envelope, LG_NET_ROLE_SERVER,
        LG_NET_ROUTE_DELIVERY_DATAGRAM))
    return false;

  if (!atomic_load_explicit(
        &this->sessionValid, memory_order_acquire) ||
      packet->envelope.headerSize != LG_NET_ENVELOPE_WIRE_SIZE ||
      !packet->envelope.sequence ||
      packet->envelope.messageVersion != LG_NET_MESSAGE_VERSION_CURRENT ||
      (packet->envelope.flags &
        (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_FINAL)) !=
        (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_FINAL) ||
      (packet->envelope.flags &
        (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_EARLY_DATA)))
    return false;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << packet->envelope.service;
  const bool     valid       =
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    (this->receivedServices & serviceMask) &&
    (this->datagramServices & serviceMask) &&
    packet->envelope.serviceVersion ==
      this->serviceVersions[packet->envelope.service] &&
    this->serviceMaxDatagram[packet->envelope.service] &&
    packet->payloadLength <=
      this->serviceMaxDatagram[packet->envelope.service] &&
    lgNetSequenceAccepted(lgNetSequenceTrackerCheck(
      &this->datagramReceive, packet->envelope.sessionEpoch,
      NETWORK_SEQUENCE_CONNECTION_EPOCH, packet->envelope.sequence));
  LG_UNLOCK(this->lock);
  return valid;
}

static bool networkConnectionDrainReceive(NetworkConnection * this)
{
  for (;;)
  {
    LGNetStreamPacket streamPacket;
    const LGNetStreamResult result = lgNetStreamParserNext(
        &this->receiveParser, &streamPacket, NULL);
    if (result == LG_NET_STREAM_NEED_DATA)
      return true;
    if (result != LG_NET_STREAM_PACKET)
      return false;

    LGNetPacketView packet =
    {
      .envelope      = streamPacket.envelope,
      .payload       = streamPacket.payload[0].data,
      .payloadLength = streamPacket.payloadLength,
      .wireSize      = streamPacket.wireSize,
    };
    if (streamPacket.payload[1].size)
    {
      memcpy(this->receivePayload, streamPacket.payload[0].data,
          streamPacket.payload[0].size);
      memcpy(this->receivePayload + streamPacket.payload[0].size,
          streamPacket.payload[1].data, streamPacket.payload[1].size);
      packet.payload = this->receivePayload;
    }

    LG_LOCK(this->lock);
    const bool sequenceValid = this->receiveSequence != UINT64_MAX &&
      packet.envelope.sequence == this->receiveSequence + 1U;
    if (sequenceValid)
      this->receiveSequence = packet.envelope.sequence;
    LG_UNLOCK(this->lock);
    if (!sequenceValid || !networkConnectionValidateControl(this, &packet) ||
        !networkConnectionHandlePacket(this, &packet, false) ||
        !lgNetStreamParserConsume(&this->receiveParser))
      return false;
  }
}

static bool networkConnectionReceive(NetworkConnection * this,
    const QUIC_BUFFER * buffers, uint32_t count)
{
  if (count && !buffers)
    return false;

  for (uint32_t i = 0; i < count; ++i)
  {
    if (buffers[i].Length && !buffers[i].Buffer)
      return false;
    size_t offset = 0;
    while (offset < buffers[i].Length)
    {
      size_t accepted;
      if (!lgNetStreamParserAppend(&this->receiveParser,
            buffers[i].Buffer + offset, buffers[i].Length - offset,
            &accepted))
        return false;
      offset += accepted;
      if (!networkConnectionDrainReceive(this))
        return false;
      if (!accepted && offset < buffers[i].Length)
        return false;
    }
  }

  return networkConnectionDrainReceive(this);
}

static void networkConnectionTrustDecision(void * opaque, bool trusted)
{
  NetworkConnection * this = opaque;

  LG_LOCK(this->lock);
  if (!this->certificatePending || !this->connection)
  {
    LG_UNLOCK(this->lock);
    return;
  }
  trusted                  = trusted && !atomic_load_explicit(
      &this->stopping, memory_order_acquire);
  this->certificatePending = false;
  HQUIC connection = this->connection;
  char  fingerprint[NETWORK_FINGERPRINT_TEXT_SIZE];
  memcpy(fingerprint, this->pendingFingerprint, sizeof(fingerprint));
  memset(this->pendingFingerprint, 0, sizeof(this->pendingFingerprint));
  LG_UNLOCK(this->lock);

  if (trusted)
    trusted = networkKnownHostsRemember(
        this->knownHosts, this->endpoint, fingerprint);
  const QUIC_STATUS status =
    this->api->ConnectionCertificateValidationComplete(connection,
      trusted, trusted ? QUIC_TLS_ALERT_CODE_SUCCESS :
        QUIC_TLS_ALERT_CODE_BAD_CERTIFICATE);
  if (QUIC_FAILED(status))
  {
    networkConnectionFail(this, LG_TRANSPORT_ERROR);
    this->api->ConnectionShutdown(connection,
        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
  }
}

static QUIC_STATUS networkConnectionCertificate(NetworkConnection * this,
    const QUIC_CONNECTION_EVENT * event)
{
  const QUIC_BUFFER * certificate =
    (const QUIC_BUFFER *)event->PEER_CERTIFICATE_RECEIVED.Certificate;
  if (!certificate || !certificate->Buffer || !certificate->Length)
    return QUIC_STATUS_BAD_CERTIFICATE;

  uint8_t fingerprint[NETWORK_FINGERPRINT_SIZE];
  char    fingerprintText[NETWORK_FINGERPRINT_TEXT_SIZE];
  if (!networkFingerprintSHA256(certificate->Buffer,
        certificate->Length, fingerprint))
    return QUIC_STATUS_BAD_CERTIFICATE;
  networkFingerprintFormat(fingerprint, fingerprintText);

  switch (networkKnownHostsCheck(
      this->knownHosts, this->endpoint, fingerprintText))
  {
    case NETWORK_KNOWN_HOST_MATCH:
      return QUIC_STATUS_SUCCESS;

    case NETWORK_KNOWN_HOST_CHANGED:
      DEBUG_ERROR("The certificate for %s changed; remove its entry from "
          "network_known_hosts before trusting a replacement",
          this->endpoint);
      return QUIC_STATUS_BAD_CERTIFICATE;

    case NETWORK_KNOWN_HOST_ERROR:
      DEBUG_ERROR("Failed to check the network server fingerprint");
      return QUIC_STATUS_BAD_CERTIFICATE;

    case NETWORK_KNOWN_HOST_UNKNOWN:
      break;
  }

  if (!this->trustPrompt)
    return QUIC_STATUS_BAD_CERTIFICATE;

  LG_LOCK(this->lock);
  if (this->certificatePending ||
      atomic_load_explicit(&this->stopping, memory_order_acquire))
  {
    LG_UNLOCK(this->lock);
    return QUIC_STATUS_BAD_CERTIFICATE;
  }
  this->certificatePending = true;
  memcpy(this->pendingFingerprint, fingerprintText,
      sizeof(this->pendingFingerprint));
  LG_UNLOCK(this->lock);

  if (!this->trustPrompt(this->opaque, this->endpoint, fingerprintText,
        networkConnectionTrustDecision, this))
  {
    LG_LOCK(this->lock);
    const bool validationCompleted = !this->certificatePending;
    if (!validationCompleted)
    {
      this->certificatePending = false;
      memset(this->pendingFingerprint, 0,
          sizeof(this->pendingFingerprint));
    }
    LG_UNLOCK(this->lock);
    return validationCompleted ?
      QUIC_STATUS_PENDING : QUIC_STATUS_BAD_CERTIFICATE;
  }

  /* MsQuic marks validation pending before invoking this callback and permits
   * ConnectionCertificateValidationComplete from the callback. Always return
   * PENDING once the prompt owns the decision, including during cancellation. */
  return QUIC_STATUS_PENDING;
}

static QUIC_STATUS QUIC_API networkConnectionCallback(HQUIC handle,
    void * opaque, QUIC_CONNECTION_EVENT * event)
{
  NetworkConnection * this = opaque;

  switch (event->Type)
  {
    case QUIC_CONNECTION_EVENT_CONNECTED:
    {
      if (atomic_load_explicit(&this->stopping, memory_order_acquire))
      {
        this->api->ConnectionShutdown(handle,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        return QUIC_STATUS_SUCCESS;
      }

      HQUIC       stream = NULL;
      QUIC_STATUS status = this->api->StreamOpen(handle,
          QUIC_STREAM_OPEN_FLAG_NONE, networkStreamCallback, this, &stream);
      if (QUIC_FAILED(status))
      {
        networkConnectionFail(this, LG_TRANSPORT_ERROR);
        this->api->ConnectionShutdown(handle,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
        return QUIC_STATUS_SUCCESS;
      }

      LG_LOCK(this->lock);
      this->controlStream = stream;
      LG_UNLOCK(this->lock);
      status = this->api->StreamStart(
          stream, QUIC_STREAM_START_FLAG_IMMEDIATE |
            QUIC_STREAM_START_FLAG_SHUTDOWN_ON_FAIL);
      if (QUIC_FAILED(status))
      {
        networkConnectionFail(this, LG_TRANSPORT_ERROR);
        this->api->ConnectionShutdown(handle,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
      }
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      networkConnectionFail(this, LG_TRANSPORT_UNAVAILABLE);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
      networkConnectionFail(this, LG_TRANSPORT_DISCONNECTED);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      networkConnectionSetState(
          this, false, LG_TRANSPORT_DISCONNECTED);
      lgSignalEvent(this->connectEvent);
      lgSignalEvent(this->closeEvent);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
      this->api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
          (void *)networkRejectedStreamCallback, this);
      this->api->StreamShutdown(event->PEER_STREAM_STARTED.Stream,
          QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
            QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE, 0);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
    {
      LGNetPacketView packet;
      if (networkConnectionDecodeDatagram(
            this, event->DATAGRAM_RECEIVED.Buffer, &packet) && this->packet)
        this->packet(this->opaque, &packet, true);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
      LG_LOCK(this->lock);
      this->datagramSendEnabled   =
        event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
      this->maxDatagramSendLength =
        event->DATAGRAM_STATE_CHANGED.MaxSendLength;
      LG_UNLOCK(this->lock);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
            event->DATAGRAM_SEND_STATE_CHANGED.State))
        free(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_CERTIFICATE_RECEIVED:
      return networkConnectionCertificate(this, event);

    default:
      return QUIC_STATUS_SUCCESS;
  }
}

static QUIC_STATUS QUIC_API networkStreamCallback(HQUIC handle,
    void * opaque, QUIC_STREAM_EVENT * event)
{
  NetworkConnection * this = opaque;

  switch (event->Type)
  {
    case QUIC_STREAM_EVENT_START_COMPLETE:
      if (QUIC_FAILED(event->START_COMPLETE.Status) ||
          !networkConnectionSendHello(this))
      {
        networkConnectionFail(this, LG_TRANSPORT_ERROR);
        this->api->ConnectionShutdown(this->connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
      }
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_RECEIVE:
      if (!networkConnectionReceive(
            this, event->RECEIVE.Buffers, event->RECEIVE.BufferCount))
      {
        networkConnectionFail(this, LG_TRANSPORT_ERROR);
        this->api->ConnectionShutdown(this->connection,
            QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
      }
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
      free(event->SEND_COMPLETE.ClientContext);
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
      this->api->ConnectionShutdown(this->connection,
          QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
      LG_LOCK(this->lock);
      if (this->controlStream == handle)
        this->controlStream = NULL;
      LG_UNLOCK(this->lock);
      this->api->StreamClose(handle);
      return QUIC_STATUS_SUCCESS;

    default:
      return QUIC_STATUS_SUCCESS;
  }
}

static QUIC_STATUS QUIC_API networkRejectedStreamCallback(HQUIC handle,
    void * opaque, QUIC_STREAM_EVENT * event)
{
  NetworkConnection * this = opaque;
  if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE)
    this->api->StreamClose(handle);
  return QUIC_STATUS_SUCCESS;
}

bool networkConnectionCreate(NetworkConnection ** result,
    const NetworkConnectionParams * params)
{
  if (!result || !params || !params->address || !params->knownHosts ||
      (!!params->trustPrompt != !!params->trustCancel) ||
      params->capabilityCount > LG_NET_MAX_CAPABILITY_RECORDS ||
      (params->capabilityCount && !params->capabilities))
    return false;

  NetworkConnection * this = calloc(1, sizeof(*this));
  if (!this)
    return false;

  LG_LOCK_INIT(this->lock);
  LG_LOCK_INIT(this->stateCallbackLock);
  atomic_init(&this->stopping, false);
  atomic_init(&this->sessionValid, false);

  for (uint32_t index = 0; index < params->capabilityCount; ++index)
  {
    const LGNetCapability * capability = &params->capabilities[index];
    if (!networkCapabilityOfferValid(capability))
      goto fail;

    const uint16_t serviceMask = UINT16_C(1) << capability->service;
    if (this->offeredServices & serviceMask)
      goto fail;
    this->offeredCapabilities[capability->service] = *capability;
    this->offeredServices                          |= serviceMask;
    ++this->offeredCapabilityCount;
  }

  this->address  = networkDuplicate(params->address);
  this->password = networkDuplicate(params->password);
  this->endpoint = networkEndpoint(params->address, params->port);
  if (!this->address || (params->password && !this->password) ||
      !this->endpoint)
    goto fail;

  this->port        = params->port;
  this->knownHosts  = params->knownHosts;
  this->trustPrompt = params->trustPrompt;
  this->trustCancel = params->trustCancel;
  this->packet      = params->packet;
  this->state       = params->state;
  this->opaque      = params->opaque;

  this->connectEvent = lgCreateEvent(false, 0);
  this->closeEvent   = lgCreateEvent(false, 0);
  if (!this->connectEvent || !this->closeEvent)
    goto fail;

  this->receiveData    = malloc(NETWORK_MAX_CONTROL_BUFFER);
  this->receivePayload = malloc(NETWORK_MAX_CONTROL_PAYLOAD);
  if (!this->receiveData || !this->receivePayload ||
      !lgNetStreamParserInit(&this->receiveParser, this->receiveData,
        NETWORK_MAX_CONTROL_BUFFER, NETWORK_MAX_CONTROL_PAYLOAD))
    goto fail;

  if (QUIC_FAILED(MsQuicOpen2(&this->api)))
    goto fail;

  const QUIC_REGISTRATION_CONFIG registration =
  {
    .AppName          = "looking-glass-client",
    .ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY,
  };
  if (QUIC_FAILED(this->api->RegistrationOpen(
        &registration, &this->registration)))
    goto fail;

  const QUIC_BUFFER alpn =
  {
    .Length = LG_NET_ALPN_LENGTH,
    .Buffer = (uint8_t *)LG_NET_ALPN,
  };
  const QUIC_SETTINGS settings =
  {
    .IsSet =
    {
      .HandshakeIdleTimeoutMs = true,
      .IdleTimeoutMs          = true,
      .InitialRttMs           = true,
      .KeepAliveIntervalMs    = true,
      .PeerBidiStreamCount    = true,
      .PeerUnidiStreamCount   = true,
      .DatagramReceiveEnabled = true,
    },
    .HandshakeIdleTimeoutMs  = NETWORK_HANDSHAKE_TIMEOUT_MS,
    .IdleTimeoutMs           = NETWORK_IDLE_TIMEOUT_MS,
    .InitialRttMs            = NETWORK_INITIAL_RTT_MS,
    .KeepAliveIntervalMs     = NETWORK_KEEPALIVE_INTERVAL_MS,
    .PeerBidiStreamCount     = NETWORK_MAX_PEER_STREAMS,
    .PeerUnidiStreamCount    = NETWORK_MAX_PEER_STREAMS,
    .DatagramReceiveEnabled  = true,
  };
  if (QUIC_FAILED(this->api->ConfigurationOpen(this->registration,
        &alpn, 1, &settings, sizeof(settings), NULL,
        &this->configuration)))
    goto fail;

  /* PKI chain validation is disabled because identity is enforced by the
   * certificate callback and the persistent TOFU store above. */
  const QUIC_CREDENTIAL_CONFIG credential =
  {
    .Type  = QUIC_CREDENTIAL_TYPE_NONE,
    .Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
      QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION |
      QUIC_CREDENTIAL_FLAG_INDICATE_CERTIFICATE_RECEIVED |
      QUIC_CREDENTIAL_FLAG_USE_PORTABLE_CERTIFICATES,
  };
  if (QUIC_FAILED(this->api->ConfigurationLoadCredential(
        this->configuration, &credential)))
    goto fail;

  *result = this;
  return true;

fail:
  networkConnectionDestroy(&this);
  return false;
}

void networkConnectionDestroy(NetworkConnection ** connection)
{
  if (!connection || !*connection)
    return;

  NetworkConnection * this = *connection;
  networkConnectionDisconnect(this);
  if (this->configuration)
    this->api->ConfigurationClose(this->configuration);
  if (this->registration)
    this->api->RegistrationClose(this->registration);
  if (this->api)
    MsQuicClose(this->api);
  if (this->closeEvent)
    lgFreeEvent(this->closeEvent);
  if (this->connectEvent)
    lgFreeEvent(this->connectEvent);
  LG_LOCK_FREE(this->stateCallbackLock);
  LG_LOCK_FREE(this->lock);
  free(this->receivePayload);
  free(this->receiveData);
  free(this->endpoint);
  if (this->password)
    networkAuthSecureZero(this->password, strlen(this->password));
  free(this->password);
  free(this->address);
  free(this);
  *connection = NULL;
}

LG_TransportStatus networkConnectionConnect(NetworkConnection * this,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * cancelledOpaque)
{
  if (!this || !session || this->connection)
    return LG_TRANSPORT_ERROR;
  if (cancelled && cancelled(cancelledOpaque))
    return LG_TRANSPORT_DISCONNECTED;

  atomic_store_explicit(&this->stopping, false, memory_order_release);
  atomic_store_explicit(&this->sessionValid, false, memory_order_release);
  lgResetEvent(this->connectEvent);
  lgResetEvent(this->closeEvent);

  LG_LOCK(this->lock);
  this->connectStatus          = LG_TRANSPORT_UNAVAILABLE;
  this->connecting             = true;
  this->sessionEpoch           = 0;
  this->sendSequence           = 0;
  this->datagramSendSequence   = 0;
  this->receiveSequence        = 0;
  this->protocolVersion        = 0;
  this->authMode               = LG_NET_AUTH_NONE;
  this->helloAcknowledged      = false;
  this->authenticationComplete = false;
  this->capabilitiesComplete   = false;
  this->connectFailed          = false;
  this->expectedCapabilities   = 0;
  this->receivedCapabilities   = 0;
  this->receivedServices       = 0;
  this->reliableServices       = 0;
  this->datagramServices       = 0;
  this->maxPayloadLength       = NETWORK_MAX_CONTROL_PAYLOAD;
  this->maxDatagramPayload     = 0;
  this->maxDatagramSendLength  = 0;
  this->datagramSendEnabled    = false;
  networkConnectionClearAuthProofNL(this);
  memset(this->serviceVersions, 0, sizeof(this->serviceVersions));
  memset(this->serviceMaxPayload, 0, sizeof(this->serviceMaxPayload));
  memset(this->serviceMaxDatagram, 0, sizeof(this->serviceMaxDatagram));
  lgNetStreamParserReset(&this->receiveParser);
  memset(&this->session, 0, sizeof(this->session));
  snprintf(this->session.version, sizeof(this->session.version),
      "LG network protocol %u", LG_NET_PROTOCOL_VERSION_CURRENT);
  snprintf(this->session.name, sizeof(this->session.name),
      "%s", this->endpoint);
  this->session.os = LG_TRANSPORT_OS_OTHER;
  LG_UNLOCK(this->lock);

  QUIC_STATUS status = this->api->ConnectionOpen(this->registration,
      networkConnectionCallback, this, &this->connection);
  if (QUIC_FAILED(status))
  {
    LG_LOCK(this->lock);
    this->connecting    = false;
    this->connectStatus = LG_TRANSPORT_ERROR;
    LG_UNLOCK(this->lock);
    this->connection = NULL;
    return LG_TRANSPORT_ERROR;
  }

  status = this->api->ConnectionStart(this->connection, this->configuration,
      QUIC_ADDRESS_FAMILY_UNSPEC, this->address, this->port);
  if (QUIC_FAILED(status))
  {
    this->api->ConnectionClose(this->connection);
    this->connection = NULL;
    LG_LOCK(this->lock);
    this->connecting    = false;
    this->connectStatus = LG_TRANSPORT_UNAVAILABLE;
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  while (!lgWaitEvent(this->connectEvent, NETWORK_CONNECT_POLL_MS))
    if (cancelled && cancelled(cancelledOpaque))
    {
      networkConnectionCancel(this);
      break;
    }

  LG_LOCK(this->lock);
  const LG_TransportStatus connectStatus = this->connectStatus;
  this->connecting = false;
  if (connectStatus == LG_TRANSPORT_OK)
    *session = this->session;
  LG_UNLOCK(this->lock);

  if (connectStatus != LG_TRANSPORT_OK)
    networkConnectionDisconnect(this);
  return connectStatus;
}

void networkConnectionCancel(NetworkConnection * this)
{
  if (!this)
    return;

  atomic_store_explicit(&this->stopping, true, memory_order_release);
  if (this->trustCancel)
    this->trustCancel(this->opaque);

  LG_LOCK(this->stateCallbackLock);
  LG_LOCK(this->lock);
  HQUIC      connection         = this->connection;
  const bool certificatePending =
    this->certificatePending && connection;
  atomic_store_explicit(&this->sessionValid, false, memory_order_release);
  this->certificatePending = false;
  memset(this->pendingFingerprint, 0, sizeof(this->pendingFingerprint));
  networkConnectionClearAuthProofNL(this);
  if (this->connecting)
  {
    this->connectStatus = LG_TRANSPORT_DISCONNECTED;
    this->connectFailed = true;
  }
  LG_UNLOCK(this->lock);
  LG_UNLOCK(this->stateCallbackLock);
  if (certificatePending)
    this->api->ConnectionCertificateValidationComplete(connection, false,
        QUIC_TLS_ALERT_CODE_BAD_CERTIFICATE);
  if (connection)
    this->api->ConnectionShutdown(connection,
        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
  if (this->connectEvent)
    lgSignalEvent(this->connectEvent);
}

void networkConnectionDisconnect(NetworkConnection * this)
{
  if (!this)
    return;

  networkConnectionCancel(this);

  LG_LOCK(this->lock);
  HQUIC connection = this->connection;
  LG_UNLOCK(this->lock);
  if (!connection)
    return;

  lgWaitEvent(this->closeEvent, TIMEOUT_INFINITE);
  LG_LOCK(this->stateCallbackLock);
  LG_LOCK(this->lock);
  const bool ownsConnection = this->connection == connection;
  if (ownsConnection)
  {
    this->connection         = NULL;
    this->controlStream      = NULL;
    this->certificatePending = false;
    memset(this->pendingFingerprint, 0, sizeof(this->pendingFingerprint));
  }
  LG_UNLOCK(this->lock);

  if (ownsConnection)
  {
    this->api->ConnectionClose(connection);
    atomic_store_explicit(
        &this->sessionValid, false, memory_order_release);
    if (this->state)
      this->state(this->opaque, false, LG_TRANSPORT_DISCONNECTED);
  }
  LG_UNLOCK(this->stateCallbackLock);
}

bool networkConnectionValid(const NetworkConnection * this)
{
  return this && atomic_load_explicit(
    &this->sessionValid, memory_order_acquire);
}

bool networkConnectionServiceAvailable(NetworkConnection * this,
    LGNetService service, LGNetCapabilityFlags required)
{
  if (!networkConnectionValid(this) ||
      service <= LG_NET_SERVICE_CORE || service > LG_NET_SERVICE_USB ||
      required & ~(LG_NET_CAPABILITY_RELIABLE |
        LG_NET_CAPABILITY_DATAGRAM))
    return false;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  const bool     available   =
    (this->receivedServices & serviceMask) &&
    (!(required & LG_NET_CAPABILITY_RELIABLE) ||
      (this->reliableServices & serviceMask)) &&
    (!(required & LG_NET_CAPABILITY_DATAGRAM) ||
      (this->datagramServices & serviceMask));
  LG_UNLOCK(this->lock);
  return available;
}

bool networkConnectionServiceVersion(NetworkConnection * this,
    LGNetService service, uint16_t * version)
{
  if (!version || !networkConnectionValid(this) ||
      service <= LG_NET_SERVICE_CORE || service >= LG_NET_SERVICE_USB)
    return false;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  const bool     available   =
    (this->receivedServices & serviceMask) != 0;
  if (available)
    *version = this->serviceVersions[service];
  LG_UNLOCK(this->lock);
  return available;
}

LG_TransportStatus networkConnectionSend(NetworkConnection * this,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  if (!this || !networkConnectionValid(this))
    return LG_TRANSPORT_DISCONNECTED;

  return networkConnectionSendPacket(this, service, messageType,
      serviceVersion, flags, componentEpoch, requestID,
      payload, payloadLength, false) ?
    LG_TRANSPORT_OK : LG_TRANSPORT_ERROR;
}

LG_TransportStatus networkConnectionSendDatagram(NetworkConnection * this,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength, bool discardIfBlocked)
{
  if (!this || !networkConnectionValid(this))
    return LG_TRANSPORT_DISCONNECTED;
  LGNetEnvelope routeEnvelope;
  lgNetEnvelopeInit(&routeEnvelope, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  if ((payloadLength && !payload) || service == LG_NET_SERVICE_CORE ||
      flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
        LG_NET_ENVELOPE_EARLY_DATA) ||
      !lgNetEnvelopeRouteValid(&routeEnvelope, LG_NET_ROLE_CLIENT,
        LG_NET_ROUTE_DELIVERY_DATAGRAM))
    return LG_TRANSPORT_ERROR;

  if (payloadLength > LG_NET_MAX_PAYLOAD_LENGTH)
    return LG_TRANSPORT_ERROR;

  const size_t packetSize = LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;

  LGNetPacketParameters parameters;
  lgNetPacketParametersInit(&parameters, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  parameters.flags          = flags | LG_NET_ENVELOPE_DATAGRAM |
    LG_NET_ENVELOPE_FINAL;
  parameters.componentEpoch = componentEpoch;
  parameters.requestID      = requestID;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  HQUIC          connection  = this->connection;
  if (!connection ||
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire) ||
      !(this->receivedServices & serviceMask) ||
      !(this->datagramServices & serviceMask) ||
      this->serviceVersions[service] != serviceVersion ||
      !this->datagramSendEnabled ||
      payloadLength > this->serviceMaxDatagram[service] ||
      packetSize > this->maxDatagramSendLength ||
      this->datagramSendSequence == UINT64_MAX)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  struct NetworkSend * send = malloc(sizeof(*send) + packetSize);
  if (!send)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_ERROR;
  }

  parameters.sessionEpoch = this->sessionEpoch;
  parameters.sequence     = ++this->datagramSendSequence;
  size_t wireSize;
  if (!lgNetPacketConstruct(send->data, packetSize, &wireSize,
        &parameters, payload, payloadLength) || wireSize != packetSize)
  {
    --this->datagramSendSequence;
    LG_UNLOCK(this->lock);
    free(send);
    return LG_TRANSPORT_ERROR;
  }

  send->buffer.Buffer = send->data;
  send->buffer.Length = (uint32_t)packetSize;
  QUIC_SEND_FLAGS sendFlags = QUIC_SEND_FLAG_PRIORITY_WORK;
  if (discardIfBlocked)
    sendFlags |= QUIC_SEND_FLAG_CANCEL_ON_BLOCKED;
  const QUIC_STATUS status = this->api->DatagramSend(connection,
      &send->buffer, 1, sendFlags, send);
  if (QUIC_FAILED(status))
    --this->datagramSendSequence;
  LG_UNLOCK(this->lock);

  if (QUIC_FAILED(status))
  {
    free(send);
    return LG_TRANSPORT_ERROR;
  }
  return LG_TRANSPORT_OK;
}
