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

#include "Windows/input_event.h"
#include "Windows/window_size.h"
#include "input/keycodes.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

enum TraceType
{
  TRACE_POSITION,
  TRACE_RELATIVE,
  TRACE_BUTTON,
  TRACE_WHEEL,
  TRACE_KEY,
  TRACE_TEXT,
  TRACE_MODIFIERS,
  TRACE_LEDS,
  TRACE_ENTER,
  TRACE_FOCUS,
};

struct Trace
{
  enum TraceType type;
  int            value;
  double         x;
  double         y;
  bool           state;
  char           text[16];
};

#define TRACE_CAPACITY 256

struct Fixture
{
  WindowsInput input;
  struct Trace trace[TRACE_CAPACITY];
  size_t       count;
};

static struct Trace * push(struct Fixture * fixture, enum TraceType type)
{
  CHECK(fixture->count < TRACE_CAPACITY);
  struct Trace * trace = &fixture->trace[fixture->count++];
  memset(trace, 0, sizeof(*trace));
  trace->type = type;
  return trace;
}

static void position(void * opaque, double x, double y)
{
  struct Trace * trace = push(opaque, TRACE_POSITION);
  trace->x = x;
  trace->y = y;
}

static void relative(void * opaque, double x, double y,
    double rawX, double rawY)
{
  struct Trace * trace = push(opaque, TRACE_RELATIVE);
  trace->x = x;
  trace->y = y;
  CHECK(x == rawX);
  CHECK(y == rawY);
}

static void button(void * opaque, unsigned int button, bool pressed)
{
  struct Trace * trace = push(opaque, TRACE_BUTTON);
  trace->value = button;
  trace->state = pressed;
}

static void wheel(void * opaque, double motion)
{
  struct Trace * trace = push(opaque, TRACE_WHEEL);
  trace->x = motion;
}

static void key(void * opaque, int scancode, bool pressed)
{
  struct Trace * trace = push(opaque, TRACE_KEY);
  trace->value = scancode;
  trace->state = pressed;
}

static void textInput(void * opaque, const char * text)
{
  struct Trace * trace = push(opaque, TRACE_TEXT);
  snprintf(trace->text, sizeof(trace->text), "%s", text);
}

static void modifiers(void * opaque, bool ctrl, bool shift,
    bool alt, bool super)
{
  struct Trace * trace = push(opaque, TRACE_MODIFIERS);
  trace->value = ctrl | shift << 1 | alt << 2 | super << 3;
}

static void leds(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  struct Trace * trace = push(opaque, TRACE_LEDS);
  trace->value = numLock | capsLock << 1 | scrollLock << 2;
}

static void enter(void * opaque, bool entered)
{
  struct Trace * trace = push(opaque, TRACE_ENTER);
  trace->state = entered;
}

static void focus(void * opaque, bool focused)
{
  struct Trace * trace = push(opaque, TRACE_FOCUS);
  trace->state = focused;
}

static const LG_DSInputSink sink =
{
  .position  = position,
  .relative  = relative,
  .button    = button,
  .wheel     = wheel,
  .key       = key,
  .text      = textInput,
  .modifiers = modifiers,
  .leds      = leds,
  .enter     = enter,
  .focus     = focus,
};

static void init(struct Fixture * fixture)
{
  memset(fixture, 0, sizeof(*fixture));
  windowsInputInit(&fixture->input, &sink, fixture);
}

static double doubleFromBits(uint64_t bits)
{
  double value;
  _Static_assert(sizeof(value) == sizeof(bits), "Unexpected double size");
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static void testScanCodes(void)
{
  CHECK(windowsInputMapScanCode(0x1E, false) == LG_KEY_A);
  CHECK(windowsInputMapScanCode(0x1D, false) == LG_KEY_LEFTCTRL);
  CHECK(windowsInputMapScanCode(0x1D, true ) == LG_KEY_RIGHTCTRL);
  CHECK(windowsInputMapScanCode(0x5B, true ) == LG_KEY_LEFTMETA);
  CHECK(windowsInputMapScanCode(0x53, true ) == LG_KEY_DELETE);
  CHECK(windowsInputMapScanCode(0x00, false) == LG_KEY_RESERVED);
  CHECK(windowsInputMapScanCode(0xFF, false) == LG_KEY_RESERVED);
  CHECK(windowsInputMapPrintScreen(false) == LG_KEY_PRINT);
  CHECK(windowsInputMapPrintScreen(true ) == LG_KEY_SYSRQ);
}

static void testKeyboardRouting(void)
{
  struct Fixture fixture;
  init(&fixture);

  windowsInputText(&fixture.input, "ignored");
  CHECK(!windowsInputKey(&fixture.input, 0x1E, false, true, false));
  CHECK(windowsInputFocus(&fixture.input, true));
  CHECK(!windowsInputFocus(&fixture.input, true));
  CHECK(windowsInputKey(&fixture.input, 0x1E, false, true, false));
  windowsInputText(&fixture.input, "a");
  windowsInputSetKeyboardGrabbed(&fixture.input, true);
  CHECK(!windowsInputKey(&fixture.input, 0x1E, false, false, false));
  CHECK(windowsInputKey(&fixture.input, 0x1E, false, false, true));
  windowsInputText(&fixture.input, "ignored");
  windowsInputState(&fixture.input, true, false, true, false,
      true, false, true);

  CHECK(fixture.count == 6);
  CHECK(fixture.trace[0].type  == TRACE_FOCUS);
  CHECK(fixture.trace[0].state == true);
  CHECK(fixture.trace[1].type  == TRACE_KEY);
  CHECK(fixture.trace[1].value == LG_KEY_A);
  CHECK(fixture.trace[1].state == true);
  CHECK(fixture.trace[2].type  == TRACE_TEXT);
  CHECK(strcmp(fixture.trace[2].text, "a") == 0);
  CHECK(fixture.trace[3].type  == TRACE_KEY);
  CHECK(fixture.trace[3].value == LG_KEY_A);
  CHECK(fixture.trace[3].state == false);
  CHECK(fixture.trace[4].type  == TRACE_MODIFIERS);
  CHECK(fixture.trace[4].value == 5);
  CHECK(fixture.trace[5].type  == TRACE_LEDS);
  CHECK(fixture.trace[5].value == 5);
}

static void testPrintScreenLatch(void)
{
  struct Fixture fixture;
  init(&fixture);

  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, true, true) == LG_KEY_SYSRQ);
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, false, true) == LG_KEY_SYSRQ);
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, false, false) == LG_KEY_SYSRQ);
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, false, true) == LG_KEY_PRINT);
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, true, false) == LG_KEY_PRINT);

  CHECK(windowsInputFocus(&fixture.input, true));
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, true, true) == LG_KEY_SYSRQ);
  CHECK(windowsInputFocus(&fixture.input, false));
  CHECK(windowsInputMapPrintScreenEvent(
      &fixture.input, false, false) == LG_KEY_PRINT);
}

static void testHeldKeys(void)
{
  struct Fixture fixture;
  init(&fixture);

  const int keys[] =
  {
    LG_KEY_A,
    LG_KEY_RESERVED,
    LG_KEY_LEFTSHIFT,
    LG_KEY_MAX,
  };

  CHECK(windowsInputSyncHeldKeys(&fixture.input,
      keys, sizeof(keys) / sizeof(*keys), false) == 0);
  CHECK(windowsInputFocus(&fixture.input, true));
  CHECK(windowsInputSyncHeldKeys(&fixture.input,
      keys, sizeof(keys) / sizeof(*keys), false) == 2);
  windowsInputTrackedState(&fixture.input);

  windowsInputSetKeyboardGrabbed(&fixture.input, true);
  CHECK(windowsInputSyncHeldKeys(&fixture.input, keys, 1, false) == 0);
  CHECK(windowsInputSyncHeldKeys(&fixture.input, keys, 1, true ) == 1);
  CHECK(windowsInputSyncHeldKeys(&fixture.input, NULL, 1, true) == 0);

  CHECK(fixture.count == 6);
  CHECK(fixture.trace[0].type  == TRACE_FOCUS);
  CHECK(fixture.trace[1].type  == TRACE_KEY);
  CHECK(fixture.trace[1].value == LG_KEY_LEFTSHIFT);
  CHECK(fixture.trace[2].type  == TRACE_KEY);
  CHECK(fixture.trace[2].value == LG_KEY_A);
  CHECK(fixture.trace[3].type  == TRACE_MODIFIERS);
  CHECK(fixture.trace[3].value == 2);
  CHECK(fixture.trace[4].type  == TRACE_LEDS);
  CHECK(fixture.trace[5].type  == TRACE_KEY);
  CHECK(fixture.trace[5].value == LG_KEY_A);
}

static void testHeldKeyModifierOrder(void)
{
  struct Fixture fixture;
  init(&fixture);

  const int keys[] =
  {
    LG_KEY_SYSRQ,
    LG_KEY_LEFTALT,
  };

  CHECK(windowsInputFocus(&fixture.input, true));
  CHECK(windowsInputSyncHeldKeys(&fixture.input,
      keys, sizeof(keys) / sizeof(*keys), false) == 2);
  CHECK(fixture.count == 3);
  CHECK(fixture.trace[1].type  == TRACE_KEY);
  CHECK(fixture.trace[1].value == LG_KEY_LEFTALT);
  CHECK(fixture.trace[2].type  == TRACE_KEY);
  CHECK(fixture.trace[2].value == LG_KEY_SYSRQ);
}

static void testKeyboardStateTransitions(void)
{
  struct Fixture fixture;
  init(&fixture);

  CHECK(windowsInputFocus(&fixture.input, true));
  windowsInputSetKeyboardGrabbed(&fixture.input, true);
  windowsInputState(&fixture.input, false, false, false, false,
      true, false, false);

  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_LEFTCTRL, true, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_RIGHTCTRL, true, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_LEFTCTRL, false, true));
  windowsInputTrackedState(&fixture.input);
  CHECK(fixture.trace[6].type  == TRACE_MODIFIERS);
  CHECK(fixture.trace[6].value == 1);

  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_RIGHTCTRL, false, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_CAPSLOCK, true, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_CAPSLOCK, true, true));
  windowsInputTrackedState(&fixture.input);
  CHECK(fixture.trace[12].type  == TRACE_LEDS);
  CHECK(fixture.trace[12].value == 3);

  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_CAPSLOCK, false, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_CAPSLOCK, true, true));
  windowsInputTrackedState(&fixture.input);
  CHECK(fixture.trace[16].type  == TRACE_LEDS);
  CHECK(fixture.trace[16].value == 1);

  CHECK(windowsInputFocus(&fixture.input, false));
  CHECK(windowsInputFocus(&fixture.input, true));
  windowsInputTrackedState(&fixture.input);
  CHECK(fixture.trace[19].type  == TRACE_MODIFIERS);
  CHECK(fixture.trace[19].value == 0);

  CHECK(fixture.input.numLock);
  CHECK(!fixture.input.capsLock);
  CHECK(!fixture.input.scrollLock);
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_NUMLOCK, true, true));
  CHECK(!fixture.input.numLock);
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_NUMLOCK, true, true));
  CHECK(!fixture.input.numLock);
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_NUMLOCK, false, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_NUMLOCK, true, true));
  CHECK(fixture.input.numLock);
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_SCROLLLOCK, true, true));
  CHECK(fixture.input.scrollLock);

  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_LEFTSHIFT, true, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_RIGHTALT, true, true));
  CHECK(windowsInputKeyCode(
      &fixture.input, LG_KEY_LEFTMETA, true, true));
  windowsInputTrackedState(&fixture.input);
  CHECK(fixture.count == 31);
  CHECK(fixture.trace[29].type  == TRACE_MODIFIERS);
  CHECK(fixture.trace[29].value == 14);
  CHECK(fixture.trace[30].type  == TRACE_LEDS);
  CHECK(fixture.trace[30].value == 5);
}

static void testPointerRouting(void)
{
  struct Fixture fixture;
  init(&fixture);

  CHECK(windowsInputPointerEnter(&fixture.input, 10.0, 20.0));
  windowsInputPointerMotion(&fixture.input, 12.0, 24.0);
  CHECK(windowsInputPointerButton(&fixture.input, 1, true));
  CHECK(windowsInputPointerButton(&fixture.input, 1, false));
  windowsInputPointerWheel(&fixture.input, 0.5);
  windowsInputPointerWheel(&fixture.input, 0.5);
  windowsInputSetPointerGrabbed(&fixture.input, true);
  windowsInputPointerMotion(&fixture.input, 14.0, 28.0);
  windowsInputRelativeMotion(&fixture.input, 2.0, -3.0);
  CHECK(!windowsInputPointerLeave(&fixture.input));
  windowsInputSetPointerGrabbed(&fixture.input, false);
  CHECK(!fixture.input.entered);

  CHECK(fixture.count == 11);
  CHECK(fixture.trace[0].type  == TRACE_ENTER);
  CHECK(fixture.trace[1].type  == TRACE_POSITION);
  CHECK(fixture.trace[2].type  == TRACE_POSITION);
  CHECK(fixture.trace[3].type  == TRACE_BUTTON);
  CHECK(fixture.trace[3].state == true);
  CHECK(fixture.trace[4].type  == TRACE_BUTTON);
  CHECK(fixture.trace[4].state == false);
  CHECK(fixture.trace[5].type  == TRACE_WHEEL);
  CHECK(fixture.trace[5].x     == -0.5);
  CHECK(fixture.trace[6].type  == TRACE_BUTTON);
  CHECK(fixture.trace[6].value == 4);
  CHECK(fixture.trace[6].state == true);
  CHECK(fixture.trace[7].type  == TRACE_BUTTON);
  CHECK(fixture.trace[7].value == 4);
  CHECK(fixture.trace[7].state == false);
  CHECK(fixture.trace[8].type  == TRACE_WHEEL);
  CHECK(fixture.trace[8].x     == -0.5);
  CHECK(fixture.trace[9].type  == TRACE_RELATIVE);
  CHECK(fixture.trace[10].type  == TRACE_ENTER);
  CHECK(fixture.trace[10].state == false);
}

static void testPointerLeaveAndCaptureLoss(void)
{
  struct Fixture fixture;
  init(&fixture);

  CHECK(windowsInputPointerEnter(&fixture.input, 1.0, 2.0));
  CHECK(windowsInputPointerButton(&fixture.input, 1, true));
  CHECK(windowsInputPointerButton(&fixture.input, 3, true));
  CHECK(!windowsInputPointerLeave(&fixture.input));
  CHECK(fixture.input.leavePending);
  windowsInputReleaseButtons(&fixture.input);
  CHECK(fixture.input.buttons == 0);
  CHECK(!fixture.input.entered);
  CHECK(!fixture.input.leavePending);

  CHECK(fixture.count == 7);
  CHECK(fixture.trace[4].type  == TRACE_BUTTON);
  CHECK(fixture.trace[4].value == 1);
  CHECK(fixture.trace[4].state == false);
  CHECK(fixture.trace[5].type  == TRACE_BUTTON);
  CHECK(fixture.trace[5].value == 3);
  CHECK(fixture.trace[5].state == false);
  CHECK(fixture.trace[6].type  == TRACE_ENTER);
  CHECK(fixture.trace[6].state == false);

  CHECK(windowsInputPointerEnter(&fixture.input, 3.0, 4.0));
  windowsInputSetPointerGrabbed(&fixture.input, true);
  CHECK(!windowsInputPointerLeave(&fixture.input));
  CHECK(fixture.input.leavePending);
  windowsInputSetPointerGrabbed(&fixture.input, false);
  CHECK(!fixture.input.entered);

  CHECK(windowsInputPointerEnter(&fixture.input, 5.0, 6.0));
  CHECK(windowsInputPointerButton(&fixture.input, 1, true));
  CHECK(!windowsInputPointerLeave(&fixture.input));
  CHECK(!windowsInputPointerEnter(&fixture.input, 7.0, 8.0));
  CHECK(!fixture.input.leavePending);
  CHECK(windowsInputPointerButton(&fixture.input, 1, false));
  CHECK(fixture.input.entered);
}

static size_t countTrace(const struct Fixture * fixture,
    enum TraceType type, int value, bool state)
{
  size_t count = 0;
  for (size_t i = 0; i < fixture->count; ++i)
  {
    const struct Trace * trace = &fixture->trace[i];
    if (trace->type == type && trace->value == value &&
        trace->state == state)
      ++count;
  }

  return count;
}

static void testPointerWheel(void)
{
  struct Fixture fixture;
  init(&fixture);

  CHECK(windowsInputPointerEnter(&fixture.input, 0.0, 0.0));
  windowsInputPointerWheel(&fixture.input, 2.25);
  windowsInputPointerWheel(&fixture.input, 0.75);
  windowsInputPointerWheel(&fixture.input, -2.5);
  windowsInputPointerWheel(&fixture.input, -0.5);

  CHECK(fixture.input.wheel == 0.0);
  CHECK(countTrace(&fixture, TRACE_BUTTON, 4, true ) == 3);
  CHECK(countTrace(&fixture, TRACE_BUTTON, 4, false) == 3);
  CHECK(countTrace(&fixture, TRACE_BUTTON, 5, true ) == 3);
  CHECK(countTrace(&fixture, TRACE_BUTTON, 5, false) == 3);
  CHECK(countTrace(&fixture, TRACE_WHEEL, 0, false) == 4);

  const size_t count    = fixture.count;
  const double nan      = doubleFromBits(UINT64_C(0x7FF8000000000000));
  const double infinity = doubleFromBits(UINT64_C(0x7FF0000000000000));
  windowsInputPointerWheel(&fixture.input, 0.0);
  windowsInputPointerWheel(&fixture.input, nan);
  windowsInputPointerWheel(&fixture.input, infinity);
  CHECK(fixture.count == count);
}

static void testInvalidPointerInput(void)
{
  struct Fixture fixture;
  init(&fixture);

  const double nan      = doubleFromBits(UINT64_C(0x7FF8000000000000));
  const double infinity = doubleFromBits(UINT64_C(0x7FF0000000000000));
  CHECK(!windowsInputPointerEnter(&fixture.input, nan, 0.0));
  CHECK(!windowsInputPointerEnter(&fixture.input, 0.0, infinity));
  CHECK(!windowsInputPointerButton(&fixture.input, 0, true));
  CHECK(!windowsInputPointerButton(&fixture.input, 32, true));
  CHECK(windowsInputPointerEnter(&fixture.input, 1.0, 1.0));
  CHECK(windowsInputPointerButton(&fixture.input, 1, true));
  CHECK(!windowsInputPointerButton(&fixture.input, 1, true));
  CHECK(windowsInputPointerButton(&fixture.input, 1, false));
  CHECK(!windowsInputPointerButton(&fixture.input, 1, false));

  const size_t count = fixture.count;
  windowsInputPointerMotion(&fixture.input, nan, 1.0);
  windowsInputPointerMotion(&fixture.input, 1.0, infinity);
  windowsInputRelativeMotion(&fixture.input, nan, 1.0);
  CHECK(fixture.count == count);

  windowsInputText(&fixture.input, NULL);
  windowsInputText(&fixture.input, "");
  CHECK(fixture.count == count);
}

static void testWindowSizePolicy(void)
{
  WindowsWindowSize state;
  windowsWindowSizeInit(&state, 800, 600);
  CHECK(state.width  == 800);
  CHECK(state.height == 600);
  CHECK(!state.pending);

  CHECK(!windowsWindowSizeRequest(&state, 1024, 768, true));
  CHECK(state.width  == 1024);
  CHECK(state.height == 768);
  CHECK(state.pending);

  int width  = -1;
  int height = -1;
  CHECK(!windowsWindowSizeTakePending(
      &state, true, &width, &height));
  CHECK(width  == -1);
  CHECK(height == -1);
  CHECK(state.pending);

  CHECK(!windowsWindowSizeRequest(&state, 1280, 720, true));
  CHECK(windowsWindowSizeTakePending(
      &state, false, &width, &height));
  CHECK(width  == 1280);
  CHECK(height == 720);
  CHECK(!state.pending);

  CHECK(windowsWindowSizeRequest(&state, 640, 480, false));
  CHECK(!state.pending);
  CHECK(!windowsWindowSizeTakePending(
      &state, false, &width, &height));
}

int main(void)
{
  testScanCodes();
  testKeyboardRouting();
  testPrintScreenLatch();
  testHeldKeys();
  testHeldKeyModifierOrder();
  testKeyboardStateTransitions();
  testPointerRouting();
  testPointerLeaveAndCaptureLoss();
  testPointerWheel();
  testInvalidPointerInput();
  testWindowSizePolicy();
  return 0;
}
