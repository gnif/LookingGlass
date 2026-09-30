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

#include "control.h"

#include "common/locking.h"

#include <LGProtocol/NetworkServices.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_CONTROL_STATUS_CAPACITY 64U

struct NetworkControlEntry
{
  uint64_t               id;
  LGNetControlStatusCode status;
  bool                   used;
};

struct NetworkControl
{
  NetworkConnection * connection;
  LG_Lock             lock;

  bool     connected;
  bool     available;
  uint16_t serviceVersion;
  uint64_t nextID;
  unsigned nextEntry;

  struct NetworkControlEntry entries[NETWORK_CONTROL_STATUS_CAPACITY];
};

static uint64_t networkControlNextID(uint64_t id)
{
  ++id;
  return id ? id : 1U;
}

static bool networkControlTerminal(LGNetControlStatusCode status)
{
  return status != LG_NET_CONTROL_STATUS_PENDING;
}

static struct NetworkControlEntry * networkControlFindNL(
    NetworkControl * control, uint64_t id)
{
  for (size_t index = 0; index < NETWORK_CONTROL_STATUS_CAPACITY; ++index)
    if (control->entries[index].used && control->entries[index].id == id)
      return &control->entries[index];
  return NULL;
}

static struct NetworkControlEntry * networkControlReserveNL(
    NetworkControl * control, uint64_t id)
{
  for (size_t offset = 0;
      offset < NETWORK_CONTROL_STATUS_CAPACITY; ++offset)
  {
    const unsigned               index =
      (control->nextEntry + offset) % NETWORK_CONTROL_STATUS_CAPACITY;
    struct NetworkControlEntry * entry = &control->entries[index];
    if (entry->used && !networkControlTerminal(entry->status))
      continue;

    *entry             = (struct NetworkControlEntry)
    {
      .id     = id,
      .status = LG_NET_CONTROL_STATUS_PENDING,
      .used   = true,
    };
    control->nextEntry =
      (index + 1U) % NETWORK_CONTROL_STATUS_CAPACITY;
    return entry;
  }
  return NULL;
}

bool networkControlCreate(NetworkControl ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkControl * control = calloc(1, sizeof(*control));
  if (!control)
    return false;

  LG_LOCK_INIT(control->lock);
  control->connection = connection;
  *result             = control;
  return true;
}

void networkControlDestroy(NetworkControl ** result)
{
  if (!result || !*result)
    return;

  NetworkControl * control = *result;
  LG_LOCK_FREE(control->lock);
  free(control);
  *result = NULL;
}

void networkControlConnectionState(NetworkControl * control, bool connected)
{
  uint16_t   version   = 0;
  const bool available = connected &&
    networkConnectionServiceAvailable(control->connection,
      LG_NET_SERVICE_CONTROL, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(control->connection,
      LG_NET_SERVICE_CONTROL, &version);

  LG_LOCK(control->lock);
  control->connected      = connected;
  control->available      = available;
  control->serviceVersion = available ? version : 0;
  control->nextEntry      = 0;
  memset(control->entries, 0, sizeof(control->entries));
  LG_UNLOCK(control->lock);
}

bool networkControlPacket(NetworkControl * control,
    const LGNetPacketView * packet, bool datagram)
{
  if (!control || !packet || datagram ||
      packet->envelope.service != LG_NET_SERVICE_CONTROL ||
      packet->envelope.messageType != LG_NET_CONTROL_MESSAGE_STATUS ||
      packet->envelope.componentEpoch != 0 ||
      !packet->envelope.requestID)
    return false;

  LGNetControlStatus status;
  if (lgNetControlStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      status.controlID != packet->envelope.requestID)
    return false;

  const bool               terminal =
    status.status != LG_NET_CONTROL_STATUS_PENDING;
  const LGNetEnvelopeFlags expected =
    LG_NET_ENVELOPE_RESPONSE |
    LG_NET_ENVELOPE_RELIABLE |
    (terminal ? LG_NET_ENVELOPE_FINAL : 0);
  if (packet->envelope.flags != expected)
    return false;

  LG_LOCK(control->lock);
  struct NetworkControlEntry * entry =
    networkControlFindNL(control, status.controlID);
  const bool                   valid =
    control->connected && control->available && entry;
  if (valid)
    entry->status = status.status;
  LG_UNLOCK(control->lock);
  return valid;
}

static bool networkControlFrameFlags(
    LG_TransportFrameScheduleFlags source,
    LGNetControlFrameScheduleFlags * destination)
{
  const LG_TransportFrameScheduleFlags known =
    LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE    |
    LG_TRANSPORT_FRAME_SCHEDULE_RELEASE   |
    LG_TRANSPORT_FRAME_SCHEDULE_RESET     |
    LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE;
  if (!destination || !source || (source & ~known))
    return false;

  *destination = 0;
  if (source & LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE)
    *destination |= LG_NET_CONTROL_FRAME_SCHEDULE_ACTIVE;
  if (source & LG_TRANSPORT_FRAME_SCHEDULE_RELEASE)
    *destination |= LG_NET_CONTROL_FRAME_SCHEDULE_RELEASE;
  if (source & LG_TRANSPORT_FRAME_SCHEDULE_RESET)
    *destination |= LG_NET_CONTROL_FRAME_SCHEDULE_RESET;
  if (source & LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE)
    *destination |= LG_NET_CONTROL_FRAME_SCHEDULE_IMMEDIATE;
  return true;
}

LG_TransportStatus networkControlSend(NetworkControl * control,
    const LG_TransportControl * request, LG_TransportControlToken * token)
{
  if (!control || !request || !token)
    return LG_TRANSPORT_ERROR;

  uint64_t id;
  uint16_t version;
  LG_LOCK(control->lock);
  if (!control->connected)
  {
    LG_UNLOCK(control->lock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!control->available)
  {
    LG_UNLOCK(control->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  id = networkControlNextID(control->nextID);
  if (!networkControlReserveNL(control, id))
  {
    LG_UNLOCK(control->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }
  control->nextID = id;
  version         = control->serviceVersion;
  LG_UNLOCK(control->lock);

  uint8_t  payload[LG_NET_CONTROL_FRAME_SCHEDULE_WIRE_SIZE];
  uint32_t payloadLength;
  uint16_t messageType;
  bool     encoded;
  switch (request->type)
  {
    case LG_TRANSPORT_CONTROL_SET_CURSOR_POS:
    {
      const LGNetControlCursorPosition position =
      {
        .controlID = id,
        .x         = request->cursorPos.x,
        .y         = request->cursorPos.y,
      };
      messageType   = LG_NET_CONTROL_MESSAGE_CURSOR_POSITION;
      payloadLength = LG_NET_CONTROL_CURSOR_POSITION_WIRE_SIZE;
      encoded       = lgNetControlCursorPositionEncode(
          payload, payloadLength, &position);
      break;
    }

    case LG_TRANSPORT_CONTROL_WINDOW_SIZE:
    {
      const LGNetControlDisplaySize display =
      {
        .controlID = id,
        .width     = request->windowSize.width,
        .height    = request->windowSize.height,
      };
      messageType   = LG_NET_CONTROL_MESSAGE_DISPLAY_SIZE;
      payloadLength = LG_NET_CONTROL_DISPLAY_SIZE_WIRE_SIZE;
      encoded       = lgNetControlDisplaySizeEncode(
          payload, payloadLength, &display);
      break;
    }

    case LG_TRANSPORT_CONTROL_FRAME_SCHEDULE:
    {
      LGNetControlFrameScheduleFlags flags;
      if (!networkControlFrameFlags(
            request->frameSchedule.flags, &flags))
        encoded = false;
      else
      {
        const LGNetControlFrameSchedule schedule =
        {
          .controlID                  = id,
          .generation                 = request->frameSchedule.generation,
          .flags                      = flags,
          .periodNs                   = request->frameSchedule.period,
          .targetSlackNs              = request->frameSchedule.targetSlack,
          .phaseErrorNs               = request->frameSchedule.phaseError,
          .feedbackFrameSerial        =
            request->frameSchedule.feedbackFrameSerial,
          .feedbackScheduleEpoch      =
            request->frameSchedule.feedbackScheduleEpoch,
          .feedbackDeadlineSerial     =
            request->frameSchedule.feedbackDeadlineSerial,
          .leaseMs                    = request->frameSchedule.lease,
        };
        encoded = lgNetControlFrameScheduleEncode(
            payload, sizeof(payload), &schedule);
      }
      messageType   = LG_NET_CONTROL_MESSAGE_FRAME_SCHEDULE;
      payloadLength = LG_NET_CONTROL_FRAME_SCHEDULE_WIRE_SIZE;
      break;
    }

    default:
      encoded       = false;
      messageType   = 0;
      payloadLength = 0;
      break;
  }

  LG_TransportStatus status = LG_TRANSPORT_ERROR;
  if (encoded)
    status = networkConnectionSend(control->connection,
        LG_NET_SERVICE_CONTROL, messageType, version,
        LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL,
        0, id, payload, payloadLength);

  if (status != LG_TRANSPORT_OK)
  {
    LG_LOCK(control->lock);
    struct NetworkControlEntry * entry = networkControlFindNL(control, id);
    if (entry)
      *entry = (struct NetworkControlEntry) { 0 };
    LG_UNLOCK(control->lock);
    return status;
  }

  *token = id;
  return LG_TRANSPORT_OK;
}

LG_TransportStatus networkControlStatus(NetworkControl * control,
    LG_TransportControlToken token)
{
  if (!control || !token)
    return LG_TRANSPORT_ERROR;

  LG_LOCK(control->lock);
  if (!control->connected)
  {
    LG_UNLOCK(control->lock);
    return LG_TRANSPORT_DISCONNECTED;
  }
  if (!control->available)
  {
    LG_UNLOCK(control->lock);
    return LG_TRANSPORT_UNAVAILABLE;
  }

  const struct NetworkControlEntry * entry  =
    networkControlFindNL(control, token);
  const LGNetControlStatusCode       status = entry ? entry->status : 0;
  LG_UNLOCK(control->lock);

  switch (status)
  {
    case LG_NET_CONTROL_STATUS_APPLIED:
      return LG_TRANSPORT_OK;

    case LG_NET_CONTROL_STATUS_PENDING:
      return LG_TRANSPORT_UNAVAILABLE;

    case LG_NET_CONTROL_STATUS_BUSY:
    case LG_NET_CONTROL_STATUS_UNSUPPORTED:
    case LG_NET_CONTROL_STATUS_STALE:
    case LG_NET_CONTROL_STATUS_INVALID:
    case LG_NET_CONTROL_STATUS_ERROR:
    default:
      return LG_TRANSPORT_ERROR;
  }
}
