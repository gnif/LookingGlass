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
#include <LGProtocol/NetworkServices.h>
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
#define NETWORK_MAX_CONTROL_PAYLOAD           \
  (LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE + LG_NET_CURSOR_MAX_SHAPE_LENGTH)
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

#define NETWORK_SEND_SERVICE_COUNT               \
  (LG_NET_SERVICE_CONTROL + 1U)
#define NETWORK_SEND_RELIABLE_COUNT_MAX          512U
#define NETWORK_SEND_RELIABLE_BYTES_MAX          \
  (UINT64_C(128) * 1024U * 1024U)
#define NETWORK_SEND_RELIABLE_COUNT_RESERVED     64U
#define NETWORK_SEND_RELIABLE_BYTES_RESERVED     \
  (UINT64_C(32) * 1024U * 1024U)
#define NETWORK_SEND_DATAGRAM_COUNT_MAX          512U
#define NETWORK_SEND_DATAGRAM_BYTES_MAX          \
  (UINT64_C(1) * 1024U * 1024U)
#define NETWORK_SEND_DATAGRAM_COUNT_RESERVED     64U
#define NETWORK_SEND_DATAGRAM_BYTES_RESERVED     \
  (UINT64_C(256) * 1024U)
#define NETWORK_SEND_BULK_COUNT_MAX              256U
#define NETWORK_SEND_BULK_BYTES_MAX              \
  (UINT64_C(256) * 1024U * 1024U)
#define NETWORK_SEND_RESUME_NUMERATOR            3U
#define NETWORK_SEND_RESUME_DENOMINATOR          4U

_Static_assert(NETWORK_AUTH_SHA256_SIZE == LG_NET_PASSWORD_SHA256_LENGTH,
    "Network authentication SHA-256 sizes differ");

struct NetworkSend
{
  QUIC_BUFFER           buffer;
  LGNetService          service;
  uint32_t              length;
  NetworkPacketDelivery delivery;
  bool                  accounted;
  uint8_t               data[];
};

struct NetworkSendLimits
{
  uint32_t count;
  uint64_t bytes;
};

struct NetworkSendAccounting
{
  uint32_t count;
  uint64_t bytes;
  uint32_t serviceCount[NETWORK_SEND_SERVICE_COUNT];
  uint64_t serviceBytes[NETWORK_SEND_SERVICE_COUNT];
  bool     serviceBlocked[NETWORK_SEND_SERVICE_COUNT];
  uint64_t serviceBlockedBytes[NETWORK_SEND_SERVICE_COUNT];
};

struct NetworkBulkStream
{
  struct NetworkBulkStream * next;
  NetworkConnection        * connection;
  HQUIC                      handle;
  LGNetService               service;
  uint64_t                   streamID;
  uint64_t                   sendSequence;
  uint64_t                   receiveSequence;
  bool                       outbound;
  bool                       bound;
  bool                       openingReturned;
  bool                       shutdownComplete;
  uint8_t                    bindingData[LG_NET_STREAM_BINDING_WIRE_SIZE];
  size_t                     bindingLength;
  LGNetStreamParser          receiveParser;
  uint8_t                  * receiveData;
  uint8_t                  * receivePayload;
  size_t                     receivePayloadSize;
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
  NetworkSendReadyFn         sendReady;
  void                     * opaque;

  LGNetCapability  offeredCapabilities[LG_NET_SERVICE_CONTROL + 1U];
  uint32_t         offeredCapabilityCount;
  uint16_t         offeredServices;

  bool                certificatePending;
  char                pendingFingerprint[NETWORK_FINGERPRINT_TEXT_SIZE];

  LG_TransportStatus   connectStatus;
  QUIC_STATUS          lastTransportStatus;
  QUIC_UINT62          lastTransportError;
  QUIC_UINT62          lastPeerError;
  bool                 lastTransportErrorValid;
  bool                 lastPeerErrorValid;
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
  bool                 guestInfoReceived;
  bool                 connectFailed;
  uint32_t             expectedCapabilities;
  uint32_t             receivedCapabilities;
  uint16_t             receivedServices;
  uint16_t             reliableServices;
  uint16_t             datagramServices;
  uint16_t             bulkServices;
  uint16_t             maxPeerBulkStreams;
  uint16_t             peerBulkStreams;
  uint16_t             serviceVersions[LG_NET_SERVICE_CONTROL + 1U];
  uint16_t             serviceMaxUnidirectional
    [LG_NET_SERVICE_CONTROL + 1U];
  uint16_t             servicePeerBulkStreams
    [LG_NET_SERVICE_CONTROL + 1U];
  uint32_t             maxPayloadLength;
  uint32_t             maxDatagramPayload;
  uint16_t             maxDatagramSendLength;
  bool                 datagramSendEnabled;
  uint32_t             serviceMaxPayload[LG_NET_SERVICE_CONTROL + 1U];
  uint32_t             serviceMaxDatagram[LG_NET_SERVICE_CONTROL + 1U];

  struct NetworkSendAccounting reliableSend;
  struct NetworkSendAccounting datagramSend;
  struct NetworkSendAccounting bulkSend;

  struct NetworkBulkStream * outboundBulk
    [LG_NET_SERVICE_CONTROL + 1U];
  struct NetworkBulkStream * inboundBulk;

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
static QUIC_STATUS QUIC_API networkBulkStreamCallback(HQUIC handle,
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

static bool networkGuestText(char * destination, size_t capacity,
    const uint8_t * source, uint16_t length)
{
  if (!destination || !capacity || length >= capacity ||
      (length && (!source || memchr(source, 0, length))))
    return false;

  if (length)
    memcpy(destination, source, length);
  destination[length] = 0;
  return true;
}

static bool networkGuestOS(LGNetCoreGuestOS source,
    LG_TransportGuestOS * destination)
{
  if (!destination)
    return false;

  switch (source)
  {
    case LG_NET_CORE_GUEST_OS_LINUX:
      *destination = LG_TRANSPORT_OS_LINUX;
      return true;

    case LG_NET_CORE_GUEST_OS_BSD:
      *destination = LG_TRANSPORT_OS_BSD;
      return true;

    case LG_NET_CORE_GUEST_OS_OSX:
      *destination = LG_TRANSPORT_OS_OSX;
      return true;

    case LG_NET_CORE_GUEST_OS_WINDOWS:
      *destination = LG_TRANSPORT_OS_WINDOWS;
      return true;

    case LG_NET_CORE_GUEST_OS_OTHER:
      *destination = LG_TRANSPORT_OS_OTHER;
      return true;
  }
  return false;
}

static bool networkCapabilityOfferValid(
    const LGNetCapability * capability)
{
  const LGNetServiceVersionInfo * version = capability ?
    lgNetServiceVersionInfo(capability->service) : NULL;
  if (!lgNetCapabilityValid(capability) ||
      !version ||
      capability->service <= LG_NET_SERVICE_CORE ||
      capability->service > LG_NET_SERVICE_CONTROL ||
      capability->service == LG_NET_SERVICE_USB ||
      capability->flags & LG_NET_CAPABILITY_SELECTED ||
      capability->versionMin < version->minimum ||
      capability->versionMax > version->maximum ||
      capability->maxBidirectionalStreams ||
      capability->maxUnidirectionalStreams > NETWORK_MAX_PEER_STREAMS ||
      (!!(capability->flags & LG_NET_CAPABILITY_BULK) !=
        !!capability->maxUnidirectionalStreams) ||
      !(capability->flags & (LG_NET_CAPABILITY_RELIABLE |
        LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_BULK)))
    return false;

  return true;
}

static bool networkConnectionServiceValid(LGNetService service)
{
  return service > LG_NET_SERVICE_CORE &&
    service <= LG_NET_SERVICE_CONTROL && service != LG_NET_SERVICE_USB;
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

static bool networkConnectionRecordTransportError(NetworkConnection * this,
    QUIC_STATUS status, QUIC_UINT62 error)
{
  LG_LOCK(this->lock);
  const bool changed = !this->lastTransportErrorValid ||
    this->lastTransportStatus != status || this->lastTransportError != error;
  this->lastTransportStatus     = status;
  this->lastTransportError      = error;
  this->lastTransportErrorValid = true;
  LG_UNLOCK(this->lock);
  return changed;
}

static bool networkConnectionRecordPeerError(NetworkConnection * this,
    QUIC_UINT62 error)
{
  LG_LOCK(this->lock);
  const bool changed = !this->lastPeerErrorValid ||
    this->lastPeerError != error;
  this->lastPeerError      = error;
  this->lastPeerErrorValid = true;
  LG_UNLOCK(this->lock);
  return changed;
}

static struct NetworkSendLimits networkConnectionSendLimits(
    LGNetService service, NetworkPacketDelivery delivery)
{
  if (delivery == NETWORK_PACKET_DELIVERY_DATAGRAM)
  {
    switch (service)
    {
      case LG_NET_SERVICE_VIDEO:
        return (struct NetworkSendLimits)
        {
          .count = 256U,
          .bytes = 768U * 1024U,
        };

      case LG_NET_SERVICE_AUDIO:
        return (struct NetworkSendLimits)
        {
          .count = 256U,
          .bytes = 512U * 1024U,
        };

      case LG_NET_SERVICE_INPUT:
        return (struct NetworkSendLimits)
        {
          .count = 128U,
          .bytes = 256U * 1024U,
        };

      default:
        return (struct NetworkSendLimits)
        {
          .count = 64U,
          .bytes = 256U * 1024U,
        };
    }
  }

  if (delivery == NETWORK_PACKET_DELIVERY_BULK)
  {
    switch (service)
    {
      case LG_NET_SERVICE_VIDEO:
        return (struct NetworkSendLimits)
        {
          .count = 128U,
          .bytes = UINT64_C(192) * 1024U * 1024U,
        };

      case LG_NET_SERVICE_CLIPBOARD:
      case LG_NET_SERVICE_FILE:
      case LG_NET_SERVICE_USB:
        return (struct NetworkSendLimits)
        {
          .count = 128U,
          .bytes = UINT64_C(128) * 1024U * 1024U,
        };

      default:
        return (struct NetworkSendLimits)
        {
          .count = 64U,
          .bytes = UINT64_C(64) * 1024U * 1024U,
        };
    }
  }

  switch (service)
  {
    case LG_NET_SERVICE_CURSOR:
      return (struct NetworkSendLimits)
      {
        .count = 8U,
        .bytes = UINT64_C(32) * 1024U * 1024U,
      };

    case LG_NET_SERVICE_VIDEO:
      return (struct NetworkSendLimits)
      {
        .count = 64U,
        .bytes = UINT64_C(72) * 1024U * 1024U,
      };

    case LG_NET_SERVICE_AUDIO:
      return (struct NetworkSendLimits)
      {
        .count = 256U,
        .bytes = UINT64_C(16) * 1024U * 1024U,
      };

    case LG_NET_SERVICE_CLIPBOARD:
    case LG_NET_SERVICE_FILE:
      return (struct NetworkSendLimits)
      {
        .count = 64U,
        .bytes = UINT64_C(32) * 1024U * 1024U,
      };

    case LG_NET_SERVICE_CORE:
      return (struct NetworkSendLimits)
      {
        .count = 64U,
        .bytes = UINT64_C(20) * 1024U * 1024U,
      };

    default:
      return (struct NetworkSendLimits)
      {
        .count = 64U,
        .bytes = UINT64_C(20) * 1024U * 1024U,
      };
  }
}

static bool networkConnectionSendPriority(LGNetService service)
{
  return service == LG_NET_SERVICE_CORE ||
    service == LG_NET_SERVICE_RECOVERY ||
    service == LG_NET_SERVICE_CURSOR ||
    service == LG_NET_SERVICE_INPUT ||
    service == LG_NET_SERVICE_CONTROL;
}

static void networkConnectionResetSendAccountingNL(
    NetworkConnection * this)
{
  memset(&this->reliableSend, 0, sizeof(this->reliableSend));
  memset(&this->datagramSend, 0, sizeof(this->datagramSend));
  memset(&this->bulkSend, 0, sizeof(this->bulkSend));
}

static struct NetworkSendAccounting * networkConnectionSendAccountingNL(
    NetworkConnection * this, NetworkPacketDelivery delivery)
{
  switch (delivery)
  {
    case NETWORK_PACKET_DELIVERY_RELIABLE:
      return &this->reliableSend;

    case NETWORK_PACKET_DELIVERY_DATAGRAM:
      return &this->datagramSend;

    case NETWORK_PACKET_DELIVERY_BULK:
      return &this->bulkSend;
  }

  return NULL;
}

static LG_TransportStatus networkConnectionReserveSendNL(
    NetworkConnection * this, LGNetService service,
    NetworkPacketDelivery delivery, uint64_t packetSize)
{
  if (service >= NETWORK_SEND_SERVICE_COUNT)
    return LG_TRANSPORT_ERROR;

  const struct NetworkSendLimits   limits          =
    networkConnectionSendLimits(service, delivery);
  uint32_t                         totalCountLimit;
  uint64_t                         totalByteLimit;
  switch (delivery)
  {
    case NETWORK_PACKET_DELIVERY_RELIABLE:
      totalCountLimit = NETWORK_SEND_RELIABLE_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_RELIABLE_BYTES_MAX;
      break;

    case NETWORK_PACKET_DELIVERY_DATAGRAM:
      totalCountLimit = NETWORK_SEND_DATAGRAM_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_DATAGRAM_BYTES_MAX;
      break;

    case NETWORK_PACKET_DELIVERY_BULK:
      totalCountLimit = NETWORK_SEND_BULK_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_BULK_BYTES_MAX;
      break;

    default:
      return LG_TRANSPORT_ERROR;
  }

  if (delivery != NETWORK_PACKET_DELIVERY_BULK &&
      !networkConnectionSendPriority(service))
  {
    totalCountLimit -= delivery == NETWORK_PACKET_DELIVERY_DATAGRAM ?
      NETWORK_SEND_DATAGRAM_COUNT_RESERVED :
      NETWORK_SEND_RELIABLE_COUNT_RESERVED;
    totalByteLimit  -= delivery == NETWORK_PACKET_DELIVERY_DATAGRAM ?
      NETWORK_SEND_DATAGRAM_BYTES_RESERVED :
      NETWORK_SEND_RELIABLE_BYTES_RESERVED;
  }
  struct NetworkSendAccounting * accounting =
    networkConnectionSendAccountingNL(this, delivery);
  if (!accounting)
    return LG_TRANSPORT_ERROR;

  if (packetSize > limits.bytes || packetSize > totalByteLimit)
    return LG_TRANSPORT_ERROR;

  if (accounting->count >= totalCountLimit ||
      accounting->serviceCount[service] >= limits.count ||
      accounting->bytes > totalByteLimit - packetSize ||
      accounting->serviceBytes[service] > limits.bytes - packetSize)
  {
    accounting->serviceBlocked[service]      = true;
    accounting->serviceBlockedBytes[service] =
      accounting->serviceBlockedBytes[service] > packetSize ?
        accounting->serviceBlockedBytes[service] : packetSize;
    return LG_TRANSPORT_UNAVAILABLE;
  }

  ++accounting->count;
  ++accounting->serviceCount[service];
  accounting->bytes                 += packetSize;
  accounting->serviceBytes[service] += packetSize;
  return LG_TRANSPORT_OK;
}

static void networkConnectionMarkSendBlockedNL(NetworkConnection * this,
    LGNetService service, NetworkPacketDelivery delivery,
    uint64_t packetSize)
{
  struct NetworkSendAccounting * accounting =
    networkConnectionSendAccountingNL(this, delivery);
  if (!accounting || service >= NETWORK_SEND_SERVICE_COUNT)
    return;

  accounting->serviceBlocked[service]      = true;
  accounting->serviceBlockedBytes[service] =
    accounting->serviceBlockedBytes[service] > packetSize ?
      accounting->serviceBlockedBytes[service] : packetSize;
}

static bool networkConnectionSendCanResumeNL(
    NetworkConnection * this, LGNetService service,
    NetworkPacketDelivery delivery)
{
  const struct NetworkSendLimits         limits          =
    networkConnectionSendLimits(service, delivery);
  uint32_t                               totalCountLimit;
  uint64_t                               totalByteLimit;
  switch (delivery)
  {
    case NETWORK_PACKET_DELIVERY_RELIABLE:
      totalCountLimit = NETWORK_SEND_RELIABLE_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_RELIABLE_BYTES_MAX;
      break;

    case NETWORK_PACKET_DELIVERY_DATAGRAM:
      totalCountLimit = NETWORK_SEND_DATAGRAM_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_DATAGRAM_BYTES_MAX;
      break;

    case NETWORK_PACKET_DELIVERY_BULK:
      totalCountLimit = NETWORK_SEND_BULK_COUNT_MAX;
      totalByteLimit  = NETWORK_SEND_BULK_BYTES_MAX;
      break;

    default:
      return false;
  }

  if (delivery != NETWORK_PACKET_DELIVERY_BULK &&
      !networkConnectionSendPriority(service))
  {
    totalCountLimit -= delivery == NETWORK_PACKET_DELIVERY_DATAGRAM ?
      NETWORK_SEND_DATAGRAM_COUNT_RESERVED :
      NETWORK_SEND_RELIABLE_COUNT_RESERVED;
    totalByteLimit  -= delivery == NETWORK_PACKET_DELIVERY_DATAGRAM ?
      NETWORK_SEND_DATAGRAM_BYTES_RESERVED :
      NETWORK_SEND_RELIABLE_BYTES_RESERVED;
  }
  const struct NetworkSendAccounting * accounting   =
    networkConnectionSendAccountingNL(this, delivery);
  if (!accounting)
    return false;
  const uint32_t                       totalCount   = accounting->count;
  const uint64_t                       totalBytes   = accounting->bytes;
  const uint32_t                       serviceCount =
    accounting->serviceCount[service];
  const uint64_t                       serviceBytes =
    accounting->serviceBytes[service];
  const uint64_t                       blockedBytes =
    accounting->serviceBlockedBytes[service];

  if (atomic_load_explicit(&this->stopping, memory_order_acquire) ||
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire) ||
      (delivery == NETWORK_PACKET_DELIVERY_RELIABLE &&
        !this->controlStream) ||
      (delivery == NETWORK_PACKET_DELIVERY_DATAGRAM &&
        (!this->datagramSendEnabled ||
          blockedBytes > this->maxDatagramSendLength)) ||
      (delivery == NETWORK_PACKET_DELIVERY_BULK &&
        (!this->outboundBulk[service] ||
          !this->outboundBulk[service]->bound)))
    return false;

  const uint32_t totalResumeCount   =
    totalCountLimit * NETWORK_SEND_RESUME_NUMERATOR /
      NETWORK_SEND_RESUME_DENOMINATOR;
  const uint64_t totalResumeBytes   =
    totalByteLimit * NETWORK_SEND_RESUME_NUMERATOR /
      NETWORK_SEND_RESUME_DENOMINATOR;
  const uint32_t serviceResumeCount =
    limits.count * NETWORK_SEND_RESUME_NUMERATOR /
      NETWORK_SEND_RESUME_DENOMINATOR;
  const uint64_t serviceResumeBytes =
    limits.bytes * NETWORK_SEND_RESUME_NUMERATOR /
      NETWORK_SEND_RESUME_DENOMINATOR;

  return totalCount <= totalResumeCount &&
    totalBytes <= totalResumeBytes &&
    serviceCount <= serviceResumeCount &&
    serviceBytes <= serviceResumeBytes &&
    totalCount < totalCountLimit &&
    serviceCount < limits.count &&
    blockedBytes <= totalByteLimit - totalBytes &&
    blockedBytes <= limits.bytes - serviceBytes;
}

static uint16_t networkConnectionReadyServicesNL(
    NetworkConnection * this, NetworkPacketDelivery delivery)
{
  uint16_t                       ready      = 0;
  struct NetworkSendAccounting * accounting =
    networkConnectionSendAccountingNL(this, delivery);
  if (!accounting)
    return 0;

  for (LGNetService service = LG_NET_SERVICE_CORE;
       service < NETWORK_SEND_SERVICE_COUNT; ++service)
  {
    if (!accounting->serviceBlocked[service] ||
        !networkConnectionSendCanResumeNL(this, service, delivery))
      continue;

    accounting->serviceBlocked[service]      = false;
    accounting->serviceBlockedBytes[service] = 0;
    ready                                   |= UINT16_C(1) << service;
  }
  return ready;
}

static uint16_t networkConnectionReleaseSendNL(
    NetworkConnection * this, const struct NetworkSend * send)
{
  if (!send->accounted)
    return 0;

  if (send->service >= NETWORK_SEND_SERVICE_COUNT)
  {
    DEBUG_ERROR("Network send accounting service out of range");
    return 0;
  }

  struct NetworkSendAccounting * accounting =
    networkConnectionSendAccountingNL(this, send->delivery);
  if (!accounting)
  {
    DEBUG_ERROR("Network send accounting delivery out of range");
    return 0;
  }
  if (!accounting->count || !accounting->serviceCount[send->service] ||
      accounting->bytes < send->length ||
      accounting->serviceBytes[send->service] < send->length)
  {
    DEBUG_ERROR("Network send accounting underflow");
    networkConnectionResetSendAccountingNL(this);
    return 0;
  }

  --accounting->count;
  --accounting->serviceCount[send->service];
  accounting->bytes                       -= send->length;
  accounting->serviceBytes[send->service] -= send->length;
  return networkConnectionReadyServicesNL(this, send->delivery);
}

static void networkConnectionNotifySendReady(NetworkConnection * this,
    uint16_t reliable, uint16_t datagram, uint16_t bulk)
{
  if (!this->sendReady)
    return;

  for (LGNetService service = LG_NET_SERVICE_CORE;
       service < NETWORK_SEND_SERVICE_COUNT; ++service)
  {
    const uint16_t serviceMask = UINT16_C(1) << service;
    if (reliable & serviceMask)
      this->sendReady(this->opaque, service,
          NETWORK_PACKET_DELIVERY_RELIABLE);
    if (datagram & serviceMask)
      this->sendReady(this->opaque, service,
          NETWORK_PACKET_DELIVERY_DATAGRAM);
    if (bulk & serviceMask)
      this->sendReady(this->opaque, service,
          NETWORK_PACKET_DELIVERY_BULK);
  }
}

static LG_TransportStatus networkConnectionSendPacket(
    NetworkConnection * this,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength, bool allowConnecting)
{
  LGNetEnvelope routeEnvelope;
  lgNetEnvelopeInit(&routeEnvelope, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  if (payloadLength > NETWORK_MAX_CONTROL_PAYLOAD ||
      (payloadLength && !payload) ||
      (service != LG_NET_SERVICE_CORE &&
        !networkConnectionServiceValid(service)) ||
      (flags &
        (LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_EARLY_DATA)) ||
      !lgNetEnvelopeRouteValid(&routeEnvelope, LG_NET_ROLE_CLIENT,
        LG_NET_ROUTE_DELIVERY_RELIABLE) ||
      atomic_load_explicit(&this->stopping, memory_order_acquire))
    return LG_TRANSPORT_ERROR;

  const size_t packetSize = LG_NET_ENVELOPE_WIRE_SIZE +
    payloadLength;

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
        !atomic_load_explicit(&this->sessionValid, memory_order_acquire)))
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!serviceReady || payloadLength > maxPayloadLength)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  if (this->sendSequence == UINT64_MAX)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_ERROR;
  }

  const LG_TransportStatus reserveStatus =
    networkConnectionReserveSendNL(this, service,
      NETWORK_PACKET_DELIVERY_RELIABLE, packetSize);
  if (reserveStatus != LG_TRANSPORT_OK)
  {
    LG_UNLOCK(this->lock);
    return reserveStatus;
  }

  struct NetworkSend * send = malloc(sizeof(*send) + packetSize);
  if (!send)
  {
    const struct NetworkSend rollback =
    {
      .service   = service,
      .length    = (uint32_t)packetSize,
      .delivery  = NETWORK_PACKET_DELIVERY_RELIABLE,
      .accounted = true,
    };
    const uint16_t ready = networkConnectionReleaseSendNL(this, &rollback);
    LG_UNLOCK(this->lock);
    networkConnectionNotifySendReady(this, ready, 0, 0);
    return LG_TRANSPORT_ERROR;
  }

  parameters.sessionEpoch = this->sessionEpoch;
  parameters.sequence     = ++this->sendSequence;
  send->service           = service;
  send->length            = (uint32_t)packetSize;
  send->delivery          = NETWORK_PACKET_DELIVERY_RELIABLE;
  send->accounted         = true;
  size_t wireSize;
  if (!lgNetPacketConstruct(send->data, packetSize, &wireSize,
        &parameters, payload, payloadLength) || wireSize != packetSize)
  {
    --this->sendSequence;
    const uint16_t ready = networkConnectionReleaseSendNL(this, send);
    LG_UNLOCK(this->lock);
    free(send);
    networkConnectionNotifySendReady(this, ready, 0, 0);
    return LG_TRANSPORT_ERROR;
  }

  send->buffer.Buffer = send->data;
  send->buffer.Length = (uint32_t)packetSize;

  const QUIC_STATUS status = this->api->StreamSend(stream,
      &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
  uint16_t          ready  = 0;
  if (QUIC_FAILED(status))
  {
    --this->sendSequence;
    ready = networkConnectionReleaseSendNL(this, send);
  }
  LG_UNLOCK(this->lock);

  if (QUIC_FAILED(status))
  {
    free(send);
    networkConnectionNotifySendReady(this, ready, 0, 0);
    return LG_TRANSPORT_ERROR;
  }
  return LG_TRANSPORT_OK;
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

  if (networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
      LG_NET_CORE_MESSAGE_HELLO, LG_NET_CORE_VERSION_CURRENT,
      LG_NET_ENVELOPE_FINAL, 0, NETWORK_HELLO_REQUEST_ID,
      payload, sizeof(payload), true) != LG_TRANSPORT_OK)
    goto cleanup;

  for (LGNetService service = LG_NET_SERVICE_RECOVERY;
       service <= LG_NET_SERVICE_CONTROL; ++service)
  {
    if (service == LG_NET_SERVICE_USB)
      continue;
    if (!(this->offeredServices & (UINT16_C(1) << service)))
      continue;

    uint8_t capabilityPayload[LG_NET_CAPABILITY_WIRE_SIZE];
    if (!lgNetCapabilityEncode(capabilityPayload,
          sizeof(capabilityPayload), &this->offeredCapabilities[service]) ||
        networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
          LG_NET_CORE_MESSAGE_CAPABILITY, LG_NET_CORE_VERSION_CURRENT,
          LG_NET_ENVELOPE_FINAL, 0, NETWORK_HELLO_REQUEST_ID,
          capabilityPayload, sizeof(capabilityPayload), true) !=
            LG_TRANSPORT_OK)
      goto cleanup;
  }

  result = networkConnectionSendPacket(this, LG_NET_SERVICE_CORE,
      LG_NET_CORE_MESSAGE_CAPABILITIES_DONE,
      LG_NET_CORE_VERSION_CURRENT, LG_NET_ENVELOPE_FINAL,
      0, NETWORK_HELLO_REQUEST_ID, NULL, 0, true) == LG_TRANSPORT_OK;

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

  this->connectStatus           = LG_TRANSPORT_OK;
  this->lastTransportErrorValid = false;
  this->lastPeerErrorValid      = false;
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

  if (!networkConnectionServiceValid(capability.service))
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
      capability.maxBidirectionalStreams ||
      (!!(selectedFlags & LG_NET_CAPABILITY_BULK) !=
        !!capability.maxUnidirectionalStreams) ||
      capability.maxUnidirectionalStreams >
        NETWORK_MAX_PEER_STREAMS - this->maxPeerBulkStreams ||
      (this->receivedServices & serviceMask))
  {
    LG_UNLOCK(this->lock);
    return false;
  }

  const LGNetService service = capability.service;
  this->receivedServices |= serviceMask;
  if (capability.flags & LG_NET_CAPABILITY_RELIABLE)
    this->reliableServices |= UINT16_C(1) << service;
  if (capability.flags & LG_NET_CAPABILITY_DATAGRAM)
    this->datagramServices |= UINT16_C(1) << service;
  if (capability.flags & LG_NET_CAPABILITY_BULK)
  {
    this->bulkServices       |= UINT16_C(1) << service;
    this->maxPeerBulkStreams += capability.maxUnidirectionalStreams;
  }
  this->serviceVersions[service]             = capability.versionMin;
  this->serviceMaxUnidirectional[service]    =
    capability.maxUnidirectionalStreams;
  this->serviceMaxPayload[service]           = capability.maxPayloadLength;
  this->serviceMaxDatagram[service]          =
    capability.maxDatagramPayload;
  ++this->receivedCapabilities;
  LG_UNLOCK(this->lock);
  return true;
}

static bool networkConnectionRequiredCapabilitiesSelected(
    const NetworkConnection * this)
{
  for (LGNetService service = LG_NET_SERVICE_RECOVERY;
       service <= LG_NET_SERVICE_CONTROL; ++service)
  {
    if (service == LG_NET_SERVICE_USB)
      continue;
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
    this->guestInfoReceived &&
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
  if (services & (UINT16_C(1) << LG_NET_SERVICE_CONTROL))
    this->session.features |=
      LG_TRANSPORT_FEATURE_SET_CURSOR_POS |
      LG_TRANSPORT_FEATURE_WINDOW_SIZE;
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
      payload, (uint32_t)payloadSize, true) == LG_TRANSPORT_OK;

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
          packet->envelope.sessionEpoch != sessionEpoch ||
          packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT)
        return false;
      return networkConnectionHandleCapability(this, packet);

    case LG_NET_CORE_MESSAGE_GUEST_INFO:
    {
      LGNetCoreGuestInfo info;
      if (packet->envelope.flags != responseFlags ||
          packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
          packet->envelope.componentEpoch != 0 ||
          packet->envelope.sessionEpoch != sessionEpoch ||
          packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
          lgNetCoreGuestInfoDecode(&info,
            packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
        return false;

      LG_TransportSession guest = { 0 };
      if (!networkGuestOS(info.os, &guest.os) ||
          !networkGuestText(guest.version, sizeof(guest.version),
            info.version, info.versionLength) ||
          !networkGuestText(guest.osName, sizeof(guest.osName),
            info.osName, info.osNameLength) ||
          !networkGuestText(guest.capture, sizeof(guest.capture),
            info.capture, info.captureLength) ||
          !networkGuestText(guest.cpuModel, sizeof(guest.cpuModel),
            info.cpuModel, info.cpuModelLength))
        return false;
      memcpy(guest.uuid, info.uuid, sizeof(guest.uuid));
      guest.uuidValid = networkBytesNonzero(guest.uuid, sizeof(guest.uuid));
      guest.cpus      = info.processors;
      guest.cores     = info.cores;
      guest.sockets   = info.sockets;

      LG_LOCK(this->lock);
      const bool valid = this->helloAcknowledged &&
        this->authenticationComplete && !this->capabilitiesComplete &&
        !this->guestInfoReceived &&
        this->receivedCapabilities == this->expectedCapabilities &&
        networkConnectionRequiredCapabilitiesSelected(this);
      if (valid)
      {
        memcpy(this->session.version, guest.version,
            sizeof(this->session.version));
        memcpy(this->session.uuid, guest.uuid, sizeof(this->session.uuid));
        memcpy(this->session.osName, guest.osName,
            sizeof(this->session.osName));
        memcpy(this->session.capture, guest.capture,
            sizeof(this->session.capture));
        memcpy(this->session.cpuModel, guest.cpuModel,
            sizeof(this->session.cpuModel));
        this->session.uuidValid = guest.uuidValid;
        this->session.os        = guest.os;
        this->session.cpus      = guest.cpus;
        this->session.cores     = guest.cores;
        this->session.sockets   = guest.sockets;
        this->guestInfoReceived = true;
      }
      LG_UNLOCK(this->lock);
      return valid;
    }

    case LG_NET_CORE_MESSAGE_CAPABILITIES_DONE:
    {
      if (packet->envelope.flags != responseFlags ||
          packet->envelope.requestID != NETWORK_HELLO_REQUEST_ID ||
          packet->envelope.componentEpoch != 0 ||
          packet->envelope.sessionEpoch != sessionEpoch ||
          packet->envelope.serviceVersion != LG_NET_CORE_VERSION_CURRENT ||
          packet->payloadLength)
        return false;

      bool ready;
      LG_LOCK(this->lock);
      const bool valid = !this->capabilitiesComplete &&
        this->helloAcknowledged &&
        this->guestInfoReceived &&
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
    const LGNetPacketView * packet, NetworkPacketDelivery delivery)
{
  if (packet->envelope.service == LG_NET_SERVICE_CORE)
    return delivery == NETWORK_PACKET_DELIVERY_RELIABLE &&
      networkConnectionHandleCore(this, packet);

  if (!networkConnectionServiceValid(packet->envelope.service) ||
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire))
    return false;

  LG_LOCK(this->lock);
  const uint16_t serviceMask =
    UINT16_C(1) << packet->envelope.service;
  uint16_t deliveryServices;
  switch (delivery)
  {
    case NETWORK_PACKET_DELIVERY_RELIABLE:
      deliveryServices = this->reliableServices;
      break;

    case NETWORK_PACKET_DELIVERY_DATAGRAM:
      deliveryServices = this->datagramServices;
      break;

    case NETWORK_PACKET_DELIVERY_BULK:
      deliveryServices = this->bulkServices;
      break;

    default:
      LG_UNLOCK(this->lock);
      return false;
  }
  const bool sessionValid =
    packet->envelope.sessionEpoch == this->sessionEpoch &&
    (this->receivedServices & serviceMask) &&
    (deliveryServices & serviceMask) &&
    packet->envelope.serviceVersion ==
      this->serviceVersions[packet->envelope.service] &&
    packet->payloadLength <=
      this->serviceMaxPayload[packet->envelope.service];
  LG_UNLOCK(this->lock);
  if (!sessionValid)
    return false;

  if (this->packet)
    this->packet(this->opaque, packet, delivery);
  return true;
}

static bool networkConnectionValidateControl(NetworkConnection * this,
    const LGNetPacketView * packet)
{
  uint32_t maxPayloadLength;
  LG_LOCK(this->lock);
  maxPayloadLength = this->maxPayloadLength;
  LG_UNLOCK(this->lock);

  const bool intermediate =
    (packet->envelope.service == LG_NET_SERVICE_RECOVERY &&
      packet->envelope.messageType == LG_NET_RECOVERY_MESSAGE_STATUS) ||
    (packet->envelope.service == LG_NET_SERVICE_CONTROL &&
      packet->envelope.messageType == LG_NET_CONTROL_MESSAGE_STATUS);
  return packet->envelope.headerSize == LG_NET_ENVELOPE_WIRE_SIZE &&
    packet->payloadLength <= maxPayloadLength &&
    packet->envelope.messageVersion == LG_NET_MESSAGE_VERSION_CURRENT &&
    (packet->envelope.flags & LG_NET_ENVELOPE_RELIABLE) &&
    (intermediate || (packet->envelope.flags & LG_NET_ENVELOPE_FINAL)) &&
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
      (packet->envelope.service != LG_NET_SERVICE_CORE &&
        !networkConnectionServiceValid(packet->envelope.service)) ||
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
        !networkConnectionHandlePacket(this, &packet,
          NETWORK_PACKET_DELIVERY_RELIABLE) ||
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

static void networkBulkStreamFree(struct NetworkBulkStream * stream)
{
  if (!stream)
    return;

  free(stream->receivePayload);
  free(stream->receiveData);
  free(stream);
}

static void networkBulkStreamDetachNL(struct NetworkBulkStream * stream)
{
  NetworkConnection * this = stream->connection;
  if (stream->outbound)
  {
    if (stream->service < NETWORK_SEND_SERVICE_COUNT &&
        this->outboundBulk[stream->service] == stream)
      this->outboundBulk[stream->service] = NULL;
    return;
  }

  struct NetworkBulkStream ** link = &this->inboundBulk;
  while (*link && *link != stream)
    link = &(*link)->next;
  if (*link != stream)
    return;
  *link = stream->next;

  if (this->peerBulkStreams)
    --this->peerBulkStreams;
  if (stream->bound && stream->service < NETWORK_SEND_SERVICE_COUNT &&
      this->servicePeerBulkStreams[stream->service])
    --this->servicePeerBulkStreams[stream->service];
}

static bool networkBulkStreamActivateReceive(
    struct NetworkBulkStream * stream)
{
  LGNetStreamBinding binding;
  if (lgNetStreamBindingDecode(&binding, stream->bindingData,
        sizeof(stream->bindingData)) != LG_NET_PARSE_OK ||
      binding.originRole != LG_NET_ROLE_SERVER ||
      binding.flags != (LG_NET_STREAM_BINDING_UNIDIRECTIONAL |
        LG_NET_STREAM_BINDING_BULK) ||
      binding.streamID != stream->streamID ||
      !networkConnectionServiceValid(binding.service))
    return false;

  NetworkConnection * this = stream->connection;
  uint32_t            maxPayloadLength;
  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << binding.service;
  const bool     valid       =
    !atomic_load_explicit(&this->stopping, memory_order_acquire) &&
    atomic_load_explicit(&this->sessionValid, memory_order_acquire) &&
    binding.sessionEpoch == this->sessionEpoch &&
    (this->receivedServices & serviceMask) &&
    (this->bulkServices & serviceMask) &&
    this->serviceMaxUnidirectional[binding.service] &&
    this->servicePeerBulkStreams[binding.service] <
      this->serviceMaxUnidirectional[binding.service];
  maxPayloadLength = valid ?
    this->serviceMaxPayload[binding.service] : 0;
  LG_UNLOCK(this->lock);
  if (!valid || !maxPayloadLength)
    return false;

  const size_t   receiveSize =
    LG_NET_ENVELOPE_WIRE_SIZE + (size_t)maxPayloadLength;
  uint8_t      * receiveData = malloc(receiveSize);
  if (!receiveData)
    return false;

  stream->receiveData = receiveData;
  if (!lgNetStreamParserInit(&stream->receiveParser, receiveData,
        receiveSize, maxPayloadLength))
  {
    free(stream->receiveData);
    stream->receiveData = NULL;
    return false;
  }

  LG_LOCK(this->lock);
  const bool stateValid =
    !atomic_load_explicit(&this->stopping, memory_order_acquire) &&
    atomic_load_explicit(&this->sessionValid, memory_order_acquire) &&
    binding.sessionEpoch == this->sessionEpoch &&
    (this->receivedServices & serviceMask) &&
    (this->bulkServices & serviceMask) &&
    this->serviceMaxPayload[binding.service] == maxPayloadLength &&
    this->serviceMaxUnidirectional[binding.service] &&
    this->servicePeerBulkStreams[binding.service] <
      this->serviceMaxUnidirectional[binding.service];
  if (stateValid)
  {
    stream->service = binding.service;
    stream->bound   = true;
    ++this->servicePeerBulkStreams[binding.service];
  }
  LG_UNLOCK(this->lock);
  if (!stateValid)
  {
    free(stream->receiveData);
    stream->receiveData = NULL;
    return false;
  }

  return true;
}

static bool networkConnectionValidateBulk(
    const struct NetworkBulkStream * stream,
    const LGNetPacketView * packet)
{
  return stream->bound &&
    packet->envelope.service == stream->service &&
    packet->envelope.headerSize == LG_NET_ENVELOPE_WIRE_SIZE &&
    packet->envelope.messageVersion == LG_NET_MESSAGE_VERSION_CURRENT &&
    (packet->envelope.flags & LG_NET_ENVELOPE_FINAL) &&
    !(packet->envelope.flags & (LG_NET_ENVELOPE_RELIABLE |
      LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_EARLY_DATA)) &&
    lgNetEnvelopeRouteValid(&packet->envelope, LG_NET_ROLE_SERVER,
      LG_NET_ROUTE_DELIVERY_BULK);
}

static bool networkBulkStreamDrainReceive(
    struct NetworkBulkStream * stream)
{
  NetworkConnection * this = stream->connection;
  for (;;)
  {
    LGNetStreamPacket       streamPacket;
    const LGNetStreamResult result = lgNetStreamParserNext(
        &stream->receiveParser, &streamPacket, NULL);
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
      if (stream->receivePayloadSize < streamPacket.payloadLength)
      {
        uint8_t * receivePayload = realloc(
            stream->receivePayload, streamPacket.payloadLength);
        if (!receivePayload)
          return false;
        stream->receivePayload     = receivePayload;
        stream->receivePayloadSize = streamPacket.payloadLength;
      }
      memcpy(stream->receivePayload, streamPacket.payload[0].data,
          streamPacket.payload[0].size);
      memcpy(stream->receivePayload + streamPacket.payload[0].size,
          streamPacket.payload[1].data, streamPacket.payload[1].size);
      packet.payload = stream->receivePayload;
    }

    LG_LOCK(this->lock);
    const bool sequenceValid = stream->bound &&
      stream->receiveSequence != UINT64_MAX &&
      packet.envelope.sequence == stream->receiveSequence + 1U;
    if (sequenceValid)
      stream->receiveSequence = packet.envelope.sequence;
    LG_UNLOCK(this->lock);
    if (!sequenceValid ||
        !networkConnectionValidateBulk(stream, &packet) ||
        !networkConnectionHandlePacket(this, &packet,
          NETWORK_PACKET_DELIVERY_BULK) ||
        !lgNetStreamParserConsume(&stream->receiveParser))
      return false;

    if (!lgNetStreamParserBuffered(&stream->receiveParser))
      lgNetStreamParserReset(&stream->receiveParser);
  }
}

static bool networkBulkStreamReceive(struct NetworkBulkStream * stream,
    const QUIC_BUFFER * buffers, uint32_t count)
{
  if (count && !buffers)
    return false;

  for (uint32_t index = 0; index < count; ++index)
  {
    if (buffers[index].Length && !buffers[index].Buffer)
      return false;

    size_t offset = 0;
    while (offset < buffers[index].Length)
    {
      if (!stream->bound)
      {
        const size_t remaining     = buffers[index].Length - offset;
        size_t       bindingLength = sizeof(stream->bindingData) -
          stream->bindingLength;
        if (bindingLength > remaining)
          bindingLength = remaining;
        memcpy(stream->bindingData + stream->bindingLength,
            buffers[index].Buffer + offset, bindingLength);
        stream->bindingLength += bindingLength;
        offset                += bindingLength;
        if (stream->bindingLength == sizeof(stream->bindingData) &&
            !networkBulkStreamActivateReceive(stream))
          return false;
        continue;
      }

      size_t accepted;
      if (!lgNetStreamParserAppend(&stream->receiveParser,
            buffers[index].Buffer + offset,
            buffers[index].Length - offset, &accepted))
        return false;
      offset += accepted;
      if (!networkBulkStreamDrainReceive(stream) ||
          (!accepted &&
            !lgNetStreamParserWritable(&stream->receiveParser)))
        return false;
    }
  }

  return !stream->bound || networkBulkStreamDrainReceive(stream);
}

static LG_TransportStatus networkConnectionOpenBulkStream(
    NetworkConnection * this, LGNetService service, uint64_t packetSize)
{
  struct NetworkBulkStream * stream = calloc(1, sizeof(*stream));
  if (!stream)
    return LG_TRANSPORT_ERROR;
  stream->connection = this;
  stream->service    = service;
  stream->outbound   = true;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  HQUIC          connection  = this->connection;
  if (!connection ||
      atomic_load_explicit(&this->stopping, memory_order_acquire) ||
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire))
  {
    LG_UNLOCK(this->lock);
    networkBulkStreamFree(stream);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!(this->receivedServices & serviceMask) ||
      !(this->bulkServices & serviceMask) ||
      !this->serviceMaxUnidirectional[service])
  {
    LG_UNLOCK(this->lock);
    networkBulkStreamFree(stream);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  if (this->outboundBulk[service])
  {
    this->outboundBulk[service]->openingReturned = true;
    networkConnectionMarkSendBlockedNL(this, service,
        NETWORK_PACKET_DELIVERY_BULK, packetSize);
    LG_UNLOCK(this->lock);
    networkBulkStreamFree(stream);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  this->outboundBulk[service] = stream;
  networkConnectionMarkSendBlockedNL(this, service,
      NETWORK_PACKET_DELIVERY_BULK, packetSize);
  LG_UNLOCK(this->lock);

  HQUIC       handle;
  QUIC_STATUS status = this->api->StreamOpen(connection,
      QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
      networkBulkStreamCallback, stream, &handle);
  if (QUIC_FAILED(status))
  {
    LG_LOCK(this->lock);
    networkBulkStreamDetachNL(stream);
    this->bulkSend.serviceBlocked[service]      = false;
    this->bulkSend.serviceBlockedBytes[service] = 0;
    LG_UNLOCK(this->lock);
    networkBulkStreamFree(stream);
    return LG_TRANSPORT_ERROR;
  }
  stream->handle = handle;

  status = this->api->StreamStart(handle,
      QUIC_STREAM_START_FLAG_IMMEDIATE);
  if (QUIC_FAILED(status))
  {
    LG_LOCK(this->lock);
    networkBulkStreamDetachNL(stream);
    this->bulkSend.serviceBlocked[service]      = false;
    this->bulkSend.serviceBlockedBytes[service] = 0;
    LG_UNLOCK(this->lock);
    this->api->StreamClose(handle);
    networkBulkStreamFree(stream);
    return LG_TRANSPORT_ERROR;
  }

  uint16_t ready = 0;
  LG_LOCK(this->lock);
  stream->openingReturned = true;
  if (stream->bound && !stream->shutdownComplete)
    ready = networkConnectionReadyServicesNL(
        this, NETWORK_PACKET_DELIVERY_BULK);
  const bool bound            = stream->bound && !stream->shutdownComplete;
  const bool shutdownComplete = stream->shutdownComplete;
  LG_UNLOCK(this->lock);
  if (shutdownComplete)
    networkBulkStreamFree(stream);
  networkConnectionNotifySendReady(this, 0, 0, ready);
  return shutdownComplete ? LG_TRANSPORT_DISCONNECTED :
    bound ? LG_TRANSPORT_OK : LG_TRANSPORT_UNAVAILABLE;
}

static void networkConnectionProtocolFailure(NetworkConnection * this)
{
  networkConnectionFail(this, LG_TRANSPORT_ERROR);

  LG_LOCK(this->lock);
  HQUIC connection = this->connection;
  LG_UNLOCK(this->lock);
  if (connection)
    this->api->ConnectionShutdown(connection,
        QUIC_CONNECTION_SHUTDOWN_FLAG_SILENT, 0);
}

static bool networkConnectionAcceptBulkStream(NetworkConnection * this,
    HQUIC handle, QUIC_STREAM_OPEN_FLAGS flags)
{
  if (flags != QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL)
    return false;

  QUIC_UINT62 streamID;
  uint32_t    streamIDSize = sizeof(streamID);
  if (QUIC_FAILED(this->api->GetParam(handle, QUIC_PARAM_STREAM_ID,
        &streamIDSize, &streamID)) || streamIDSize != sizeof(streamID) ||
      (streamID & 3U) != 3U)
    return false;

  struct NetworkBulkStream * stream = calloc(1, sizeof(*stream));
  if (!stream)
    return false;
  stream->connection = this;
  stream->handle     = handle;
  stream->streamID   = streamID;

  LG_LOCK(this->lock);
  const bool accepted =
    !atomic_load_explicit(&this->stopping, memory_order_acquire) &&
    atomic_load_explicit(&this->sessionValid, memory_order_acquire) &&
    this->maxPeerBulkStreams &&
    this->peerBulkStreams < this->maxPeerBulkStreams;
  if (accepted)
  {
    stream->next       = this->inboundBulk;
    this->inboundBulk  = stream;
    ++this->peerBulkStreams;
  }
  LG_UNLOCK(this->lock);
  if (!accepted)
  {
    networkBulkStreamFree(stream);
    return false;
  }

  this->api->SetCallbackHandler(handle,
      (void *)networkBulkStreamCallback, stream);
  return true;
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
    {
      const QUIC_STATUS status =
        event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status;
      const QUIC_UINT62 error  =
        event->SHUTDOWN_INITIATED_BY_TRANSPORT.ErrorCode;
      if (networkConnectionRecordTransportError(this, status, error))
        DEBUG_WARN("Network transport closed the connection: status 0x%x, "
            "error 0x%llx", (unsigned)status,
            (unsigned long long)error);
      networkConnectionFail(this, LG_TRANSPORT_UNAVAILABLE);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    {
      const QUIC_UINT62 error =
        event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
      if (error && networkConnectionRecordPeerError(this, error))
        DEBUG_WARN("Network server closed the connection: error 0x%llx",
            (unsigned long long)error);
      networkConnectionFail(this, LG_TRANSPORT_DISCONNECTED);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
      networkConnectionSetState(
          this, false, LG_TRANSPORT_DISCONNECTED);
      lgSignalEvent(this->connectEvent);
      lgSignalEvent(this->closeEvent);
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
      if (!networkConnectionAcceptBulkStream(this,
            event->PEER_STREAM_STARTED.Stream,
            event->PEER_STREAM_STARTED.Flags))
      {
        this->api->SetCallbackHandler(event->PEER_STREAM_STARTED.Stream,
            (void *)networkRejectedStreamCallback, this);
        this->api->StreamShutdown(event->PEER_STREAM_STARTED.Stream,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT |
              QUIC_STREAM_SHUTDOWN_FLAG_IMMEDIATE, 0);
        networkConnectionProtocolFailure(this);
      }
      return QUIC_STATUS_SUCCESS;

    case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
    {
      LGNetPacketView packet;
      if (networkConnectionDecodeDatagram(
            this, event->DATAGRAM_RECEIVED.Buffer, &packet) && this->packet)
        this->packet(this->opaque, &packet,
            NETWORK_PACKET_DELIVERY_DATAGRAM);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED:
    {
      uint16_t ready;
      LG_LOCK(this->lock);
      this->datagramSendEnabled   =
        event->DATAGRAM_STATE_CHANGED.SendEnabled != FALSE;
      this->maxDatagramSendLength =
        event->DATAGRAM_STATE_CHANGED.MaxSendLength;
      ready                        =
        networkConnectionReadyServicesNL(
          this, NETWORK_PACKET_DELIVERY_DATAGRAM);
      LG_UNLOCK(this->lock);
      networkConnectionNotifySendReady(this, 0, ready, 0);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
      if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
            event->DATAGRAM_SEND_STATE_CHANGED.State))
      {
        struct NetworkSend * send =
          event->DATAGRAM_SEND_STATE_CHANGED.ClientContext;
        LG_LOCK(this->lock);
        const uint16_t ready =
          networkConnectionReleaseSendNL(this, send);
        LG_UNLOCK(this->lock);
        free(send);
        networkConnectionNotifySendReady(this, 0, ready, 0);
      }
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
    {
      struct NetworkSend * send = event->SEND_COMPLETE.ClientContext;
      LG_LOCK(this->lock);
      const uint16_t ready = networkConnectionReleaseSendNL(this, send);
      LG_UNLOCK(this->lock);
      free(send);
      networkConnectionNotifySendReady(this, ready, 0, 0);
      return QUIC_STATUS_SUCCESS;
    }

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

static QUIC_STATUS QUIC_API networkBulkStreamCallback(HQUIC handle,
    void * opaque, QUIC_STREAM_EVENT * event)
{
  struct NetworkBulkStream * stream = opaque;
  NetworkConnection        * this   = stream->connection;

  switch (event->Type)
  {
    case QUIC_STREAM_EVENT_START_COMPLETE:
    {
      if (QUIC_FAILED(event->START_COMPLETE.Status))
      {
        LG_LOCK(this->lock);
        networkBulkStreamDetachNL(stream);
        const bool deferFree =
          stream->outbound && !stream->openingReturned;
        stream->shutdownComplete = true;
        LG_UNLOCK(this->lock);
        this->api->StreamClose(handle);
        if (!deferFree)
          networkBulkStreamFree(stream);
        networkConnectionProtocolFailure(this);
        return QUIC_STATUS_SUCCESS;
      }
      if (!stream->outbound || !event->START_COMPLETE.ID ||
          (event->START_COMPLETE.ID & 3U) != 2U)
      {
        this->api->StreamShutdown(handle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        networkConnectionProtocolFailure(this);
        return QUIC_STATUS_SUCCESS;
      }

      uint64_t sessionEpoch;
      LG_LOCK(this->lock);
      const uint16_t serviceMask = UINT16_C(1) << stream->service;
      const bool     stateValid  =
        !atomic_load_explicit(&this->stopping, memory_order_acquire) &&
        atomic_load_explicit(&this->sessionValid, memory_order_acquire) &&
        this->outboundBulk[stream->service] == stream &&
        (this->receivedServices & serviceMask) &&
        (this->bulkServices & serviceMask);
      sessionEpoch = this->sessionEpoch;
      LG_UNLOCK(this->lock);
      if (!stateValid)
      {
        this->api->StreamShutdown(handle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        return QUIC_STATUS_SUCCESS;
      }

      LGNetStreamBinding binding;
      lgNetStreamBindingInit(&binding, stream->service, LG_NET_ROLE_CLIENT,
          event->START_COMPLETE.ID, sessionEpoch,
          LG_NET_STREAM_BINDING_UNIDIRECTIONAL |
            LG_NET_STREAM_BINDING_BULK);
      struct NetworkSend * send = malloc(
          sizeof(*send) + LG_NET_STREAM_BINDING_WIRE_SIZE);
      if (!send || !lgNetStreamBindingEncode(send->data,
            LG_NET_STREAM_BINDING_WIRE_SIZE, &binding))
      {
        free(send);
        networkConnectionProtocolFailure(this);
        return QUIC_STATUS_SUCCESS;
      }

      send->buffer.Buffer = send->data;
      send->buffer.Length = LG_NET_STREAM_BINDING_WIRE_SIZE;
      send->service       = stream->service;
      send->length        = LG_NET_STREAM_BINDING_WIRE_SIZE;
      send->delivery      = NETWORK_PACKET_DELIVERY_BULK;
      send->accounted     = false;

      uint16_t ready = 0;
      LG_LOCK(this->lock);
      const bool        sendValid =
        !atomic_load_explicit(&this->stopping, memory_order_acquire) &&
        atomic_load_explicit(&this->sessionValid, memory_order_acquire) &&
        this->sessionEpoch == sessionEpoch &&
        this->outboundBulk[stream->service] == stream;
      const QUIC_STATUS status    = sendValid ?
        this->api->StreamSend(handle, &send->buffer, 1,
          QUIC_SEND_FLAG_NONE, send) : QUIC_STATUS_ABORTED;
      if (QUIC_SUCCEEDED(status))
      {
        stream->streamID = event->START_COMPLETE.ID;
        stream->bound    = true;
        if (stream->openingReturned)
          ready = networkConnectionReadyServicesNL(
              this, NETWORK_PACKET_DELIVERY_BULK);
      }
      LG_UNLOCK(this->lock);

      if (QUIC_FAILED(status))
      {
        free(send);
        if (sendValid)
          networkConnectionProtocolFailure(this);
        else
          this->api->StreamShutdown(handle,
              QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
        return QUIC_STATUS_SUCCESS;
      }

      networkConnectionNotifySendReady(this, 0, 0, ready);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_STREAM_EVENT_RECEIVE:
      if (stream->outbound || !networkBulkStreamReceive(stream,
            event->RECEIVE.Buffers, event->RECEIVE.BufferCount))
        networkConnectionProtocolFailure(this);
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_SEND_COMPLETE:
    {
      struct NetworkSend * send = event->SEND_COMPLETE.ClientContext;
      LG_LOCK(this->lock);
      const uint16_t ready = networkConnectionReleaseSendNL(this, send);
      LG_UNLOCK(this->lock);
      free(send);
      networkConnectionNotifySendReady(this, 0, 0, ready);
      return QUIC_STATUS_SUCCESS;
    }

    case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
      if (!atomic_load_explicit(&this->stopping, memory_order_acquire))
        networkConnectionProtocolFailure(this);
      return QUIC_STATUS_SUCCESS;

    case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
    {
      LG_LOCK(this->lock);
      networkBulkStreamDetachNL(stream);
      const bool deferFree =
        stream->outbound && !stream->openingReturned;
      stream->shutdownComplete = deferFree;
      LG_UNLOCK(this->lock);
      this->api->StreamClose(handle);
      if (!deferFree)
        networkBulkStreamFree(stream);
      return QUIC_STATUS_SUCCESS;
    }

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

  uint16_t offeredBulkStreams = 0;
  for (uint32_t index = 0; index < params->capabilityCount; ++index)
  {
    const LGNetCapability * capability = &params->capabilities[index];
    if (!networkCapabilityOfferValid(capability) ||
        capability->maxUnidirectionalStreams >
          NETWORK_MAX_PEER_STREAMS - offeredBulkStreams)
      goto fail;

    const uint16_t serviceMask = UINT16_C(1) << capability->service;
    if (this->offeredServices & serviceMask)
      goto fail;
    this->offeredCapabilities[capability->service] = *capability;
    this->offeredServices                          |= serviceMask;
    offeredBulkStreams += capability->maxUnidirectionalStreams;
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
  this->sendReady   = params->sendReady;
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
  this->guestInfoReceived      = false;
  this->connectFailed          = false;
  this->expectedCapabilities   = 0;
  this->receivedCapabilities   = 0;
  this->receivedServices       = 0;
  this->reliableServices       = 0;
  this->datagramServices       = 0;
  this->bulkServices           = 0;
  this->maxPeerBulkStreams     = 0;
  this->peerBulkStreams        = 0;
  this->maxPayloadLength       = NETWORK_MAX_CONTROL_PAYLOAD;
  this->maxDatagramPayload     = 0;
  this->maxDatagramSendLength  = 0;
  this->datagramSendEnabled    = false;
  networkConnectionResetSendAccountingNL(this);
  networkConnectionClearAuthProofNL(this);
  memset(this->serviceVersions, 0, sizeof(this->serviceVersions));
  memset(this->serviceMaxUnidirectional, 0,
      sizeof(this->serviceMaxUnidirectional));
  memset(this->servicePeerBulkStreams, 0,
      sizeof(this->servicePeerBulkStreams));
  memset(this->serviceMaxPayload, 0, sizeof(this->serviceMaxPayload));
  memset(this->serviceMaxDatagram, 0, sizeof(this->serviceMaxDatagram));
  lgNetStreamParserReset(&this->receiveParser);
  memset(&this->session, 0, sizeof(this->session));
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
    if (networkConnectionRecordTransportError(this, status, 0))
      DEBUG_WARN("MsQuic ConnectionStart failed for %s: 0x%x",
          this->endpoint, (unsigned)status);
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
    networkConnectionResetSendAccountingNL(this);
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
      !networkConnectionServiceValid(service) ||
      required & ~(LG_NET_CAPABILITY_RELIABLE |
        LG_NET_CAPABILITY_DATAGRAM | LG_NET_CAPABILITY_BULK))
    return false;

  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  const bool     available   =
    (this->receivedServices & serviceMask) &&
    (!(required & LG_NET_CAPABILITY_RELIABLE) ||
      (this->reliableServices & serviceMask)) &&
    (!(required & LG_NET_CAPABILITY_DATAGRAM) ||
      (this->datagramServices & serviceMask)) &&
    (!(required & LG_NET_CAPABILITY_BULK) ||
      (this->bulkServices & serviceMask));
  LG_UNLOCK(this->lock);
  return available;
}

bool networkConnectionServiceVersion(NetworkConnection * this,
    LGNetService service, uint16_t * version)
{
  if (!version || !networkConnectionValid(this) ||
      !networkConnectionServiceValid(service))
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
      payload, payloadLength, false);
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
  if ((payloadLength && !payload) ||
      !networkConnectionServiceValid(service) ||
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
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire))
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!(this->receivedServices & serviceMask) ||
      !(this->datagramServices & serviceMask) ||
      this->serviceVersions[service] != serviceVersion ||
      payloadLength > this->serviceMaxDatagram[service])
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  if (this->datagramSendSequence == UINT64_MAX)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_ERROR;
  }
  if (!this->datagramSendEnabled ||
      packetSize > this->maxDatagramSendLength)
  {
    const struct NetworkSendLimits limits =
      networkConnectionSendLimits(
        service, NETWORK_PACKET_DELIVERY_DATAGRAM);
    if (packetSize <= limits.bytes &&
        packetSize <= NETWORK_SEND_DATAGRAM_BYTES_MAX)
    {
      this->datagramSend.serviceBlocked[service]      = true;
      this->datagramSend.serviceBlockedBytes[service] =
        this->datagramSend.serviceBlockedBytes[service] > packetSize ?
          this->datagramSend.serviceBlockedBytes[service] : packetSize;
    }
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  const LG_TransportStatus reserveStatus =
    networkConnectionReserveSendNL(this, service,
      NETWORK_PACKET_DELIVERY_DATAGRAM, packetSize);
  if (reserveStatus != LG_TRANSPORT_OK)
  {
    LG_UNLOCK(this->lock);
    return reserveStatus;
  }

  struct NetworkSend * send = malloc(sizeof(*send) + packetSize);
  if (!send)
  {
    const struct NetworkSend rollback =
    {
      .service   = service,
      .length    = (uint32_t)packetSize,
      .delivery  = NETWORK_PACKET_DELIVERY_DATAGRAM,
      .accounted = true,
    };
    const uint16_t ready = networkConnectionReleaseSendNL(this, &rollback);
    LG_UNLOCK(this->lock);
    networkConnectionNotifySendReady(this, 0, ready, 0);
    return LG_TRANSPORT_ERROR;
  }

  parameters.sessionEpoch = this->sessionEpoch;
  parameters.sequence     = ++this->datagramSendSequence;
  send->service           = service;
  send->length            = (uint32_t)packetSize;
  send->delivery          = NETWORK_PACKET_DELIVERY_DATAGRAM;
  send->accounted         = true;
  size_t wireSize;
  if (!lgNetPacketConstruct(send->data, packetSize, &wireSize,
        &parameters, payload, payloadLength) || wireSize != packetSize)
  {
    --this->datagramSendSequence;
    const uint16_t ready = networkConnectionReleaseSendNL(this, send);
    LG_UNLOCK(this->lock);
    free(send);
    networkConnectionNotifySendReady(this, 0, ready, 0);
    return LG_TRANSPORT_ERROR;
  }

  send->buffer.Buffer = send->data;
  send->buffer.Length = (uint32_t)packetSize;
  QUIC_SEND_FLAGS sendFlags = QUIC_SEND_FLAG_PRIORITY_WORK;
  if (discardIfBlocked)
    sendFlags |= QUIC_SEND_FLAG_CANCEL_ON_BLOCKED;
  const QUIC_STATUS status = this->api->DatagramSend(connection,
      &send->buffer, 1, sendFlags, send);
  uint16_t          ready  = 0;
  if (QUIC_FAILED(status))
  {
    --this->datagramSendSequence;
    ready = networkConnectionReleaseSendNL(this, send);
  }
  LG_UNLOCK(this->lock);

  if (QUIC_FAILED(status))
  {
    free(send);
    networkConnectionNotifySendReady(this, 0, ready, 0);
    return LG_TRANSPORT_ERROR;
  }
  return LG_TRANSPORT_OK;
}

LG_TransportStatus networkConnectionSendBulk(NetworkConnection * this,
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  if (!this || !networkConnectionValid(this))
    return LG_TRANSPORT_DISCONNECTED;

  LGNetEnvelope routeEnvelope;
  lgNetEnvelopeInit(&routeEnvelope, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  if ((payloadLength && !payload) ||
      !networkConnectionServiceValid(service) ||
      flags & (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
        LG_NET_ENVELOPE_EARLY_DATA) ||
      !lgNetEnvelopeRouteValid(&routeEnvelope, LG_NET_ROLE_CLIENT,
        LG_NET_ROUTE_DELIVERY_BULK) ||
      payloadLength > LG_NET_MAX_PAYLOAD_LENGTH)
    return LG_TRANSPORT_ERROR;

  const size_t                   packetSize =
    LG_NET_ENVELOPE_WIRE_SIZE + payloadLength;
  const struct NetworkSendLimits limits     =
    networkConnectionSendLimits(service, NETWORK_PACKET_DELIVERY_BULK);
  if (packetSize > limits.bytes ||
      packetSize > NETWORK_SEND_BULK_BYTES_MAX)
    return LG_TRANSPORT_ERROR;

  LGNetPacketParameters parameters;
  lgNetPacketParametersInit(&parameters, service, messageType, serviceVersion,
      LG_NET_MESSAGE_VERSION_CURRENT);
  parameters.flags          = flags | LG_NET_ENVELOPE_FINAL;
  parameters.componentEpoch = componentEpoch;
  parameters.requestID      = requestID;

retry:
  LG_LOCK(this->lock);
  const uint16_t serviceMask = UINT16_C(1) << service;
  if (!this->connection ||
      atomic_load_explicit(&this->stopping, memory_order_acquire) ||
      !atomic_load_explicit(&this->sessionValid, memory_order_acquire))
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!(this->receivedServices & serviceMask) ||
      !(this->bulkServices & serviceMask) ||
      !this->serviceMaxUnidirectional[service] ||
      this->serviceVersions[service] != serviceVersion ||
      payloadLength > this->serviceMaxPayload[service])
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  struct NetworkBulkStream * stream = this->outboundBulk[service];
  if (!stream)
  {
    LG_UNLOCK(this->lock);
    const LG_TransportStatus openStatus =
      networkConnectionOpenBulkStream(this, service, packetSize);
    if (openStatus == LG_TRANSPORT_OK)
      goto retry;
    return openStatus;
  }
  if (!stream->bound)
  {
    stream->openingReturned = true;
    networkConnectionMarkSendBlockedNL(this, service,
        NETWORK_PACKET_DELIVERY_BULK, packetSize);
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  if (stream->sendSequence == UINT64_MAX)
  {
    LG_UNLOCK(this->lock);
    return LG_TRANSPORT_ERROR;
  }

  const LG_TransportStatus reserveStatus =
    networkConnectionReserveSendNL(this, service,
      NETWORK_PACKET_DELIVERY_BULK, packetSize);
  if (reserveStatus != LG_TRANSPORT_OK)
  {
    LG_UNLOCK(this->lock);
    return reserveStatus;
  }

  struct NetworkSend * send = malloc(sizeof(*send) + packetSize);
  if (!send)
  {
    const struct NetworkSend rollback =
    {
      .service   = service,
      .length    = (uint32_t)packetSize,
      .delivery  = NETWORK_PACKET_DELIVERY_BULK,
      .accounted = true,
    };
    const uint16_t ready = networkConnectionReleaseSendNL(this, &rollback);
    LG_UNLOCK(this->lock);
    networkConnectionNotifySendReady(this, 0, 0, ready);
    return LG_TRANSPORT_ERROR;
  }

  parameters.sessionEpoch = this->sessionEpoch;
  parameters.sequence     = ++stream->sendSequence;
  send->service           = service;
  send->length            = (uint32_t)packetSize;
  send->delivery          = NETWORK_PACKET_DELIVERY_BULK;
  send->accounted         = true;
  size_t wireSize;
  if (!lgNetPacketConstruct(send->data, packetSize, &wireSize,
        &parameters, payload, payloadLength) || wireSize != packetSize)
  {
    --stream->sendSequence;
    const uint16_t ready = networkConnectionReleaseSendNL(this, send);
    LG_UNLOCK(this->lock);
    free(send);
    networkConnectionNotifySendReady(this, 0, 0, ready);
    return LG_TRANSPORT_ERROR;
  }

  send->buffer.Buffer = send->data;
  send->buffer.Length = (uint32_t)packetSize;
  const QUIC_STATUS status = this->api->StreamSend(stream->handle,
      &send->buffer, 1, QUIC_SEND_FLAG_NONE, send);
  uint16_t          ready  = 0;
  if (QUIC_FAILED(status))
  {
    --stream->sendSequence;
    ready = networkConnectionReleaseSendNL(this, send);
  }
  LG_UNLOCK(this->lock);

  if (QUIC_FAILED(status))
  {
    free(send);
    networkConnectionNotifySendReady(this, 0, 0, ready);
    return LG_TRANSPORT_ERROR;
  }
  return LG_TRANSPORT_OK;
}
