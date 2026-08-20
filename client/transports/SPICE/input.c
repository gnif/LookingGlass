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

#include "common/locking.h"

#include <purespice.h>
#include <stddef.h>
#include <stdlib.h>

struct SpiceInput
{
  LG_Lock stateLock;
  LG_Lock statusDispatch;

  bool             available;
  bool             keyboardLEDsValid;
  uint8_t          keyboardLEDs;
  uint32_t         statusGeneration;
  LG_InputStatusFn statusCallback;
  void           * statusOpaque;
};

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static bool spiceSupports(void * opaque, LG_InputSupport support)
{
  (void)opaque;
  (void)support;
  return false;
}

static void spiceSetStatusListener(void * opaque,
    LG_InputStatusFn callback, void * callbackOpaque)
{
  SpiceInput * input = opaque;
  LG_LOCK(input->statusDispatch);

  LG_LOCK(input->stateLock);
  input->statusCallback = callback;
  input->statusOpaque   = callbackOpaque;
  const LG_InputStatus status =
  {
    .available         = input->available,
    .generation        = input->statusGeneration,
    .keyboardLEDsValid = input->available && input->keyboardLEDsValid,
    .keyboardLEDs      = input->keyboardLEDs,
  };
  LG_UNLOCK(input->stateLock);

  if (callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(input->statusDispatch);
}

static bool spiceKeyDown(void * opaque, int key)
{
  SpiceInput * input = opaque;
  const uint32_t ps2 = linux_to_ps2[key];

  LG_LOCK(input->stateLock);
  const bool result = input->available &&
    (!ps2 || purespice_keyDown(ps2));
  LG_UNLOCK(input->stateLock);
  return result;
}

static bool spiceKeyUp(void * opaque, int key)
{
  SpiceInput * input = opaque;
  const uint32_t ps2 = linux_to_ps2[key];

  LG_LOCK(input->stateLock);
  const bool result = !ps2 || purespice_keyUp(ps2);
  LG_UNLOCK(input->stateLock);
  return result;
}

static bool spiceKeyboardLEDs(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  SpiceInput * input = opaque;
  const uint32_t modifiers =
    (scrollLock ? 1 /* SPICE_SCROLL_LOCK_MODIFIER */ : 0) |
    (numLock    ? 2 /* SPICE_NUM_LOCK_MODIFIER    */ : 0) |
    (capsLock   ? 4 /* SPICE_CAPS_LOCK_MODIFIER   */ : 0);

  LG_LOCK(input->stateLock);
  const bool result = input->available &&
    purespice_keyModifiers(modifiers);
  LG_UNLOCK(input->stateLock);
  return result;
}

static bool spiceMouseMotion(void * opaque, int32_t x, int32_t y)
{
  SpiceInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool result = input->available && purespice_mouseMotion(x, y);
  LG_UNLOCK(input->stateLock);
  return result;
}

static bool spiceMousePress(void * opaque, unsigned int button)
{
  if (button > 7)
    return true;

  SpiceInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool result = input->available && purespice_mousePress(button);
  LG_UNLOCK(input->stateLock);
  return result;
}

static bool spiceMouseRelease(void * opaque, unsigned int button)
{
  if (button > 7)
    return true;

  SpiceInput * input = opaque;
  LG_LOCK(input->stateLock);
  const bool result = purespice_mouseRelease(button);
  LG_UNLOCK(input->stateLock);
  return result;
}

static const LG_InputOps l_inputOps =
{
  .name              = "SPICE",
  .supports          = spiceSupports,
  .setStatusListener = spiceSetStatusListener,
  .keyDown           = spiceKeyDown,
  .keyUp             = spiceKeyUp,
  .keyboardLEDs      = spiceKeyboardLEDs,
  .mouseMotion       = spiceMouseMotion,
  .mousePosition     = NULL,
  .mousePress        = spiceMousePress,
  .mouseRelease      = spiceMouseRelease,
  .reset             = NULL,
};

bool spiceInput_init(SpiceInput ** input)
{
  *input = calloc(1, sizeof(**input));
  if (!*input)
    return false;

  LG_LOCK_INIT((*input)->stateLock);
  LG_LOCK_INIT((*input)->statusDispatch);
  return true;
}

void spiceInput_free(SpiceInput ** input)
{
  if (!input || !*input)
    return;

  LG_LOCK_FREE((*input)->statusDispatch);
  LG_LOCK_FREE((*input)->stateLock);
  free(*input);
  *input = NULL;
}

const LG_InputOps * spiceInput_getOps(void)
{
  return &l_inputOps;
}

void spiceInput_setAvailable(SpiceInput * input, bool available)
{
  LG_LOCK(input->statusDispatch);
  LG_LOCK(input->stateLock);

  const bool oldLEDsValid =
    input->available && input->keyboardLEDsValid;
  const bool changed = input->available != available;
  if (changed)
  {
    input->available        = available;
    input->statusGeneration = nextGeneration(input->statusGeneration);
  }
  if (!available)
    input->keyboardLEDsValid = false;

  const bool statusChanged = changed || oldLEDsValid !=
    (input->available && input->keyboardLEDsValid);

  const LG_InputStatusFn callback       = input->statusCallback;
  void                 * callbackOpaque = input->statusOpaque;
  const LG_InputStatus status =
  {
    .available         = input->available,
    .generation        = input->statusGeneration,
    .keyboardLEDsValid = input->available && input->keyboardLEDsValid,
    .keyboardLEDs      = input->keyboardLEDs,
  };
  LG_UNLOCK(input->stateLock);

  if (statusChanged && callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(input->statusDispatch);
}

void spiceInput_setKeyboardLEDs(SpiceInput * input, uint32_t modifiers)
{
  const uint8_t keyboardLEDs =
    (modifiers & 2 ? LG_KEYBOARD_LED_NUM_LOCK    : 0) |
    (modifiers & 4 ? LG_KEYBOARD_LED_CAPS_LOCK   : 0) |
    (modifiers & 1 ? LG_KEYBOARD_LED_SCROLL_LOCK : 0);

  LG_LOCK(input->statusDispatch);
  LG_LOCK(input->stateLock);
  const bool changed = !input->keyboardLEDsValid ||
    input->keyboardLEDs != keyboardLEDs;
  input->keyboardLEDsValid = true;
  input->keyboardLEDs      = keyboardLEDs;

  const LG_InputStatusFn callback       = input->statusCallback;
  void                 * callbackOpaque = input->statusOpaque;
  const LG_InputStatus status =
  {
    .available         = input->available,
    .generation        = input->statusGeneration,
    .keyboardLEDsValid = input->available,
    .keyboardLEDs      = input->keyboardLEDs,
  };
  LG_UNLOCK(input->stateLock);

  if (changed && callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(input->statusDispatch);
}
