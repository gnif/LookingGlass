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
  bool     claimPending;
  bool     claimed;
  bool     available;
  uint64_t claimantID;
  uint64_t claimEpoch;
  uint64_t sequence;
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
  LG_InputStatusFn statusCallback;
  void           * statusOpaque;
};

static _Thread_local NetworkInput * l_networkInputCallback;

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
  input->mouseButtons   = 0;
  input->mouseAbsolute  = false;
  input->absoluteX      = 0;
  input->absoluteY      = 0;
  input->absoluteWidth  = 0;
  input->absoluteHeight = 0;
  memset(input->keyState, 0, sizeof(input->keyState));
  input->modifiers      = 0;
  networkInputClearLEDProjectionNL(input);
}

static LG_TransportStatus networkInputSendReliableNL(NetworkInput * input,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  return networkConnectionSend(input->connection, LG_NET_SERVICE_INPUT,
      messageType, input->serviceVersion,
      flags | LG_NET_ENVELOPE_FINAL, input->claimEpoch, requestID,
      payload, payloadLength);
}

static bool networkInputSendClaimNL(NetworkInput * input)
{
  if (!input->connected || !input->serviceAvailable || input->destroying)
    return false;

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
  if (networkInputSendReliableNL(input, LG_NET_INPUT_MESSAGE_CLAIM,
        LG_NET_ENVELOPE_ACK_REQUIRED, claimEpoch,
        payload, sizeof(payload)) != LG_TRANSPORT_OK)
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

static bool networkInputSendControlNL(NetworkInput * input,
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
  if (!lgNetInputControlEncode(payload, sizeof(payload), &control) ||
      networkInputSendReliableNL(input, messageType,
        0, 0, payload, sizeof(payload)) != LG_TRANSPORT_OK)
    return false;

  input->sequence = sequence;
  return true;
}

static bool networkInputAbandonClaimNL(NetworkInput * input,
    bool tryReset)
{
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
  networkInputClearStateNL(input);
  networkInputInvalidateLEDsNL(input);
  return networkInputSetAvailableNL(input, false);
}

static bool networkInputSendResetNL(NetworkInput * input)
{
  const bool failed = input->claimed &&
    (!networkConnectionValid(input->connection) ||
      !networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RESET,
        LG_NET_INPUT_CLAIM_KEYBOARD | LG_NET_INPUT_CLAIM_POINTER, 0));
  networkInputClearStateNL(input);
  return failed && networkInputAbandonClaimNL(input, false);
}

static bool networkInputSendReleaseNL(NetworkInput * input)
{
  bool sent = true;
  if ((input->claimed || input->claimPending) &&
      networkConnectionValid(input->connection))
    sent = networkInputSendControlNL(input, LG_NET_INPUT_MESSAGE_RELEASE,
        input->claimFlags, 0);

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

static bool networkInputSendKeyboardNL(NetworkInput * input,
    uint16_t usage, bool down, bool repeat)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return false;

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
  if (!lgNetInputKeyboardEncode(payload, sizeof(payload), &keyboard) ||
      networkInputSendReliableNL(input, LG_NET_INPUT_MESSAGE_KEYBOARD,
        0, 0, payload, sizeof(payload)) != LG_TRANSPORT_OK)
    return false;

  input->sequence = sequence;
  return true;
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

  if (!networkInputSendKeyboardNL(input, key->usage, true, false))
    return false;
  networkInputRecordLEDToggleNL(input, key->led, false);
  if (networkInputSendKeyboardNL(input, key->usage, false, false))
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
    if (input->claimPending && now >= input->nextClaimNs)
      input->claimPending = false;
    if (!input->claimPending && !input->claimed &&
        now >= input->nextClaimNs)
      networkInputSendClaimNL(input);
    else if (input->claimed && now >= input->nextKeepaliveNs)
    {
      if (networkInputSendControlNL(input,
            LG_NET_INPUT_MESSAGE_KEEPALIVE,
            input->claimFlags, NETWORK_INPUT_LEASE_MS))
        input->nextKeepaliveNs = networkInputDeadline(
            NETWORK_INPUT_KEEPALIVE_MS);
      else
        notify |= networkInputAbandonClaimNL(input, true);
    }

    networkInputSyncKeyboardLEDsNL(input, &notify);
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

  if (!networkInputSendKeyboardNL(input, usage, down, false))
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
    if (!down)
      notify = networkInputAbandonClaimNL(input, true);
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
  LG_UNLOCK(input->stateLock);
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

static bool networkInputSendRelativeNL(NetworkInput * input,
    int32_t x, int32_t y, int32_t wheelX, int32_t wheelY,
    uint32_t buttons, uint32_t changedButtons, bool motion)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return false;

  LGNetInputRelative relative =
  {
    .sequence       = sequence,
    .timestampNs    = nanotime(),
    .deltaX         = x,
    .deltaY         = y,
    .wheelX         = wheelX,
    .wheelY         = wheelY,
    .buttons        = buttons,
    .changedButtons = changedButtons,
  };
  if (!relative.timestampNs)
    relative.timestampNs = 1;

  uint8_t payload[LG_NET_INPUT_RELATIVE_WIRE_SIZE];
  if (!lgNetInputRelativeEncode(payload, sizeof(payload), &relative))
    return false;

  LG_TransportStatus status = LG_TRANSPORT_UNAVAILABLE;
  if (motion && networkConnectionServiceAvailable(input->connection,
        LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_DATAGRAM))
    status = networkConnectionSendDatagram(input->connection,
        LG_NET_SERVICE_INPUT, LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE,
        input->serviceVersion, 0, input->claimEpoch, 0,
        payload, sizeof(payload), true);
  if (status != LG_TRANSPORT_OK)
    status = networkInputSendReliableNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_RELATIVE, 0, 0,
        payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return false;

  input->sequence      = sequence;
  input->mouseAbsolute = false;
  return true;
}

static bool networkInputSendAbsoluteNL(NetworkInput * input,
    uint32_t x, uint32_t y, uint32_t width, uint32_t height,
    int32_t wheelX, int32_t wheelY, uint32_t buttons,
    uint32_t changedButtons, bool motion)
{
  uint64_t sequence;
  if (!networkInputNextSequenceNL(input, &sequence))
    return false;

  LGNetInputAbsolute absolute =
  {
    .sequence       = sequence,
    .timestampNs    = nanotime(),
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
    return false;

  LG_TransportStatus status = LG_TRANSPORT_UNAVAILABLE;
  if (motion && networkConnectionServiceAvailable(input->connection,
        LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_DATAGRAM))
    status = networkConnectionSendDatagram(input->connection,
        LG_NET_SERVICE_INPUT, LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE,
        input->serviceVersion, 0, input->claimEpoch, 0,
        payload, sizeof(payload), true);
  if (status != LG_TRANSPORT_OK)
    status = networkInputSendReliableNL(input,
        LG_NET_INPUT_MESSAGE_MOUSE_ABSOLUTE, 0, 0,
        payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return false;

  input->sequence       = sequence;
  input->mouseAbsolute  = true;
  input->absoluteX      = x;
  input->absoluteY      = y;
  input->absoluteWidth  = width;
  input->absoluteHeight = height;
  return true;
}

static bool networkInputMouseMotion(void * opaque, int32_t x, int32_t y)
{
  NetworkInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool result = input->available &&
    (input->claimFlags & LG_NET_INPUT_CLAIM_POINTER) &&
    networkInputSendRelativeNL(input, x, y, 0, 0,
        input->mouseButtons, 0, true);
  LG_UNLOCK(input->stateLock);
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
  const bool result = input->available &&
    (input->claimFlags & LG_NET_INPUT_CLAIM_POINTER) &&
    networkInputSendAbsoluteNL(input, x, y, width, height, 0, 0,
        input->mouseButtons, 0, true);
  LG_UNLOCK(input->stateLock);
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

  const uint32_t buttons = down ?
    input->mouseButtons | mask : input->mouseButtons & ~mask;
  bool result;
  if (input->mouseAbsolute && input->absoluteWidth &&
      input->absoluteHeight)
    result = networkInputSendAbsoluteNL(input,
        input->absoluteX, input->absoluteY,
        input->absoluteWidth, input->absoluteHeight,
        0, wheelY, buttons, mask, false);
  else
    result = networkInputSendRelativeNL(input, 0, 0, 0, wheelY,
        buttons, mask, false);
  if (result && mask)
    input->mouseButtons = buttons;
  else if (!result && !down && mask)
    notify = networkInputAbandonClaimNL(input, true);
  LG_UNLOCK(input->stateLock);
  if (notify)
    networkInputNotifyStatus(input);
  return result;
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
  input->connected        = false;
  input->serviceAvailable = false;
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

  uint16_t   serviceVersion   = 0;
  const bool serviceAvailable = connected &&
    networkConnectionServiceAvailable(input->connection,
      LG_NET_SERVICE_INPUT, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(input->connection,
      LG_NET_SERVICE_INPUT, &serviceVersion);
  bool       notify           = false;

  LG_LOCK(input->stateLock);
  if (!connected || !serviceAvailable)
  {
    notify |= networkInputSendResetNL(input);
    networkInputSendReleaseNL(input);
    input->connected        = connected;
    input->serviceAvailable = false;
    input->serviceVersion   = 0;
    networkInputInvalidateLEDsNL(input);
    notify |=
      networkInputSetAvailableNL(input, false);
  }
  else
  {
    input->connected        = true;
    input->serviceAvailable = true;
    input->serviceVersion   = serviceVersion;
    input->claimPending     = false;
    input->claimed          = false;
    input->claimFlags       = 0;
    input->sequence         = 0;
    input->nextClaimNs      = 0;
    networkInputClearStateNL(input);
    networkInputInvalidateLEDsNL(input);
    notify |= networkInputSetAvailableNL(input, false);
    networkInputSendClaimNL(input);
  }
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
        networkInputClearStateNL(input);
        networkInputInvalidateLEDsNL(input);
      }
      notify = networkInputSetAvailableNL(input, input->claimed);
      break;

    case LG_NET_INPUT_STATUS_UNSUPPORTED:
      input->claimPending     = false;
      input->claimed          = false;
      input->claimFlags       = 0;
      input->serviceAvailable = false;
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
      networkInputClearStateNL(input);
      networkInputInvalidateLEDsNL(input);
      input->nextClaimNs  = networkInputDeadline(
          NETWORK_INPUT_CLAIM_RETRY_MS);
      notify = networkInputSetAvailableNL(input, false);
      break;
  }
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

  switch (packet->envelope.messageType)
  {
    case LG_NET_INPUT_MESSAGE_CLAIM_RESULT:
      if (!(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) ||
          packet->envelope.requestID != packet->envelope.componentEpoch)
        return false;
      return networkInputHandleStatus(input, packet);

    case LG_NET_INPUT_MESSAGE_STATUS:
      return networkInputHandleStatus(input, packet);

    case LG_NET_INPUT_MESSAGE_KEYBOARD_LEDS:
      return networkInputHandleLEDs(input, packet);

    default:
      return false;
  }
}
