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
  input->sink   = sink;
  input->opaque = opaque;
}

bool x11InputFocus(X11Input * input, bool focused, double x, double y,
    const uint32_t * keys, size_t count)
{
  (void)keys;
  (void)count;

  if (input->focused == focused)
    return false;

  input->focused = focused;
  input->sink->position(input->opaque, x, y);
  input->sink->focus(input->opaque, focused);
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
  if (!raw && !input->entered)
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
  input->sink->relative(input->opaque, x, y, rawX, rawY);
}

void x11InputKeyboardKey(X11Input * input, unsigned int keycode,
    int minKeycode, bool pressed, bool raw, bool inputActive,
    const char * text)
{
  if (raw)
  {
    if (!inputActive)
      return;
  }
  else if (!input->focused)
    return;

  input->sink->key(input->opaque, (int)keycode - minKeycode, pressed);

  if (!raw && pressed && text)
    input->sink->text(input->opaque, text);
}

void x11InputKeyboardState(X11Input * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock)
{
  (void)numLock;
  (void)capsLock;
  (void)scrollLock;
  input->sink->modifiers(input->opaque, ctrl, shift, alt, super);
}
