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

#include "common/event.h"
#include "common/locking.h"
#include "common/time.h"

#include <LGProtocol/NetworkServices.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_INPUT_TIMER_MS            50U
#define NETWORK_INPUT_LEASE_MS            500U
#define NETWORK_INPUT_KEEPALIVE_MS        200U
#define NETWORK_INPUT_CLAIM_RETRY_MS      500U
#define NETWORK_INPUT_LED_SYNC_DELAY_MS   20U
#define NETWORK_INPUT_LED_ACK_TIMEOUT_MS  1000U
#define NETWORK_INPUT_USAGE_PAGE_KEYBOARD 0x07U
#define NETWORK_INPUT_KEYBOARD_USAGE_MAX  231U
#define NETWORK_INPUT_QUEUE_SIZE          128U
#define NETWORK_INPUT_QUEUE_RESERVE       2U
#define NETWORK_INPUT_QUEUE_CAPACITY      \
  (NETWORK_INPUT_QUEUE_SIZE + NETWORK_INPUT_QUEUE_RESERVE)
#define NETWORK_INPUT_QUEUE_PAYLOAD_MAX   LG_NET_INPUT_ABSOLUTE_WIRE_SIZE

#define NETWORK_INPUT_CLAIM_FLAGS \
  (LG_NET_INPUT_CLAIM_KEYBOARD | LG_NET_INPUT_CLAIM_POINTER | \
    LG_NET_INPUT_CLAIM_EXCLUSIVE)

#define NETWORK_INPUT_LED_MASK \
  (LG_NET_KEYBOARD_LED_NUM_LOCK | LG_NET_KEYBOARD_LED_CAPS_LOCK | \
    LG_NET_KEYBOARD_LED_SCROLL_LOCK)

struct NetworkInputLockKey
{
  uint32_t led;
  uint16_t usage;
};

typedef struct NetworkInputPkt
{
  uint8_t            payload[NETWORK_INPUT_QUEUE_PAYLOAD_MAX];
  uint64_t           componentEpoch;
  uint64_t           requestID;
  uint32_t           payloadLength;
  LGNetEnvelopeFlags flags;
  uint16_t           messageType;
}
NetworkInputPkt;

_Static_assert(LG_NET_INPUT_CLAIM_WIRE_SIZE <=
      NETWORK_INPUT_QUEUE_PAYLOAD_MAX &&
    LG_NET_INPUT_RELATIVE_WIRE_SIZE <=
      NETWORK_INPUT_QUEUE_PAYLOAD_MAX &&
    LG_NET_INPUT_ABSOLUTE_WIRE_SIZE <=
      NETWORK_INPUT_QUEUE_PAYLOAD_MAX &&
    LG_NET_INPUT_KEYBOARD_WIRE_SIZE <=
      NETWORK_INPUT_QUEUE_PAYLOAD_MAX &&
    LG_NET_INPUT_CONTROL_WIRE_SIZE <=
      NETWORK_INPUT_QUEUE_PAYLOAD_MAX,
    "Network input reliable queue payloads are too small");

static const struct NetworkInputLockKey NETWORK_INPUT_LOCK_KEYS[] =
{
  { LG_NET_KEYBOARD_LED_NUM_LOCK,    83 },
  { LG_NET_KEYBOARD_LED_CAPS_LOCK,   57 },
  { LG_NET_KEYBOARD_LED_SCROLL_LOCK, 71 },
};

struct NetworkInput
{
  NetworkConnection * connection;
  LGTimer           * timer;
  LGEvent           * statusIdle;

  LG_Lock stateLock;

  bool     destroying;
  bool     connected;
  bool     serviceAvailable;
  bool     datagramAvailable;
  bool     claimPending;
  bool     claimed;
  bool     available;
  uint64_t claimantID;
  uint64_t claimEpoch;
  uint64_t sequence;
  uint64_t pointerSequence;
  uint64_t nextClaimNs;
  uint64_t nextKeepaliveNs;

  uint32_t         claimFlags;
  uint16_t         serviceVersion;
  uint32_t         mouseButtons;
  bool             mouseAbsolute;
  uint32_t         absoluteX;
  uint32_t         absoluteY;
  uint32_t         absoluteWidth;
  uint32_t         absoluteHeight;
  int64_t          cumulativeX;
  int64_t          cumulativeY;
  bool             pendingRelative;
  bool             pendingAbsolute;
  bool             pendingMotionReliable;
  bool             motionBarrier;
  uint64_t         reliableBarrierSequence;
  NetworkInputPkt  reliableQueue[NETWORK_INPUT_QUEUE_CAPACITY];
  size_t           reliableHead;
  size_t           reliableCount;
  /* Preserve every held usage. The receiver reconstructs a six-key boot
   * report and emits ErrorRollOver while more than six keys are held. */
  bool             keyState[NETWORK_INPUT_KEYBOARD_USAGE_MAX + 1U];
  uint8_t          modifiers;
  bool             keyboardLEDsValid;
  uint8_t          keyboardLEDs;
  uint32_t         keyboardLEDsValidMask;
  uint32_t         pendingKeyboardLEDs;
  uint16_t         pendingKeyboardLEDCounts[
    sizeof(NETWORK_INPUT_LOCK_KEYS) / sizeof(*NETWORK_INPUT_LOCK_KEYS)];
  uint64_t         pendingKeyboardLEDsUntil;
  uint64_t         keyboardLEDsSyncAfter;
  uint32_t         staleKeyboardLEDs;
  uint64_t         staleKeyboardLEDsUntil;
  uint64_t         keyboardLEDSequence;
  bool             desiredKeyboardLEDsValid;
  uint8_t          desiredKeyboardLEDs;

  uint32_t         statusGeneration;
  bool             statusNotifyPending;
  bool             statusDispatching;
  bool             sendReadyPending;
  LG_InputStatusFn statusCallback;
  void           * statusOpaque;
};

static _Thread_local NetworkInput * l_networkInputCallback;
static _Thread_local NetworkInput * l_networkInputSending;

static void networkInputResumeSendsNL(NetworkInput * input, bool * notify);

static uint64_t networkInputNext64(uint64_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint32_t networkInputNext32(uint32_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint64_t networkInputDeadline(uint32_t milliseconds)
{
  return nanotime() + (uint64_t)milliseconds * UINT64_C(1000000);
}

static bool networkInputKeyboardHeldNL(const NetworkInput * input)
{
  for (size_t usage = 1;
      usage <= NETWORK_INPUT_KEYBOARD_USAGE_MAX; ++usage)
    if (input->keyState[usage])
      return true;
  return false;
}

static bool networkInputLEDTransitionsNL(const NetworkInput * input)
{
  for (size_t i = 0;
      i < sizeof(NETWORK_INPUT_LOCK_KEYS) /
        sizeof(*NETWORK_INPUT_LOCK_KEYS); ++i)
    if (input->pendingKeyboardLEDCounts[i])
      return true;
  return false;
}

static uint32_t networkInputProjectedLEDsNL(const NetworkInput * input)
{
  return input->keyboardLEDs ^ input->pendingKeyboardLEDs;
}

static void networkInputClearLEDProjectionNL(NetworkInput * input)
{
  input->pendingKeyboardLEDs      = 0;
  input->pendingKeyboardLEDsUntil = 0;
  memset(input->pendingKeyboardLEDCounts, 0,
    sizeof(input->pendingKeyboardLEDCounts));
}

static void networkInputInvalidateLEDsNL(NetworkInput * input)
{
  input->keyboardLEDsValid       = false;
  input->keyboardLEDs            = 0;
  input->keyboardLEDsValidMask   = 0;
  input->keyboardLEDSequence     = 0;
  input->keyboardLEDsSyncAfter   = 0;
  input->staleKeyboardLEDs       = 0;
  input->staleKeyboardLEDsUntil  = 0;
  networkInputClearLEDProjectionNL(input);
}

static void networkInputRecordLEDToggleNL(NetworkInput * input,
    uint32_t led, bool physical)
{
  for (size_t i = 0;
      i < sizeof(NETWORK_INPUT_LOCK_KEYS) /
        sizeof(*NETWORK_INPUT_LOCK_KEYS); ++i)
  {
    if (NETWORK_INPUT_LOCK_KEYS[i].led != led)
      continue;

    if (input->pendingKeyboardLEDCounts[i] != UINT16_MAX)
      ++input->pendingKeyboardLEDCounts[i];
    input->pendingKeyboardLEDs      ^= led;
    input->pendingKeyboardLEDsUntil = networkInputDeadline(
        NETWORK_INPUT_LED_ACK_TIMEOUT_MS);
    break;
  }

  if (physical)
  {
    if (!input->desiredKeyboardLEDsValid ||
        ((networkInputProjectedLEDsNL(input) ^
          input->desiredKeyboardLEDs) & led))
      input->staleKeyboardLEDs |= led;
    else
      input->staleKeyboardLEDs &= ~led;

    input->staleKeyboardLEDsUntil = input->staleKeyboardLEDs ?
      networkInputDeadline(NETWORK_INPUT_LED_ACK_TIMEOUT_MS) : 0;
  }

  input->keyboardLEDsSyncAfter = networkInputDeadline(
      NETWORK_INPUT_LED_SYNC_DELAY_MS);
}

static void networkInputAcknowledgeLEDsNL(NetworkInput * input,
    uint32_t changed)
{
  for (size_t i = 0;
      i < sizeof(NETWORK_INPUT_LOCK_KEYS) /
        sizeof(*NETWORK_INPUT_LOCK_KEYS); ++i)
  {
    const uint32_t led = NETWORK_INPUT_LOCK_KEYS[i].led;
    if (!(changed & led) || !input->pendingKeyboardLEDCounts[i])
      continue;

    --input->pendingKeyboardLEDCounts[i];
    input->pendingKeyboardLEDs ^= led;
  }

  if (!networkInputLEDTransitionsNL(input))
    input->pendingKeyboardLEDsUntil = 0;
}

static LG_InputStatus networkInputStatusNL(const NetworkInput * input)
{
  return (LG_InputStatus)
  {
    .available         = input->available,
    .generation        = input->statusGeneration,
    .keyboardLEDsValid = input->available && input->keyboardLEDsValid,
    .keyboardLEDs      = input->keyboardLEDs,
  };
}

static void networkInputDispatchStatus(NetworkInput * input,
    LG_InputStatusFn callback, void * opaque, LG_InputStatus status)
{
  while (callback)
  {
    NetworkInput * previous      = l_networkInputCallback;
    l_networkInputCallback       = input;
    callback(opaque, &status);
    l_networkInputCallback       = previous;

    LG_LOCK(input->stateLock);
    if (input->statusNotifyPending && input->statusCallback)
    {
      input->statusNotifyPending = false;
      callback                   = input->statusCallback;
      opaque                     = input->statusOpaque;
      status                     = networkInputStatusNL(input);
      LG_UNLOCK(input->stateLock);
      continue;
    }

    input->statusDispatching = false;
    lgSignalEvent(input->statusIdle);
    LG_UNLOCK(input->stateLock);
    break;
  }
}

static void networkInputNotifyStatus(NetworkInput * input)
{
  LG_InputStatusFn callback;
  void           * opaque;
  LG_InputStatus   status;

  LG_LOCK(input->stateLock);
  if (!input->statusNotifyPending || input->statusDispatching)
  {
    LG_UNLOCK(input->stateLock);
    return;
  }

  input->statusNotifyPending = false;
  callback                   = input->statusCallback;
  opaque                     = input->statusOpaque;
  status                     = networkInputStatusNL(input);
  if (!callback)
  {
    LG_UNLOCK(input->stateLock);
    return;
  }

  input->statusDispatching = true;
  lgResetEvent(input->statusIdle);
  LG_UNLOCK(input->stateLock);

  networkInputDispatchStatus(input, callback, opaque, status);
}

static bool networkInputSetAvailableNL(NetworkInput * input, bool available)
{
  if (input->available == available)
    return false;

  input->available           = available;
  input->statusGeneration    =
    networkInputNext32(input->statusGeneration);
  input->statusNotifyPending = true;
  return true;
}

static void networkInputClearStateNL(NetworkInput * input)
{
  input->mouseButtons            = 0;
  input->mouseAbsolute           = false;
  input->absoluteX               = 0;
  input->absoluteY               = 0;
  input->absoluteWidth           = 0;
  input->absoluteHeight          = 0;
  input->cumulativeX             = 0;
  input->cumulativeY             = 0;
  input->pointerSequence         = 0;
  input->pendingRelative         = false;
  input->pendingAbsolute         = false;
  input->pendingMotionReliable   = false;
  input->motionBarrier           = true;
  input->reliableBarrierSequence = 0;
  input->sendReadyPending        = false;
  memset(input->keyState, 0, sizeof(input->keyState));
  input->modifiers      = 0;
  networkInputClearLEDProjectionNL(input);
}

static void networkInputClearReliableNL(NetworkInput * input)
{
  input->reliableHead  = 0;
  input->reliableCount = 0;
}

static LG_TransportStatus networkInputSendReliableNL(NetworkInput * input,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t componentEpoch,
    uint64_t requestID, const void * payload, uint32_t payloadLength)
{
  NetworkInput      * previous;
  LG_TransportStatus  status;

  previous              = l_networkInputSending;
  l_networkInputSending = input;
  status                = networkConnectionSend(
      input->connection, LG_NET_SERVICE_INPUT,
      messageType, input->serviceVersion,
      flags | LG_NET_ENVELOPE_FINAL, componentEpoch, requestID,
      payload, payloadLength);
  l_networkInputSending = previous;
  return status;
}

static LG_TransportStatus networkInputSendDatagramNL(NetworkInput * input,
    uint16_t messageType, uint64_t applicationSequence,
    const void * payload, uint32_t payloadLength)
{
  NetworkInput      * previous;
  LG_TransportStatus  status;

  previous              = l_networkInputSending;
  l_networkInputSending = input;
  status                = networkConnectionSendDatagram(
      input->connection, LG_NET_SERVICE_INPUT, messageType,
      input->serviceVersion, 0, input->claimEpoch, 0,
      payload, payloadLength, applicationSequence, true);
  l_networkInputSending = previous;
  return status;
}

static bool networkInputTransportFatal(LG_TransportStatus status)
{
  return status != LG_TRANSPORT_OK && status != LG_TRANSPORT_UNAVAILABLE &&
    status != LG_TRANSPORT_TIMEOUT;
}

static bool networkInputQueueReliableNL(NetworkInput * input,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  const bool   emergency = messageType == LG_NET_INPUT_MESSAGE_RESET ||
    messageType == LG_NET_INPUT_MESSAGE_RELEASE;
  const size_t limit     = emergency ?
    NETWORK_INPUT_QUEUE_CAPACITY : NETWORK_INPUT_QUEUE_SIZE;
  if (input->reliableCount >= limit ||
      payloadLength > NETWORK_INPUT_QUEUE_PAYLOAD_MAX ||
      (payloadLength && !payload))
    return false;

  const size_t      index  =
    (input->reliableHead + input->reliableCount) %
      NETWORK_INPUT_QUEUE_CAPACITY;
  NetworkInputPkt * packet = &input->reliableQueue[index];
  if (payloadLength)
    memcpy(packet->payload, payload, payloadLength);
  packet->componentEpoch = input->claimEpoch;
  packet->requestID      = requestID;
  packet->payloadLength  = payloadLength;
  packet->flags          = flags;
  packet->messageType    = messageType;
  ++input->reliableCount;
  return true;
}

static LG_TransportStatus networkInputDrainReliableNL(NetworkInput * input)
{
  if (!input->reliableCount)
    return LG_TRANSPORT_OK;

  const NetworkInputPkt   * packet =
    &input->reliableQueue[input->reliableHead];
  const LG_TransportStatus  status = networkInputSendReliableNL(input,
      packet->messageType, packet->flags, packet->componentEpoch,
      packet->requestID, packet->payload, packet->payloadLength);
  if (status != LG_TRANSPORT_OK)
    return status;

  input->reliableHead =
    (input->reliableHead + 1U) % NETWORK_INPUT_QUEUE_CAPACITY;
  if (!--input->reliableCount)
    input->reliableHead = 0;
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus networkInputSubmitReliableNL(NetworkInput * input,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  if (!input->reliableCount)
  {
    const LG_TransportStatus status = networkInputSendReliableNL(input,
        messageType, flags, input->claimEpoch, requestID,
        payload, payloadLength);
    if (status == LG_TRANSPORT_OK || networkInputTransportFatal(status))
      return status;
  }

  return networkInputQueueReliableNL(input, messageType, flags, requestID,
      payload, payloadLength) ? LG_TRANSPORT_OK : LG_TRANSPORT_ERROR;
}

static bool networkInputSendClaimNL(NetworkInput * input)
{
  if (!input->connected || !input->serviceAvailable || input->destroying ||
      input->reliableCount)
    return false;

  networkInputClearReliableNL(input);
  networkInputClearStateNL(input);
  networkInputInvalidateLEDsNL(input);
  const uint64_t        claimEpoch = networkInputNext64(input->claimEpoch);
  const LGNetInputClaim claim      =
  {
    .claimantID = input->claimantID,
    .claimEpoch = claimEpoch,
    .leaseMs    = NETWORK_INPUT_LEASE_MS,
    .flags      = NETWORK_INPUT_CLAIM_FLAGS,
  };
  uint8_t payload[LG_NET_INPUT_CLAIM_WIRE_SIZE];
  if (!lgNetInputClaimEncode(payload, sizeof(payload), &claim))
    return false;

  input->claimEpoch = claimEpoch;
  input->sequence   = 0;
  const LG_TransportStatus status = networkInputSendReliableNL(input,
      LG_NET_INPUT_MESSAGE_CLAIM,
      LG_NET_ENVELOPE_ACK_REQUIRED, claimEpoch, claimEpoch,
      payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
  {
    input->nextClaimNs = networkInputDeadline(
        NETWORK_INPUT_CLAIM_RETRY_MS);
    return false;
  }

  input->claimPending    = true;
  input->claimed         = false;
  input->claimFlags      = 0;
  input->nextClaimNs     = networkInputDeadline(NETWORK_INPUT_LEASE_MS);
  input->nextKeepaliveNs = 0;
  return true;
}

static LG_TransportStatus networkInputSendControlNL(NetworkInput * input,
    uint16_t messageType, uint32_t flags, uint32_t leaseMs)
{
  const uint64_t          sequence = networkInputNext64(input->sequence);
  const LGNetInputControl control  =
  {
    .claimantID = input->claimantID,
    .claimEpoch = input->claimEpoch,
    .sequence   = sequence,
    .flags      = flags,
    .leaseMs    = leaseMs,
  };
  uint8_t payload[LG_NET_INPUT_CONTROL_WIRE_SIZE];
  if (!lgNetInputControlEncode(payload, sizeof(payload), &control))
    return LG_TRANSPORT_ERROR;

  const LG_TransportStatus status = networkInputSubmitReliableNL(input,
      messageType, 0, 0, payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return status;

  input->sequence = sequence;
  return LG_TRANSPORT_OK;
}

static bool networkInputAbandonClaimNL(NetworkInput * input,
    bool tryReset)
{
  networkInputClearReliableNL(input);
  networkInputClearStateNL(input);
  networkInputInvalidateLEDsNL(input);
  if (input->claimed && networkConnectionValid(input->connection))
  {
    if (tryReset)
      networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RESET,
        LG_NET_INPUT_CLAIM_KEYBOARD | LG_NET_INPUT_CLAIM_POINTER, 0);
    networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RELEASE,
        input->claimFlags, 0);
  }

  input->claimPending    = false;
  input->claimed         = false;
  input->claimFlags      = 0;
  input->claimEpoch      = networkInputNext64(input->claimEpoch);
  input->nextKeepaliveNs = 0;
  input->nextClaimNs     = networkInputDeadline(
      NETWORK_INPUT_CLAIM_RETRY_MS);
  return networkInputSetAvailableNL(input, false);
}

static bool networkInputSendResetNL(NetworkInput * input)
{
  LG_TransportStatus status = LG_TRANSPORT_OK;
  if (input->claimed)
  {
    networkInputClearReliableNL(input);
    networkInputClearStateNL(input);
    status = networkConnectionValid(input->connection) ?
      networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RESET,
        LG_NET_INPUT_CLAIM_KEYBOARD | LG_NET_INPUT_CLAIM_POINTER, 0) :
      LG_TRANSPORT_DISCONNECTED;
  }
  else
    networkInputClearStateNL(input);
  if (status == LG_TRANSPORT_DISCONNECTED)
    networkInputClearReliableNL(input);
  if (!networkInputTransportFatal(status))
    return false;
  return networkInputAbandonClaimNL(input, false);
}

static bool networkInputSendReleaseNL(NetworkInput * input)
{
  bool sent = true;
  if ((input->claimed || input->claimPending) &&
      networkConnectionValid(input->connection))
    sent = networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RELEASE,
        input->claimFlags, 0) == LG_TRANSPORT_OK;

  input->claimPending    = false;
  input->claimed         = false;
  input->claimFlags      = 0;
  input->nextKeepaliveNs = 0;
  return sent;
}

static bool networkInputNextSequenceNL(NetworkInput * input,
    uint64_t * sequence)
{
  if (!input->available || !input->claimed)
    return false;

  *sequence = networkInputNext64(input->sequence);
  return true;
}

static LG_TransportStatus networkInputSendKeyboardNL(NetworkInput * input,
    uint16_t usage, bool down, bool repeat)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return LG_TRANSPORT_UNAVAILABLE;

  LGNetInputKeyboard keyboard =
  {
    .sequence    = sequence,
    .timestampNs = nanotime(),
    .usagePage   = NETWORK_INPUT_USAGE_PAGE_KEYBOARD,
    .usage       = usage,
    .scanCode    = 0,
    .flags       = (down ? LG_NET_KEYBOARD_DOWN : 0) |
      (repeat ? LG_NET_KEYBOARD_REPEAT : 0),
    .modifiers   = input->modifiers,
  };
  if (!keyboard.timestampNs)
    keyboard.timestampNs = 1;

  uint8_t payload[LG_NET_INPUT_KEYBOARD_WIRE_SIZE];
  if (!lgNetInputKeyboardEncode(payload, sizeof(payload), &keyboard))
    return LG_TRANSPORT_ERROR;

  const LG_TransportStatus status = networkInputSubmitReliableNL(input,
      LG_NET_INPUT_MESSAGE_KEYBOARD, 0, 0, payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return status;

  input->sequence = sequence;
  return LG_TRANSPORT_OK;
}

static uint32_t networkInputLEDForUsage(uint16_t usage)
{
  switch (usage)
  {
    case 83:
      return LG_NET_KEYBOARD_LED_NUM_LOCK;

    case 57:
      return LG_NET_KEYBOARD_LED_CAPS_LOCK;

    case 71:
      return LG_NET_KEYBOARD_LED_SCROLL_LOCK;

    default:
      return 0;
  }
}

static bool networkInputTapLockKeyNL(NetworkInput * input,
    const struct NetworkInputLockKey * key, bool * notify)
{
  if (key->usage > NETWORK_INPUT_KEYBOARD_USAGE_MAX ||
      input->keyState[key->usage])
    return false;

  if (networkInputSendKeyboardNL(input, key->usage, true, false) !=
      LG_TRANSPORT_OK)
    return false;
  networkInputRecordLEDToggleNL(input, key->led, false);
  if (networkInputSendKeyboardNL(input, key->usage, false, false) ==
      LG_TRANSPORT_OK)
    return true;

  *notify |= networkInputSendResetNL(input);
  input->keyboardLEDsSyncAfter = networkInputDeadline(
      NETWORK_INPUT_LED_ACK_TIMEOUT_MS);
  return false;
}

static void networkInputSyncKeyboardLEDsNL(NetworkInput * input,
    bool * notify)
{
  const uint64_t now = nanotime();
  if (networkInputLEDTransitionsNL(input) &&
      now >= input->pendingKeyboardLEDsUntil)
  {
    networkInputClearLEDProjectionNL(input);
    input->keyboardLEDsSyncAfter = now +
      (uint64_t)NETWORK_INPUT_LED_SYNC_DELAY_MS * UINT64_C(1000000);
  }

  if (input->staleKeyboardLEDs &&
      now >= input->staleKeyboardLEDsUntil)
  {
    input->desiredKeyboardLEDsValid = false;
    input->staleKeyboardLEDs        = 0;
    input->staleKeyboardLEDsUntil   = 0;
  }

  if (!input->available || !input->keyboardLEDsValid ||
      !input->desiredKeyboardLEDsValid ||
      networkInputLEDTransitionsNL(input) ||
      now < input->keyboardLEDsSyncAfter)
    return;

  const uint32_t difference =
    (networkInputProjectedLEDsNL(input) ^
      input->desiredKeyboardLEDs) &
    NETWORK_INPUT_LED_MASK & ~input->staleKeyboardLEDs;
  if (!difference)
  {
    if (!input->staleKeyboardLEDs)
      input->desiredKeyboardLEDsValid = false;
    return;
  }
  if (networkInputKeyboardHeldNL(input))
    return;

  for (size_t i = 0;
      i < sizeof(NETWORK_INPUT_LOCK_KEYS) /
        sizeof(*NETWORK_INPUT_LOCK_KEYS); ++i)
  {
    if (!(difference & NETWORK_INPUT_LOCK_KEYS[i].led))
      continue;
    if (!networkInputTapLockKeyNL(input,
          &NETWORK_INPUT_LOCK_KEYS[i], notify))
      break;
  }
}

static bool networkInputTimer(void * opaque)
{
  NetworkInput * input  = opaque;
  bool           notify = false;
  LG_LOCK(input->stateLock);
  if (input->destroying)
  {
    LG_UNLOCK(input->stateLock);
    return false;
  }

  const uint64_t now = nanotime();

  if (input->connected && input->serviceAvailable)
  {
    if (input->sendReadyPending || input->reliableCount ||
        input->pendingRelative || input->pendingAbsolute)
      networkInputResumeSendsNL(input, &notify);
    if (input->claimPending && now >= input->nextClaimNs)
      input->claimPending = false;
    if (!input->claimPending && !input->claimed &&
        now >= input->nextClaimNs)
      networkInputSendClaimNL(input);
    else if (input->claimed && now >= input->nextKeepaliveNs)
    {
      const LG_TransportStatus status = networkInputSendControlNL(input,
          LG_NET_INPUT_MESSAGE_KEEPALIVE,
          input->claimFlags, NETWORK_INPUT_LEASE_MS);
      if (status == LG_TRANSPORT_OK)
        input->nextKeepaliveNs = networkInputDeadline(
            NETWORK_INPUT_KEEPALIVE_MS);
      else if (networkInputTransportFatal(status))
        notify |= networkInputAbandonClaimNL(input, true);
    }

    networkInputSyncKeyboardLEDsNL(input, &notify);
    if (input->sendReadyPending)
      networkInputResumeSendsNL(input, &notify);
  }
  LG_UNLOCK(input->stateLock);

  networkInputNotifyStatus(input);
  return true;
}

static bool networkInputSupports(void * opaque, LG_InputSupport support)
{
  NetworkInput * input = opaque;
  if (support != LG_INPUT_SUPPORT_MOUSE_ABSOLUTE)
    return false;

  LG_LOCK(input->stateLock);
  const bool supported = input->available &&
    (input->claimFlags & LG_NET_INPUT_CLAIM_POINTER);
  LG_UNLOCK(input->stateLock);
  return supported;
}

static void networkInputSetStatusListener(void * opaque,
    LG_InputStatusFn callback, void * callbackOpaque)
{
  NetworkInput * input     = opaque;
  const bool     reentrant = l_networkInputCallback == input;

  LG_LOCK(input->stateLock);
  input->statusCallback      = NULL;
  input->statusOpaque        = NULL;
  input->statusNotifyPending = false;
  const bool dispatching = input->statusDispatching;
  LG_UNLOCK(input->stateLock);

  if (dispatching && !reentrant)
    lgWaitEvent(input->statusIdle, TIMEOUT_INFINITE);
  if (!callback)
    return;

  LG_LOCK(input->stateLock);
  input->statusCallback      = callback;
  input->statusOpaque        = callbackOpaque;
  input->statusNotifyPending = false;
  const LG_InputStatus status        = networkInputStatusNL(input);
  const bool           startDispatch = !input->statusDispatching;
  if (startDispatch)
  {
    input->statusDispatching = true;
    lgResetEvent(input->statusIdle);
  }
  LG_UNLOCK(input->stateLock);

  if (startDispatch)
    networkInputDispatchStatus(input, callback, callbackOpaque, status);
  else
  {
    NetworkInput * previous      = l_networkInputCallback;
    l_networkInputCallback       = input;
    callback(callbackOpaque, &status);
    l_networkInputCallback       = previous;
  }
}

static bool networkInputUpdateKey(void * opaque, int key, bool down)
{
  if (key < 0 || key >= LG_KEY_MAX)
    return false;

  const uint16_t usage = lg_keycode_to_hid[key];
  if (!usage || usage > NETWORK_INPUT_KEYBOARD_USAGE_MAX)
    return true;

  NetworkInput * input  = opaque;
  bool           notify = false;
  LG_LOCK(input->stateLock);
  if (!input->available ||
      !(input->claimFlags & LG_NET_INPUT_CLAIM_KEYBOARD))
  {
    LG_UNLOCK(input->stateLock);
    return false;
  }

  const bool previous = input->keyState[usage];
  if (previous == down)
  {
    LG_UNLOCK(input->stateLock);
    return true;
  }

  input->keyState[usage] = down;
  if (usage >= 224 && usage <= 231)
  {
    const uint8_t mask = UINT8_C(1) << (usage - 224);
    if (down)
      input->modifiers |= mask;
    else
      input->modifiers &= (uint8_t)~mask;
  }

  const LG_TransportStatus status =
    networkInputSendKeyboardNL(input, usage, down, false);
  if (status != LG_TRANSPORT_OK)
  {
    input->keyState[usage] = previous;
    if (usage >= 224 && usage <= 231)
    {
      const uint8_t mask = UINT8_C(1) << (usage - 224);
      if (previous)
        input->modifiers |= mask;
      else
        input->modifiers &= (uint8_t)~mask;
    }
    if (!down && networkInputTransportFatal(status))
      notify = networkInputAbandonClaimNL(input, true);
    if (input->sendReadyPending)
      networkInputResumeSendsNL(input, &notify);
    LG_UNLOCK(input->stateLock);
    if (notify)
      networkInputNotifyStatus(input);
    return false;
  }

  const uint32_t led = networkInputLEDForUsage(usage);
  if (down && led)
  {
    if (input->keyboardLEDsValid)
      networkInputRecordLEDToggleNL(input, led, true);
    else
    {
      input->desiredKeyboardLEDsValid = false;
      input->keyboardLEDsSyncAfter    = networkInputDeadline(
          NETWORK_INPUT_LED_SYNC_DELAY_MS);
    }
  }
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return true;
}

static bool networkInputKeyDown(void * opaque, int key)
{
  return networkInputUpdateKey(opaque, key, true);
}

static bool networkInputKeyUp(void * opaque, int key)
{
  return networkInputUpdateKey(opaque, key, false);
}

static bool networkInputKeyboardLEDs(void * opaque, bool numLock,
    bool capsLock, bool scrollLock)
{
  NetworkInput * input   = opaque;
  bool           notify  = false;
  const uint8_t  desired =
    (numLock    ? LG_NET_KEYBOARD_LED_NUM_LOCK    : 0) |
    (capsLock   ? LG_NET_KEYBOARD_LED_CAPS_LOCK   : 0) |
    (scrollLock ? LG_NET_KEYBOARD_LED_SCROLL_LOCK : 0);

  LG_LOCK(input->stateLock);
  input->desiredKeyboardLEDs      = desired;
  input->desiredKeyboardLEDsValid = true;
  input->staleKeyboardLEDs        &=
    networkInputProjectedLEDsNL(input) ^ desired;
  if (!input->staleKeyboardLEDs)
    input->staleKeyboardLEDsUntil = 0;
  const uint64_t syncAfter = networkInputDeadline(
      NETWORK_INPUT_LED_SYNC_DELAY_MS);
  if (input->keyboardLEDsSyncAfter < syncAfter)
    input->keyboardLEDsSyncAfter = syncAfter;
  networkInputSyncKeyboardLEDsNL(input, &notify);
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  const bool available = input->available;
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return available;
}

static void networkInputKeyboardLEDsReset(void * opaque)
{
  NetworkInput * input = opaque;
  LG_LOCK(input->stateLock);
  input->desiredKeyboardLEDsValid = false;
  input->keyboardLEDsSyncAfter    = 0;
  input->staleKeyboardLEDs        = 0;
  input->staleKeyboardLEDsUntil   = 0;
  LG_UNLOCK(input->stateLock);
}

static int64_t networkInputSaturatingAdd(int64_t value, int32_t delta)
{
  if (delta > 0 && value > INT64_MAX - delta)
    return INT64_MAX;
  if (delta < 0 && value < INT64_MIN - delta)
    return INT64_MIN;
  return value + delta;
}

static LG_TransportStatus networkInputTransmitRelativeNL(
    NetworkInput * input, int32_t wheelX, int32_t wheelY,
    uint32_t buttons, uint32_t changedButtons, bool datagram,
    bool acknowledge)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return LG_TRANSPORT_UNAVAILABLE;

  LGNetInputRelative relative =
  {
    .sequence       = sequence,
    .timestampNs    = nanotime(),
    .cumulativeX    = input->cumulativeX,
    .cumulativeY    = input->cumulativeY,
    .wheelX         = wheelX,
    .wheelY         = wheelY,
    .buttons        = buttons,
    .changedButtons = changedButtons,
  };
  if (!relative.timestampNs)
    relative.timestampNs = 1;

  uint8_t payload[LG_NET_INPUT_RELATIVE_WIRE_SIZE];
  if (!lgNetInputRelativeEncode(payload, sizeof(payload), &relative))
    return LG_TRANSPORT_ERROR;

  const LG_TransportStatus status = datagram ?
    networkInputSendDatagramNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE, sequence,
        payload, sizeof(payload)) :
    networkInputSubmitReliableNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE,
        acknowledge ? LG_NET_ENVELOPE_ACK_REQUIRED : 0,
        acknowledge ? sequence : 0,
        payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return status;

  input->sequence        = sequence;
  input->pointerSequence = sequence;
  if (acknowledge)
    input->reliableBarrierSequence = sequence;
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus networkInputTransmitAbsoluteNL(
    NetworkInput * input, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height,
    int32_t wheelX, int32_t wheelY, uint32_t buttons,
    uint32_t changedButtons, bool datagram, bool acknowledge)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return LG_TRANSPORT_UNAVAILABLE;

  LGNetInputAbsolute absolute =
  {
    .sequence       = sequence,
    .timestampNs    = nanotime(),
    .cumulativeX    = input->cumulativeX,
    .cumulativeY    = input->cumulativeY,
    .x              = (int32_t)x,
    .y              = (int32_t)y,
    .width          = width,
    .height         = height,
    .wheelX         = wheelX,
    .wheelY         = wheelY,
    .buttons        = buttons,
    .changedButtons = changedButtons,
  };
  if (!absolute.timestampNs)
    absolute.timestampNs = 1;

  uint8_t payload[LG_NET_INPUT_ABSOLUTE_WIRE_SIZE];
  if (!lgNetInputAbsoluteEncode(payload, sizeof(payload), &absolute))
    return LG_TRANSPORT_ERROR;

  const LG_TransportStatus status = datagram ?
    networkInputSendDatagramNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE, sequence,
        payload, sizeof(payload)) :
    networkInputSubmitReliableNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE,
        acknowledge ? LG_NET_ENVELOPE_ACK_REQUIRED : 0,
        acknowledge ? sequence : 0,
        payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return status;

  input->sequence        = sequence;
  input->pointerSequence = sequence;
  if (acknowledge)
    input->reliableBarrierSequence = sequence;
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus networkInputFlushMotionNL(NetworkInput * input)
{
  if (input->reliableCount)
    return LG_TRANSPORT_UNAVAILABLE;

  if (input->pendingAbsolute)
  {
    const bool               acknowledge = input->motionBarrier ||
      !input->mouseAbsolute;
    const bool               datagram    = input->datagramAvailable &&
      !input->pendingMotionReliable && !input->motionBarrier &&
      !input->reliableBarrierSequence;
    const LG_TransportStatus status      = networkInputTransmitAbsoluteNL(input,
        input->absoluteX, input->absoluteY,
        input->absoluteWidth, input->absoluteHeight,
        0, 0, input->mouseButtons, 0, datagram, acknowledge);
    if (status != LG_TRANSPORT_OK)
      return status;
    input->mouseAbsolute         = true;
    input->pendingRelative       = false;
    input->pendingAbsolute       = false;
    input->pendingMotionReliable = false;
    input->motionBarrier         = false;
    return LG_TRANSPORT_OK;
  }

  if (input->pendingRelative)
  {
    const bool               acknowledge = input->motionBarrier ||
      input->mouseAbsolute;
    const bool               datagram    = input->datagramAvailable &&
      !input->pendingMotionReliable && !input->motionBarrier &&
      !input->reliableBarrierSequence;
    const LG_TransportStatus status      = networkInputTransmitRelativeNL(input,
        0, 0, input->mouseButtons, 0, datagram, acknowledge);
    if (status != LG_TRANSPORT_OK)
      return status;
    input->mouseAbsolute         = false;
    input->pendingRelative       = false;
    input->pendingAbsolute       = false;
    input->pendingMotionReliable = false;
    input->motionBarrier         = false;
  }
  return LG_TRANSPORT_OK;
}

static LG_TransportStatus networkInputSendRelativeNL(NetworkInput * input,
    int32_t x, int32_t y, int32_t wheelX, int32_t wheelY,
    uint32_t buttons, uint32_t changedButtons, bool motion)
{
  if (motion)
  {
    const bool preserveReliable = input->pendingRelative &&
      input->pendingMotionReliable;
    if (!input->pendingRelative &&
        (input->mouseAbsolute || input->pendingAbsolute))
    {
      const LG_TransportStatus baselineStatus =
        networkInputTransmitAbsoluteNL(input,
          input->absoluteX, input->absoluteY,
          input->absoluteWidth, input->absoluteHeight,
          0, 0, buttons, 0, false, true);
      if (baselineStatus != LG_TRANSPORT_OK)
        return baselineStatus;

      input->pendingAbsolute = false;
      input->mouseAbsolute   = true;
    }
    input->cumulativeX           =
      networkInputSaturatingAdd(input->cumulativeX, x);
    input->cumulativeY           =
      networkInputSaturatingAdd(input->cumulativeY, y);
    input->pendingRelative       = true;
    input->pendingAbsolute       = false;
    input->pendingMotionReliable = preserveReliable || input->mouseAbsolute ||
      input->motionBarrier || input->reliableCount;
    return networkInputFlushMotionNL(input);
  }

  const LG_TransportStatus status = networkInputTransmitRelativeNL(input,
      wheelX, wheelY, buttons, changedButtons, false, true);
  if (status == LG_TRANSPORT_OK)
  {
    input->mouseAbsolute         = false;
    input->pendingRelative       = false;
    input->pendingAbsolute       = false;
    input->pendingMotionReliable = false;
    input->motionBarrier         = false;
  }
  return status;
}

static LG_TransportStatus networkInputSendAbsoluteNL(NetworkInput * input,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    int32_t wheelX, int32_t wheelY, uint32_t buttons,
    uint32_t changedButtons, bool motion)
{
  input->absoluteX      = x;
  input->absoluteY      = y;
  input->absoluteWidth  = width;
  input->absoluteHeight = height;

  if (motion)
  {
    const bool preserveReliable = input->pendingAbsolute &&
      input->pendingMotionReliable;
    const bool rebaseline        = input->pendingRelative;
    input->pendingRelative       = false;
    input->pendingAbsolute       = true;
    input->pendingMotionReliable = preserveReliable || rebaseline ||
      !input->mouseAbsolute || input->motionBarrier || input->reliableCount;
    return networkInputFlushMotionNL(input);
  }

  const LG_TransportStatus status = networkInputTransmitAbsoluteNL(input,
      x, y, width, height, wheelX, wheelY,
      buttons, changedButtons, false, true);
  if (status == LG_TRANSPORT_OK)
  {
    input->mouseAbsolute         = true;
    input->pendingRelative       = false;
    input->pendingAbsolute       = false;
    input->pendingMotionReliable = false;
    input->motionBarrier         = false;
  }
  return status;
}

static void networkInputResumeSendsNL(NetworkInput * input, bool * notify)
{
  do
  {
    input->sendReadyPending = false;
    if (!input->connected || !input->serviceAvailable || input->destroying)
      return;

    const LG_TransportStatus reliableStatus =
      networkInputDrainReliableNL(input);
    if (networkInputTransportFatal(reliableStatus))
    {
      *notify |= networkInputAbandonClaimNL(input, true);
      return;
    }
    if (reliableStatus != LG_TRANSPORT_OK)
      return;
    if (input->reliableCount)
    {
      if (input->sendReadyPending)
        continue;
      if (networkConnectionAwaitInputSend(input->connection,
            NETWORK_PACKET_DELIVERY_RELIABLE))
      {
        input->sendReadyPending = true;
        continue;
      }
      return;
    }

    if (!input->claimPending && !input->claimed)
    {
      networkInputSendClaimNL(input);
      continue;
    }
    if (!input->claimed ||
        (!input->pendingRelative && !input->pendingAbsolute))
      continue;

    const LG_TransportStatus status = networkInputFlushMotionNL(input);
    if (networkInputTransportFatal(status))
    {
      *notify |= networkInputAbandonClaimNL(input, true);
      return;
    }
  }
  while (input->sendReadyPending);
}

void networkInputSendReady(NetworkInput * input,
    NetworkPacketDelivery delivery)
{
  if (!input || delivery == NETWORK_PACKET_DELIVERY_BULK)
    return;
  if (l_networkInputSending == input)
  {
    input->sendReadyPending = true;
    return;
  }

  bool notify = false;
  LG_LOCK(input->stateLock);
  const bool motionReliable = input->reliableCount ||
    input->pendingMotionReliable ||
    input->motionBarrier || input->reliableBarrierSequence ||
    !input->datagramAvailable;
  const bool relevant       = delivery == (motionReliable ?
    NETWORK_PACKET_DELIVERY_RELIABLE :
    NETWORK_PACKET_DELIVERY_DATAGRAM);
  if (relevant || (!input->claimPending && !input->claimed))
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
}

void networkInputDatagramLost(NetworkInput * input,
    uint64_t claimEpoch, uint64_t pointerSequence)
{
  if (!input)
    return;

  bool notify = false;
  LG_LOCK(input->stateLock);
  if (!input->destroying && input->connected &&
      input->serviceAvailable && input->claimed &&
      input->claimEpoch == claimEpoch && pointerSequence &&
      input->pointerSequence == pointerSequence &&
      !input->motionBarrier && !input->reliableBarrierSequence)
  {
    if (!input->pendingRelative && !input->pendingAbsolute)
    {
      input->pendingRelative = !input->mouseAbsolute;
      input->pendingAbsolute = input->mouseAbsolute;
    }
    input->pendingMotionReliable = true;
    input->motionBarrier         = true;
    networkInputResumeSendsNL(input, &notify);
  }
  LG_UNLOCK(input->stateLock);

  if (notify)
    networkInputNotifyStatus(input);
}

static bool networkInputMouseMotion(void * opaque, int32_t x, int32_t y)
{
  NetworkInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool         eligible = input->available &&
    (input->claimFlags & LG_NET_INPUT_CLAIM_POINTER);
  LG_TransportStatus status   = LG_TRANSPORT_UNAVAILABLE;
  if (eligible)
    status = networkInputSendRelativeNL(input, x, y, 0, 0,
        input->mouseButtons, 0, true);
  bool               result   = eligible &&
    !networkInputTransportFatal(status);
  bool               notify   = false;
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  result &= input->available;
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return result;
}

static bool networkInputMousePosition(void * opaque, uint32_t x,
    uint32_t y, uint32_t width, uint32_t height)
{
  if (!width || !height ||
      width > LG_NET_INPUT_ABSOLUTE_MAX_WIDTH ||
      height > LG_NET_INPUT_ABSOLUTE_MAX_HEIGHT ||
      x >= width || y >= height)
    return false;

  NetworkInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool         eligible = input->available &&
    (input->claimFlags & LG_NET_INPUT_CLAIM_POINTER);
  LG_TransportStatus status   = LG_TRANSPORT_UNAVAILABLE;
  if (eligible)
    status = networkInputSendAbsoluteNL(input, x, y, width, height, 0, 0,
        input->mouseButtons, 0, true);
  bool               result   = eligible &&
    !networkInputTransportFatal(status);
  bool               notify   = false;
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  result &= input->available;
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return result;
}

static uint32_t networkInputButtonMask(unsigned int button)
{
  switch (button)
  {
    case 1:
      return LG_NET_POINTER_BUTTON_LEFT;

    case 2:
      return LG_NET_POINTER_BUTTON_MIDDLE;

    case 3:
      return LG_NET_POINTER_BUTTON_RIGHT;

    case 6:
      return LG_NET_POINTER_BUTTON_BACK;

    case 7:
      return LG_NET_POINTER_BUTTON_FORWARD;

    default:
      if (button >= 8 && button < 32)
        return UINT32_C(1) << (button - 3U);
      return 0;
  }
}

static bool networkInputUpdateButton(void * opaque,
    unsigned int button, bool down)
{
  NetworkInput * input  = opaque;
  bool           notify = false;
  LG_LOCK(input->stateLock);
  if (!input->available ||
      !(input->claimFlags & LG_NET_INPUT_CLAIM_POINTER))
  {
    LG_UNLOCK(input->stateLock);
    return false;
  }

  int32_t wheelY = 0;
  if (button == 4 || button == 5)
  {
    if (!down)
    {
      LG_UNLOCK(input->stateLock);
      return true;
    }
    wheelY = button == 4 ? 1 : -1;
  }

  const uint32_t mask = networkInputButtonMask(button);
  if (!mask && !wheelY)
  {
    LG_UNLOCK(input->stateLock);
    return false;
  }

  const uint32_t     buttons  = down ?
    input->mouseButtons | mask : input->mouseButtons & ~mask;
  LG_TransportStatus status;
  const bool         absolute = input->pendingAbsolute ||
    (input->mouseAbsolute && !input->pendingRelative);
  if (absolute && input->absoluteWidth &&
      input->absoluteHeight)
    status = networkInputSendAbsoluteNL(input,
        input->absoluteX, input->absoluteY,
        input->absoluteWidth, input->absoluteHeight,
        0, wheelY, buttons, mask, false);
  else
    status = networkInputSendRelativeNL(input, 0, 0, 0, wheelY,
        buttons, mask, false);
  if (status == LG_TRANSPORT_OK && mask)
    input->mouseButtons = buttons;
  else if (networkInputTransportFatal(status) && !down && mask)
    notify = networkInputAbandonClaimNL(input, true);
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return status == LG_TRANSPORT_OK;
}

static bool networkInputMousePress(void * opaque, unsigned int button)
{
  return networkInputUpdateButton(opaque, button, true);
}

static bool networkInputMouseRelease(void * opaque, unsigned int button)
{
  return networkInputUpdateButton(opaque, button, false);
}

static void networkInputReset(void * opaque)
{
  NetworkInput * input  = opaque;
  bool           notify = false;
  LG_LOCK(input->stateLock);
  notify = networkInputSendResetNL(input);
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
}

static const LG_InputOps NETWORK_INPUT_OPS =
{
  .name              = "Network",
  .supports          = networkInputSupports,
  .setStatusListener = networkInputSetStatusListener,
  .keyDown           = networkInputKeyDown,
  .keyUp             = networkInputKeyUp,
  .keyboardLEDs      = networkInputKeyboardLEDs,
  .keyboardLEDsReset = networkInputKeyboardLEDsReset,
  .mouseMotion       = networkInputMouseMotion,
  .mousePosition     = networkInputMousePosition,
  .mousePress        = networkInputMousePress,
  .mouseRelease      = networkInputMouseRelease,
  .reset             = networkInputReset,
};

bool networkInputCreate(NetworkInput ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkInput * input = calloc(1, sizeof(*input));
  if (!input)
    return false;

  LG_LOCK_INIT(input->stateLock);
  input->connection       = connection;
  input->claimantID       = nanotime() ^
    ((uint64_t)(uintptr_t)input << 17);
  input->statusGeneration = 1;
  if (!input->claimantID)
    input->claimantID = 1;

  input->statusIdle = lgCreateEvent(false, 0);
  if (!input->statusIdle)
  {
    LG_LOCK_FREE(input->stateLock);
    free(input);
    return false;
  }
  lgSignalEvent(input->statusIdle);

  if (!lgCreateTimer(NETWORK_INPUT_TIMER_MS,
        networkInputTimer, input, &input->timer))
  {
    lgFreeEvent(input->statusIdle);
    LG_LOCK_FREE(input->stateLock);
    free(input);
    return false;
  }

  *result = input;
  return true;
}

void networkInputDestroy(NetworkInput ** handle)
{
  if (!handle || !*handle)
    return;

  NetworkInput * input = *handle;
  LG_LOCK(input->stateLock);
  input->destroying = true;
  LG_UNLOCK(input->stateLock);
  lgTimerDestroy(input->timer);

  networkInputSetStatusListener(input, NULL, NULL);
  LG_LOCK(input->stateLock);
  networkInputSendResetNL(input);
  networkInputSendReleaseNL(input);
  networkInputClearReliableNL(input);
  input->connected         = false;
  input->serviceAvailable  = false;
  input->datagramAvailable = false;
  networkInputSetAvailableNL(input, false);
  LG_UNLOCK(input->stateLock);

  lgFreeEvent(input->statusIdle);
  LG_LOCK_FREE(input->stateLock);
  free(input);
  *handle = NULL;
}

const LG_InputOps * networkInputGetOps(void)
{
  return &NETWORK_INPUT_OPS;
}

void networkInputConnectionState(NetworkInput * input, bool connected)
{
  if (!input)
    return;

  uint16_t   serviceVersion    = 0;
  const bool serviceAvailable  = connected &&
    networkConnectionServiceAvailable(input->connection,
      LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(input->connection,
      LG_NET_SERVICE_INPUT, &serviceVersion);
  const bool datagramAvailable = serviceAvailable &&
    networkConnectionServiceAvailable(input->connection,
      LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_DATAGRAM);
  bool       notify            = false;

  LG_LOCK(input->stateLock);
  if (!connected || !serviceAvailable)
  {
    notify |= networkInputSendResetNL(input);
    networkInputSendReleaseNL(input);
    networkInputClearReliableNL(input);
    input->connected         = connected;
    input->serviceAvailable  = false;
    input->datagramAvailable = false;
    input->serviceVersion    = 0;
    networkInputInvalidateLEDsNL(input);
    notify |=
      networkInputSetAvailableNL(input, false);
  }
  else
  {
    input->connected         = true;
    input->serviceAvailable  = true;
    input->datagramAvailable = datagramAvailable;
    input->serviceVersion    = serviceVersion;
    input->claimPending      = false;
    input->claimed           = false;
    input->claimFlags        = 0;
    input->sequence          = 0;
    input->nextClaimNs       = 0;
    networkInputClearReliableNL(input);
    networkInputClearStateNL(input);
    networkInputInvalidateLEDsNL(input);
    notify |= networkInputSetAvailableNL(input, false);
    networkInputSendClaimNL(input);
  }
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);

  if (notify)
    networkInputNotifyStatus(input);
}

static bool networkInputHandleStatus(NetworkInput * input,
    const LGNetPacketView * packet)
{
  LGNetInputStatus status;
  if (lgNetInputStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;
  if (packet->envelope.componentEpoch != status.claimEpoch)
    return false;
  const bool barrierAck =
    packet->envelope.messageType == LG_NET_INPUT_MESSAGE_STATUS &&
    (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) &&
    packet->envelope.requestID &&
    packet->envelope.requestID <= status.lastSequence;

  bool notify = false;
  LG_LOCK(input->stateLock);
  if (!input->connected || status.claimEpoch != input->claimEpoch)
  {
    LG_UNLOCK(input->stateLock);
    return true;
  }

  switch (status.status)
  {
    case LG_NET_INPUT_STATUS_ACCEPTED:
      input->claimPending    = false;
      input->claimed         =
        (status.flags & NETWORK_INPUT_CLAIM_FLAGS) ==
          NETWORK_INPUT_CLAIM_FLAGS;
      input->claimFlags      = input->claimed ? status.flags : 0;
      input->nextKeepaliveNs = networkInputDeadline(
          status.leaseRemainingMs > NETWORK_INPUT_KEEPALIVE_MS ?
            NETWORK_INPUT_KEEPALIVE_MS :
            status.leaseRemainingMs > 1U ?
              status.leaseRemainingMs / 2U : 1U);
      if (!input->claimed)
      {
        input->nextClaimNs = networkInputDeadline(
            NETWORK_INPUT_CLAIM_RETRY_MS);
        networkInputClearReliableNL(input);
        networkInputClearStateNL(input);
        networkInputInvalidateLEDsNL(input);
      }
      notify = networkInputSetAvailableNL(input, input->claimed);
      break;

    case LG_NET_INPUT_STATUS_UNSUPPORTED:
      input->claimPending      = false;
      input->claimed           = false;
      input->claimFlags        = 0;
      input->serviceAvailable  = false;
      input->datagramAvailable = false;
      networkInputClearReliableNL(input);
      networkInputClearStateNL(input);
      networkInputInvalidateLEDsNL(input);
      notify = networkInputSetAvailableNL(input, false);
      break;

    case LG_NET_INPUT_STATUS_REJECTED:
    case LG_NET_INPUT_STATUS_RELEASED:
    case LG_NET_INPUT_STATUS_EXPIRED:
    case LG_NET_INPUT_STATUS_ERROR:
      input->claimPending = false;
      input->claimed      = false;
      input->claimFlags   = 0;
      networkInputClearReliableNL(input);
      networkInputClearStateNL(input);
      networkInputInvalidateLEDsNL(input);
      input->nextClaimNs  = networkInputDeadline(
          NETWORK_INPUT_CLAIM_RETRY_MS);
      notify = networkInputSetAvailableNL(input, false);
      break;
  }
  if (status.status == LG_NET_INPUT_STATUS_ACCEPTED && barrierAck &&
      input->reliableBarrierSequence &&
      packet->envelope.requestID >= input->reliableBarrierSequence)
  {
    input->reliableBarrierSequence = 0;
    networkInputResumeSendsNL(input, &notify);
  }
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  LG_UNLOCK(input->stateLock);

  if (notify)
    networkInputNotifyStatus(input);
  return true;
}

static bool networkInputHandleLEDs(NetworkInput * input,
    const LGNetPacketView * packet)
{
  LGNetInputLEDs leds;
  if (lgNetInputLEDsDecode(&leds,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  bool notify = false;
  LG_LOCK(input->stateLock);
  if (!input->connected || !input->claimed ||
      packet->envelope.componentEpoch != input->claimEpoch ||
      leds.sequence <= input->keyboardLEDSequence)
  {
    LG_UNLOCK(input->stateLock);
    return true;
  }

  const bool     oldValid      = input->keyboardLEDsValid;
  const uint8_t  oldState      = input->keyboardLEDs;
  const uint32_t oldValidMask  = input->keyboardLEDsValidMask;
  const uint32_t mergedState   =
    (input->keyboardLEDs & ~leds.validMask) | leds.state;
  const uint32_t changed       =
    (input->keyboardLEDs ^ mergedState) &
      leds.validMask & oldValidMask;

  input->keyboardLEDSequence   = leds.sequence;
  input->keyboardLEDsValidMask |= leds.validMask;
  input->keyboardLEDs          = (uint8_t)mergedState;
  input->keyboardLEDsValid     =
    (input->keyboardLEDsValidMask & NETWORK_INPUT_LED_MASK) ==
      NETWORK_INPUT_LED_MASK;
  networkInputAcknowledgeLEDsNL(input, changed);
  networkInputSyncKeyboardLEDsNL(input, &notify);
  if (input->sendReadyPending)
    networkInputResumeSendsNL(input, &notify);
  notify |= oldValid != input->keyboardLEDsValid ||
    oldState != input->keyboardLEDs;
  if (notify)
    input->statusNotifyPending = true;
  LG_UNLOCK(input->stateLock);

  if (notify)
    networkInputNotifyStatus(input);
  return true;
}

bool networkInputPacket(NetworkInput * input,
    const LGNetPacketView * packet, bool datagram)
{
  if (!input || !packet || datagram ||
      packet->envelope.service != LG_NET_SERVICE_INPUT)
    return false;

  const LGNetEnvelopeFlags baseFlags =
    LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_FINAL;
  switch (packet->envelope.messageType)
  {
    case LG_NET_INPUT_MESSAGE_CLAIM_RESULT:
      if (packet->envelope.flags !=
            (baseFlags | LG_NET_ENVELOPE_RESPONSE) ||
          packet->envelope.requestID != packet->envelope.componentEpoch)
        return false;
      return networkInputHandleStatus(input, packet);

    case LG_NET_INPUT_MESSAGE_STATUS:
    {
      const LGNetEnvelopeFlags expectedFlags = packet->envelope.requestID ?
        baseFlags | LG_NET_ENVELOPE_RESPONSE : baseFlags;
      if (packet->envelope.flags != expectedFlags)
        return false;
      return networkInputHandleStatus(input, packet);
    }

    case LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS:
      if (packet->envelope.flags != baseFlags ||
          packet->envelope.requestID)
        return false;
      return networkInputHandleLEDs(input, packet);

    default:
      return false;
  }
}
