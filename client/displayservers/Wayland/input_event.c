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

#include <linux/input.h>
#include <string.h>

static const double WL_SCROLL_STEP      = 15.0;
static const double WL_HALF_SCROLL_STEP = 7.5;

void wlInputInit(WaylandInput * input, const LG_DSInputSink * sink,
    void * opaque)
{
  memset(input, 0, sizeof(*input));
  input->sink   = sink;
  input->opaque = opaque;
}

bool wlInputPointerEnter(WaylandInput * input, bool mainSurface)
{
  if (!mainSurface)
    return false;

  input->pointerInSurface = true;
  input->sink->enter(input->opaque, true);
  return true;
}

bool wlInputPointerLeave(WaylandInput * input, bool mainSurface)
{
  if (!mainSurface)
    return false;

  input->pointerInSurface = false;
  input->sink->enter(input->opaque, false);
  return true;
}

void wlInputPointerDisconnect(WaylandInput * input, bool notify)
{
  input->pointerInSurface = false;
  if (notify)
    input->sink->enter(input->opaque, false);
}

void wlInputPointerMotion(WaylandInput * input, double x, double y)
{
  input->sink->position(input->opaque, x, y);
}

static unsigned int mapButton(uint32_t button)
{
  switch (button)
  {
    case BTN_LEFT:
      return 1;
    case BTN_MIDDLE:
      return 2;
    case BTN_RIGHT:
      return 3;
    case BTN_SIDE:
      return 6;
    case BTN_EXTRA:
      return 7;
    case BTN_FORWARD:
      return 8;
    case BTN_BACK:
      return 9;
    case BTN_TASK:
      return 10;
  }

  return 0;
}

void wlInputPointerButton(WaylandInput * input, uint32_t button,
    bool pressed)
{
  const unsigned int mapped = mapButton(button);
  if (mapped)
    input->sink->button(input->opaque, mapped, pressed);
}

void wlInputPointerAxis(WaylandInput * input, bool vertical, double delta)
{
  if (!vertical)
    return;

  input->scrollState += delta;

  while (input->scrollState > WL_HALF_SCROLL_STEP)
  {
    input->sink->button(input->opaque, 5, true);
    input->sink->button(input->opaque, 5, false);
    input->scrollState -= WL_SCROLL_STEP;
  }

  while (input->scrollState < -WL_HALF_SCROLL_STEP)
  {
    input->sink->button(input->opaque, 4, true);
    input->sink->button(input->opaque, 4, false);
    input->scrollState += WL_SCROLL_STEP;
  }

  input->sink->wheel(input->opaque, delta / WL_SCROLL_STEP);
}

void wlInputRelativeMotion(WaylandInput * input, bool active,
    double x, double y, double rawX, double rawY)
{
  if (active)
    input->sink->relative(input->opaque, x, y, rawX, rawY);
}

void wlInputRealignPointer(WaylandInput * input, double x, double y)
{
  if (input->pointerInSurface)
    input->sink->position(input->opaque, x, y);
}

bool wlInputKeyboardEnter(WaylandInput * input, bool mainSurface,
    const uint32_t * keys, size_t count)
{
  if (!mainSurface)
    return false;

  input->focused = true;
  input->sink->focus(input->opaque, true);
  for (size_t i = 0; i < count; ++i)
    input->sink->key(input->opaque, keys[i], true);
  return true;
}

bool wlInputKeyboardLeave(WaylandInput * input, bool mainSurface)
{
  if (!mainSurface)
    return false;

  input->focused = false;
  input->sink->focus(input->opaque, false);
  return true;
}

void wlInputKeyboardKey(WaylandInput * input, uint32_t key, bool pressed,
    const char * text)
{
  if (!input->focused)
    return;

  input->sink->key(input->opaque, key, pressed);
  if (pressed && text && *text)
    input->sink->text(input->opaque, text);
}

void wlInputKeyboardState(WaylandInput * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock)
{
  input->sink->modifiers(input->opaque, ctrl, shift, alt, super);
  input->sink->leds(input->opaque, numLock, capsLock, scrollLock);
}
