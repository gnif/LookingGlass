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
  uint32_t            generation;
};

static struct
{
  LG_RWLock activeLock;

  struct InputBinding fallback;
  struct InputBinding transport;
  struct InputBinding active;
  bool                useTransport;

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
  return (struct InputBinding)
  {
    .ops           = ops,
    .opaque        = opaque,
    .available     = available,
    .mouseAbsolute = available && ops->mousePosition &&
      ops->supports(opaque, LG_INPUT_SUPPORT_MOUSE_ABSOLUTE),
    .generation    = 0,
  };
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

static void updateActiveNL(void)
{
  const struct InputBinding next =
    l_input.useTransport && l_input.transport.available ?
      l_input.transport :
    l_input.fallback.available ? l_input.fallback :
      (struct InputBinding) { 0 };

  if (next.ops == l_input.active.ops &&
      next.opaque == l_input.active.opaque)
  {
    l_input.active = next;
    return;
  }

  resetActiveNL();
  l_input.active = next;

  if (l_input.active.ops)
  {
    if (l_input.active.ops->reset)
      l_input.active.ops->reset(l_input.active.opaque);
    DEBUG_INFO("Using Input: %s", l_input.active.ops->name);
  }
  else
    DEBUG_INFO("Input is unavailable");
}

static void updateStatusNL(struct InputBinding * binding,
    const LG_InputStatus * status)
{
  const bool wasActive =
    l_input.active.ops    == binding->ops &&
    l_input.active.opaque == binding->opaque;

  if (wasActive && !status->available)
  {
    resetActiveNL();
    l_input.active = (struct InputBinding) { 0 };
  }

  binding->available     = status->available;
  binding->mouseAbsolute = status->available &&
    binding->ops->mousePosition &&
    binding->ops->supports(binding->opaque,
      LG_INPUT_SUPPORT_MOUSE_ABSOLUTE);
  binding->generation    = status->generation;
  updateActiveNL();
}

static void fallbackStatusChanged(void * opaque,
    const LG_InputStatus * status)
{
  if (!status)
    return;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  if (l_input.fallback.ops && l_input.fallback.opaque == opaque)
    updateStatusNL(&l_input.fallback, status);
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

static void transportStatusChanged(void * opaque,
    const LG_InputStatus * status)
{
  if (!status)
    return;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  if (l_input.transport.ops && l_input.transport.opaque == opaque)
    updateStatusNL(&l_input.transport, status);
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
}

void lgInput_init(void)
{
  l_input.fallback     = (struct InputBinding) { 0 };
  l_input.transport    = (struct InputBinding) { 0 };
  l_input.active       = (struct InputBinding) { 0 };
  l_input.useTransport = true;

  for (int key = 0; key < KEY_MAX; ++key)
    atomic_init(&l_input.keys[key], false);
  atomic_init(&l_input.buttons, 0);

  LG_RWLOCK_INIT(l_input.activeLock);
}

void lgInput_free(void)
{
  struct InputBinding fallback;
  struct InputBinding transport;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  resetActiveNL();
  fallback             = l_input.fallback;
  transport            = l_input.transport;
  l_input.active       = (struct InputBinding) { 0 };
  l_input.fallback     = (struct InputBinding) { 0 };
  l_input.transport    = (struct InputBinding) { 0 };
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (fallback.ops && fallback.ops->setStatusListener)
    fallback.ops->setStatusListener(fallback.opaque, NULL, NULL);
  if (transport.ops && transport.ops->setStatusListener)
    transport.ops->setStatusListener(transport.opaque, NULL, NULL);

  LG_RWLOCK_FREE(l_input.activeLock);
}

void lgInput_setFallback(const LG_InputOps * ops, void * opaque)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid fallback input operations");
    ops    = NULL;
    opaque = NULL;
  }

  const struct InputBinding next = makeBinding(ops, opaque);
  struct InputBinding old;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  old              = l_input.fallback;
  l_input.fallback = next;
  updateActiveNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);
  if (next.ops && next.ops->setStatusListener)
    next.ops->setStatusListener(next.opaque,
        fallbackStatusChanged, next.opaque);
}

void lgInput_setTransport(const LG_InputOps * ops, void * opaque)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid transport input operations");
    ops    = NULL;
    opaque = NULL;
  }

  const struct InputBinding next = makeBinding(ops, opaque);
  struct InputBinding old;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  old               = l_input.transport;
  l_input.transport = next;
  updateActiveNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);
  if (next.ops && next.ops->setStatusListener)
    next.ops->setStatusListener(next.opaque,
        transportStatusChanged, next.opaque);
}

void lgInput_dropTransport(void)
{
  struct InputBinding old;

  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  old = l_input.transport;
  if (l_input.useTransport && l_input.active.ops == l_input.transport.ops &&
      l_input.active.opaque == l_input.transport.opaque)
  {
    l_input.active = (struct InputBinding) { 0 };
    clearStateNL();
  }

  l_input.transport = (struct InputBinding) { 0 };
  updateActiveNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);
}

void lgInput_useTransport(bool enable)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  l_input.useTransport = enable;
  updateActiveNL();
  LG_UNLOCK_EXCLUSIVE(l_input.activeLock);
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
  LG_LOCK_SHARED(l_input.activeLock);
  const bool result = l_input.active.ops &&
    (!l_input.active.ops->keyboardLEDs ||
     l_input.active.ops->keyboardLEDs(l_input.active.opaque,
       numLock, capsLock, scrollLock));
  LG_UNLOCK_SHARED(l_input.activeLock);
  return result;
}

void lgInput_releaseKeys(void)
{
  LG_LOCK_EXCLUSIVE(l_input.activeLock);
  releaseKeysNL();
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
