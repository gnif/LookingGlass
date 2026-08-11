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

#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_RETIRED_LISTENERS 8U

typedef struct FakeCalls
{
  unsigned supports;
  unsigned listenerSet;
  unsigned listenerClear;
  unsigned keyDown;
  unsigned keyUp;
  unsigned motion;
  unsigned position;
  unsigned press;
  unsigned release;
  unsigned reset;
  unsigned dead;
  unsigned fault;
}
FakeCalls;

typedef struct RetiredListener
{
  LG_InputStatusFn callback;
  void           * opaque;
}
RetiredListener;

typedef struct FakeInput
{
  bool             alive;
  bool             available;
  bool             absolute;
  bool             keyDownResult;
  bool             keyUpResult;
  bool             motionResult;
  bool             positionResult;
  bool             pressResult;
  bool             releaseResult;
  uint32_t         generation;
  LG_InputStatusFn listener;
  void           * listenerOpaque;
  RetiredListener  retired[TEST_RETIRED_LISTENERS];
  unsigned         retiredCount;
  FakeCalls        calls;
  bool             remoteKeys[KEY_MAX];
  uint32_t         remoteButtons;
  int              lastKeyDown;
  int              lastKeyUp;
  int32_t          lastMotionX;
  int32_t          lastMotionY;
  uint32_t         lastPositionX;
  uint32_t         lastPositionY;
  uint32_t         lastPositionWidth;
  uint32_t         lastPositionHeight;
  unsigned         lastPress;
  unsigned         lastRelease;
}
FakeInput;

typedef struct TestState
{
  FakeInput fallback;
  FakeInput transport;
  FakeInput replacement;
}
TestState;

#define CHECK(x) \
  do \
  { \
    if (!(x)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", \
          __FILE__, __LINE__, #x); \
      return false; \
    } \
  } \
  while (0)

static bool fakeAlive(FakeInput * input)
{
  if (input->alive)
    return true;

  ++input->calls.dead;
  return false;
}

static bool fakeSupports(void * opaque, LG_InputSupport support)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;

  ++input->calls.supports;
  return support == LG_INPUT_SUPPORT_MOUSE_ABSOLUTE && input->absolute;
}

static void fakeSetStatusListener(void * opaque,
    LG_InputStatusFn callback, void * callbackOpaque)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return;

  if (!callback)
  {
    ++input->calls.listenerClear;
    if (input->listener)
    {
      if (input->retiredCount < TEST_RETIRED_LISTENERS)
      {
        RetiredListener * retired =
          &input->retired[input->retiredCount++];
        retired->callback = input->listener;
        retired->opaque   = input->listenerOpaque;
      }
      else
        ++input->calls.fault;
    }
    input->listener       = NULL;
    input->listenerOpaque = NULL;
    return;
  }

  ++input->calls.listenerSet;
  input->listener       = callback;
  input->listenerOpaque = callbackOpaque;
  const LG_InputStatus status =
  {
    .available  = input->available,
    .generation = input->generation,
  };
  callback(callbackOpaque, &status);
}

static bool fakeKeyDown(void * opaque, int key)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;
  if (key < 0 || key >= KEY_MAX)
  {
    ++input->calls.fault;
    return false;
  }

  ++input->calls.keyDown;
  input->lastKeyDown = key;
  if (!input->keyDownResult)
    return false;
  input->remoteKeys[key] = true;
  return true;
}

static bool fakeKeyUp(void * opaque, int key)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;
  if (key < 0 || key >= KEY_MAX)
  {
    ++input->calls.fault;
    return false;
  }

  ++input->calls.keyUp;
  input->lastKeyUp = key;
  if (!input->keyUpResult)
    return false;
  input->remoteKeys[key] = false;
  return true;
}

static bool fakeMouseMotion(void * opaque, int32_t x, int32_t y)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;

  ++input->calls.motion;
  input->lastMotionX = x;
  input->lastMotionY = y;
  return input->motionResult;
}

static bool fakeMousePosition(void * opaque, uint32_t x, uint32_t y,
    uint32_t width, uint32_t height)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;

  ++input->calls.position;
  input->lastPositionX      = x;
  input->lastPositionY      = y;
  input->lastPositionWidth  = width;
  input->lastPositionHeight = height;
  return input->positionResult;
}

static bool fakeMousePress(void * opaque, unsigned int button)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;
  if (button == 0 || button >= 32)
  {
    ++input->calls.fault;
    return false;
  }

  ++input->calls.press;
  input->lastPress = button;
  if (!input->pressResult)
    return false;
  input->remoteButtons |= UINT32_C(1) << button;
  return true;
}

static bool fakeMouseRelease(void * opaque, unsigned int button)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return false;
  if (button == 0 || button >= 32)
  {
    ++input->calls.fault;
    return false;
  }

  ++input->calls.release;
  input->lastRelease = button;
  if (!input->releaseResult)
    return false;
  input->remoteButtons &= ~(UINT32_C(1) << button);
  return true;
}

static void fakeReset(void * opaque)
{
  FakeInput * input = opaque;
  if (!fakeAlive(input))
    return;

  ++input->calls.reset;
  memset(input->remoteKeys, 0, sizeof(input->remoteKeys));
  input->remoteButtons = 0;
}

static const LG_InputOps inputOps =
{
  .name              = "test-input",
  .supports          = fakeSupports,
  .setStatusListener = fakeSetStatusListener,
  .keyDown           = fakeKeyDown,
  .keyUp             = fakeKeyUp,
  .mouseMotion       = fakeMouseMotion,
  .mousePosition     = fakeMousePosition,
  .mousePress        = fakeMousePress,
  .mouseRelease      = fakeMouseRelease,
  .reset             = fakeReset,
};

static const LG_InputOps noPositionOps =
{
  .name              = "test-no-position",
  .supports          = fakeSupports,
  .setStatusListener = fakeSetStatusListener,
  .keyDown           = fakeKeyDown,
  .keyUp             = fakeKeyUp,
  .mouseMotion       = fakeMouseMotion,
  .mousePress        = fakeMousePress,
  .mouseRelease      = fakeMouseRelease,
  .reset             = fakeReset,
};

static void fakeInit(FakeInput * input)
{
  memset(input, 0, sizeof(*input));
  input->alive          = true;
  input->available      = true;
  input->generation     = 1;
  input->keyDownResult  = true;
  input->keyUpResult    = true;
  input->motionResult   = true;
  input->positionResult = true;
  input->pressResult    = true;
  input->releaseResult  = true;
}

static void stateInit(TestState * state)
{
  fakeInit(&state->fallback);
  fakeInit(&state->transport);
  fakeInit(&state->replacement);
}

static bool setAvailable(FakeInput * input, bool available)
{
  CHECK(input->listener);
  input->available = available;
  if (++input->generation == 0)
    ++input->generation;

  const LG_InputStatus status =
  {
    .available  = available,
    .generation = input->generation,
  };
  input->listener(input->listenerOpaque, &status);
  return true;
}

static bool sendRetired(FakeInput * input, unsigned index, bool available)
{
  CHECK(index < input->retiredCount);
  CHECK(input->retired[index].callback);

  const LG_InputStatus status =
  {
    .available  = available,
    .generation = input->generation + 100,
  };
  input->retired[index].callback(input->retired[index].opaque, &status);
  return true;
}

static bool bindBoth(TestState * state)
{
  lgInput_setFallback(&inputOps, &state->fallback);
  CHECK(lgInput_available());
  lgInput_setTransport(&inputOps, &state->transport);
  CHECK(lgInput_available());
  CHECK(state->fallback.calls.listenerSet == 1);
  CHECK(state->transport.calls.listenerSet == 1);
  return true;
}

static bool testPreference(TestState * state)
{
  CHECK(bindBoth(state));

  CHECK(lgInput_mouseMotion(1, 2));
  CHECK(state->transport.calls.motion == 1);
  CHECK(state->fallback.calls.motion == 0);

  lgInput_useTransport(false);
  CHECK(lgInput_mouseMotion(3, 4));
  CHECK(state->fallback.calls.motion == 1);
  CHECK(state->transport.calls.motion == 1);

  lgInput_useTransport(true);
  CHECK(lgInput_mouseMotion(5, 6));
  CHECK(state->transport.calls.motion == 2);

  CHECK(setAvailable(&state->transport, false));
  CHECK(lgInput_available());
  CHECK(lgInput_mouseMotion(7, 8));
  CHECK(state->fallback.calls.motion == 2);

  CHECK(setAvailable(&state->transport, true));
  CHECK(lgInput_mouseMotion(9, 10));
  CHECK(state->transport.calls.motion == 3);

  CHECK(setAvailable(&state->fallback, false));
  CHECK(lgInput_available());
  CHECK(setAvailable(&state->transport, false));
  CHECK(!lgInput_available());
  CHECK(!lgInput_mouseMotion(11, 12));
  CHECK(state->fallback.calls.motion == 2);
  CHECK(state->transport.calls.motion == 3);

  CHECK(setAvailable(&state->fallback, true));
  CHECK(lgInput_available());
  CHECK(lgInput_mouseMotion(13, 14));
  CHECK(state->fallback.calls.motion == 3);
  return true;
}

static bool testHandoff(TestState * state)
{
  CHECK(bindBoth(state));

  CHECK(lgInput_keyDown(KEY_A));
  CHECK(lgInput_mousePress(1));
  CHECK(state->transport.remoteKeys[KEY_A]);
  CHECK(state->transport.remoteButtons & (UINT32_C(1) << 1));

  const unsigned transportKeyUp   = state->transport.calls.keyUp;
  const unsigned transportRelease = state->transport.calls.release;
  const unsigned transportReset   = state->transport.calls.reset;
  const unsigned fallbackReset    = state->fallback.calls.reset;
  CHECK(setAvailable(&state->transport, false));
  CHECK(state->transport.calls.keyUp == transportKeyUp + 1);
  CHECK(state->transport.lastKeyUp == KEY_A);
  CHECK(state->transport.calls.release == transportRelease + 1);
  CHECK(state->transport.lastRelease == 1);
  CHECK(state->transport.calls.reset == transportReset + 1);
  CHECK(state->fallback.calls.reset == fallbackReset + 1);
  CHECK(!state->transport.remoteKeys[KEY_A]);
  CHECK(state->transport.remoteButtons == 0);

  const unsigned fallbackKeyUp   = state->fallback.calls.keyUp;
  const unsigned fallbackRelease = state->fallback.calls.release;
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(lgInput_mouseRelease(1));
  CHECK(state->fallback.calls.keyUp == fallbackKeyUp);
  CHECK(state->fallback.calls.release == fallbackRelease);

  CHECK(lgInput_keyDown(KEY_B));
  CHECK(lgInput_mousePress(2));
  CHECK(state->fallback.remoteKeys[KEY_B]);
  CHECK(setAvailable(&state->transport, true));
  CHECK(state->fallback.calls.keyUp == fallbackKeyUp + 1);
  CHECK(state->fallback.lastKeyUp == KEY_B);
  CHECK(state->fallback.calls.release == fallbackRelease + 1);
  CHECK(state->fallback.lastRelease == 2);
  CHECK(!state->fallback.remoteKeys[KEY_B]);
  CHECK(state->fallback.remoteButtons == 0);

  CHECK(lgInput_keyDown(KEY_C));
  CHECK(lgInput_mousePress(3));
  const unsigned secondTransportKeyUp   = state->transport.calls.keyUp;
  const unsigned secondTransportRelease = state->transport.calls.release;
  lgInput_useTransport(false);
  CHECK(state->transport.calls.keyUp == secondTransportKeyUp + 1);
  CHECK(state->transport.lastKeyUp == KEY_C);
  CHECK(state->transport.calls.release == secondTransportRelease + 1);
  CHECK(state->transport.lastRelease == 3);
  CHECK(!state->transport.remoteKeys[KEY_C]);
  CHECK(state->transport.remoteButtons == 0);
  return true;
}

static bool testStale(TestState * state)
{
  CHECK(bindBoth(state));
  lgInput_setTransport(&inputOps, &state->transport);
  CHECK(state->transport.retiredCount == 1);
  CHECK(state->transport.calls.listenerSet == 2);
  CHECK(state->transport.calls.listenerClear == 1);

  const unsigned fallbackMotion  = state->fallback.calls.motion;
  const unsigned transportMotion = state->transport.calls.motion;
  CHECK(sendRetired(&state->transport, 0, false));
  CHECK(lgInput_available());
  CHECK(lgInput_mouseMotion(1, 1));
  CHECK(state->transport.calls.motion == transportMotion + 1);
  CHECK(state->fallback.calls.motion == fallbackMotion);

  CHECK(setAvailable(&state->transport, false));
  CHECK(sendRetired(&state->transport, 0, true));
  CHECK(lgInput_mouseMotion(2, 2));
  CHECK(state->fallback.calls.motion == fallbackMotion + 1);
  CHECK(state->transport.calls.motion == transportMotion + 1);

  CHECK(setAvailable(&state->transport, true));
  CHECK(lgInput_mouseMotion(3, 3));
  CHECK(state->transport.calls.motion == transportMotion + 2);
  return true;
}

static bool testDeadDrop(TestState * state)
{
  CHECK(bindBoth(state));
  CHECK(lgInput_keyDown(KEY_A));
  CHECK(lgInput_mousePress(1));

  LG_InputStatusFn stale         = state->transport.listener;
  void           * staleOpaque   = state->transport.listenerOpaque;
  const unsigned   keyUp         = state->transport.calls.keyUp;
  const unsigned   release       = state->transport.calls.release;
  const unsigned   reset         = state->transport.calls.reset;
  const unsigned   listenerClear = state->transport.calls.listenerClear;
  state->transport.alive = false;
  lgInput_dropTransport();

  CHECK(state->transport.calls.keyUp == keyUp);
  CHECK(state->transport.calls.release == release);
  CHECK(state->transport.calls.reset == reset);
  CHECK(state->transport.calls.listenerClear == listenerClear);
  CHECK(state->transport.calls.dead == 0);
  CHECK(state->transport.remoteKeys[KEY_A]);
  CHECK(state->transport.remoteButtons & (UINT32_C(1) << 1));
  CHECK(lgInput_available());

  const unsigned fallbackKeyUp   = state->fallback.calls.keyUp;
  const unsigned fallbackRelease = state->fallback.calls.release;
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(lgInput_mouseRelease(1));
  CHECK(state->fallback.calls.keyUp == fallbackKeyUp);
  CHECK(state->fallback.calls.release == fallbackRelease);
  CHECK(lgInput_mouseMotion(4, 5));
  CHECK(state->fallback.calls.motion == 1);

  const LG_InputStatus status =
  {
    .available  = false,
    .generation = state->transport.generation + 1,
  };
  stale(staleOpaque, &status);
  CHECK(lgInput_available());
  CHECK(lgInput_mouseMotion(6, 7));
  CHECK(state->fallback.calls.motion == 2);
  CHECK(state->transport.calls.dead == 0);
  return true;
}

static bool testRollback(TestState * state)
{
  lgInput_setTransport(&inputOps, &state->transport);
  CHECK(lgInput_available());

  state->transport.keyDownResult = false;
  CHECK(!lgInput_keyDown(KEY_A));
  CHECK(state->transport.calls.keyDown == 1);
  state->transport.keyDownResult = true;
  CHECK(lgInput_keyDown(KEY_A));
  CHECK(state->transport.calls.keyDown == 2);
  CHECK(lgInput_keyDown(KEY_A));
  CHECK(state->transport.calls.keyDown == 2);

  state->transport.keyUpResult = false;
  CHECK(!lgInput_keyUp(KEY_A));
  CHECK(state->transport.calls.keyUp == 1);
  CHECK(state->transport.remoteKeys[KEY_A]);
  state->transport.keyUpResult = true;
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(state->transport.calls.keyUp == 2);
  CHECK(!state->transport.remoteKeys[KEY_A]);
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(state->transport.calls.keyUp == 2);

  state->transport.pressResult = false;
  CHECK(!lgInput_mousePress(1));
  CHECK(state->transport.calls.press == 1);
  state->transport.pressResult = true;
  CHECK(lgInput_mousePress(1));
  CHECK(state->transport.calls.press == 2);
  CHECK(lgInput_mousePress(1));
  CHECK(state->transport.calls.press == 2);

  state->transport.releaseResult = false;
  CHECK(!lgInput_mouseRelease(1));
  CHECK(state->transport.calls.release == 1);
  CHECK(state->transport.remoteButtons & (UINT32_C(1) << 1));
  state->transport.releaseResult = true;
  CHECK(lgInput_mouseRelease(1));
  CHECK(state->transport.calls.release == 2);
  CHECK(state->transport.remoteButtons == 0);
  CHECK(lgInput_mouseRelease(1));
  CHECK(state->transport.calls.release == 2);
  return true;
}

static bool testDuplicate(TestState * state)
{
  lgInput_setTransport(&inputOps, &state->transport);
  CHECK(lgInput_available());

  CHECK(!lgInput_keyDown(-1));
  CHECK(!lgInput_keyDown(KEY_MAX));
  CHECK(!lgInput_keyUp(-1));
  CHECK(!lgInput_keyUp(KEY_MAX));
  CHECK(!lgInput_mousePress(0));
  CHECK(!lgInput_mousePress(32));
  CHECK(!lgInput_mouseRelease(0));
  CHECK(!lgInput_mouseRelease(32));
  CHECK(state->transport.calls.keyDown == 0);
  CHECK(state->transport.calls.keyUp == 0);
  CHECK(state->transport.calls.press == 0);
  CHECK(state->transport.calls.release == 0);

  CHECK(lgInput_keyDown(KEY_A));
  CHECK(lgInput_keyDown(KEY_A));
  CHECK(state->transport.calls.keyDown == 1);
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(lgInput_keyUp(KEY_A));
  CHECK(state->transport.calls.keyUp == 1);

  CHECK(lgInput_mousePress(1));
  CHECK(lgInput_mousePress(1));
  CHECK(state->transport.calls.press == 1);
  CHECK(lgInput_mouseRelease(1));
  CHECK(lgInput_mouseRelease(1));
  CHECK(state->transport.calls.release == 1);

  CHECK(lgInput_keyDown(KEY_B));
  CHECK(lgInput_keyDown(KEY_C));
  const unsigned keyUp = state->transport.calls.keyUp;
  lgInput_releaseKeys();
  CHECK(state->transport.calls.keyUp == keyUp + 2);
  CHECK(!state->transport.remoteKeys[KEY_B]);
  CHECK(!state->transport.remoteKeys[KEY_C]);
  lgInput_releaseKeys();
  CHECK(state->transport.calls.keyUp == keyUp + 2);
  return true;
}

static bool testAbsolute(TestState * state)
{
  state->fallback.absolute = true;
  CHECK(bindBoth(state));
  CHECK(!lgInput_supports(LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(!lgInput_mousePosition(10, 20, 100, 200));
  CHECK(state->transport.calls.position == 0);

  lgInput_useTransport(false);
  CHECK(lgInput_supports(LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(lgInput_mousePosition(10, 20, 100, 200));
  CHECK(state->fallback.calls.position == 1);
  CHECK(state->fallback.lastPositionX == 10);
  CHECK(state->fallback.lastPositionY == 20);
  CHECK(state->fallback.lastPositionWidth == 100);
  CHECK(state->fallback.lastPositionHeight == 200);

  lgInput_useTransport(true);
  state->transport.absolute = true;
  CHECK(setAvailable(&state->transport, true));
  CHECK(lgInput_supports(LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(lgInput_mousePosition(30, 40, 300, 400));
  CHECK(state->transport.calls.position == 1);

  state->replacement.absolute = true;
  lgInput_setTransport(&noPositionOps, &state->replacement);
  CHECK(lgInput_available());
  CHECK(!lgInput_supports(LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(!lgInput_mousePosition(50, 60, 500, 600));
  CHECK(state->replacement.calls.position == 0);

  CHECK(setAvailable(&state->replacement, false));
  CHECK(lgInput_supports(LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(lgInput_mousePosition(70, 80, 700, 800));
  CHECK(state->fallback.calls.position == 2);
  return true;
}

typedef bool (*TestFn)(TestState * state);

static const struct
{
  const char * name;
  TestFn       run;
}
tests[] =
{
  { "preference", testPreference },
  { "handoff"   , testHandoff    },
  { "stale"     , testStale      },
  { "dead-drop" , testDeadDrop   },
  { "rollback"  , testRollback   },
  { "duplicate" , testDuplicate  },
  { "absolute"  , testAbsolute   },
};

static unsigned stateFaults(const TestState * state)
{
  return state->fallback.calls.fault + state->fallback.calls.dead +
    state->transport.calls.fault + state->transport.calls.dead +
    state->replacement.calls.fault + state->replacement.calls.dead;
}

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr,
        "usage: %s <preference|handoff|stale|dead-drop|rollback|"
        "duplicate|absolute>\n",
        argv[0]);
    return 2;
  }

  TestFn test = NULL;
  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      test = tests[i].run;
      break;
    }
  if (!test)
  {
    fprintf(stderr, "unknown test case: %s\n", argv[1]);
    return 2;
  }

  debug_init();
  TestState state;
  stateInit(&state);
  lgInput_init();
  bool passed = test(&state);
  lgInput_free();

  const unsigned faults = stateFaults(&state);
  if (faults)
  {
    fprintf(stderr, "fake provider observed %u invalid/dead calls\n", faults);
    passed = false;
  }
  return passed ? 0 : 1;
}
