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

#include <string.h>

static unsigned int mapButton(unsigned int button)
{
  if (button >= 1 && button <= 5)
    return button;

  /* X11 reserves buttons 6 and 7 for horizontal scrolling. */
  if (button >= 8 && button < 34)
    return button - 2;

  return 0;
}

void x11InputInit(X11Input * input, const LG_DSInputSink * sink,
    void * opaque)
{
  memset(input, 0, sizeof(*input));
  atomic_init(&input->pointerGrabbed, false);
  atomic_init(&input->keyboardGrabbed, false);
  input->sink   = sink;
  input->opaque = opaque;
}

void x11InputSetPointerGrabbed(X11Input * input, bool grabbed)
{
  atomic_store_explicit(&input->pointerGrabbed, grabbed,
      memory_order_release);
}

bool x11InputIsPointerGrabbed(const X11Input * input)
{
  return atomic_load_explicit(&input->pointerGrabbed,
      memory_order_acquire);
}

void x11InputSetKeyboardGrabbed(X11Input * input, bool grabbed)
{
  atomic_store_explicit(&input->keyboardGrabbed, grabbed,
      memory_order_release);
}

bool x11InputIsKeyboardGrabbed(const X11Input * input)
{
  return atomic_load_explicit(&input->keyboardGrabbed,
      memory_order_acquire);
}

bool x11InputFocus(X11Input * input, bool focused,
    const uint32_t * keys, size_t count)
{
  if (input->focused == focused)
    return false;

  input->focused = focused;
  input->sink->focus(input->opaque, focused);

  if (focused)
    for (size_t i = 0; i < count; ++i)
      input->sink->key(input->opaque, keys[i], true);

  return true;
}

bool x11InputPointerEnter(X11Input * input, bool mainWindow,
    bool normal, double x, double y)
{
  if (input->entered || !mainWindow || !normal)
    return false;

  input->entered = true;
  input->sink->enter(input->opaque, true);
  input->sink->position(input->opaque, x, y);
  return true;
}

bool x11InputPointerLeave(X11Input * input, bool mainWindow,
    bool normal, bool captureMode)
{
  if (!input->entered || !mainWindow || input->buttons || captureMode ||
      !normal)
    return false;

  input->entered = false;
  input->sink->enter(input->opaque, false);
  return true;
}

void x11InputPointerMotion(X11Input * input, double x, double y)
{
  input->sink->position(input->opaque, x, y);
}

void x11InputPointerButton(X11Input * input, unsigned int detail,
    bool pressed, bool raw)
{
  if (!input->entered || raw != x11InputIsPointerGrabbed(input))
    return;

  const unsigned int button = mapButton(detail);
  if (!button)
    return;

  if (!raw)
  {
    if (button == 4 || button == 5)
    {
      if (pressed)
      {
        input->sink->button(input->opaque, button, true);
        input->sink->button(input->opaque, button, false);
        input->sink->wheel(input->opaque, button == 4 ? -1.0 : 1.0);
      }
      return;
    }
  }

  const uint32_t mask = UINT32_C(1) << button;
  if (pressed)
    input->buttons |= mask;
  else
    input->buttons &= ~mask;

  input->sink->button(input->opaque, button, pressed);
}

void x11InputRelativeMotion(X11Input * input,
    double x, double y, double rawX, double rawY)
{
  if (!input->entered || !x11InputIsPointerGrabbed(input))
    return;

  input->sink->relative(input->opaque, x, y, rawX, rawY);
}

bool x11InputKeyboardKey(X11Input * input, unsigned int keycode,
    int minKeycode, bool pressed, bool raw)
{
  if (!input->focused || raw != x11InputIsKeyboardGrabbed(input))
    return false;

  input->sink->key(input->opaque, (int)keycode - minKeycode, pressed);
  return true;
}

void x11InputKeyboardText(X11Input * input, const char * text)
{
  if (text && *text)
    input->sink->text(input->opaque, text);
}

void x11InputKeyboardState(X11Input * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock)
{
  input->sink->modifiers(input->opaque, ctrl, shift, alt, super);
  input->sink->leds(input->opaque, numLock, capsLock, scrollLock);
}

size_t x11InputHeldKeys(
    const char keymap[X11_INPUT_KEYMAP_SIZE],
    int minKeycode, int maxKeycode,
    uint32_t * keys, size_t capacity)
{
  if (minKeycode < 0 || maxKeycode < minKeycode)
    return 0;

  size_t count = 0;
  for (int keycode = minKeycode;
       keycode <= maxKeycode && keycode < X11_INPUT_KEYMAP_SIZE * 8;
       ++keycode)
  {
    const unsigned char keyByte = (unsigned char)keymap[keycode / 8];
    if (!(keyByte & (1U << (keycode % 8))))
      continue;

    if (count == capacity)
      break;

    keys[count++] = keycode - minKeycode;
  }

  return count;
}
