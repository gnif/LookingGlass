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

#include "common/debug.h"
#include "common/locking.h"

#include <linux/input.h>
#include <stdatomic.h>

struct InputBinding
{
  const LG_InputOps * ops;
  void              * opaque;
  bool                available;
  bool                mouseAbsolute;
  bool                keyboardLEDsValid;
  uint8_t             keyboardLEDs;
  uint32_t            epoch;
  uint32_t            generation;
};

static struct
{
  LG_Lock   bindingLock;
  LG_RWLock activeLock;
  LG_Lock   keyboardLEDsDispatch;
  LG_Lock   availabilityDispatch;
  uint32_t  nextBindingEpoch;

  LGInputKeyboardLEDsFn keyboardLEDsCallback;
  void                  * keyboardLEDsOpaque;
  LGInputAvailabilityFn availabilityCallback;
  void                  * availabilityOpaque;

  struct InputBinding fallback;
  struct InputBinding transport;
  struct InputBinding active;
  bool                useTransport;
  bool                notifiedAvailable;
  bool                keyboardLEDsSyncEnabled;
  bool                desiredKeyboardLEDsValid;
  uint8_t             desiredKeyboardLEDs;

  _Atomic(bool)     keys[KEY_MAX];
  _Atomic(uint32_t) buttons;
}
l_input;

static bool validOps(const LG_InputOps * ops)
{
  return
    ops               &&
    ops->name         &&
    ops->supports     &&
    ops->keyDown      &&
    ops->keyUp        &&
    ops->mouseMotion  &&
    ops->mousePress   &&
    ops->mouseRelease;
}

static struct InputBinding makeBinding(const LG_InputOps * ops,
    void * opaque)
{
  const bool available = ops && !ops->setStatusListener;
  uint32_t epoch = 0;
  if (ops)
  {
    epoch = ++l_input.nextBindingEpoch;
    if (!epoch)
      epoch = ++l_input.nextBindingEpoch;
  }

  return (struct InputBinding)
  {
    .ops           = ops,
    .opaque        = opaque,
    .available     = available,
    .mouseAbsolute = available && ops->mousePosition &&
      ops->supports(opaque, LG_INPUT_SUPPORT_MOUSE_ABSOLUTE),
    .epoch         = epoch,
  };
}

static bool bindingEqual(const struct InputBinding * a,
    const struct InputBinding * b)
{
  return a->ops        == b->ops &&
    a->opaque          == b->opaque &&
    a->epoch           == b->epoch;
}

static bool keyboardLEDsEqual(const struct InputBinding * a,
    const struct InputBinding * b)
{
  const bool aValid = a->ops && a->available && a->keyboardLEDsValid;
  const bool bValid = b->ops && b->available && b->keyboardLEDsValid;
  return aValid == bValid && (!aValid || a->keyboardLEDs == b->keyboardLEDs);
}

static void dispatchKeyboardLEDs(void)
{
  LG_LOCK(l_input.keyboardLEDsDispatch);
  LG_LOCK_SHARED(l_input.activeLock);
  LGInputKeyboardLEDsFn callback = l_input.keyboardLEDsCallback;
  void                * opaque   = l_input.keyboardLEDsOpaque;
  const bool            valid    = l_input.active.ops &&
    l_input.active.available &&
    l_input.active.keyboardLEDsValid;
  const uint8_t           leds   = l_input.active.keyboardLEDs;
  LG_UNLOCK_SHARED(l_input.activeLock);

  if (callback)
    callback(opaque, valid, leds);
  LG_UNLOCK(l_input.keyboardLEDsDispatch);
}

static bool updateAvailabilityNL(void)
{
  const bool available = l_input.active.ops != NULL;
  if (l_input.notifiedAvailable == available)
    return false;

  l_input.notifiedAvailable = available;
  return true;
}

static void dispatchAvailability(void)
{
  LG_LOCK(l_input.availabilityDispatch);
  LG_LOCK_SHARED(l_input.activeLock);
  LGInputAvailabilityFn callback  = l_input.availabilityCallback;
  void                * opaque    = l_input.availabilityOpaque;
  const bool            available = l_input.active.ops != NULL;
  LG_UNLOCK_SHARED(l_input.activeLock);

  if (callback)
    callback(opaque, available);
  LG_UNLOCK(l_input.availabilityDispatch);
}

static void releaseKeysNL(void)
{
  for (int key = 0; key < KEY_MAX; ++key)
    if (atomic_exchange_explicit(&l_input.keys[key], false,
          memory_order_relaxed) && l_input.active.ops)
      l_input.active.ops->keyUp(l_input.active.opaque, key);
}

static void releaseButtonsNL(void)
{
  const uint32_t buttons = atomic_exchange_explicit(
      &l_input.buttons, 0, memory_order_relaxed);

  if (!l_input.active.ops)
    return;

  for (unsigned int button = 1; button < 32; ++button)
    if (buttons & (UINT32_C(1) << button))
      l_input.active.ops->mouseRelease(l_input.active.opaque, button);
}

static void resetActiveNL(void)
{
  releaseKeysNL();
  releaseButtonsNL();
  if (l_input.active.ops && l_input.active.ops->reset)
    l_input.active.ops->reset(l_input.active.opaque);
}

static void clearStateNL(void)
{
  for (int key = 0; key < KEY_MAX; ++key)
    atomic_store_explicit(&l_input.keys[key], false, memory_order_relaxed);

  atomic_store_explicit(&l_input.buttons, 0, memory_order_relaxed);
}

static void resetKeyboardLEDsNL(const struct InputBinding * binding)
{
  if (binding->ops && binding->ops->keyboardLEDsReset)
    binding->ops->keyboardLEDsReset(binding->opaque);
}

static bool syncKeyboardLEDsNL(void)
{
  if (!l_input.keyboardLEDsSyncEnabled ||
      !l_input.desiredKeyboardLEDsValid)
    return true;
  if (!l_input.active.ops)
    return false;
  if (!l_input.active.ops->keyboardLEDs)
    return true;

  const uint8_t leds = l_input.desiredKeyboardLEDs;
  return l_input.active.ops->keyboardLEDs(l_input.active.opaque,
      (leds & LG_KEYBOARD_LED_NUM_LOCK) != 0,
      (leds & LG_KEYBOARD_LED_CAPS_LOCK) != 0,
      (leds & LG_KEYBOARD_LED_SCROLL_LOCK) != 0);
}

static bool updateActiveNL(bool dropActive)
{
  const struct InputBinding next =
    l_input.useTransport && l_input.transport.available ?
      l_input.transport :
    l_input.fallback.available ? l_input.fallback :
      (struct InputBinding) { 0 };

  if (bindingEqual(&next, &l_input.active))
  {
    const bool keyboardLEDsChanged =
      !keyboardLEDsEqual(&next, &l_input.active);
    l_input.active = next;
    return keyboardLEDsChanged;
  }

  const bool keyboardLEDsChanged =
    !keyboardLEDsEqual(&next, &l_input.active);

  resetKeyboardLEDsNL(&l_input.active);
  if (dropActive)
    clearStateNL();
  else
    resetActiveNL();
  l_input.active = next;

  if (l_input.active.ops)
  {
    if (l_input.active.ops->reset)
      l_input.active.ops->reset(l_input.active.opaque);
    syncKeyboardLEDsNL();
    DEBUG_INFO("Using Input: %s", l_input.active.ops->name);
  }
  else
    DEBUG_INFO("Input is unavailable");
  return keyboardLEDsChanged;
}

static bool updateStatusNL(struct InputBinding * binding,
    const LG_InputStatus * status)
{
  const struct InputBinding oldActive         = l_input.active;
  const bool                wasActive         =
    bindingEqual(&l_input.active, binding);
  const bool                generationChanged = wasActive &&
    binding->generation && status->available &&
    binding->generation != status->generation;

  if (wasActive && !status->available)
  {
    resetKeyboardLEDsNL(&l_input.active);
    resetActiveNL();
    l_input.active = (struct InputBinding) { 0 };
  }

  binding->available         = status->available;
  binding->mouseAbsolute     = status->available &&
    binding->ops->mousePosition &&
    binding->ops->supports(binding->opaque,
      LG_INPUT_SUPPORT_MOUSE_ABSOLUTE);
  binding->keyboardLEDsValid = status->available &&
    status->keyboardLEDsValid;
  binding->keyboardLEDs      = status->keyboardLEDs;
  binding->generation        = status->generation;
  updateActiveNL(false);
  if (generationChanged)
    syncKeyboardLEDsNL();
  return !keyboardLEDsEqual(&oldActive, &l_input.active);
}

static void fallbackStatusChanged(void * opaque,
    const LG_InputStatus * status)
{
  if (!status)
    return;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  bool keyboardLEDsChanged = false;
  if (l_input.fallback.ops &&
      l_input.fallback.epoch == (uint32_t)(uintptr_t)opaque)
    keyboardLEDsChanged = updateStatusNL(&l_input.fallback, status);
  const bool availabilityChanged = updateAvailabilityNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  if (keyboardLEDsChanged)
    dispatchKeyboardLEDs();
  if (availabilityChanged)
    dispatchAvailability();
}

static void transportStatusChanged(void * opaque,
    const LG_InputStatus * status)
{
  if (!status)
    return;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  bool keyboardLEDsChanged = false;
  if (l_input.transport.ops &&
      l_input.transport.epoch == (uint32_t)(uintptr_t)opaque)
    keyboardLEDsChanged = updateStatusNL(&l_input.transport, status);
  const bool availabilityChanged = updateAvailabilityNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  if (keyboardLEDsChanged)
    dispatchKeyboardLEDs();
  if (availabilityChanged)
    dispatchAvailability();
}

void lgInput_init(void)
{
  l_input.fallback                  = (struct InputBinding) { 0 };
  l_input.transport                 = (struct InputBinding) { 0 };
  l_input.active                    = (struct InputBinding) { 0 };
  l_input.useTransport              = true;
  l_input.notifiedAvailable         = false;
  l_input.keyboardLEDsSyncEnabled   = false;
  l_input.desiredKeyboardLEDsValid  = false;
  l_input.desiredKeyboardLEDs       = 0;

  for (int key = 0; key < KEY_MAX; ++key)
    atomic_init(&l_input.keys[key], false);
  atomic_init(&l_input.buttons, 0);

  LG_LOCK_INIT(l_input.bindingLock);
  LG_RWLOCK_INIT(l_input.activeLock);
  LG_LOCK_INIT(l_input.keyboardLEDsDispatch);
  LG_LOCK_INIT(l_input.availabilityDispatch);
}

void lgInput_free(void)
{
  struct InputBinding fallback;
  struct InputBinding transport;

  LG_LOCK(l_input.bindingLock);
  LG_LOCK(l_input.keyboardLEDsDispatch);
  LG_LOCK(l_input.availabilityDispatch);
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  resetKeyboardLEDsNL(&l_input.active);
  resetActiveNL();
  fallback                       = l_input.fallback;
  transport                      = l_input.transport;
  l_input.active                 = (struct InputBinding) { 0 };
  l_input.fallback               = (struct InputBinding) { 0 };
  l_input.transport              = (struct InputBinding) { 0 };
  l_input.keyboardLEDsCallback   = NULL;
  l_input.keyboardLEDsOpaque     = NULL;
  l_input.availabilityCallback   = NULL;
  l_input.availabilityOpaque     = NULL;
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  LG_UNLOCK(l_input.availabilityDispatch);
  LG_UNLOCK(l_input.keyboardLEDsDispatch);

  if (fallback.ops && fallback.ops->setStatusListener)
    fallback.ops->setStatusListener(fallback.opaque, NULL, NULL);
  if (transport.ops && transport.ops->setStatusListener)
    transport.ops->setStatusListener(transport.opaque, NULL, NULL);

  LG_UNLOCK(l_input.bindingLock);
  LG_RWLOCK_FREE(l_input.activeLock);
  LG_LOCK_FREE(l_input.availabilityDispatch);
  LG_LOCK_FREE(l_input.keyboardLEDsDispatch);
  LG_LOCK_FREE(l_input.bindingLock);
}

static void setBinding(struct InputBinding * target,
    const LG_InputOps * ops, void * opaque, LG_InputStatusFn statusFn)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid input operations");
    ops    = NULL;
    opaque = NULL;
  }

  struct InputBinding old;

  LG_LOCK(l_input.bindingLock);
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  old = *target;
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);

  const struct InputBinding next = makeBinding(ops, opaque);
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  *target = next;
  const bool keyboardLEDsChanged = updateActiveNL(false);
  const bool availabilityChanged = updateAvailabilityNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (keyboardLEDsChanged)
    dispatchKeyboardLEDs();
  if (availabilityChanged)
    dispatchAvailability();

  if (next.ops && next.ops->setStatusListener)
    next.ops->setStatusListener(next.opaque, statusFn,
        (void *)(uintptr_t)next.epoch);
  LG_UNLOCK(l_input.bindingLock);
}

static void dropBinding(struct InputBinding * target)
{
  LG_LOCK(l_input.bindingLock);
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  const bool wasActive = bindingEqual(&l_input.active, target);
  *target = (struct InputBinding) { 0 };
  const bool keyboardLEDsChanged = updateActiveNL(wasActive);
  const bool availabilityChanged = updateAvailabilityNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  if (keyboardLEDsChanged)
    dispatchKeyboardLEDs();
  if (availabilityChanged)
    dispatchAvailability();
  LG_UNLOCK(l_input.bindingLock);
}

void lgInput_setFallback(const LG_InputOps * ops, void * opaque)
{
  setBinding(&l_input.fallback, ops, opaque, fallbackStatusChanged);
}

void lgInput_dropFallback(void)
{
  dropBinding(&l_input.fallback);
}

void lgInput_setTransport(const LG_InputOps * ops, void * opaque)
{
  setBinding(&l_input.transport, ops, opaque, transportStatusChanged);
}

void lgInput_dropTransport(void)
{
  dropBinding(&l_input.transport);
}

void lgInput_useTransport(bool enable)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  l_input.useTransport = enable;
  const bool keyboardLEDsChanged = updateActiveNL(false);
  const bool availabilityChanged = updateAvailabilityNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  if (keyboardLEDsChanged)
    dispatchKeyboardLEDs();
  if (availabilityChanged)
    dispatchAvailability();
}

bool lgInput_available(void)
{
  LG_LOCK_SHARED(l_input.activeLock);
  const bool result = l_input.active.ops != NULL;
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_supports(LG_InputSupport support)
{
  LG_LOCK_SHARED(l_input.activeLock);
  bool result;
  switch (support)
  {
    case LG_INPUT_SUPPORT_MOUSE_ABSOLUTE:
      result = l_input.active.mouseAbsolute;
      break;

    default:
      result = false;
      break;
  }
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

void lgInput_setKeyboardLEDsListener(
    LGInputKeyboardLEDsFn callback, void * opaque)
{
  LG_LOCK(l_input.keyboardLEDsDispatch);
  LG_LOCK_SHARED(l_input.activeLock);
  l_input.keyboardLEDsCallback = callback;
  l_input.keyboardLEDsOpaque   = opaque;
  const bool valid = l_input.active.ops &&
    l_input.active.available &&
    l_input.active.keyboardLEDsValid;
  const uint8_t leds = l_input.active.keyboardLEDs;
  LG_UNLOCK_SHARED(l_input.activeLock);

  if (callback)
    callback(opaque, valid, leds);
  LG_UNLOCK(l_input.keyboardLEDsDispatch);
}

void lgInput_setAvailabilityListener(
    LGInputAvailabilityFn callback, void * opaque)
{
  LG_LOCK(l_input.availabilityDispatch);
  LG_LOCK_SHARED(l_input.activeLock);
  l_input.availabilityCallback = callback;
  l_input.availabilityOpaque   = opaque;
  const bool available = l_input.active.ops != NULL;
  LG_UNLOCK_SHARED(l_input.activeLock);

  if (callback)
    callback(opaque, available);
  LG_UNLOCK(l_input.availabilityDispatch);
}

bool lgInput_keyDown(int key)
{
  if (key < 0 || key >= KEY_MAX)
    return false;

  LG_LOCK_SHARED(l_input.activeLock);
  if (atomic_exchange_explicit(
        &l_input.keys[key], true, memory_order_relaxed))
  {
    LG_UNLOCK_SHARED(l_input.activeLock);
    return true;
  }

  const bool result = l_input.active.ops &&
    l_input.active.ops->keyDown(l_input.active.opaque, key);
  if (!result)
    atomic_store_explicit(&l_input.keys[key], false, memory_order_relaxed);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_keyUp(int key)
{
  if (key < 0 || key >= KEY_MAX)
    return false;

  LG_LOCK_SHARED(l_input.activeLock);
  if (!atomic_exchange_explicit(
        &l_input.keys[key], false, memory_order_relaxed))
  {
    LG_UNLOCK_SHARED(l_input.activeLock);
    return true;
  }

  const bool result = l_input.active.ops &&
    l_input.active.ops->keyUp(l_input.active.opaque, key);
  if (!result)
    atomic_store_explicit(&l_input.keys[key], true, memory_order_relaxed);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_keyboardLEDs(bool numLock, bool capsLock, bool scrollLock)
{
  const uint8_t desired =
    (numLock    ? LG_KEYBOARD_LED_NUM_LOCK    : 0) |
    (capsLock   ? LG_KEYBOARD_LED_CAPS_LOCK   : 0) |
    (scrollLock ? LG_KEYBOARD_LED_SCROLL_LOCK : 0);

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  l_input.desiredKeyboardLEDs      = desired;
  l_input.desiredKeyboardLEDsValid = true;
  const bool result = syncKeyboardLEDsNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
  return result;
}

void lgInput_setKeyboardLEDsSync(bool enable)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  if (l_input.keyboardLEDsSyncEnabled == enable)
  {
    LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
    return;
  }

  l_input.keyboardLEDsSyncEnabled = enable;
  if (enable)
    syncKeyboardLEDsNL();
  else
  {
    resetKeyboardLEDsNL(&l_input.fallback);
    if (l_input.transport.ops != l_input.fallback.ops ||
        l_input.transport.opaque != l_input.fallback.opaque)
      resetKeyboardLEDsNL(&l_input.transport);
  }
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

void lgInput_releaseKeys(void)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  releaseKeysNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

void lgInput_releaseButtons(void)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  releaseButtonsNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

void lgInput_releaseAll(void)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  releaseKeysNL();
  releaseButtonsNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

bool lgInput_mouseMotion(int32_t x, int32_t y)
{
  LG_LOCK_SHARED(l_input.activeLock);
  const bool result = l_input.active.ops &&
    l_input.active.ops->mouseMotion(l_input.active.opaque, x, y);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_mousePosition(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height)
{
  LG_LOCK_SHARED(l_input.activeLock);
  const bool result = l_input.active.mouseAbsolute &&
    l_input.active.ops->mousePosition(l_input.active.opaque,
      x, y, width, height);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_mousePress(unsigned int button)
{
  if (button == 0 || button >= 32)
    return false;

  const uint32_t mask = UINT32_C(1) << button;
  LG_LOCK_SHARED(l_input.activeLock);
  if (atomic_fetch_or_explicit(
        &l_input.buttons, mask, memory_order_relaxed) & mask)
  {
    LG_UNLOCK_SHARED(l_input.activeLock);
    return true;
  }

  const bool result = l_input.active.ops &&
    l_input.active.ops->mousePress(l_input.active.opaque, button);
  if (!result)
    atomic_fetch_and_explicit(
        &l_input.buttons, ~mask, memory_order_relaxed);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

bool lgInput_mouseRelease(unsigned int button)
{
  if (button == 0 || button >= 32)
    return false;

  const uint32_t mask = UINT32_C(1) << button;
  LG_LOCK_SHARED(l_input.activeLock);
  if (!(atomic_fetch_and_explicit(
          &l_input.buttons, ~mask, memory_order_relaxed) & mask))
  {
    LG_UNLOCK_SHARED(l_input.activeLock);
    return true;
  }

  const bool result = l_input.active.ops &&
    l_input.active.ops->mouseRelease(l_input.active.opaque, button);
  if (!result)
    atomic_fetch_or_explicit(&l_input.buttons, mask, memory_order_relaxed);
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}
