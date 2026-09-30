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

#include "recovery.h"

#include "common/event.h"
#include "common/locking.h"

#include <LGProtocol/NetworkServices.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_RECOVERY_QUERY_TIMEOUT_MS   500U
#define NETWORK_RECOVERY_DEFAULT_TIMEOUT_MS 30000U
#define NETWORK_RECOVERY_REQUEST_CAPACITY   16U

struct NetworkRecoveryRequestEntry
{
  uint64_t           id;
  uint64_t           session;
  uint32_t           serial;
  LG_RecoveryRequest request;
  bool               acknowledged;
};

struct NetworkRecovery
{
  NetworkConnection * connection;
  LGEvent           * queryEvent;
  LG_Lock             lock;

  bool     connected;
  bool     available;
  uint16_t serviceVersion;
  uint64_t nextID;
  uint32_t nextSerial;

  bool              queryPending;
  uint64_t          queryID;
  bool              infoValid;
  LGNetRecoveryInfo info;
  char              version[LG_NET_RECOVERY_MAX_VERSION_LENGTH + 1U];

  uint32_t           requestSerial;
  LG_RecoveryRequest request;
  uint32_t           ackSerial;
  LG_RecoveryRequest ackRequest;

  struct NetworkRecoveryRequestEntry requests[NETWORK_RECOVERY_REQUEST_CAPACITY];
  unsigned                           nextRequest;
};

static uint64_t networkRecoveryNextID(uint64_t id)
{
  ++id;
  return id ? id : 1U;
}

static uint32_t networkRecoveryNextSerial(uint32_t serial)
{
  ++serial;
  return serial ? serial : 1U;
}

static bool networkRecoverySequenceNewer(uint64_t sequence,
    uint64_t reference)
{
  return sequence > reference;
}

static struct NetworkRecoveryRequestEntry * networkRecoveryFindRequestNL(
    NetworkRecovery * recovery, uint64_t id)
{
  for (size_t index = 0;
      index < NETWORK_RECOVERY_REQUEST_CAPACITY; ++index)
    if (recovery->requests[index].id == id)
      return &recovery->requests[index];
  return NULL;
}

static struct NetworkRecoveryRequestEntry * networkRecoveryReserveRequestNL(
    NetworkRecovery * recovery)
{
  for (size_t offset = 0;
      offset < NETWORK_RECOVERY_REQUEST_CAPACITY; ++offset)
  {
    const unsigned index =
      (recovery->nextRequest + offset) % NETWORK_RECOVERY_REQUEST_CAPACITY;
    struct NetworkRecoveryRequestEntry * entry = &recovery->requests[index];
    if (entry->id && !entry->acknowledged)
      continue;

    recovery->nextRequest =
      (index + 1U) % NETWORK_RECOVERY_REQUEST_CAPACITY;
    return entry;
  }
  return NULL;
}

static bool networkRecoveryState(
    LGNetRecoveryState state, LG_RecoveryState * result)
{
  switch (state)
  {
    case LG_NET_RECOVERY_STATE_UNKNOWN:
      *result = LG_RECOVERY_STATE_UNKNOWN;
      return true;
    case LG_NET_RECOVERY_STATE_NORMAL:
      *result = LG_RECOVERY_STATE_NORMAL;
      return true;
    case LG_NET_RECOVERY_STATE_SWITCHING:
      *result = LG_RECOVERY_STATE_SWITCHING;
      return true;
    case LG_NET_RECOVERY_STATE_ACTIVE:
      *result = LG_RECOVERY_STATE_ACTIVE;
      return true;
    case LG_NET_RECOVERY_STATE_FAILED:
      *result = LG_RECOVERY_STATE_FAILED;
      return true;
    default:
      return false;
  }
}

static bool networkRecoveryError(
    LGNetRecoveryError error, LG_RecoveryError * result)
{
  switch (error)
  {
    case LG_NET_RECOVERY_ERROR_NONE:
      *result = LG_RECOVERY_ERR_NONE;
      return true;
    case LG_NET_RECOVERY_ERROR_UNSUPPORTED:
      *result = LG_RECOVERY_ERR_UNSUPPORTED;
      return true;
    case LG_NET_RECOVERY_ERROR_HELPER_UNAVAILABLE:
      *result = LG_RECOVERY_ERR_HELPER_UNAVAILABLE;
      return true;
    case LG_NET_RECOVERY_ERROR_TOPOLOGY_FAILED:
      *result = LG_RECOVERY_ERR_TOPOLOGY_FAILED;
      return true;
    case LG_NET_RECOVERY_ERROR_NO_FALLBACK_DISPLAY:
      *result = LG_RECOVERY_ERR_NO_FALLBACK_DISPLAY;
      return true;
    case LG_NET_RECOVERY_ERROR_BUSY:
      *result = LG_RECOVERY_ERR_BUSY;
      return true;
    case LG_NET_RECOVERY_ERROR_CAPACITY:
      *result = LG_RECOVERY_ERR_CAPACITY;
      return true;
    case LG_NET_RECOVERY_ERROR_TIMEOUT:
      /* The public transport ABI has no timeout-specific recovery error. */
      *result = LG_RECOVERY_ERR_HELPER_UNAVAILABLE;
      return true;
    default:
      return false;
  }
}

static void networkRecoveryClearNL(NetworkRecovery * recovery)
{
  recovery->queryPending  = false;
  recovery->queryID       = 0;
  recovery->infoValid     = false;
  recovery->info          = (LGNetRecoveryInfo) { 0 };
  recovery->version[0]    = '\0';
  recovery->requestSerial = 0;
  recovery->request       = LG_RECOVERY_REQ_NONE;
  recovery->ackSerial     = 0;
  recovery->ackRequest    = LG_RECOVERY_REQ_NONE;
  recovery->nextRequest   = 0;
  memset(recovery->requests, 0, sizeof(recovery->requests));
}

static bool networkRecoveryBeginQuery(NetworkRecovery * recovery,
    uint64_t * queryID, uint16_t * version)
{
  LG_LOCK(recovery->lock);
  if (!recovery->connected || !recovery->available)
  {
    LG_UNLOCK(recovery->lock);
    return false;
  }

  if (recovery->queryPending)
  {
    *queryID = recovery->queryID;
    *version = recovery->serviceVersion;
    LG_UNLOCK(recovery->lock);
    return true;
  }

  recovery->nextID       = networkRecoveryNextID(recovery->nextID);
  recovery->queryID      = recovery->nextID;
  recovery->queryPending = true;
  *queryID               = recovery->queryID;
  *version               = recovery->serviceVersion;
  lgResetEvent(recovery->queryEvent);
  LG_UNLOCK(recovery->lock);

  const LG_TransportStatus status = networkConnectionSend(
      recovery->connection, LG_NET_SERVICE_RECOVERY,
      LG_NET_RECOVERY_MESSAGE_GET_INFO, *version,
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL,
      0, *queryID, NULL, 0);
  if (status == LG_TRANSPORT_OK)
    return true;

  LG_LOCK(recovery->lock);
  if (recovery->queryID == *queryID)
  {
    recovery->queryPending = false;
    recovery->queryID      = 0;
  }
  LG_UNLOCK(recovery->lock);
  lgSignalEvent(recovery->queryEvent);
  return false;
}

bool networkRecoveryCreate(NetworkRecovery ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkRecovery * recovery = calloc(1, sizeof(*recovery));
  if (!recovery)
    return false;

  LG_LOCK_INIT(recovery->lock);
  recovery->queryEvent = lgCreateEvent(false, 0);
  if (!recovery->queryEvent)
  {
    LG_LOCK_FREE(recovery->lock);
    free(recovery);
    return false;
  }

  recovery->connection = connection;
  *result               = recovery;
  return true;
}

void networkRecoveryDestroy(NetworkRecovery ** result)
{
  if (!result || !*result)
    return;

  NetworkRecovery * recovery = *result;
  lgSignalEvent(recovery->queryEvent);
  lgFreeEvent(recovery->queryEvent);
  LG_LOCK_FREE(recovery->lock);
  free(recovery);
  *result = NULL;
}

void networkRecoveryConnectionState(NetworkRecovery * recovery,
    bool connected)
{
  uint16_t   version   = 0;
  const bool available = connected &&
    networkConnectionServiceAvailable(recovery->connection,
      LG_NET_SERVICE_RECOVERY, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(recovery->connection,
      LG_NET_SERVICE_RECOVERY, &version);

  LG_LOCK(recovery->lock);
  recovery->connected      = connected;
  recovery->available      = available;
  recovery->serviceVersion = available ? version : 0;
  networkRecoveryClearNL(recovery);
  LG_UNLOCK(recovery->lock);
  lgSignalEvent(recovery->queryEvent);

  if (available)
  {
    uint64_t queryID;
    networkRecoveryBeginQuery(recovery, &queryID, &version);
  }
}

static bool networkRecoveryInfoPacket(NetworkRecovery * recovery,
    const LGNetPacketView * packet)
{
  const LGNetEnvelopeFlags expected =
    LG_NET_ENVELOPE_RESPONSE |
    LG_NET_ENVELOPE_FINAL    |
    LG_NET_ENVELOPE_RELIABLE;
  if (packet->envelope.flags != expected ||
      packet->envelope.componentEpoch != 0 ||
      !packet->envelope.requestID)
    return false;

  LGNetRecoveryInfo info;
  if (lgNetRecoveryInfoDecode(&info,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  char version[LG_NET_RECOVERY_MAX_VERSION_LENGTH + 1U];
  if (info.versionLength)
    memcpy(version, info.version, info.versionLength);
  version[info.versionLength] = '\0';
  info.version                = NULL;

  LG_LOCK(recovery->lock);
  const bool valid = recovery->connected && recovery->available &&
    recovery->queryPending &&
    recovery->queryID == packet->envelope.requestID;
  if (valid)
  {
    if (!recovery->infoValid ||
        !networkRecoverySequenceNewer(
          recovery->info.statusSequence, info.statusSequence))
    {
      recovery->info = info;
      memcpy(recovery->version, version, info.versionLength + 1U);
    }
    recovery->infoValid    = true;
    recovery->queryPending = false;
    recovery->queryID      = 0;
  }
  LG_UNLOCK(recovery->lock);
  if (valid)
    lgSignalEvent(recovery->queryEvent);
  return valid;
}

static bool networkRecoveryStatusPacket(NetworkRecovery * recovery,
    const LGNetPacketView * packet)
{
  LGNetRecoveryStatus status;
  if (lgNetRecoveryStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      status.requestID != packet->envelope.requestID ||
      packet->envelope.componentEpoch != 0)
    return false;

  const bool               terminal =
    status.state == LG_NET_RECOVERY_STATE_NORMAL ||
    status.state == LG_NET_RECOVERY_STATE_ACTIVE ||
    status.state == LG_NET_RECOVERY_STATE_FAILED;
  const LGNetEnvelopeFlags expected = LG_NET_ENVELOPE_RELIABLE |
    (status.requestID ? LG_NET_ENVELOPE_RESPONSE : 0) |
    (terminal ? LG_NET_ENVELOPE_FINAL : 0);
  if (packet->envelope.flags != expected)
    return false;

  LG_LOCK(recovery->lock);
  struct NetworkRecoveryRequestEntry * request = status.requestID ?
    networkRecoveryFindRequestNL(recovery, status.requestID) : NULL;
  const bool                           valid   =
    recovery->connected && recovery->available &&
    recovery->infoValid && status.sessionID == recovery->info.sessionID &&
    (!status.requestID ||
      (request && request->session == status.sessionID));
  if (valid && (!networkRecoverySequenceNewer(
        recovery->info.statusSequence, status.statusSequence)))
  {
    recovery->info.statusSequence = status.statusSequence;
    recovery->info.state          = status.state;
    recovery->info.error          = status.error;
    recovery->info.flags          = status.flags;
    if (request && terminal)
    {
      request->acknowledged = true;
      recovery->ackSerial   = request->serial;
      recovery->ackRequest  = request->request;
    }
  }
  LG_UNLOCK(recovery->lock);
  return valid;
}

bool networkRecoveryPacket(NetworkRecovery * recovery,
    const LGNetPacketView * packet, bool datagram)
{
  if (!recovery || !packet || datagram ||
      packet->envelope.service != LG_NET_SERVICE_RECOVERY)
    return false;

  switch (packet->envelope.messageType)
  {
    case LG_NET_RECOVERY_MESSAGE_INFO:
      return networkRecoveryInfoPacket(recovery, packet);

    case LG_NET_RECOVERY_MESSAGE_STATUS:
      return networkRecoveryStatusPacket(recovery, packet);

    default:
      return false;
  }
}

static bool networkRecoveryCopyInfoNL(const NetworkRecovery * recovery,
    LG_RecoveryInfo * result)
{
  LG_RecoveryState state;
  LG_RecoveryError error;
  if (!recovery->infoValid ||
      !networkRecoveryState(recovery->info.state, &state) ||
      !networkRecoveryError(recovery->info.error, &error))
    return false;

  memset(result, 0, sizeof(*result));
  result->abiVersion = recovery->serviceVersion;
  if (recovery->info.capabilities & LG_NET_RECOVERY_CAP_DISPLAY)
    result->capabilities |= LG_RECOVERY_CAP_DISPLAY;
  result->instance   = recovery->info.sessionID;
  result->heartbeat  = (uint32_t)recovery->info.statusSequence;
  const size_t versionLength = recovery->info.versionLength <
    sizeof(result->producerVersion) - 1U ?
      recovery->info.versionLength : sizeof(result->producerVersion) - 1U;
  memcpy(result->producerVersion, recovery->version, versionLength);
  result->producerVersion[versionLength] = '\0';
  result->versionCount = 1;
  memcpy(result->versions[0].component, "Network", sizeof("Network"));
  result->versions[0].version = recovery->serviceVersion;

  result->requestSerial       = recovery->requestSerial;
  result->request             = recovery->request;
  result->ackSerial           = recovery->ackSerial;
  result->ackRequest          = recovery->ackRequest;
  result->state               = state;
  result->error               = error;
  return true;
}

LG_TransportStatus networkRecoveryGetInfo(NetworkRecovery * recovery,
    LG_RecoveryInfo * info)
{
  if (!recovery || !info)
    return LG_TRANSPORT_ERROR;

  uint64_t queryID;
  uint16_t version;
  if (!networkRecoveryBeginQuery(recovery, &queryID, &version))
  {
    LG_LOCK(recovery->lock);
    const bool connected = recovery->connected;
    const bool copied    = networkRecoveryCopyInfoNL(recovery, info);
    LG_UNLOCK(recovery->lock);
    if (copied)
      return LG_TRANSPORT_OK;
    return connected ? LG_TRANSPORT_UNAVAILABLE : LG_TRANSPORT_DISCONNECTED;
  }

  LG_LOCK(recovery->lock);
  bool copied = networkRecoveryCopyInfoNL(recovery, info);
  LG_UNLOCK(recovery->lock);
  if (copied)
    return LG_TRANSPORT_OK;

  lgWaitEvent(recovery->queryEvent, NETWORK_RECOVERY_QUERY_TIMEOUT_MS);
  LG_LOCK(recovery->lock);
  const bool connected = recovery->connected;
  copied               = networkRecoveryCopyInfoNL(recovery, info);
  LG_UNLOCK(recovery->lock);
  if (copied)
    return LG_TRANSPORT_OK;
  return connected ? LG_TRANSPORT_UNAVAILABLE : LG_TRANSPORT_DISCONNECTED;
}

LG_TransportStatus networkRecoveryRequest(NetworkRecovery * recovery,
    LG_RecoveryRequest request, uint64_t * instance, uint32_t * serial)
{
  if (!recovery || (request != LG_RECOVERY_REQ_NORMAL &&
        request != LG_RECOVERY_REQ_RECOVERY))
    return LG_TRANSPORT_ERROR;

  LG_RecoveryInfo          current;
  const LG_TransportStatus infoStatus =
    networkRecoveryGetInfo(recovery, &current);
  if (infoStatus != LG_TRANSPORT_OK)
    return infoStatus;
  if (!(current.capabilities & LG_RECOVERY_CAP_DISPLAY))
    return LG_TRANSPORT_UNAVAILABLE;

  uint64_t             id;
  uint32_t             requestSerial;
  uint16_t             version;
  LGNetRecoveryRequest wire;
  LG_LOCK(recovery->lock);
  struct NetworkRecoveryRequestEntry * entry =
    networkRecoveryReserveRequestNL(recovery);
  if (!entry || !recovery->connected || !recovery->available ||
      !recovery->infoValid ||
      recovery->info.sessionID != current.instance)
  {
    LG_UNLOCK(recovery->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  id                   = networkRecoveryNextID(recovery->nextID);
  requestSerial        = networkRecoveryNextSerial(recovery->nextSerial);
  recovery->nextID     = id;
  recovery->nextSerial = requestSerial;
  version              = recovery->serviceVersion;
  const uint32_t timeout = recovery->info.maxTransitionMs ?
    recovery->info.maxTransitionMs : NETWORK_RECOVERY_DEFAULT_TIMEOUT_MS;
  wire = (LGNetRecoveryRequest)
  {
    .requestID              = id,
    .sessionID              = recovery->info.sessionID,
    .expectedStatusSequence = recovery->info.statusSequence,
    .timeoutMs              = timeout > LG_NET_RECOVERY_MAX_TIMEOUT_MS ?
      LG_NET_RECOVERY_MAX_TIMEOUT_MS : timeout,
    .flags                  = request == LG_RECOVERY_REQ_RECOVERY ?
      LG_NET_RECOVERY_REQUEST_ACTIVE : 0,
  };
  *entry = (struct NetworkRecoveryRequestEntry)
  {
    .id             = id,
    .session        = wire.sessionID,
    .serial         = requestSerial,
    .request        = request,
    .acknowledged   = false,
  };
  LG_UNLOCK(recovery->lock);

  uint8_t payload[LG_NET_RECOVERY_REQUEST_WIRE_SIZE];
  if (!lgNetRecoveryRequestEncode(payload, sizeof(payload), &wire))
    goto fail;

  const LG_TransportStatus status = networkConnectionSend(
      recovery->connection, LG_NET_SERVICE_RECOVERY,
      LG_NET_RECOVERY_MESSAGE_REQUEST, version,
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL,
      0, id, payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
  {
    LG_LOCK(recovery->lock);
    entry = networkRecoveryFindRequestNL(recovery, id);
    if (entry)
      *entry = (struct NetworkRecoveryRequestEntry) { 0 };
    LG_UNLOCK(recovery->lock);
    return status;
  }

  LG_LOCK(recovery->lock);
  recovery->requestSerial = requestSerial;
  recovery->request       = request;
  LG_UNLOCK(recovery->lock);
  if (instance)
    *instance = wire.sessionID;
  if (serial)
    *serial   = requestSerial;
  return LG_TRANSPORT_OK;

fail:
  LG_LOCK(recovery->lock);
  entry = networkRecoveryFindRequestNL(recovery, id);
  if (entry)
    *entry = (struct NetworkRecoveryRequestEntry) { 0 };
  LG_UNLOCK(recovery->lock);
  return LG_TRANSPORT_ERROR;
}
