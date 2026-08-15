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

#include "input.h"

#include "kb.h"

#include "common/KVMFRInput.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <lgmp/stream.h>

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_PENDING_LENGTH              128
#define INPUT_MOTION_COALESCE_THRESHOLD   (INPUT_PENDING_LENGTH / 2)
#define INPUT_PENDING_RESERVED            (INPUT_PENDING_LENGTH / 2)
#define INPUT_KEEPALIVE_US                 200000
#define INPUT_IDLE_RELEASE_US              400000
#define INPUT_RELEASE_TIMEOUT_US           50000
#define INPUT_WORKER_IDLE_MS               10
#define INPUT_WORKER_RETRY_MS              1
#define INPUT_STATS_INTERVAL_US            5000000
#define INPUT_MAX_SPLIT_REPORTS            4
#define INPUT_MOUSE_DELTA_MIN              (INT16_MIN * INPUT_MAX_SPLIT_REPORTS)
#define INPUT_MOUSE_DELTA_MAX              (INT16_MAX * INPUT_MAX_SPLIT_REPORTS)

enum LGMPInputMouseMode
{
  LGMP_INPUT_MOUSE_NONE,
  LGMP_INPUT_MOUSE_RELATIVE,
  LGMP_INPUT_MOUSE_ABSOLUTE,
};

struct LGMPInputPending
{
  KVMFRInputMessage message;
  bool pureMotion;
};

struct LGMPInputCounters
{
  uint64_t immediateSends;
  uint64_t deferredSends;
  uint64_t localEnqueues;
  uint64_t initialFull;
  uint64_t retryFull;
  uint64_t relativeCoalesces;
  uint64_t absoluteCoalesces;
  uint64_t reservedRejects;
  uint64_t motionEvictions;
  uint64_t discreteOverflowFailures;
  uint64_t discreteOverflowResets;
  uint64_t claims;
  uint64_t releases;
  uint64_t keepalives;
  uint64_t terminalFailures;
  unsigned pendingHighWater;
};

struct LGMPInputStats
{
  struct LGMPInputCounters counters;
  uint64_t                 lastReport;
};

struct LGMPInput
{
  PLGMPClient       client;
  PLGMPClientQueue  queue;
  PLGMPClientStream stream;
  LG_Lock           lock;
  LGEvent         * event;
  LGThread        * thread;
  atomic_bool       stop;
  bool              connected;
  bool              claimed;
  bool              available;
  bool              ownerBlocked;

  uint32_t generation;
  uint32_t sequence;
  uint32_t publishedGeneration;
  uint32_t publishedSequence;
  bool     publishedClaimed;
  uint64_t lastSend;

  struct LGMPInputPending pending[INPUT_PENDING_LENGTH];
  unsigned                pendingHead;
  unsigned                pendingCount;

  enum LGMPInputMouseMode mouseMode;
  uint32_t                mouseButtons;
  uint16_t                absoluteX;
  uint16_t                absoluteY;
  uint8_t                 keyState[KVMFR_INPUT_KEYBOARD_USAGE_MAX + 1];
  uint64_t                lastInput;

  uint32_t clientID;
  uint32_t capabilities;
  KVMFRInputStreamEndpoint streamEndpoint;
  uint32_t endpointGeneration;
  uint32_t statusSerial;
  uint32_t statusOwnerClientID;
  uint32_t statusOwnerGeneration;
  bool     ownerConfirmed;
  bool     streamEndpointBound;
  bool     statusValid;
  bool     notifyStatus;

  LG_InputStatusFn statusCallback;
  void           * statusOpaque;

  struct LGMPInputStats stats;
};

static bool buildKeyboardPayload(const LGMPInput * input,
    KVMFRInputPayload * payload);
static bool queueMouse(LGMPInput * input, enum LGMPInputMouseMode mode,
    int32_t x, int32_t y, int32_t wheel, uint32_t buttons,
    bool pureMotion, bool * wake);
static void discardProtocolState(LGMPInput * input);

static struct LGMPInputPending * pendingAt(
    LGMPInput * input, unsigned position)
{
  return &input->pending[
    (input->pendingHead + position) % INPUT_PENDING_LENGTH];
}

static LG_InputStatus inputStatus(const LGMPInput * input)
{
  return (LG_InputStatus)
  {
    .available  = input->available,
    .generation = input->endpointGeneration,
  };
}

static void notifyInputStatus(LGMPInput * input)
{
  LG_InputStatusFn callback;
  void           * opaque;
  LG_InputStatus   status;

  LG_LOCK(input->lock);
  if (!input->notifyStatus)
  {
    LG_UNLOCK(input->lock);
    return;
  }

  input->notifyStatus = false;
  callback = input->statusCallback;
  opaque   = input->statusOpaque;
  status   = inputStatus(input);
  LG_UNLOCK(input->lock);

  if (callback)
    callback(opaque, &status);
}

static void published(LGMPInput * input,
    const KVMFRInputMessage * message)
{
  input->publishedGeneration = message->generation;
  input->publishedSequence   = message->sequence;
  input->lastSend            = microtime();

  switch (message->type)
  {
    case KVMFR_INPUT_MESSAGE_CLAIM:
      input->publishedClaimed = true;
      ++input->stats.counters.claims;
      break;

    case KVMFR_INPUT_MESSAGE_RELEASE:
      input->publishedClaimed = false;
      ++input->stats.counters.releases;
      break;

    case KVMFR_INPUT_MESSAGE_KEEPALIVE:
      ++input->stats.counters.keepalives;
      break;
  }
}

static void detachInputStream(LGMPInput * input)
{
  if (input->stream)
    lgmpClientStreamDetach(&input->stream);
  input->streamEndpointBound = false;
}

static void connectionFailed(LGMPInput * input, LGMP_STATUS status)
{
  if (input->connected)
  {
    DEBUG_WARN("LGMP input transport failed: %s", lgmpStatusString(status));
    ++input->stats.counters.terminalFailures;
  }

  if (input->available || input->endpointGeneration)
    input->notifyStatus = true;
  input->connected              = false;
  input->claimed                = false;
  input->available              = false;
  input->ownerBlocked           = false;
  input->pendingHead            = 0;
  input->pendingCount           = 0;
  input->mouseMode              = LGMP_INPUT_MOUSE_NONE;
  input->mouseButtons           = 0;
  input->publishedClaimed       = false;
  input->lastInput              = 0;
  input->capabilities           = 0;
  input->streamEndpointBound    = false;
  detachInputStream(input);
  memset(&input->streamEndpoint, 0,
    sizeof(input->streamEndpoint));
  input->statusValid            = false;
  input->ownerConfirmed         = false;
  memset(input->keyState, 0, sizeof(input->keyState));
  atomic_store_explicit(&input->stop, true, memory_order_release);
}

static LGMP_STATUS trySend(LGMPInput * input,
    const KVMFRInputMessage * message, bool deferred)
{
  if (!input->stream)
    return LGMP_ERR_STREAM_UNBOUND;

  LGMPStreamBuffer buffer = { 0 };
  LGMP_STATUS status = lgmpClientStreamWriteAcquire(
    input->stream, &buffer);
  if (status == LGMP_OK)
  {
    if (buffer.capacity < sizeof(*message))
    {
      lgmpClientStreamWriteCancel(input->stream, &buffer);
      status = LGMP_ERR_INVALID_SIZE;
    }
    else
    {
      memcpy(buffer.data, message, sizeof(*message));
      status = lgmpClientStreamWriteCommit(input->stream,
        &buffer, sizeof(*message));
      if (status != LGMP_OK)
        lgmpClientStreamWriteCancel(input->stream, &buffer);
    }
  }

  if (status == LGMP_OK)
  {
    if (deferred)
      ++input->stats.counters.deferredSends;
    else
      ++input->stats.counters.immediateSends;
    return status;
  }

  if (status == LGMP_ERR_STREAM_FULL)
  {
    if (deferred)
      ++input->stats.counters.retryFull;
    else
      ++input->stats.counters.initialFull;
  }
  return status;
}

static bool retryableSendStatus(LGMP_STATUS status)
{
  return status == LGMP_ERR_STREAM_FULL;
}

static void sendFailed(LGMPInput * input, LGMP_STATUS status)
{
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    DEBUG_WARN("LGMP input stream binding was lost: %s",
      lgmpStatusString(status));
    const bool notify = input->available || input->endpointGeneration;
    detachInputStream(input);
    discardProtocolState(input);
    input->available = false;
    if (notify)
      input->notifyStatus = true;
    return;
  }

  connectionFailed(input, status);
}

static bool coalesceMotion(LGMPInput * input,
    KVMFRInputMessageType type, const KVMFRInputPayload * payload)
{
  if (input->pendingCount < INPUT_MOTION_COALESCE_THRESHOLD)
    return false;

  struct LGMPInputPending * tail = pendingAt(
    input, input->pendingCount - 1);
  if (!tail->pureMotion || tail->message.type != type ||
      tail->message.generation != input->generation)
    return false;

  if (type == KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE)
  {
    tail->message.payload = *payload;
    ++input->stats.counters.absoluteCoalesces;
    return true;
  }

  if (type != KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE ||
      tail->message.payload.mouseRelative.buttons !=
        payload->mouseRelative.buttons)
    return false;

  const int64_t x =
    (int64_t)tail->message.payload.mouseRelative.deltaX +
    payload->mouseRelative.deltaX;
  const int64_t y =
    (int64_t)tail->message.payload.mouseRelative.deltaY +
    payload->mouseRelative.deltaY;
  if (x < INPUT_MOUSE_DELTA_MIN || x > INPUT_MOUSE_DELTA_MAX ||
      y < INPUT_MOUSE_DELTA_MIN || y > INPUT_MOUSE_DELTA_MAX)
    return false;

  tail->message.payload.mouseRelative.deltaX = (int32_t)x;
  tail->message.payload.mouseRelative.deltaY = (int32_t)y;
  ++input->stats.counters.relativeCoalesces;
  return true;
}

static bool discardPendingMotion(LGMPInput * input)
{
  for (unsigned i = input->pendingCount; i > 0; --i)
  {
    const unsigned position = i - 1;
    struct LGMPInputPending * item = pendingAt(input, position);
    if (!item->pureMotion)
      continue;

    const uint32_t generation = item->message.generation;
    for (unsigned j = position; j + 1 < input->pendingCount; ++j)
      *pendingAt(input, j) = *pendingAt(input, j + 1);
    --input->pendingCount;

    for (unsigned j = position; j < input->pendingCount; ++j)
    {
      item = pendingAt(input, j);
      if (item->message.generation == generation)
        item->message.sequence = item->message.sequence == 1 ?
          UINT32_MAX : item->message.sequence - 1;
    }

    if (generation == input->generation)
      input->sequence = input->sequence == 1 ?
        UINT32_MAX : input->sequence - 1;
    ++input->stats.counters.motionEvictions;
    return true;
  }
  return false;
}

static bool submitPayload(LGMPInput * input, KVMFRInputMessageType type,
    const KVMFRInputPayload * payload, bool pureMotion, bool * wake)
{
  if (!input->connected || !input->stream)
    return false;

  const bool inputMessage =
    type == KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE ||
    type == KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE ||
    type == KVMFR_INPUT_MESSAGE_KEYBOARD;

  if (pureMotion && coalesceMotion(input, type, payload))
  {
    input->lastInput = microtime();
    return true;
  }
  if (pureMotion && input->pendingCount >=
      INPUT_PENDING_LENGTH - INPUT_PENDING_RESERVED)
  {
    ++input->stats.counters.reservedRejects;
    return false;
  }
  if (!pureMotion && input->pendingCount == INPUT_PENDING_LENGTH &&
      !discardPendingMotion(input))
  {
    ++input->stats.counters.discreteOverflowFailures;
    return false;
  }

  const uint32_t previousSequence = input->sequence;
  if (++input->sequence == 0)
    input->sequence = 1;

  KVMFRInputMessage message =
  {
    .type       = type,
    .generation = input->generation,
    .sequence   = input->sequence,
    .payload    = *payload,
  };

  if (!input->pendingCount)
  {
    const LGMP_STATUS status = trySend(input, &message, false);
    if (status == LGMP_OK)
    {
      published(input, &message);
      if (inputMessage)
        input->lastInput = microtime();
      return true;
    }

    if (!retryableSendStatus(status))
    {
      input->sequence = previousSequence;
      sendFailed(input, status);
      return false;
    }
  }

  if (input->pendingCount == INPUT_PENDING_LENGTH)
  {
    input->sequence = previousSequence;
    return false;
  }

  struct LGMPInputPending * item = pendingAt(input, input->pendingCount++);
  item->message    = message;
  item->pureMotion = pureMotion;
  ++input->stats.counters.localEnqueues;
  if (input->pendingCount > input->stats.counters.pendingHighWater)
    input->stats.counters.pendingHighWater = input->pendingCount;
  if (inputMessage)
    input->lastInput = microtime();
  *wake = true;
  return true;
}

static bool claim(LGMPInput * input, bool * wake)
{
  if (input->claimed)
    return true;
  if (!input->available || input->ownerBlocked ||
      !input->stream || !input->streamEndpointBound)
    return false;

  if (++input->generation == 0)
    ++input->generation;
  input->sequence       = 0;
  input->claimed        = true;
  input->ownerConfirmed = false;

  const KVMFRInputPayload payload = { 0 };
  if (submitPayload(input, KVMFR_INPUT_MESSAGE_CLAIM,
      &payload, false, wake))
    return true;

  input->claimed = false;
  return false;
}

static void clearInputState(LGMPInput * input)
{
  input->mouseMode    = LGMP_INPUT_MOUSE_NONE;
  input->mouseButtons = 0;
  input->absoluteX    = 0;
  input->absoluteY    = 0;
  memset(input->keyState, 0, sizeof(input->keyState));
}

static bool inputStateHeld(const LGMPInput * input)
{
  if (input->mouseButtons)
    return true;

  for (unsigned usage = 1;
      usage <= KVMFR_INPUT_KEYBOARD_USAGE_MAX; ++usage)
    if (input->keyState[usage])
      return true;
  return false;
}

static void discardProtocolState(LGMPInput * input)
{
  input->pendingHead         = 0;
  input->pendingCount        = 0;
  input->claimed             = false;
  input->sequence            = 0;
  input->publishedGeneration = 0;
  input->publishedSequence   = 0;
  input->publishedClaimed    = false;
  input->ownerConfirmed      = false;
}

static bool restoreInputState(LGMPInput * input, bool * wake)
{
  if (!inputStateHeld(input))
    return true;
  if (!claim(input, wake))
    return false;

  KVMFRInputPayload keyboard = { 0 };
  const bool keyboardHeld = buildKeyboardPayload(input, &keyboard);
  if (keyboardHeld && !submitPayload(input, KVMFR_INPUT_MESSAGE_KEYBOARD,
      &keyboard, false, wake))
    return false;

  if (input->mouseButtons &&
      input->mouseMode != LGMP_INPUT_MOUSE_NONE &&
      !queueMouse(input, input->mouseMode, 0, 0, 0,
        input->mouseButtons, false, wake))
    return false;
  return true;
}

static bool validInputStatus(const KVMFRInputStatus * status)
{
  static const uint32_t capabilities =
    KVMFR_INPUT_CAP_MOUSE_RELATIVE |
    KVMFR_INPUT_CAP_MOUSE_ABSOLUTE |
    KVMFR_INPUT_CAP_KEYBOARD;
  static const uint32_t flags =
    KVMFR_INPUT_STATUS_AVAILABLE |
    KVMFR_INPUT_STATUS_HAS_OWNER;

  if (status->version != KVMFR_INPUT_VERSION ||
      status->capabilities & ~capabilities ||
      status->flags & ~flags ||
      !status->generation ||
      !status->lease || !status->maxButtons ||
      status->maxButtons > KVMFR_INPUT_MOUSE_BUTTON_COUNT ||
      status->streamVersion != KVMFR_INPUT_STREAM_VERSION ||
      status->streamEndpointCount !=
        KVMFR_INPUT_STREAM_ENDPOINT_COUNT ||
      !status->streamGeneration)
    return false;

  for (size_t i = 0; i < sizeof(status->streamReserved) /
      sizeof(status->streamReserved[0]); ++i)
    if (status->streamReserved[i])
      return false;

  for (unsigned i = 0;
      i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    const KVMFRInputStreamEndpoint * endpoint =
      &status->streamEndpoint[i];
    const uint32_t endpointFlags =
      KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE |
      KVMFR_INPUT_STREAM_ENDPOINT_BOUND;
    if (endpoint->flags & ~endpointFlags || endpoint->reserved ||
        !(endpoint->flags &
          KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE))
      return false;

    const bool endpointBound =
      (endpoint->flags & KVMFR_INPUT_STREAM_ENDPOINT_BOUND) != 0;
    if (endpoint->stream.magic != LGMP_STREAM_DESCRIPTOR_MAGIC ||
        endpoint->stream.size != sizeof(endpoint->stream) ||
        endpoint->stream.version != LGMP_STREAM_DESCRIPTOR_VERSION ||
        !endpoint->stream.offset ||
        !endpoint->stream.regionSize ||
        endpoint->stream.direction != LGMP_STREAM_CLIENT_TO_HOST ||
        endpoint->stream.policy != LGMP_STREAM_RELIABLE_FIFO ||
        endpoint->stream.slotCount !=
          KVMFR_INPUT_STREAM_SLOT_COUNT ||
        endpoint->stream.slotSize != KVMFR_INPUT_STREAM_SLOT_SIZE ||
        (endpointBound ?
          !endpoint->boundClientID || !endpoint->bindingGeneration :
          endpoint->boundClientID || endpoint->bindingGeneration))
      return false;

    for (unsigned j = 0; j < i; ++j)
    {
      const KVMFRInputStreamEndpoint * previous =
        &status->streamEndpoint[j];
      if (previous->stream.offset == endpoint->stream.offset ||
          (endpointBound &&
            previous->boundClientID == endpoint->boundClientID))
        return false;
    }
  }

  const bool available =
    (status->flags & KVMFR_INPUT_STATUS_AVAILABLE) != 0;
  const bool hasOwner =
    (status->flags & KVMFR_INPUT_STATUS_HAS_OWNER) != 0;
  if (!available && (status->capabilities || hasOwner))
    return false;
  if (available &&
      (status->capabilities &
        (KVMFR_INPUT_CAP_MOUSE_RELATIVE | KVMFR_INPUT_CAP_KEYBOARD)) !=
      (KVMFR_INPUT_CAP_MOUSE_RELATIVE | KVMFR_INPUT_CAP_KEYBOARD))
    return false;
  return hasOwner ?
    status->ownerClientID && status->ownerGeneration :
    !status->ownerClientID && !status->ownerGeneration;
}

static struct LGMPStreamDescriptor importStreamDescriptor(
    const KVMFRStreamDescriptor * source)
{
  return (struct LGMPStreamDescriptor)
  {
    .magic      = source->magic,
    .version    = source->version,
    .size       = source->size,
    .offset     = source->offset,
    .regionSize = source->regionSize,
    .direction  = source->direction,
    .policy     = source->policy,
    .slotCount  = source->slotCount,
    .slotSize   = source->slotSize,
  };
}

static bool reconcileInputStream(LGMPInput * input,
    const KVMFRInputStatus * status)
{
  const KVMFRInputStreamEndpoint * desired = NULL;
  for (unsigned i = 0; i < status->streamEndpointCount; ++i)
    if ((status->streamEndpoint[i].flags &
          KVMFR_INPUT_STREAM_ENDPOINT_BOUND) &&
        status->streamEndpoint[i].boundClientID == input->clientID)
    {
      desired = &status->streamEndpoint[i];
      break;
    }

  const bool oldBound = input->streamEndpointBound;
  const KVMFRInputStreamEndpoint oldEndpoint = input->streamEndpoint;
  if (desired && input->stream && oldBound &&
      memcmp(&oldEndpoint, desired, sizeof(*desired)) == 0)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    const LGMP_STATUS bindingStatus = lgmpClientStreamGetBinding(
      input->stream, &clientID, &epoch);
    if (bindingStatus == LGMP_OK && clientID == input->clientID &&
        epoch == desired->bindingGeneration)
      return false;
  }

  detachInputStream(input);
  memset(&input->streamEndpoint, 0, sizeof(input->streamEndpoint));
  if (!desired)
    return oldBound;

  input->streamEndpoint = *desired;
  const struct LGMPStreamDescriptor descriptor =
    importStreamDescriptor(&desired->stream);
  LGMP_STATUS attachStatus = lgmpClientStreamAttach(
    input->client, &descriptor, &input->stream);
  if (attachStatus != LGMP_OK)
  {
    DEBUG_WARN("Failed to attach LGMP input stream: %s",
      lgmpStatusString(attachStatus));
    return oldBound;
  }

  uint32_t clientID = 0;
  uint32_t epoch = 0;
  attachStatus = lgmpClientStreamGetBinding(
    input->stream, &clientID, &epoch);
  if (attachStatus != LGMP_OK || clientID != input->clientID ||
      epoch != desired->bindingGeneration)
  {
    if (attachStatus != LGMP_OK)
      DEBUG_WARN("Failed to inspect LGMP input stream binding: %s",
        lgmpStatusString(attachStatus));
    else
      DEBUG_WARN("LGMP input stream binding changed during attach");
    detachInputStream(input);
    return oldBound;
  }

  input->streamEndpointBound = true;
  return !oldBound ||
    memcmp(&oldEndpoint, desired, sizeof(*desired)) != 0;
}

static void applyInputStatus(LGMPInput * input,
    const KVMFRInputStatus * status, uint32_t serial, bool * wake)
{
  const bool     wasValid       = input->statusValid;
  const bool     wasAvailable   = input->available;
  const uint32_t oldCapabilities = input->capabilities;
  const uint32_t oldGeneration   = input->endpointGeneration;
  const bool     targetAvailable =
    (status->flags & KVMFR_INPUT_STATUS_AVAILABLE) != 0;
  const bool     endpointChanged = wasValid &&
    oldGeneration != status->generation;
  bool restore = false;

  input->statusValid           = true;
  input->statusSerial          = serial;
  input->capabilities          = status->capabilities;
  input->endpointGeneration    = status->generation;
  input->statusOwnerClientID   = status->ownerClientID;
  input->statusOwnerGeneration = status->ownerGeneration;
  const bool streamChanged = reconcileInputStream(input, status);
  const bool available = targetAvailable &&
    input->streamEndpointBound;
  input->available = available;

  if (endpointChanged || !available || streamChanged)
  {
    discardProtocolState(input);
    restore = available && (endpointChanged || streamChanged);
  }

  if (status->flags & KVMFR_INPUT_STATUS_HAS_OWNER)
  {
    if (input->claimed &&
        status->ownerClientID == input->clientID &&
        status->ownerGeneration == input->generation)
    {
      input->ownerBlocked   = false;
      input->ownerConfirmed = true;
    }
    else
    {
      discardProtocolState(input);
      input->ownerBlocked = true;
      restore = false;
    }
  }
  else
  {
    if (input->ownerBlocked ||
        (input->claimed && input->ownerConfirmed))
    {
      discardProtocolState(input);
      restore = available;
    }
    input->ownerBlocked = false;
  }

  if (restore && !restoreInputState(input, wake))
    discardProtocolState(input);

  if (!wasValid || wasAvailable != available ||
      oldCapabilities != status->capabilities ||
      oldGeneration != status->generation)
    input->notifyStatus = true;
}

static void processInputStatus(LGMPInput * input, bool * wake)
{
  LGMP_STATUS result = lgmpClientAdvanceToLast(input->queue);
  if (result == LGMP_ERR_QUEUE_EMPTY)
    return;
  if (result != LGMP_OK)
  {
    connectionFailed(input, result);
    return;
  }

  LGMPMessage message;
  result = lgmpClientProcess(input->queue, &message);
  if (result == LGMP_ERR_QUEUE_EMPTY)
    return;
  if (result != LGMP_OK)
  {
    connectionFailed(input, result);
    return;
  }

  KVMFRInputStatus status = { 0 };
  const bool valid = message.udata && message.udata <= UINT32_MAX &&
    message.size == sizeof(status);
  if (valid)
    memcpy(&status, message.mem, sizeof(status));

  result = lgmpClientMessageDone(input->queue);
  if (result != LGMP_OK)
  {
    connectionFailed(input, result);
    return;
  }

  const uint32_t serial = (uint32_t)message.udata;
  if (!valid || !validInputStatus(&status))
  {
    DEBUG_WARN("Ignoring invalid LGMP input status");
    return;
  }
  if (input->statusValid &&
      (int32_t)(serial - input->statusSerial) <= 0)
    return;

  applyInputStatus(input, &status, serial, wake);
}

static bool release(LGMPInput * input, bool * wake)
{
  input->pendingHead  = 0;
  input->pendingCount = 0;
  input->claimed      = false;
  clearInputState(input);

  if (!input->publishedClaimed)
    return true;

  input->generation = input->publishedGeneration;
  input->sequence   = input->publishedSequence;
  const KVMFRInputPayload payload = { 0 };
  return submitPayload(input, KVMFR_INPUT_MESSAGE_RELEASE,
    &payload, false, wake);
}

static bool flushPending(LGMPInput * input)
{
  bool progress = false;
  while (input->connected && input->pendingCount)
  {
    struct LGMPInputPending * item = pendingAt(input, 0);
    const LGMP_STATUS status = trySend(input, &item->message, true);
    if (retryableSendStatus(status))
      return progress;
    if (status != LGMP_OK)
    {
      sendFailed(input, status);
      return progress;
    }

    progress = true;
    published(input, &item->message);
    input->pendingHead =
      (input->pendingHead + 1) % INPUT_PENDING_LENGTH;
    --input->pendingCount;
  }
  return progress;
}

static bool collectStats(LGMPInput * input, uint64_t now, bool force,
    struct LGMPInputCounters * result)
{
  if (!force && now - input->stats.lastReport < INPUT_STATS_INTERVAL_US)
    return false;

  input->stats.lastReport = now;
  *result = input->stats.counters;
  memset(&input->stats.counters, 0, sizeof(input->stats.counters));
  input->stats.counters.pendingHighWater = input->pendingCount;

  return result->immediateSends || result->deferredSends ||
    result->localEnqueues || result->initialFull || result->retryFull ||
    result->relativeCoalesces || result->absoluteCoalesces ||
    result->reservedRejects || result->motionEvictions ||
    result->discreteOverflowFailures ||
    result->discreteOverflowResets || result->claims ||
    result->releases || result->keepalives ||
    result->terminalFailures;
}

static void logStats(const struct LGMPInputCounters * stats)
{
  DEBUG_TRACE("LGMP input: sent immediate/deferred %lu/%lu"
    ", queued %lu (high %u), initial/retry full %lu/%lu"
    ", coalesced relative/absolute %lu/%lu"
    ", motion rejected/evicted %lu/%lu"
    ", overflow failures/resets %lu/%lu"
    ", published claim/release/keepalive %lu/%lu"
    "/%lu, terminal failures %lu",
    stats->immediateSends, stats->deferredSends,
    stats->localEnqueues, stats->pendingHighWater,
    stats->initialFull, stats->retryFull, stats->relativeCoalesces,
    stats->absoluteCoalesces, stats->reservedRejects,
    stats->motionEvictions, stats->discreteOverflowFailures,
    stats->discreteOverflowResets, stats->claims, stats->releases,
    stats->keepalives, stats->terminalFailures);
}

static void releaseOnDisconnect(LGMPInput * input)
{
  if (!input->queue || !input->stream || !input->publishedClaimed ||
      !input->publishedGeneration ||
      !lgmpClientSessionValid(input->client))
    return;

  KVMFRInputMessage message =
  {
    .type       = KVMFR_INPUT_MESSAGE_RELEASE,
    .generation = input->publishedGeneration,
    .sequence   = input->publishedSequence + 1,
  };
  if (!message.sequence)
    message.sequence = 1;

  const uint64_t deadline = microtime() + INPUT_RELEASE_TIMEOUT_US;
  const uint32_t statusSerial = input->statusSerial;
  LGMP_STATUS status;
  do
  {
    status = trySend(input, &message, true);
    if (status == LGMP_OK)
      break;
    if (!retryableSendStatus(status))
      return;
    if (input->event)
      lgWaitEvent(input->event, INPUT_WORKER_RETRY_MS);
  }
  while (microtime() < deadline);

  if (status != LGMP_OK)
  {
    DEBUG_WARN("Timed out releasing LGMP input stream ownership");
    return;
  }

  do
  {
    bool wake = false;
    processInputStatus(input, &wake);
    if (input->statusValid &&
        (int32_t)(input->statusSerial - statusSerial) > 0 &&
        (input->statusOwnerClientID != input->clientID ||
          input->statusOwnerGeneration != message.generation))
      return;
    if (input->event)
      lgWaitEvent(input->event, INPUT_WORKER_RETRY_MS);
  }
  while (microtime() < deadline);

  DEBUG_WARN("Timed out waiting for LGMP input stream release");
}

static int inputThread(void * opaque)
{
  LGMPInput * input = opaque;
  LGMPStreamPollState streamPoll;
  const LGMP_STATUS pollStatus = lgmpStreamPollInit(&streamPoll,
      (struct LGMPStreamPollConfig)
      {
        .spinCount = 32U,
        .minWaitUs = 50U,
        .maxWaitUs = 1000U,
      });
  if (pollStatus != LGMP_OK)
  {
    DEBUG_ERROR("Failed to initialize LGMP input polling: %s",
      lgmpStatusString(pollStatus));
    return -1;
  }

  while (!atomic_load_explicit(&input->stop, memory_order_acquire))
  {
    struct LGMPInputCounters stats = { 0 };
    unsigned timeout = INPUT_WORKER_IDLE_MS;
    bool wake = false;
    LG_LOCK(input->lock);
    processInputStatus(input, &wake);
    const bool streamProgress = flushPending(input);

    const uint64_t now = microtime();
    if (input->connected && input->claimed && !inputStateHeld(input) &&
        !input->pendingCount &&
        input->publishedGeneration == input->generation &&
        now - input->lastInput >= INPUT_IDLE_RELEASE_US)
    {
      release(input, &wake);
    }
    else if (input->connected && input->claimed &&
        input->publishedClaimed && !input->pendingCount &&
        input->publishedGeneration == input->generation &&
        now - input->lastSend >= INPUT_KEEPALIVE_US)
    {
      const KVMFRInputPayload payload = { 0 };
      submitPayload(input, KVMFR_INPUT_MESSAGE_KEEPALIVE,
        &payload, false, &wake);
    }

    if (input->pendingCount)
      timeout = INPUT_WORKER_RETRY_MS;
    const bool streamPending = input->pendingCount != 0;
    const bool report = collectStats(input, now, false, &stats);
    LG_UNLOCK(input->lock);

    if (report)
      logStats(&stats);
    notifyInputStatus(input);
    if (wake)
      lgSignalEvent(input->event);

    if (atomic_load_explicit(&input->stop, memory_order_acquire))
      break;

    bool signaled;
    if (streamPending)
    {
      if (streamProgress)
        lgmpStreamPollActivity(&streamPoll);
      const uint32_t waitUs = lgmpStreamPollIdle(&streamPoll);
      if (!waitUs)
        continue;
      signaled = lgWaitEventNS(input->event, waitUs * 1000U);
    }
    else
    {
      if (streamProgress)
        lgmpStreamPollActivity(&streamPoll);
      signaled = lgWaitEvent(input->event, timeout);
    }
    if (signaled)
      lgmpStreamPollActivity(&streamPoll);
  }

  struct LGMPInputCounters stats = { 0 };
  LG_LOCK(input->lock);
  const bool report = collectStats(input, microtime(), true, &stats);
  LG_UNLOCK(input->lock);
  if (report)
    logStats(&stats);
  notifyInputStatus(input);
  return 0;
}

bool lgmpInput_create(PLGMPClient client, LGMPInput ** result)
{
  LGMPInput * input = calloc(1, sizeof(*input));
  if (!input)
    return false;

  input->client = client;
  LG_LOCK_INIT(input->lock);
  atomic_init(&input->stop, false);
  *result = input;
  return true;
}

void lgmpInput_destroy(LGMPInput ** input)
{
  if (!input || !*input)
    return;

  lgmpInput_disconnect(*input);
  LG_LOCK_FREE((*input)->lock);
  free(*input);
  *input = NULL;
}

bool lgmpInput_connect(LGMPInput * input, uint32_t clientID)
{
  if (!clientID)
    return false;

  LG_LOCK(input->lock);
  if (input->connected)
  {
    const bool sameClient = input->clientID == clientID;
    LG_UNLOCK(input->lock);
    return sameClient;
  }
  if (input->thread || input->queue)
  {
    LG_UNLOCK(input->lock);
    return false;
  }

  LGMP_STATUS status = lgmpClientSubscribe(
    input->client, LGMP_Q_INPUT, &input->queue);
  if (status != LGMP_OK)
  {
    LG_UNLOCK(input->lock);
    DEBUG_WARN("Failed to subscribe to LGMP input status queue: %s",
      lgmpStatusString(status));
    return false;
  }

  input->event = lgCreateEvent(true, 0);
  if (!input->event)
  {
    lgmpClientUnsubscribe(&input->queue);
    LG_UNLOCK(input->lock);
    return false;
  }

  input->connected             = true;
  input->claimed               = false;
  input->available             = false;
  input->ownerBlocked          = false;
  input->pendingHead           = 0;
  input->pendingCount          = 0;
  input->clientID              = clientID;
  input->capabilities          = 0;
  input->streamEndpointBound   = false;
  memset(&input->streamEndpoint, 0,
    sizeof(input->streamEndpoint));
  input->endpointGeneration    = 0;
  input->statusSerial          = 0;
  input->statusOwnerClientID   = 0;
  input->statusOwnerGeneration = 0;
  input->ownerConfirmed        = false;
  input->statusValid           = false;
  input->publishedGeneration   = 0;
  input->publishedSequence     = 0;
  input->publishedClaimed      = false;
  input->lastSend              = 0;
  input->lastInput             = 0;
  input->generation            = 0;
  memset(&input->stats, 0, sizeof(input->stats));
  input->stats.lastReport      = microtime();
  clearInputState(input);
  atomic_store_explicit(&input->stop, false, memory_order_release);
  LGThread * thread;
  if (lgCreateThread("lgmpInput", inputThread, input, &thread))
  {
    input->thread = thread;
    LG_UNLOCK(input->lock);
    return true;
  }

  input->connected        = false;
  LGEvent         * event = input->event;
  PLGMPClientQueue  queue = input->queue;
  input->event            = NULL;
  input->queue            = NULL;
  LG_UNLOCK(input->lock);
  lgmpClientUnsubscribe(&queue);
  lgFreeEvent(event);
  return false;
}

void lgmpInput_disconnect(LGMPInput * input)
{
  if (!input)
    return;

  LG_LOCK(input->lock);
  if (!input->thread && !input->queue)
  {
    LG_UNLOCK(input->lock);
    return;
  }

  input->pendingHead  = 0;
  input->pendingCount = 0;
  input->connected    = false;
  input->claimed      = false;
  if (input->available || input->endpointGeneration)
    input->notifyStatus = true;
  input->available      = false;
  input->ownerBlocked   = false;
  input->capabilities   = 0;
  input->streamEndpointBound = false;
  memset(&input->streamEndpoint, 0,
    sizeof(input->streamEndpoint));
  input->statusValid    = false;
  input->ownerConfirmed = false;
  clearInputState(input);
  atomic_store_explicit(&input->stop, true, memory_order_release);
  LGThread * thread = input->thread;
  LGEvent  * event  = input->event;
  LG_UNLOCK(input->lock);

  if (event)
    lgSignalEvent(event);
  if (thread)
    lgJoinThread(thread, NULL);

  LG_LOCK(input->lock);
  releaseOnDisconnect(input);
  detachInputStream(input);
  input->available             = false;
  input->ownerBlocked          = false;
  input->statusValid           = false;
  input->ownerConfirmed        = false;
  input->notifyStatus          = false;
  input->statusOwnerClientID   = 0;
  input->statusOwnerGeneration = 0;
  memset(&input->streamEndpoint, 0,
    sizeof(input->streamEndpoint));
  PLGMPClientQueue queue  = input->queue;
  input->queue            = NULL;
  input->thread           = NULL;
  input->event            = NULL;
  input->publishedClaimed = false;
  LG_UNLOCK(input->lock);

  if (queue)
  {
    const LGMP_STATUS status = lgmpClientUnsubscribe(&queue);
    if (status != LGMP_OK && status != LGMP_ERR_INVALID_SESSION &&
        status != LGMP_ERR_QUEUE_TIMEOUT &&
        status != LGMP_ERR_QUEUE_UNSUBSCRIBED)
      DEBUG_WARN("Failed to unsubscribe from LGMP input status queue: %s",
        lgmpStatusString(status));
  }
  if (event)
    lgFreeEvent(event);
}

static bool inputSupports(void * opaque, LG_InputSupport support)
{
  LGMPInput * input = opaque;
  LG_LOCK(input->lock);

  bool result;
  switch (support)
  {
    case LG_INPUT_SUPPORT_MOUSE_ABSOLUTE:
      result = input->available &&
        (input->capabilities & KVMFR_INPUT_CAP_MOUSE_ABSOLUTE) != 0;
      break;

    default:
      result = false;
      break;
  }
  LG_UNLOCK(input->lock);
  return result;
}

static void inputSetStatusListener(void * opaque,
    LG_InputStatusFn callback, void * callbackOpaque)
{
  LGMPInput * input = opaque;
  LG_InputStatus status;

  LG_LOCK(input->lock);
  input->statusCallback = callback;
  input->statusOpaque   = callbackOpaque;
  input->notifyStatus   = false;
  status = inputStatus(input);
  LG_UNLOCK(input->lock);

  if (callback)
    callback(callbackOpaque, &status);
}

static bool buildKeyboardPayload(const LGMPInput * input,
    KVMFRInputPayload * payload)
{
  bool held = false;
  for (unsigned usage = 224; usage <= 231; ++usage)
    if (input->keyState[usage])
    {
      payload->keyboard.modifiers |= 1U << (usage - 224);
      held = true;
    }

  unsigned count = 0;
  for (unsigned usage = 1; usage < 224; ++usage)
  {
    if (!input->keyState[usage])
      continue;
    held = true;
    if (count == KVMFR_INPUT_KEYBOARD_KEY_COUNT)
    {
      memset(payload->keyboard.keys, 1,
        sizeof(payload->keyboard.keys));
      return true;
    }
    payload->keyboard.keys[count++] = (uint8_t)usage;
  }
  return held;
}

static bool updateKey(void * opaque, int key, bool pressed)
{
  if (key < 0 || key >= KEY_MAX)
    return false;

  const uint8_t usage = linux_to_hid[key];
  if (!usage)
    return true;

  LGMPInput * input = opaque;
  bool wake = false;
  LG_LOCK(input->lock);
  if (!input->connected || !input->available)
  {
    LG_UNLOCK(input->lock);
    return false;
  }

  uint8_t * state = &input->keyState[usage];
  const uint8_t previous = *state;
  if (pressed)
  {
    if (*state != UINT8_MAX)
      ++*state;
  }
  else if (*state)
    --*state;

  if (input->ownerBlocked)
  {
    LG_UNLOCK(input->lock);
    return true;
  }
  if (!claim(input, &wake))
  {
    *state = previous;
    LG_UNLOCK(input->lock);
    if (wake)
      lgSignalEvent(input->event);
    return false;
  }

  KVMFRInputPayload payload = { 0 };
  buildKeyboardPayload(input, &payload);
  const uint64_t overflowFailures =
    input->stats.counters.discreteOverflowFailures;
  bool result = submitPayload(input, KVMFR_INPUT_MESSAGE_KEYBOARD,
    &payload, false, &wake);
  if (!result && !pressed && input->connected)
  {
    if (input->stats.counters.discreteOverflowFailures !=
        overflowFailures)
      ++input->stats.counters.discreteOverflowResets;
    result = release(input, &wake);
  }
  else if (!result && input->connected)
    *state = previous;
  LG_UNLOCK(input->lock);

  if (wake)
    lgSignalEvent(input->event);
  return result;
}

static bool inputKeyDown(void * opaque, int key)
{
  return updateKey(opaque, key, true);
}

static bool inputKeyUp(void * opaque, int key)
{
  return updateKey(opaque, key, false);
}

static bool queueMouse(LGMPInput * input, enum LGMPInputMouseMode mode,
    int32_t x, int32_t y, int32_t wheel, uint32_t buttons,
    bool pureMotion, bool * wake)
{
  KVMFRInputPayload payload = { 0 };
  KVMFRInputMessageType type;
  if (mode == LGMP_INPUT_MOUSE_ABSOLUTE)
  {
    type = KVMFR_INPUT_MESSAGE_MOUSE_ABSOLUTE;
    payload.mouseAbsolute.buttons = buttons;
    payload.mouseAbsolute.x       = input->absoluteX;
    payload.mouseAbsolute.y       = input->absoluteY;
    payload.mouseAbsolute.wheel   = wheel;
  }
  else
  {
    type = KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE;
    payload.mouseRelative.buttons = buttons;
    payload.mouseRelative.deltaX  = x;
    payload.mouseRelative.deltaY  = y;
    payload.mouseRelative.wheel   = wheel;
  }

  return submitPayload(input, type, &payload, pureMotion, wake);
}

static bool inputMouseMotion(void * opaque, int32_t x, int32_t y)
{
  if (x < INPUT_MOUSE_DELTA_MIN || x > INPUT_MOUSE_DELTA_MAX ||
      y < INPUT_MOUSE_DELTA_MIN || y > INPUT_MOUSE_DELTA_MAX)
    return false;

  LGMPInput * input = opaque;
  bool wake = false;
  LG_LOCK(input->lock);
  if (input->connected && input->available && input->ownerBlocked)
  {
    input->mouseMode = LGMP_INPUT_MOUSE_RELATIVE;
    LG_UNLOCK(input->lock);
    return true;
  }

  bool result = input->connected && input->available &&
    claim(input, &wake) &&
    queueMouse(input, LGMP_INPUT_MOUSE_RELATIVE,
      x, y, 0, input->mouseButtons, true, &wake);
  if (result)
    input->mouseMode = LGMP_INPUT_MOUSE_RELATIVE;
  LG_UNLOCK(input->lock);

  if (wake)
    lgSignalEvent(input->event);
  return result;
}

static bool inputMousePosition(void * opaque, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
  if (!width || !height || x >= width || y >= height)
    return false;

  const uint16_t absoluteX = width == 1 ? 0 :
    (uint16_t)((uint64_t)x * KVMFR_INPUT_MOUSE_ABSOLUTE_MAX /
      (width - 1));
  const uint16_t absoluteY = height == 1 ? 0 :
    (uint16_t)((uint64_t)y * KVMFR_INPUT_MOUSE_ABSOLUTE_MAX /
      (height - 1));

  LGMPInput * input = opaque;
  bool wake = false;
  LG_LOCK(input->lock);
  if (!input->available ||
      !(input->capabilities & KVMFR_INPUT_CAP_MOUSE_ABSOLUTE))
  {
    LG_UNLOCK(input->lock);
    return false;
  }

  const uint16_t previousX = input->absoluteX;
  const uint16_t previousY = input->absoluteY;
  input->absoluteX = absoluteX;
  input->absoluteY = absoluteY;
  if (input->ownerBlocked)
  {
    input->mouseMode = LGMP_INPUT_MOUSE_ABSOLUTE;
    LG_UNLOCK(input->lock);
    return true;
  }

  bool result = input->connected && claim(input, &wake) &&
    queueMouse(input, LGMP_INPUT_MOUSE_ABSOLUTE,
      0, 0, 0, input->mouseButtons, true, &wake);
  if (result)
    input->mouseMode = LGMP_INPUT_MOUSE_ABSOLUTE;
  else
  {
    input->absoluteX = previousX;
    input->absoluteY = previousY;
  }
  LG_UNLOCK(input->lock);

  if (wake)
    lgSignalEvent(input->event);
  return result;
}

static uint32_t mouseButtonMask(unsigned int button)
{
  switch (button)
  {
    case 1:
      return UINT32_C(1) << 0;

    case 2:
      return UINT32_C(1) << 2;

    case 3:
      return UINT32_C(1) << 1;

    default:
      if (button >= 6 && button < 32)
        return UINT32_C(1) << (button - 3);
      return 0;
  }
}

static bool updateMouseButton(void * opaque, unsigned int button,
    bool pressed)
{
  if (button == 4 || button == 5)
  {
    if (!pressed)
      return true;

    LGMPInput * input = opaque;
    bool wake = false;
    LG_LOCK(input->lock);
    if (input->connected && input->available && input->ownerBlocked)
    {
      LG_UNLOCK(input->lock);
      return true;
    }

    const enum LGMPInputMouseMode mode =
      input->mouseMode == LGMP_INPUT_MOUSE_ABSOLUTE ?
        LGMP_INPUT_MOUSE_ABSOLUTE : LGMP_INPUT_MOUSE_RELATIVE;
    const bool result = input->connected && input->available &&
      claim(input, &wake) &&
      queueMouse(input, mode, 0, 0, button == 4 ? 1 : -1,
        input->mouseButtons, false, &wake);
    LG_UNLOCK(input->lock);
    if (wake)
      lgSignalEvent(input->event);
    return result;
  }

  const uint32_t mask = mouseButtonMask(button);
  if (!mask)
    return false;

  LGMPInput * input = opaque;
  bool wake = false;
  LG_LOCK(input->lock);
  const uint32_t buttons = pressed ?
    input->mouseButtons | mask : input->mouseButtons & ~mask;
  const enum LGMPInputMouseMode mode =
    input->mouseMode == LGMP_INPUT_MOUSE_ABSOLUTE ?
      LGMP_INPUT_MOUSE_ABSOLUTE : LGMP_INPUT_MOUSE_RELATIVE;
  if (input->connected && input->available && input->ownerBlocked)
  {
    input->mouseButtons = buttons;
    input->mouseMode    = mode;
    LG_UNLOCK(input->lock);
    return true;
  }

  const uint64_t overflowFailures =
    input->stats.counters.discreteOverflowFailures;
  bool result = input->connected && input->available &&
    claim(input, &wake) &&
    queueMouse(input, mode, 0, 0, 0, buttons, false, &wake);
  bool reset = false;
  if (!result && !pressed && input->connected)
  {
    if (input->stats.counters.discreteOverflowFailures !=
        overflowFailures)
      ++input->stats.counters.discreteOverflowResets;
    reset  = release(input, &wake);
    result = reset;
  }
  if (result && !reset)
  {
    input->mouseButtons = buttons;
    input->mouseMode    = mode;
  }
  LG_UNLOCK(input->lock);

  if (wake)
    lgSignalEvent(input->event);
  return result;
}

static bool inputMousePress(void * opaque, unsigned int button)
{
  return updateMouseButton(opaque, button, true);
}

static bool inputMouseRelease(void * opaque, unsigned int button)
{
  return updateMouseButton(opaque, button, false);
}

static void inputReset(void * opaque)
{
  LGMPInput * input = opaque;
  bool wake = false;
  LG_LOCK(input->lock);
  if (input->connected)
    release(input, &wake);
  else
    clearInputState(input);
  LG_UNLOCK(input->lock);

  if (wake)
    lgSignalEvent(input->event);
}

static const LG_InputOps INPUT_OPS =
{
  .name              = "LGMP",
  .supports          = inputSupports,
  .setStatusListener = inputSetStatusListener,
  .keyDown           = inputKeyDown,
  .keyUp             = inputKeyUp,
  .keyboardLEDs      = NULL,
  .mouseMotion       = inputMouseMotion,
  .mousePosition     = inputMousePosition,
  .mousePress        = inputMousePress,
  .mouseRelease      = inputMouseRelease,
  .reset             = inputReset,
};

const LG_InputOps * lgmpInput_getOps(void)
{
  return &INPUT_OPS;
}
