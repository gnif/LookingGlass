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

#include "Wayland/input_event.h"
#include "X11/input_event.h"
#include "test.h"

#include <linux/input-event-codes.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum Backend
{
  BACKEND_WAYLAND,
  BACKEND_X11,
};

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
  union
  {
    struct
    {
      double x;
      double y;
      double rawX;
      double rawY;
    }
    motion;

    struct
    {
      unsigned int value;
      bool         pressed;
    }
    input;

    struct
    {
      bool ctrl;
      bool shift;
      bool alt;
      bool super;
    }
    modifiers;

    struct
    {
      bool num;
      bool caps;
      bool scroll;
    }
    leds;

    double wheel;
    bool   state;
    char   text[32];
  } data;
};

#define POSITION(_x, _y) \
  { \
    .type = TRACE_POSITION, \
    .data.motion = { .x = (_x), .y = (_y) }, \
  }

#define RELATIVE(_x, _y, _rawX, _rawY) \
  { \
    .type = TRACE_RELATIVE, \
    .data.motion = \
      { .x = (_x), .y = (_y), .rawX = (_rawX), .rawY = (_rawY) }, \
  }

#define BUTTON(_button, _pressed) \
  { \
    .type = TRACE_BUTTON, \
    .data.input = { .value = (_button), .pressed = (_pressed) }, \
  }

#define WHEEL(_motion) \
  { \
    .type = TRACE_WHEEL, \
    .data.wheel = (_motion), \
  }

#define KEY(_key, _pressed) \
  { \
    .type = TRACE_KEY, \
    .data.input = { .value = (_key), .pressed = (_pressed) }, \
  }

#define TEXT(_text) \
  { \
    .type = TRACE_TEXT, \
    .data.text = _text, \
  }

#define MODIFIERS(_ctrl, _shift, _alt, _super) \
  { \
    .type = TRACE_MODIFIERS, \
    .data.modifiers = \
      { .ctrl = (_ctrl), .shift = (_shift), .alt = (_alt), \
        .super = (_super) }, \
  }

#define LEDS(_num, _caps, _scroll) \
  { \
    .type = TRACE_LEDS, \
    .data.leds = { .num = (_num), .caps = (_caps), .scroll = (_scroll) }, \
  }

#define ENTER(_entered) \
  { \
    .type = TRACE_ENTER, \
    .data.state = (_entered), \
  }

#define FOCUS(_focused) \
  { \
    .type = TRACE_FOCUS, \
    .data.state = (_focused), \
  }

#define ARRAY_LENGTH(_array) (sizeof(_array) / sizeof((_array)[0]))
#define MAX_TRACE 128

struct TraceLog
{
  struct Trace values[MAX_TRACE];
  size_t       count;
};

struct Fixture
{
  enum Backend backend;
  struct TraceLog trace;

  union
  {
    WaylandInput wayland;
    X11Input     x11;
  } input;
};

static struct Trace * pushTrace(struct TraceLog * log, enum TraceType type)
{
  CHECK(log->count < ARRAY_LENGTH(log->values));
  struct Trace * trace = &log->values[log->count++];
  memset(trace, 0, sizeof(*trace));
  trace->type = type;
  return trace;
}

static void tracePosition(void * opaque, double x, double y)
{
  struct Trace * trace = pushTrace(opaque, TRACE_POSITION);
  trace->data.motion.x = x;
  trace->data.motion.y = y;
}

static void traceRelative(void * opaque, double x, double y,
    double rawX, double rawY)
{
  struct Trace * trace = pushTrace(opaque, TRACE_RELATIVE);
  trace->data.motion.x    = x;
  trace->data.motion.y    = y;
  trace->data.motion.rawX = rawX;
  trace->data.motion.rawY = rawY;
}

static void traceButton(void * opaque, unsigned int button, bool pressed)
{
  struct Trace * trace = pushTrace(opaque, TRACE_BUTTON);
  trace->data.input.value   = button;
  trace->data.input.pressed = pressed;
}

static void traceWheel(void * opaque, double motion)
{
  struct Trace * trace = pushTrace(opaque, TRACE_WHEEL);
  trace->data.wheel = motion;
}

static void traceKey(void * opaque, int scancode, bool pressed)
{
  struct Trace * trace = pushTrace(opaque, TRACE_KEY);
  trace->data.input.value   = scancode;
  trace->data.input.pressed = pressed;
}

static void traceText(void * opaque, const char * text)
{
  struct Trace * trace = pushTrace(opaque, TRACE_TEXT);
  snprintf(trace->data.text, sizeof(trace->data.text), "%s", text);
}

static void traceModifiers(void * opaque, bool ctrl, bool shift,
    bool alt, bool super)
{
  struct Trace * trace = pushTrace(opaque, TRACE_MODIFIERS);
  trace->data.modifiers.ctrl  = ctrl;
  trace->data.modifiers.shift = shift;
  trace->data.modifiers.alt   = alt;
  trace->data.modifiers.super = super;
}

static void traceLEDs(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  struct Trace * trace = pushTrace(opaque, TRACE_LEDS);
  trace->data.leds.num    = numLock;
  trace->data.leds.caps   = capsLock;
  trace->data.leds.scroll = scrollLock;
}

static void traceEnter(void * opaque, bool entered)
{
  struct Trace * trace = pushTrace(opaque, TRACE_ENTER);
  trace->data.state = entered;
}

static void traceFocus(void * opaque, bool focused)
{
  struct Trace * trace = pushTrace(opaque, TRACE_FOCUS);
  trace->data.state = focused;
}

static const LG_DSInputSink sink =
{
  .position  = tracePosition,
  .relative  = traceRelative,
  .button    = traceButton,
  .wheel     = traceWheel,
  .key       = traceKey,
  .text      = traceText,
  .modifiers = traceModifiers,
  .leds      = traceLEDs,
  .enter     = traceEnter,
  .focus     = traceFocus,
};

static void initFixture(struct Fixture * fixture, enum Backend backend)
{
  memset(fixture, 0, sizeof(*fixture));
  fixture->backend = backend;

  if (backend == BACKEND_WAYLAND)
    wlInputInit(&fixture->input.wayland, &sink, &fixture->trace);
  else
    x11InputInit(&fixture->input.x11, &sink, &fixture->trace);
}

static void clearTrace(struct Fixture * fixture)
{
  fixture->trace.count = 0;
}

static bool sameDouble(double a, double b)
{
  const double delta = a > b ? a - b : b - a;
  return delta < 0.000000001;
}

static bool sameTrace(const struct Trace * actual,
    const struct Trace * expected)
{
  if (actual->type != expected->type)
    return false;

  switch (actual->type)
  {
    case TRACE_POSITION:
      return
        sameDouble(actual->data.motion.x, expected->data.motion.x) &&
        sameDouble(actual->data.motion.y, expected->data.motion.y);

    case TRACE_RELATIVE:
      return
        sameDouble(actual->data.motion.x, expected->data.motion.x) &&
        sameDouble(actual->data.motion.y, expected->data.motion.y) &&
        sameDouble(actual->data.motion.rawX, expected->data.motion.rawX) &&
        sameDouble(actual->data.motion.rawY, expected->data.motion.rawY);

    case TRACE_BUTTON:
    case TRACE_KEY:
      return
        actual->data.input.value == expected->data.input.value &&
        actual->data.input.pressed == expected->data.input.pressed;

    case TRACE_WHEEL:
      return sameDouble(actual->data.wheel, expected->data.wheel);

    case TRACE_TEXT:
      return strcmp(actual->data.text, expected->data.text) == 0;

    case TRACE_MODIFIERS:
      return
        actual->data.modifiers.ctrl == expected->data.modifiers.ctrl &&
        actual->data.modifiers.shift == expected->data.modifiers.shift &&
        actual->data.modifiers.alt == expected->data.modifiers.alt &&
        actual->data.modifiers.super == expected->data.modifiers.super;

    case TRACE_LEDS:
      return
        actual->data.leds.num == expected->data.leds.num &&
        actual->data.leds.caps == expected->data.leds.caps &&
        actual->data.leds.scroll == expected->data.leds.scroll;

    case TRACE_ENTER:
    case TRACE_FOCUS:
      return actual->data.state == expected->data.state;
  }

  return false;
}

static const char * traceName(enum TraceType type)
{
  static const char * names[] =
  {
    [TRACE_POSITION ] = "position",
    [TRACE_RELATIVE ] = "relative",
    [TRACE_BUTTON   ] = "button",
    [TRACE_WHEEL    ] = "wheel",
    [TRACE_KEY      ] = "key",
    [TRACE_TEXT     ] = "text",
    [TRACE_MODIFIERS] = "modifiers",
    [TRACE_LEDS     ] = "leds",
    [TRACE_ENTER    ] = "enter",
    [TRACE_FOCUS    ] = "focus",
  };
  return names[type];
}

static void printTrace(const struct Trace * trace)
{
  fprintf(stderr, "%s", traceName(trace->type));
  switch (trace->type)
  {
    case TRACE_POSITION:
      fprintf(stderr, " %.6f %.6f", trace->data.motion.x,
          trace->data.motion.y);
      break;

    case TRACE_RELATIVE:
      fprintf(stderr, " %.6f %.6f %.6f %.6f", trace->data.motion.x,
          trace->data.motion.y, trace->data.motion.rawX,
          trace->data.motion.rawY);
      break;

    case TRACE_BUTTON:
    case TRACE_KEY:
      fprintf(stderr, " %u %s", trace->data.input.value,
          trace->data.input.pressed ? "down" : "up");
      break;

    case TRACE_WHEEL:
      fprintf(stderr, " %.6f", trace->data.wheel);
      break;

    case TRACE_TEXT:
      fprintf(stderr, " %s", trace->data.text);
      break;

    case TRACE_MODIFIERS:
      fprintf(stderr, " %d %d %d %d", trace->data.modifiers.ctrl,
          trace->data.modifiers.shift, trace->data.modifiers.alt,
          trace->data.modifiers.super);
      break;

    case TRACE_LEDS:
      fprintf(stderr, " %d %d %d", trace->data.leds.num,
          trace->data.leds.caps, trace->data.leds.scroll);
      break;

    case TRACE_ENTER:
    case TRACE_FOCUS:
      fprintf(stderr, " %d", trace->data.state);
      break;
  }
  fputc('\n', stderr);
}

static void expectTrace(const struct Fixture * fixture,
    const struct Trace * expected, size_t expectedCount)
{
  bool match = fixture->trace.count == expectedCount;
  if (match)
    for (size_t i = 0; i < expectedCount; ++i)
      if (!sameTrace(&fixture->trace.values[i], &expected[i]))
      {
        match = false;
        break;
      }

  if (match)
    return;

  fprintf(stderr, "expected trace (%zu events):\n", expectedCount);
  for (size_t i = 0; i < expectedCount; ++i)
    printTrace(&expected[i]);

  fprintf(stderr, "actual trace (%zu events):\n", fixture->trace.count);
  for (size_t i = 0; i < fixture->trace.count; ++i)
    printTrace(&fixture->trace.values[i]);

  exit(EXIT_FAILURE);
}

static void pointerEnter(struct Fixture * fixture, bool mainSurface,
    double x, double y)
{
  if (fixture->backend == BACKEND_WAYLAND)
  {
    if (wlInputPointerEnter(&fixture->input.wayland, mainSurface))
      wlInputPointerMotion(&fixture->input.wayland, x, y);
  }
  else
    x11InputPointerEnter(&fixture->input.x11, mainSurface, true, x, y);
}

static void pointerLeave(struct Fixture * fixture, bool mainSurface)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputPointerLeave(&fixture->input.wayland, mainSurface);
  else
    x11InputPointerLeave(&fixture->input.x11, mainSurface, true, false);
}

static void pointerMotion(struct Fixture * fixture, double x, double y)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputPointerMotion(&fixture->input.wayland, x, y);
  else
    x11InputPointerMotion(&fixture->input.x11, x, y);
}

static unsigned int nativeButton(enum Backend backend,
    unsigned int button)
{
  if (backend == BACKEND_X11)
    return button > 5 ? button + 2 : button;

  static const unsigned int wayland[] =
  {
    [1 ] = BTN_LEFT,
    [2 ] = BTN_MIDDLE,
    [3 ] = BTN_RIGHT,
    [6 ] = BTN_SIDE,
    [7 ] = BTN_EXTRA,
    [8 ] = BTN_FORWARD,
    [9 ] = BTN_BACK,
    [10] = BTN_TASK,
  };
  CHECK(button < ARRAY_LENGTH(wayland));
  return wayland[button];
}

static void pointerButton(struct Fixture * fixture, unsigned int button,
    bool pressed)
{
  const unsigned int native = nativeButton(fixture->backend, button);
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputPointerButton(&fixture->input.wayland, native, pressed);
  else
    x11InputPointerButton(&fixture->input.x11, native, pressed, false);
}

static void unmappedButton(struct Fixture * fixture)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputPointerButton(&fixture->input.wayland, BTN_STYLUS, true);
  else
    x11InputPointerButton(&fixture->input.x11, 6, true, false);
}

static void wheelStep(struct Fixture * fixture, bool down)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputPointerAxis(&fixture->input.wayland, true, down ? 15.0 : -15.0);
  else
    x11InputPointerButton(&fixture->input.x11, down ? 5 : 4, true,
        false);
}

static void relativeMotion(struct Fixture * fixture, bool active,
    double x, double y, double rawX, double rawY)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputRelativeMotion(&fixture->input.wayland, active,
        x, y, rawX, rawY);
  else if (active)
    x11InputRelativeMotion(&fixture->input.x11, x, y, rawX, rawY);
}

static void keyboardEnter(struct Fixture * fixture, bool mainSurface,
    const uint32_t * keys, size_t count)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputKeyboardEnter(&fixture->input.wayland, mainSurface, keys, count);
  else if (mainSurface)
    x11InputFocus(&fixture->input.x11, true, keys, count);
}

static void keyboardLeave(struct Fixture * fixture, bool mainSurface)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputKeyboardLeave(&fixture->input.wayland, mainSurface);
  else if (mainSurface)
    x11InputFocus(&fixture->input.x11, false, NULL, 0);
}

static void keyboardKey(struct Fixture * fixture, unsigned int key,
    bool pressed, const char * text, bool inputActive)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputKeyboardKey(&fixture->input.wayland, key, pressed, text);
  else
    x11InputKeyboardKey(&fixture->input.x11, key + 8, 8, pressed,
        false, inputActive, text);
}

static void keyboardState(struct Fixture * fixture,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock)
{
  if (fixture->backend == BACKEND_WAYLAND)
    wlInputKeyboardState(&fixture->input.wayland,
        ctrl, shift, alt, super, numLock, capsLock, scrollLock);
  else
    x11InputKeyboardState(&fixture->input.x11,
        ctrl, shift, alt, super, numLock, capsLock, scrollLock);
}

static void activateInput(struct Fixture * fixture)
{
  pointerEnter(fixture, true, 40.0, 30.0);
  keyboardEnter(fixture, true, NULL, 0);
  clearTrace(fixture);
}

static void activatePointer(struct Fixture * fixture)
{
  pointerEnter(fixture, true, 40.0, 30.0);
  clearTrace(fixture);
}

static void testPointerEvents(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);

  pointerEnter(&fixture, false, 1.0, 2.0);
  pointerEnter(&fixture, true, 10.0, 20.0);
  pointerMotion(&fixture, 11.5, 21.25);
  pointerLeave(&fixture, false);
  pointerLeave(&fixture, true);

  static const struct Trace expected[] =
  {
    ENTER(true),
    POSITION(10.0, 20.0),
    POSITION(11.5, 21.25),
    ENTER(false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testPointerButtons(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  activatePointer(&fixture);

  static const unsigned int buttons[] = { 1, 2, 3, 6, 7, 8, 9, 10 };
  for (size_t i = 0; i < ARRAY_LENGTH(buttons); ++i)
  {
    pointerButton(&fixture, buttons[i], true);
    pointerButton(&fixture, buttons[i], false);
  }
  unmappedButton(&fixture);

  static const struct Trace expected[] =
  {
    BUTTON(1 , true), BUTTON(1 , false),
    BUTTON(2 , true), BUTTON(2 , false),
    BUTTON(3 , true), BUTTON(3 , false),
    BUTTON(6 , true), BUTTON(6 , false),
    BUTTON(7 , true), BUTTON(7 , false),
    BUTTON(8 , true), BUTTON(8 , false),
    BUTTON(9 , true), BUTTON(9 , false),
    BUTTON(10, true), BUTTON(10, false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testPointerRelease(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  activateInput(&fixture);

  pointerButton(&fixture, 1, true);

  /* Wayland's implicit pointer grab suppresses this native leave. */
  if (backend == BACKEND_X11)
    pointerLeave(&fixture, true);

  pointerButton(&fixture, 1, false);

  static const struct Trace expected[] =
  {
    BUTTON(1, true),
    BUTTON(1, false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testWheelSteps(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  activateInput(&fixture);

  wheelStep(&fixture, true);
  wheelStep(&fixture, false);

  static const struct Trace expected[] =
  {
    BUTTON(5, true),
    BUTTON(5, false),
    WHEEL(1.0),
    BUTTON(4, true),
    BUTTON(4, false),
    WHEEL(-1.0),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testWheelResidual(enum Backend backend)
{
  CHECK(backend == BACKEND_WAYLAND);
  struct Fixture fixture;
  initFixture(&fixture, backend);

  wlInputPointerAxis(&fixture.input.wayland, false, 100.0);
  wlInputPointerAxis(&fixture.input.wayland, true, 5.0);
  wlInputPointerAxis(&fixture.input.wayland, true, 5.0);
  wlInputPointerAxis(&fixture.input.wayland, true, -2.0);
  wlInputPointerAxis(&fixture.input.wayland, true, -1.0);

  static const struct Trace expected[] =
  {
    WHEEL(1.0 / 3.0),
    BUTTON(5, true),
    BUTTON(5, false),
    WHEEL(1.0 / 3.0),
    WHEEL(-2.0 / 15.0),
    BUTTON(4, true),
    BUTTON(4, false),
    WHEEL(-1.0 / 15.0),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testWheelMultiple(enum Backend backend)
{
  CHECK(backend == BACKEND_WAYLAND);
  struct Fixture fixture;
  initFixture(&fixture, backend);

  wlInputPointerAxis(&fixture.input.wayland, true, 45.0);

  static const struct Trace expected[] =
  {
    BUTTON(5, true), BUTTON(5, false),
    BUTTON(5, true), BUTTON(5, false),
    BUTTON(5, true), BUTTON(5, false),
    WHEEL(3.0),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testRelativeMotion(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  activateInput(&fixture);

  relativeMotion(&fixture, false, 1.0, 2.0, 3.0, 4.0);
  relativeMotion(&fixture, true, 1.0, 2.0, 3.0, 4.0);
  relativeMotion(&fixture, true, 1.0, 2.0, 3.0, 4.0);

  static const struct Trace expected[] =
  {
    RELATIVE(1.0, 2.0, 3.0, 4.0),
    RELATIVE(1.0, 2.0, 3.0, 4.0),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testKeyboardFocus(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  static const uint32_t held[] = { 30, 42 };

  keyboardEnter(&fixture, false, held, ARRAY_LENGTH(held));
  keyboardEnter(&fixture, true, held, ARRAY_LENGTH(held));
  keyboardLeave(&fixture, false);
  keyboardLeave(&fixture, true);

  static const struct Trace expected[] =
  {
    FOCUS(true),
    KEY(30, true),
    KEY(42, true),
    FOCUS(false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

static void testKeyboardKeys(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);

  keyboardKey(&fixture, 30, true, "ignored", true);
  keyboardKey(&fixture, 30, false, "ignored", true);
  CHECK(fixture.trace.count == 0);

  keyboardEnter(&fixture, true, NULL, 0);
  clearTrace(&fixture);

  keyboardKey(&fixture, 30, true, "a", true);
  keyboardKey(&fixture, 30, false, "ignored", true);

  static const struct Trace expected[] =
  {
    KEY(30, true),
    TEXT("a"),
    KEY(30, false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));

  keyboardLeave(&fixture, true);
  clearTrace(&fixture);
  keyboardKey(&fixture, 31, true, "s", true);
  keyboardKey(&fixture, 31, false, NULL, true);
  CHECK(fixture.trace.count == 0);
}

static void testKeyboardState(enum Backend backend)
{
  struct Fixture fixture;
  initFixture(&fixture, backend);
  keyboardEnter(&fixture, true, NULL, 0);
  clearTrace(&fixture);

  keyboardState(&fixture, true, false, true, false,
      true, false, true);
  keyboardState(&fixture, false, true, false, true,
      false, true, false);

  static const struct Trace expected[] =
  {
    MODIFIERS(true, false, true, false),
    LEDS(true, false, true),
    MODIFIERS(false, true, false, true),
    LEDS(false, true, false),
  };
  expectTrace(&fixture, expected, ARRAY_LENGTH(expected));
}

struct Test
{
  const char * name;
  void (*run)(enum Backend backend);
};

static const struct Test tests[] =
{
  { "pointer-events" , testPointerEvents  },
  { "pointer-buttons", testPointerButtons },
  { "pointer-release", testPointerRelease },
  { "wheel-steps"    , testWheelSteps     },
  { "wheel-residual" , testWheelResidual  },
  { "wheel-multiple" , testWheelMultiple  },
  { "relative-motion", testRelativeMotion },
  { "keyboard-focus" , testKeyboardFocus  },
  { "keyboard-keys"  , testKeyboardKeys   },
  { "keyboard-state" , testKeyboardState  },
};

static bool parseBackend(const char * name, enum Backend * backend)
{
  if (strcmp(name, "wayland") == 0)
  {
    *backend = BACKEND_WAYLAND;
    return true;
  }

  if (strcmp(name, "x11") == 0)
  {
    *backend = BACKEND_X11;
    return true;
  }

  return false;
}

int main(int argc, char ** argv)
{
  if (argc != 3)
  {
    fprintf(stderr, "usage: %s <wayland|x11> <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  enum Backend backend;
  if (!parseBackend(argv[1], &backend))
  {
    fprintf(stderr, "unknown backend: %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
    if (strcmp(argv[2], tests[i].name) == 0)
    {
      tests[i].run(backend);
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[2]);
  return EXIT_FAILURE;
}
