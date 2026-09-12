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

#include "input_event.h"

#include "../../src/kb.h"

#include <string.h>

enum ModifierKey
{
  MODIFIER_KEY_LEFT  = 1U << 0,
  MODIFIER_KEY_RIGHT = 1U << 1,
};

_Static_assert(sizeof(double) == sizeof(uint64_t), "Unexpected double size");

static bool validCoordinate(double value)
{
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return (bits & UINT64_C(0x7FF0000000000000)) !=
    UINT64_C(0x7FF0000000000000);
}

static bool finishPointerLeave(WindowsInput * input)
{
  if (!input->entered || !input->leavePending || input->buttons ||
      input->pointerGrabbed)
    return false;

  input->entered      = false;
  input->leavePending = false;
  input->sink->enter(input->opaque, false);
  return true;
}

static void updateKeyState(WindowsInput * input, int key, bool pressed)
{
  uint8_t * modifier = NULL;
  uint8_t   mask     = 0;
  bool    * lock     = NULL;
  bool    * lockDown = NULL;

  switch (key)
  {
    case LG_KEY_LEFTCTRL:
      modifier = &input->ctrlKeys;
      mask     = MODIFIER_KEY_LEFT;
      break;

    case LG_KEY_RIGHTCTRL:
      modifier = &input->ctrlKeys;
      mask     = MODIFIER_KEY_RIGHT;
      break;

    case LG_KEY_LEFTSHIFT:
      modifier = &input->shiftKeys;
      mask     = MODIFIER_KEY_LEFT;
      break;

    case LG_KEY_RIGHTSHIFT:
      modifier = &input->shiftKeys;
      mask     = MODIFIER_KEY_RIGHT;
      break;

    case LG_KEY_LEFTALT:
      modifier = &input->altKeys;
      mask     = MODIFIER_KEY_LEFT;
      break;

    case LG_KEY_RIGHTALT:
      modifier = &input->altKeys;
      mask     = MODIFIER_KEY_RIGHT;
      break;

    case LG_KEY_LEFTMETA:
      modifier = &input->superKeys;
      mask     = MODIFIER_KEY_LEFT;
      break;

    case LG_KEY_RIGHTMETA:
      modifier = &input->superKeys;
      mask     = MODIFIER_KEY_RIGHT;
      break;

    case LG_KEY_NUMLOCK:
      lock     = &input->numLock;
      lockDown = &input->numLockPressed;
      break;

    case LG_KEY_CAPSLOCK:
      lock     = &input->capsLock;
      lockDown = &input->capsLockPressed;
      break;

    case LG_KEY_SCROLLLOCK:
      lock     = &input->scrollLock;
      lockDown = &input->scrollLockPressed;
      break;
  }

  if (modifier)
  {
    if (pressed)
      *modifier |= mask;
    else
      *modifier &= ~mask;
  }

  if (lock)
  {
    if (pressed && !*lockDown)
      *lock     = !*lock;
    *lockDown = pressed;
  }
}

static bool modifierKey(int key)
{
  return key == LG_KEY_LEFTCTRL  || key == LG_KEY_RIGHTCTRL ||
    key == LG_KEY_LEFTSHIFT || key == LG_KEY_RIGHTSHIFT ||
    key == LG_KEY_LEFTALT   || key == LG_KEY_RIGHTALT ||
    key == LG_KEY_LEFTMETA  || key == LG_KEY_RIGHTMETA;
}

void windowsInputInit(WindowsInput * input, const LG_DSInputSink * sink,
    void * opaque)
{
  memset(input, 0, sizeof(*input));
  input->sink   = sink;
  input->opaque = opaque;
}

void windowsInputSetPointerGrabbed(WindowsInput * input, bool grabbed)
{
  if (input->pointerGrabbed == grabbed)
    return;

  input->pointerGrabbed = grabbed;
  if (!grabbed)
    finishPointerLeave(input);
}

bool windowsInputIsPointerGrabbed(const WindowsInput * input)
{
  return input->pointerGrabbed;
}

void windowsInputSetKeyboardGrabbed(WindowsInput * input, bool grabbed)
{
  input->keyboardGrabbed = grabbed;
}

bool windowsInputIsKeyboardGrabbed(const WindowsInput * input)
{
  return input->keyboardGrabbed;
}

bool windowsInputFocus(WindowsInput * input, bool focused)
{
  if (input->focused == focused)
    return false;

  input->focused = focused;
  if (!focused)
  {
    input->ctrlKeys          = 0;
    input->shiftKeys         = 0;
    input->altKeys           = 0;
    input->superKeys         = 0;
    input->numLockPressed    = false;
    input->capsLockPressed   = false;
    input->scrollLockPressed = false;
    input->printScreenKey    = LG_KEY_RESERVED;
  }

  input->sink->focus(input->opaque, focused);
  return true;
}

bool windowsInputPointerEnter(WindowsInput * input, double x, double y)
{
  if (!validCoordinate(x) || !validCoordinate(y))
    return false;

  input->leavePending = false;
  if (input->entered)
    return false;

  input->entered = true;
  input->sink->enter(input->opaque, true);
  input->sink->position(input->opaque, x, y);
  return true;
}

bool windowsInputPointerLeave(WindowsInput * input)
{
  if (!input->entered)
    return false;

  input->leavePending = true;
  return finishPointerLeave(input);
}

void windowsInputPointerMotion(WindowsInput * input, double x, double y)
{
  if (!input->entered || input->leavePending || input->pointerGrabbed ||
      !validCoordinate(x) || !validCoordinate(y))
    return;

  input->sink->position(input->opaque, x, y);
}

void windowsInputRelativeMotion(WindowsInput * input, double x, double y)
{
  if (!input->entered || !input->pointerGrabbed ||
      !validCoordinate(x) || !validCoordinate(y))
    return;

  input->sink->relative(input->opaque, x, y, x, y);
}

bool windowsInputPointerButton(WindowsInput * input, unsigned int button,
    bool pressed)
{
  if ((!input->entered && !input->pointerGrabbed) ||
      button == 0 || button >= 32)
    return false;

  const uint32_t mask = UINT32_C(1) << button;
  if (((input->buttons & mask) != 0) == pressed)
    return false;

  if (pressed)
    input->buttons |= mask;
  else
    input->buttons &= ~mask;

  input->sink->button(input->opaque, button, pressed);
  if (!pressed)
    finishPointerLeave(input);
  return true;
}

void windowsInputReleaseButtons(WindowsInput * input)
{
  const uint32_t buttons = input->buttons;
  input->buttons          = 0;

  for (unsigned int button = 1; button < 32; ++button)
    if (buttons & (UINT32_C(1) << button))
      input->sink->button(input->opaque, button, false);

  finishPointerLeave(input);
}

void windowsInputPointerWheel(WindowsInput * input, double motion)
{
  if ((!input->entered && !input->pointerGrabbed) ||
      !validCoordinate(motion) || motion == 0.0)
    return;

  input->wheel += motion;
  while (input->wheel >= 1.0)
  {
    input->sink->button(input->opaque, 4, true);
    input->sink->button(input->opaque, 4, false);
    input->wheel -= 1.0;
  }

  while (input->wheel <= -1.0)
  {
    input->sink->button(input->opaque, 5, true);
    input->sink->button(input->opaque, 5, false);
    input->wheel += 1.0;
  }

  input->sink->wheel(input->opaque, -motion);
}

int windowsInputMapScanCode(uint8_t scanCode, bool extended)
{
  const uint32_t ps2 = scanCode |
    (extended ? UINT32_C(0xE000) : UINT32_C(0));
  if (!ps2)
    return LG_KEY_RESERVED;

  for (int key = 1; key < LG_KEY_MAX; ++key)
    if (lg_keycode_to_ps2[key] == ps2)
      return key;

  return LG_KEY_RESERVED;
}

int windowsInputMapPrintScreen(bool altDown)
{
  return altDown ? LG_KEY_SYSRQ : LG_KEY_PRINT;
}

int windowsInputMapPrintScreenEvent(WindowsInput * input,
    bool altDown, bool pressed)
{
  if (pressed)
  {
    if (input->printScreenKey == LG_KEY_RESERVED)
      input->printScreenKey = windowsInputMapPrintScreen(altDown);

    return input->printScreenKey;
  }

  const int key         = input->printScreenKey == LG_KEY_RESERVED ?
    windowsInputMapPrintScreen(altDown) : input->printScreenKey;
  input->printScreenKey = LG_KEY_RESERVED;
  return key;
}

bool windowsInputKeyCode(WindowsInput * input, int key, bool pressed,
    bool grabbedEvent)
{
  if (!input->focused || grabbedEvent != input->keyboardGrabbed ||
      key <= LG_KEY_RESERVED || key >= LG_KEY_MAX)
    return false;

  updateKeyState(input, key, pressed);
  input->sink->key(input->opaque, key, pressed);
  return true;
}

bool windowsInputKey(WindowsInput * input, uint8_t scanCode, bool extended,
    bool pressed, bool grabbedEvent)
{
  return windowsInputKeyCode(input,
      windowsInputMapScanCode(scanCode, extended), pressed, grabbedEvent);
}

size_t windowsInputSyncHeldKeys(WindowsInput * input,
    const int * keys, size_t count, bool grabbedEvent)
{
  if (!keys)
    return 0;

  size_t synced = 0;
  for (unsigned int pass = 0; pass < 2; ++pass)
    for (size_t i = 0; i < count; ++i)
    {
      const bool modifier = modifierKey(keys[i]);
      if (modifier != (pass == 0))
        continue;

      if (windowsInputKeyCode(input, keys[i], true, grabbedEvent))
        ++synced;
    }

  return synced;
}

void windowsInputText(WindowsInput * input, const char * text)
{
  if (input->focused && !input->keyboardGrabbed && text && *text)
    input->sink->text(input->opaque, text);
}

void windowsInputState(WindowsInput * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock)
{
  input->numLock    = numLock;
  input->capsLock   = capsLock;
  input->scrollLock = scrollLock;
  input->sink->modifiers(input->opaque, ctrl, shift, alt, super);
  input->sink->leds(input->opaque, numLock, capsLock, scrollLock);
}

void windowsInputTrackedState(WindowsInput * input)
{
  input->sink->modifiers(input->opaque,
      input->ctrlKeys != 0, input->shiftKeys != 0,
      input->altKeys != 0, input->superKeys != 0);
  input->sink->leds(input->opaque,
      input->numLock, input->capsLock, input->scrollLock);
}
