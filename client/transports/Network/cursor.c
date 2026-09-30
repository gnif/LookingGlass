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

#include "cursor.h"

#include "common/event.h"
#include "common/locking.h"

#include <LGProtocol/NetworkServices.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_CURSOR_QUEUE_CAPACITY 64U

struct NetworkCursorItem
{
  struct NetworkCursorItem * next;
  LG_TransportPointer        pointer;
  uint8_t                  * shape;
  KVMFRColorTransform      * colorTransform;
};

struct NetworkCursorStatusReply
{
  bool                  send;
  uint64_t              stateSequence;
  uint64_t              appliedShapeID;
  uint64_t              appliedPositionID;
  uint64_t              appliedTransformID;
  LGNetCursorStatusCode status;
  uint32_t              detail;
};

struct NetworkCursor
{
  NetworkConnection * connection;
  LGEvent           * event;
  LG_Lock             lock;

  bool     connected;
  bool     available;
  bool     cancelled;
  uint16_t serviceVersion;

  struct NetworkCursorItem * head;
  struct NetworkCursorItem * tail;
  struct NetworkCursorItem * outstanding;
  unsigned                   count;

  LGNetCursorShape  shape;
  uint8_t         * shapeData;
  bool              shapeValid;
  bool              shapeSupported;

  LGNetCursorPosition position;
  bool                positionValid;
  bool                positionSupported;

  LGNetCursorTransform transform;
  bool                 transformValid;
  bool                 transformSupported;

  LGNetCursorState pendingState;
  bool             pendingStateValid;
  bool             stateApplied;
  uint64_t         appliedStateSequence;
  uint64_t         appliedShapeID;
  uint64_t         appliedPositionID;
  uint64_t         appliedTransformID;
  bool             visible;

  uint64_t colorUpdateID;
  uint32_t sdrWhiteLevel;
};

static void networkCursorItemFree(struct NetworkCursorItem * item)
{
  if (!item)
    return;
  free(item->colorTransform);
  free(item->shape);
  free(item);
}

static void networkCursorQueueClearNL(NetworkCursor * cursor)
{
  while (cursor->head)
  {
    struct NetworkCursorItem * item = cursor->head;
    cursor->head = item->next;
    networkCursorItemFree(item);
  }
  cursor->tail  = NULL;
  cursor->count = 0;
}

static void networkCursorCacheClearNL(NetworkCursor * cursor)
{
  free(cursor->shapeData);
  cursor->shapeData            = NULL;
  cursor->shape                = (LGNetCursorShape) { 0 };
  cursor->shapeValid           = false;
  cursor->shapeSupported       = false;
  cursor->position             = (LGNetCursorPosition) { 0 };
  cursor->positionValid        = false;
  cursor->positionSupported    = false;
  cursor->transform            = (LGNetCursorTransform) { 0 };
  cursor->transformValid       = false;
  cursor->transformSupported   = false;
  cursor->pendingState         = (LGNetCursorState) { 0 };
  cursor->pendingStateValid    = false;
  cursor->stateApplied         = false;
  cursor->appliedStateSequence = 0;
  cursor->appliedShapeID       = 0;
  cursor->appliedPositionID    = 0;
  cursor->appliedTransformID   = 0;
  cursor->visible              = false;
  cursor->colorUpdateID        = 0;
  cursor->sdrWhiteLevel        = LG_SDR_WHITE_LEVEL_DEFAULT;
}

static bool networkCursorItemCoalescible(
    const struct NetworkCursorItem * item)
{
  return item && !(item->pointer.flags &
    (LG_TRANSPORT_POINTER_SHAPE |
      LG_TRANSPORT_POINTER_COLOR_TRANSFORM));
}

static bool networkCursorQueueNL(NetworkCursor * cursor,
    struct NetworkCursorItem * item)
{
  if (networkCursorItemCoalescible(item) &&
      networkCursorItemCoalescible(cursor->tail))
  {
    cursor->tail->pointer = item->pointer;
    networkCursorItemFree(item);
    lgSignalEvent(cursor->event);
    return true;
  }

  if (cursor->count == NETWORK_CURSOR_QUEUE_CAPACITY)
  {
    struct NetworkCursorItem * previous = NULL;
    struct NetworkCursorItem * current  = cursor->head;
    while (current && !networkCursorItemCoalescible(current))
    {
      previous = current;
      current  = current->next;
    }
    if (!current)
      return false;
    if (previous)
      previous->next = current->next;
    else
      cursor->head = current->next;
    if (cursor->tail == current)
      cursor->tail = previous;
    --cursor->count;
    networkCursorItemFree(current);
  }

  item->next = NULL;
  if (cursor->tail)
    cursor->tail->next = item;
  else
    cursor->head = item;
  cursor->tail = item;
  ++cursor->count;
  lgSignalEvent(cursor->event);
  return true;
}

static bool networkCursorShapeType(LGNetCursorFormat format,
    KVMFRCursorType * type)
{
  switch (format)
  {
    case LG_NET_CURSOR_FORMAT_BGRA8_PREMULTIPLIED:
    case LG_NET_CURSOR_FORMAT_RGBA8_PREMULTIPLIED:
      *type = CURSOR_TYPE_COLOR;
      return true;
    case LG_NET_CURSOR_FORMAT_MASKED_COLOR:
      *type = CURSOR_TYPE_MASKED_COLOR;
      return true;
    case LG_NET_CURSOR_FORMAT_MONOCHROME:
      *type = CURSOR_TYPE_MONOCHROME;
      return true;
    default:
      return false;
  }
}

static bool networkCursorPositionSupported(
    const LGNetCursorPosition * position)
{
  return position->displayID == 0 &&
    !(position->flags & LG_NET_CURSOR_POSITION_RELATIVE) &&
    position->x >= INT16_MIN && position->x <= INT16_MAX &&
    position->y >= INT16_MIN && position->y <= INT16_MAX;
}

static bool networkCursorTransformSupported(
    const LGNetCursorTransform * transform)
{
  return transform->displayID == 0 &&
    transform->rotation == LG_NET_CURSOR_ROTATION_0 && !transform->flags &&
    transform->sourceWidth == transform->targetWidth &&
    transform->sourceHeight == transform->targetHeight &&
    transform->offsetX == 0 && transform->offsetY == 0 &&
    transform->scaleNumerator == transform->scaleDenominator;
}

static bool networkCursorShapeCopy(NetworkCursor * cursor,
    const LGNetCursorShape * shape)
{
  KVMFRCursorType type;
  const bool      supported = networkCursorShapeType(shape->format, &type) &&
    !(shape->flags & LG_NET_CURSOR_SHAPE_ANIMATED) &&
    shape->hotspotX <= INT16_MAX && shape->hotspotY <= INT16_MAX;

  uint8_t * data = malloc(shape->dataLength);
  if (!data)
    return false;
  memcpy(data, shape->data, shape->dataLength);

  if (shape->format == LG_NET_CURSOR_FORMAT_RGBA8_PREMULTIPLIED)
    for (uint32_t y = 0; y < shape->height; ++y)
      for (uint32_t x = 0; x < shape->width; ++x)
      {
        uint8_t       * pixel = data + (size_t)y * shape->pitch + x * 4U;
        const uint8_t   red   = pixel[0];
        pixel[0]               = pixel[2];
        pixel[2]               = red;
      }

  free(cursor->shapeData);
  cursor->shape          = *shape;
  cursor->shapeData      = data;
  cursor->shape.data     = data;
  cursor->shapeValid     = true;
  cursor->shapeSupported = supported;
  return true;
}

static bool networkCursorStateMissingNL(const NetworkCursor * cursor,
    const LGNetCursorState * state, bool * impossible)
{
  *impossible = false;
  if (state->flags & LG_NET_CURSOR_STATE_SHAPE_VALID)
  {
    if (!cursor->shapeValid)
      return true;
    if (cursor->shape.shapeID != state->shapeID)
    {
      *impossible = cursor->shape.shapeID > state->shapeID;
      return true;
    }
  }
  if (state->flags & LG_NET_CURSOR_STATE_POSITION_VALID)
  {
    if (!cursor->positionValid)
      return true;
    if (cursor->position.updateID != state->positionID)
    {
      *impossible = cursor->position.updateID > state->positionID;
      return true;
    }
  }
  if (state->flags & LG_NET_CURSOR_STATE_TRANSFORM_VALID)
  {
    if (!cursor->transformValid)
      return true;
    if (cursor->transform.transformID != state->transformID)
    {
      *impossible = cursor->transform.transformID > state->transformID;
      return true;
    }
  }
  return false;
}

static struct NetworkCursorStatusReply networkCursorApplyStateNL(
    NetworkCursor * cursor)
{
  struct NetworkCursorStatusReply reply = { 0 };
  if (!cursor->pendingStateValid)
    return reply;

  const LGNetCursorState state = cursor->pendingState;
  bool                   impossible;
  if (networkCursorStateMissingNL(cursor, &state, &impossible))
  {
    if (!impossible)
      return reply;
    reply.send                 = true;
    reply.stateSequence        = state.stateSequence;
    reply.status               = LG_NET_CURSOR_STATUS_INVALID;
    cursor->pendingStateValid  = false;
    return reply;
  }

  const bool shapeValid     =
    state.flags & LG_NET_CURSOR_STATE_SHAPE_VALID;
  const bool positionValid  =
    state.flags & LG_NET_CURSOR_STATE_POSITION_VALID;
  const bool transformValid =
    state.flags & LG_NET_CURSOR_STATE_TRANSFORM_VALID;
  if (state.displayID != 0 ||
      (shapeValid && !cursor->shapeSupported) ||
      (positionValid && (!cursor->positionSupported ||
        cursor->position.displayID != state.displayID)) ||
      (transformValid && (!cursor->transformSupported ||
        cursor->transform.displayID != state.displayID)))
  {
    reply.send                 = true;
    reply.stateSequence        = state.stateSequence;
    reply.status               = LG_NET_CURSOR_STATUS_UNSUPPORTED;
    cursor->pendingStateValid  = false;
    return reply;
  }

  const bool newShape          = shapeValid &&
    cursor->appliedShapeID != state.shapeID;
  const bool newPosition       = positionValid &&
    cursor->appliedPositionID != state.positionID;
  const bool visible           =
    !!(state.flags & LG_NET_CURSOR_STATE_VISIBLE) &&
    !(shapeValid &&
      (cursor->shape.flags & LG_NET_CURSOR_SHAPE_HIDDEN));
  const bool visibilityChanged =
    !cursor->stateApplied || cursor->visible != visible;

  struct NetworkCursorItem * item = NULL;
  if (newShape || newPosition || visibilityChanged)
  {
    item = calloc(1, sizeof(*item));
    if (!item)
      goto capacity;

    item->pointer.flags         = LG_TRANSPORT_POINTER_VISIBLE_VALID;
    item->pointer.sdrWhiteLevel = cursor->sdrWhiteLevel;
    if (visible)
      item->pointer.flags |= LG_TRANSPORT_POINTER_VISIBLE;
    if (newPosition)
    {
      item->pointer.flags |= LG_TRANSPORT_POINTER_POSITION;
      item->pointer.x      = (int16_t)cursor->position.x;
      item->pointer.y      = (int16_t)cursor->position.y;
    }
    if (newShape)
    {
      if (!networkCursorShapeType(cursor->shape.format,
            &item->pointer.type))
        goto capacity;
      item->shape = malloc(cursor->shape.dataLength);
      if (!item->shape)
        goto capacity;
      memcpy(item->shape, cursor->shapeData, cursor->shape.dataLength);
      item->pointer.flags  |= LG_TRANSPORT_POINTER_SHAPE;
      item->pointer.hx      = (int16_t)cursor->shape.hotspotX;
      item->pointer.hy      = (int16_t)cursor->shape.hotspotY;
      item->pointer.width   = cursor->shape.width;
      item->pointer.height  = cursor->shape.height;
      item->pointer.pitch   = cursor->shape.pitch;
      item->pointer.shape   = item->shape;
    }
    if (!networkCursorQueueNL(cursor, item))
      goto capacity;
    item = NULL;
  }

  cursor->stateApplied         = true;
  cursor->appliedStateSequence = state.stateSequence;
  cursor->appliedShapeID       = shapeValid ? state.shapeID : 0;
  cursor->appliedPositionID    = positionValid ? state.positionID : 0;
  cursor->appliedTransformID   = transformValid ? state.transformID : 0;
  cursor->visible              = visible;
  cursor->pendingStateValid    = false;
  reply.send                   = true;
  reply.stateSequence          = state.stateSequence;
  reply.appliedShapeID         = cursor->appliedShapeID;
  reply.appliedPositionID      = cursor->appliedPositionID;
  reply.appliedTransformID     = cursor->appliedTransformID;
  reply.status                 = LG_NET_CURSOR_STATUS_APPLIED;
  return reply;

capacity:
  networkCursorItemFree(item);
  cursor->pendingStateValid = false;
  reply.send                = true;
  reply.stateSequence       = state.stateSequence;
  reply.status              = LG_NET_CURSOR_STATUS_ERROR;
  return reply;
}

static bool networkCursorSendStatus(NetworkCursor * cursor,
    const struct NetworkCursorStatusReply * reply)
{
  if (!reply->send)
    return true;

  uint16_t version;
  LG_LOCK(cursor->lock);
  const bool available = cursor->connected && cursor->available;
  version              = cursor->serviceVersion;
  LG_UNLOCK(cursor->lock);
  if (!available)
    return false;

  const LGNetCursorStatus status =
  {
    .stateSequence      = reply->stateSequence,
    .appliedShapeID     = reply->appliedShapeID,
    .appliedPositionID  = reply->appliedPositionID,
    .appliedTransformID = reply->appliedTransformID,
    .status             = reply->status,
    .detail             = reply->detail,
  };
  uint8_t payload[LG_NET_CURSOR_STATUS_WIRE_SIZE];
  return lgNetCursorStatusEncode(payload, sizeof(payload), &status) &&
    networkConnectionSend(cursor->connection, LG_NET_SERVICE_CURSOR,
      LG_NET_CURSOR_MESSAGE_STATUS, version,
      LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL,
      0, status.stateSequence, payload, sizeof(payload)) == LG_TRANSPORT_OK;
}

bool networkCursorCreate(NetworkCursor ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkCursor * cursor = calloc(1, sizeof(*cursor));
  if (!cursor)
    return false;

  LG_LOCK_INIT(cursor->lock);
  cursor->event = lgCreateEvent(true, 0);
  if (!cursor->event)
  {
    LG_LOCK_FREE(cursor->lock);
    free(cursor);
    return false;
  }

  cursor->connection    = connection;
  cursor->sdrWhiteLevel = LG_SDR_WHITE_LEVEL_DEFAULT;
  *result               = cursor;
  return true;
}

void networkCursorDestroy(NetworkCursor ** result)
{
  if (!result || !*result)
    return;

  NetworkCursor * cursor = *result;
  LG_LOCK(cursor->lock);
  networkCursorQueueClearNL(cursor);
  networkCursorItemFree(cursor->outstanding);
  cursor->outstanding = NULL;
  networkCursorCacheClearNL(cursor);
  LG_UNLOCK(cursor->lock);
  lgSignalEvent(cursor->event);
  lgFreeEvent(cursor->event);
  LG_LOCK_FREE(cursor->lock);
  free(cursor);
  *result = NULL;
}

bool networkCursorConnectionState(NetworkCursor * cursor, bool connected)
{
  uint16_t   version   = 0;
  const bool available = connected &&
    networkConnectionServiceAvailable(cursor->connection,
      LG_NET_SERVICE_CURSOR, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(cursor->connection,
      LG_NET_SERVICE_CURSOR, &version);

  LG_LOCK(cursor->lock);
  cursor->connected      = connected;
  cursor->available      = available;
  cursor->cancelled      = false;
  cursor->serviceVersion = available ? version : 0;
  networkCursorQueueClearNL(cursor);
  networkCursorCacheClearNL(cursor);
  LG_UNLOCK(cursor->lock);
  lgSignalEvent(cursor->event);
  return available;
}

static bool networkCursorEnvelope(const LGNetPacketView * packet,
    bool datagram, uint64_t id)
{
  const LGNetEnvelopeFlags expected = LG_NET_ENVELOPE_FINAL |
    (datagram ? LG_NET_ENVELOPE_DATAGRAM : LG_NET_ENVELOPE_RELIABLE);
  return packet->envelope.flags == expected &&
    packet->envelope.componentEpoch == 0 &&
    packet->envelope.requestID == id;
}

static bool networkCursorPositionPacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  LGNetCursorPosition position;
  if (lgNetCursorPositionDecode(&position,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      !networkCursorEnvelope(packet, datagram, position.updateID))
    return false;

  struct NetworkCursorStatusReply reply = { 0 };
  LG_LOCK(cursor->lock);
  const bool valid = cursor->connected && cursor->available;
  if (valid && (!cursor->positionValid ||
        position.updateID > cursor->position.updateID))
  {
    cursor->position          = position;
    cursor->positionValid     = true;
    cursor->positionSupported = networkCursorPositionSupported(&position);
    reply                     = networkCursorApplyStateNL(cursor);
  }
  LG_UNLOCK(cursor->lock);
  return valid && networkCursorSendStatus(cursor, &reply);
}

static bool networkCursorShapePacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  if (datagram)
    return false;

  LGNetCursorShape shape;
  if (lgNetCursorShapeDecode(&shape,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      !networkCursorEnvelope(packet, false, shape.shapeID))
    return false;

  struct NetworkCursorStatusReply reply = { 0 };
  LG_LOCK(cursor->lock);
  const bool valid = cursor->connected && cursor->available;
  bool copied      = true;
  if (valid && (!cursor->shapeValid || shape.shapeID > cursor->shape.shapeID))
  {
    copied = networkCursorShapeCopy(cursor, &shape);
    if (copied)
      reply = networkCursorApplyStateNL(cursor);
  }
  LG_UNLOCK(cursor->lock);
  return valid && copied && networkCursorSendStatus(cursor, &reply);
}

static bool networkCursorTransformPacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  if (datagram)
    return false;

  LGNetCursorTransform transform;
  if (lgNetCursorTransformDecode(&transform,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      !networkCursorEnvelope(packet, false, transform.transformID))
    return false;

  struct NetworkCursorStatusReply reply = { 0 };
  LG_LOCK(cursor->lock);
  const bool valid = cursor->connected && cursor->available;
  if (valid && (!cursor->transformValid ||
        transform.transformID > cursor->transform.transformID))
  {
    cursor->transform          = transform;
    cursor->transformValid     = true;
    cursor->transformSupported =
      networkCursorTransformSupported(&transform);
    reply                      = networkCursorApplyStateNL(cursor);
  }
  LG_UNLOCK(cursor->lock);
  return valid && networkCursorSendStatus(cursor, &reply);
}

static bool networkCursorStatePacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  if (datagram)
    return false;

  LGNetCursorState state;
  if (lgNetCursorStateDecode(&state,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      !networkCursorEnvelope(packet, false, state.stateSequence))
    return false;

  struct NetworkCursorStatusReply reply = { 0 };
  LG_LOCK(cursor->lock);
  const bool valid = cursor->connected && cursor->available;
  if (valid && state.stateSequence > cursor->appliedStateSequence &&
      (!cursor->pendingStateValid ||
        state.stateSequence > cursor->pendingState.stateSequence))
  {
    cursor->pendingState      = state;
    cursor->pendingStateValid = true;
    reply                     = networkCursorApplyStateNL(cursor);
  }
  LG_UNLOCK(cursor->lock);
  return valid && networkCursorSendStatus(cursor, &reply);
}

static bool networkCursorColorPacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  if (datagram)
    return false;

  LGNetCursorColorTransform transform;
  if (lgNetCursorColorTransformDecode(&transform,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      !networkCursorEnvelope(packet, false, transform.updateID))
    return false;

  struct NetworkCursorItem * item = calloc(1, sizeof(*item));
  if (!item)
    return false;
  item->colorTransform = calloc(1, sizeof(*item->colorTransform));
  if (!item->colorTransform)
  {
    networkCursorItemFree(item);
    return false;
  }

  if (transform.flags & LG_NET_CURSOR_COLOR_TRANSFORM_MATRIX)
    item->colorTransform->flags |= LG_COLOR_TRANSFORM_MATRIX;
  if (transform.flags & LG_NET_CURSOR_COLOR_TRANSFORM_LUT)
    item->colorTransform->flags |= LG_COLOR_TRANSFORM_LUT;
  memcpy(item->colorTransform->matrix, transform.matrix,
      sizeof(transform.matrix));
  item->colorTransform->scalar = transform.scalar;
  if ((transform.flags & LG_NET_CURSOR_COLOR_TRANSFORM_LUT) &&
      !lgNetCursorColorLUTDecode((float *)item->colorTransform->lut,
        transform.lut, LG_NET_CURSOR_COLOR_LUT_BYTES))
  {
    networkCursorItemFree(item);
    return false;
  }
  item->pointer.flags          = LG_TRANSPORT_POINTER_COLOR_TRANSFORM;
  item->pointer.sdrWhiteLevel  = transform.sdrWhiteLevel;
  item->pointer.colorTransform = item->colorTransform;

  LG_LOCK(cursor->lock);
  const bool valid = cursor->connected && cursor->available;
  bool queued      = true;
  if (valid && transform.updateID > cursor->colorUpdateID)
  {
    cursor->colorUpdateID = transform.updateID;
    cursor->sdrWhiteLevel = transform.sdrWhiteLevel;
    if (cursor->stateApplied)
    {
      item->pointer.flags |= LG_TRANSPORT_POINTER_VISIBLE_VALID;
      if (cursor->visible)
        item->pointer.flags |= LG_TRANSPORT_POINTER_VISIBLE;
    }
    queued = networkCursorQueueNL(cursor, item);
    if (queued)
      item = NULL;
  }
  LG_UNLOCK(cursor->lock);
  networkCursorItemFree(item);
  return valid && queued;
}

static bool networkCursorStatusPacket(const LGNetPacketView * packet,
    bool datagram)
{
  if (datagram)
    return false;

  LGNetCursorStatus status;
  if (lgNetCursorStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      packet->envelope.componentEpoch != 0 ||
      packet->envelope.requestID != status.stateSequence)
    return false;

  const LGNetEnvelopeFlags base =
    LG_NET_ENVELOPE_FINAL | LG_NET_ENVELOPE_RELIABLE;
  return packet->envelope.flags == base ||
    packet->envelope.flags == (base | LG_NET_ENVELOPE_RESPONSE);
}

bool networkCursorPacket(NetworkCursor * cursor,
    const LGNetPacketView * packet, bool datagram)
{
  if (!cursor || !packet ||
      packet->envelope.service != LG_NET_SERVICE_CURSOR)
    return false;

  switch (packet->envelope.messageType)
  {
    case LG_NET_CURSOR_MESSAGE_STATE:
      return networkCursorStatePacket(cursor, packet, datagram);
    case LG_NET_CURSOR_MESSAGE_POSITION:
      return networkCursorPositionPacket(cursor, packet, datagram);
    case LG_NET_CURSOR_MESSAGE_SHAPE:
      return networkCursorShapePacket(cursor, packet, datagram);
    case LG_NET_CURSOR_MESSAGE_TRANSFORM:
      return networkCursorTransformPacket(cursor, packet, datagram);
    case LG_NET_CURSOR_MESSAGE_STATUS:
      return networkCursorStatusPacket(packet, datagram);
    case LG_NET_CURSOR_MESSAGE_COLOR_TRANSFORM:
      return networkCursorColorPacket(cursor, packet, datagram);
    default:
      return false;
  }
}

LG_TransportStatus networkCursorNext(NetworkCursor * cursor,
    LG_TransportPointer * pointer)
{
  if (!cursor || !pointer)
    return LG_TRANSPORT_ERROR;

  for (;;)
  {
    LG_LOCK(cursor->lock);
    if (!cursor->connected)
    {
      LG_UNLOCK(cursor->lock);
      return LG_TRANSPORT_DISCONNECTED;
    }
    if (!cursor->available)
    {
      LG_UNLOCK(cursor->lock);
      return LG_TRANSPORT_UNAVAILABLE;
    }
    if (cursor->cancelled)
    {
      cursor->cancelled = false;
      LG_UNLOCK(cursor->lock);
      return LG_TRANSPORT_TIMEOUT;
    }
    if (cursor->outstanding)
    {
      LG_UNLOCK(cursor->lock);
      return LG_TRANSPORT_ERROR;
    }
    if (cursor->head)
    {
      struct NetworkCursorItem * item = cursor->head;
      cursor->head = item->next;
      if (!cursor->head)
        cursor->tail = NULL;
      --cursor->count;
      item->next          = NULL;
      cursor->outstanding = item;
      *pointer            = item->pointer;
      if (cursor->head)
        lgSignalEvent(cursor->event);
      LG_UNLOCK(cursor->lock);
      return LG_TRANSPORT_OK;
    }
    LG_UNLOCK(cursor->lock);
    lgWaitEvent(cursor->event, TIMEOUT_INFINITE);
  }
}

void networkCursorRelease(NetworkCursor * cursor,
    LG_TransportPointer * pointer)
{
  if (!cursor || !pointer)
    return;

  LG_LOCK(cursor->lock);
  struct NetworkCursorItem * item = cursor->outstanding;
  cursor->outstanding = NULL;
  LG_UNLOCK(cursor->lock);
  networkCursorItemFree(item);
  memset(pointer, 0, sizeof(*pointer));
}

void networkCursorCancel(NetworkCursor * cursor)
{
  if (!cursor)
    return;

  LG_LOCK(cursor->lock);
  cursor->cancelled = true;
  LG_UNLOCK(cursor->lock);
  lgSignalEvent(cursor->event);
}

void networkCursorStop(NetworkCursor * cursor)
{
  networkCursorCancel(cursor);
}
