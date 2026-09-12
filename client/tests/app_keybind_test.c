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

#include "app.h"
#include "app_internal.h"
#include "core.h"
#include "input.h"
#include "main.h"
#include "overlay/msg.h"
#include "overlays.h"
#include "render_queue.h"
#include "sw_surface.h"
#include "test.h"
#include "transport_fallback.h"
#include "util.h"

#include "common/array.h"
#include "common/ll.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_KEY 32U

struct KeyEvent
{
  int  key;
  bool down;
};

struct Trace
{
  struct KeyEvent key[MAX_KEY];
  unsigned int    keyN;
  unsigned int    cbN;
  unsigned int    releaseN;
  unsigned int    grabN;
  unsigned int    quietN;
  unsigned int    cursorN;
  unsigned int    resetN;
  unsigned int    pointerN;
  unsigned int    minimizeN;
  unsigned int    realignN;
  int             cbKey;
  bool            input;
  bool            available;
  bool            exclusive;
  bool            fullscreen;
  bool            grab;
  bool            quiet;
  bool            cursor;
  LG_DSPointer    pointer;
};

struct Reentrant
{
  KeybindHandle handle;
  unsigned int  oldN;
  unsigned int  newN;
};

static struct Trace t;
static struct Reentrant re;
static struct LG_DisplayServerOps ds;
static ImGuiIO io;

struct AppState    g_state;
struct CursorState g_cursor;
struct AppParams   g_params;
_Atomic(enum RunState) p_appState;

const char * lg_keycode_to_display[LG_KEY_MAX] =
{
  [LG_KEY_ESC] = "Esc",
  [LG_KEY_A]   = "A",
  [LG_KEY_B]   = "B",
  [LG_KEY_C]   = "C",
};

const int lg_keycode_to_imgui[LG_KEY_MAX] = { 0 };

static bool getFullscreen(void)
{
  return t.fullscreen;
}

static void minimize(void)
{
  ++t.minimizeN;
}

static void setPointer(LG_DSPointer pointer)
{
  ++t.pointerN;
  t.pointer = pointer;
}

static void realign(void)
{
  ++t.realignN;
}

static void init(void)
{
  memset(&t, 0, sizeof(t));
  memset(&re, 0, sizeof(re));
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_cursor, 0, sizeof(g_cursor));
  memset(&g_params, 0, sizeof(g_params));
  memset(&ds, 0, sizeof(ds));
  memset(&io, 0, sizeof(io));

  t.input           = true;
  t.available       = true;

  ds.getFullscreen  = getFullscreen;
  ds.minimize       = minimize;
  ds.setPointer     = setPointer;
  ds.realignPointer = realign;

  g_state.bindings  = ll_new();
  g_state.overlays  = ll_new();
  g_state.io        = &io;
  g_state.ds        = &ds;
  g_state.focused   = true;

  g_params.escapeKey = LG_KEY_ESC;
  atomic_store(&p_appState, APP_STATE_RUNNING);

  CHECK(g_state.bindings);
  CHECK(g_state.overlays);
}

static void fini(void)
{
  app_releaseAllKeybinds();
  ll_free(g_state.bindings);
  ll_free(g_state.overlays);
}

static bool addKey(bool down, int key)
{
  CHECK(t.keyN < MAX_KEY);
  t.key[t.keyN].key    = key;
  t.key[t.keyN++].down = down;
  return true;
}

bool core_inputEnabled(void)
{
  return t.input;
}

void core_updateKeyboardGrab(void)
{
}

void core_setGrab(bool enable)
{
  ++t.grabN;
  t.grab = enable;
}

void core_setGrabQuiet(bool enable)
{
  ++t.quietN;
  t.quiet = enable;
}

void core_setCursorInView(bool enable)
{
  ++t.cursorN;
  t.cursor = enable;
}

void core_resetOverlayInputState(void)
{
  ++t.resetN;
}

void core_updateOverlayState(void)
{
}

bool inputPlatformIsKeyboardExclusive(void)
{
  return t.exclusive;
}

bool inputPlatformIsPointerExclusive(void)
{
  return false;
}

bool lgInput_available(void)
{
  return t.available;
}

bool lgInput_keyDown(int key)
{
  return addKey(true, key);
}

bool lgInput_keyUp(int key)
{
  return addKey(false, key);
}

void lgInput_releaseKeys(void)
{
  ++t.releaseN;
}

void core_handleMousePosition(double x, double y)
{
  (void)x;
  (void)y;
}

void core_handleMouseAbsolute(void)
{
}

void core_handleMouseGrabbed(double x, double y)
{
  (void)x;
  (void)y;
}

void core_handleMouseNormal(double x, double y)
{
  (void)x;
  (void)y;
}

void core_handleGrabEvent(bool active)
{
  (void)active;
}

void core_handleCaptureLost(void)
{
}

void core_updatePositionInfo(void)
{
}

void core_alignToGuest(void)
{
}

bool core_startVideoThreads(void)
{
  return true;
}

void core_stopVideoThreads(void)
{
}

bool lgInput_mousePress(unsigned int button)
{
  (void)button;
  return true;
}

bool lgInput_mouseRelease(unsigned int button)
{
  (void)button;
  return true;
}

void lgInput_releaseAll(void)
{
}

void lgInput_releaseButtons(void)
{
}

bool lgInput_keyboardLEDs(bool numLock, bool capsLock, bool scrollLock)
{
  (void)numLock;
  (void)capsLock;
  (void)scrollLock;
  return true;
}

void main_framePresented(uint64_t frameToken, uint64_t presentTime,
    bool valid)
{
  (void)frameToken;
  (void)presentTime;
  (void)valid;
}

void main_videoStreamEnabled(void)
{
}

struct LG_OverlayOps LGOverlayMsg;

void overlayAlert_show(LG_MsgAlert type, const char * format, va_list args)
{
  (void)type;
  (void)format;
  (void)args;
}

MsgBoxHandle overlayMsg_show(const char * caption,
    MsgBoxConfirmCallback confirm, void * confirmOpaque,
    MsgBoxCloseCallback close, void * closeOpaque,
    const char * format, va_list args)
{
  (void)caption;
  (void)confirm;
  (void)confirmOpaque;
  (void)close;
  (void)closeOpaque;
  (void)format;
  (void)args;
  return NULL;
}

void overlayMsg_close(MsgBoxHandle handle)
{
  (void)handle;
}

bool overlayMsg_modal(void)
{
  return false;
}

void overlayStatus_set(LGUserStatus status, bool value)
{
  (void)status;
  (void)value;
}

GraphHandle overlayGraph_register(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn)
{
  (void)name;
  (void)buffer;
  (void)min;
  (void)max;
  (void)formatFn;
  return NULL;
}

void overlayGraph_unregister(GraphHandle handle)
{
  (void)handle;
}

void overlayGraph_invalidate(GraphHandle handle)
{
  (void)handle;
}

void overlayGraph_setCompact(GraphHandle handle, bool compact)
{
  (void)handle;
  (void)compact;
}

void overlayConfig_register(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  (void)title;
  (void)callback;
  (void)udata;
}

void overlayConfig_registerTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  (void)title;
  (void)callback;
  (void)udata;
}

bool util_uiFontAddText(const char * text)
{
  (void)text;
  return true;
}

void lgTransportFallback_requestVideoActive(
    LG_TransportFallback * fallback, bool active)
{
  (void)fallback;
  (void)active;
}

bool lgSwSurface_setActive(const LG_SwSurfaceOps * ops,
    LG_Transport * transport, bool active,
    LG_TransportCancelledFn cancelled, void * opaque)
{
  (void)ops;
  (void)transport;
  (void)active;
  (void)cancelled;
  (void)opaque;
  return true;
}

uint64_t renderQueue_sourceTransition(RenderQueueSource source,
    uint64_t generation, bool swSurface,
    atomic_uint_least64_t * publishedSerial)
{
  (void)source;
  (void)generation;
  (void)swSurface;
  (void)publishedSerial;
  return 0;
}

uint64_t renderQueue_sourceSwSurfaceConfigureTransition(
    RenderQueueSource source, uint64_t generation, int width, int height,
    atomic_uint_least64_t * publishedSerial)
{
  (void)source;
  (void)generation;
  (void)width;
  (void)height;
  (void)publishedSerial;
  return 0;
}

void renderQueue_sourceCursorState(RenderQueueSource source,
    uint64_t generation, bool visible, int x, int y, int hx, int hy)
{
  (void)source;
  (void)generation;
  (void)visible;
  (void)x;
  (void)y;
  (void)hx;
  (void)hy;
}

static void count(int key, void * opaque)
{
  ++t.cbN;
  t.cbKey = key;
  CHECK(opaque == &t);
}

static void next(int key, void * opaque)
{
  struct Reentrant * state = opaque;
  ++state->newN;
}

static void replace(int key, void * opaque)
{
  struct Reentrant * state = opaque;
  ++state->oldN;
  app_releaseKeybind(&state->handle);
  CHECK(!state->handle);
  state->handle = app_registerKeybind(key, next, state, "new");
  CHECK(state->handle);
}

static void checkKey(unsigned int index, bool down, int key)
{
  CHECK(index < t.keyN);
  CHECK(t.key[index].down == down);
  CHECK(t.key[index].key == key);
}

static void testInvalid(void)
{
  init();
  CHECK(!app_registerKeybind(LG_KEY_RESERVED, count, &t, "reserved"));
  CHECK(!app_registerKeybind(LG_KEY_MAX, count, &t, "past-end"));
  CHECK(!app_registerKeybind(LG_KEY_MAX - 1, count, &t, "unmapped"));
  CHECK(ll_count(g_state.bindings) == 0);

  KeybindHandle handle = app_registerKeybind(LG_KEY_A, count, &t, "valid");
  CHECK(handle);
  CHECK(ll_count(g_state.bindings) == 1);
  fini();
}

static void testNullCallback(void)
{
  init();
  CHECK(!app_registerKeybind(LG_KEY_A, NULL, NULL, "null callback"));
  CHECK(ll_count(g_state.bindings) == 0);
  fini();
}

static void testDuplicate(void)
{
  init();
  KeybindHandle first = app_registerKeybind(LG_KEY_A, count, &t, "first");
  CHECK(first);
  CHECK(!app_registerKeybind(LG_KEY_A, count, &t, "duplicate"));
  CHECK(ll_count(g_state.bindings) == 1);

  app_releaseKeybind(&first);
  CHECK(!first);
  CHECK(ll_count(g_state.bindings) == 0);
  app_releaseKeybind(&first);
  CHECK(!first);

  first = app_registerKeybind(LG_KEY_A, count, &t, "replacement");
  CHECK(first);
  CHECK(ll_count(g_state.bindings) == 1);
  fini();
}

static void testNormal(void)
{
  init();
  app_handleKeyPress(LG_KEY_A);
  app_handleKeyPress(LG_KEY_A);
  app_handleKeyRelease(LG_KEY_A);
  CHECK(t.keyN == 3);
  checkKey(0, true , LG_KEY_A);
  checkKey(1, true , LG_KEY_A);
  checkKey(2, false, LG_KEY_A);

  t.exclusive = true;
  app_handleKeyPress(LG_KEY_B);
  app_handleKeyRelease(LG_KEY_B);
  CHECK(t.keyN == 3);

  t.exclusive = false;
  g_params.ignoreWindowsKeys = true;
  app_handleKeyPress(LG_KEY_LEFTMETA);
  app_handleKeyRelease(LG_KEY_LEFTMETA);
  CHECK(t.keyN == 3);
  fini();
}

static void testDispatch(void)
{
  init();
  CHECK(app_registerKeybind(LG_KEY_A, count, &t, "action"));
  app_handleKeyPress(LG_KEY_ESC);
  CHECK(g_state.escapeActive);
  CHECK(g_state.escapeAction == -1);
  CHECK(t.keyN == 0);

  app_handleKeyPress(LG_KEY_A);
  app_handleKeyPress(LG_KEY_A);
  CHECK(t.cbN == 2);
  CHECK(t.cbKey == LG_KEY_A);
  CHECK(g_state.escapeAction == LG_KEY_A);
  CHECK(t.keyN == 0);

  app_handleKeyRelease(LG_KEY_A);
  app_handleKeyRelease(LG_KEY_ESC);
  CHECK(!g_state.escapeActive);
  CHECK(t.cbN == 2);
  CHECK(t.keyN == 0);
  fini();
}

static void testReleaseAll(void)
{
  init();
  CHECK(app_registerKeybind(LG_KEY_A, count, &t, "a"));
  CHECK(app_registerKeybind(LG_KEY_B, count, &t, "b"));
  CHECK(app_registerKeybind(LG_KEY_C, count, &t, "c"));
  CHECK(ll_count(g_state.bindings) == 3);

  app_releaseAllKeybinds();
  CHECK(ll_count(g_state.bindings) == 0);
  app_releaseAllKeybinds();
  CHECK(ll_count(g_state.bindings) == 0);
  CHECK(app_registerKeybind(LG_KEY_A, count, &t, "again"));
  CHECK(ll_count(g_state.bindings) == 1);
  fini();
}

static void testFocus(void)
{
  init();
  t.fullscreen                    = true;
  g_state.escapeActive            = true;
  g_cursor.grab                   = true;
  g_cursor.motionValid            = true;
  g_params.releaseKeysOnFocusLoss = true;
  g_params.minimizeOnFocusLoss    = true;
  g_params.captureOnFocus         = true;

  app_handleFocusEvent(false);
  CHECK(!g_state.focused);
  CHECK(!g_state.escapeActive);
  CHECK(!g_cursor.motionValid);
  CHECK(g_cursor.realign);
  CHECK(t.releaseN == 1);
  CHECK(t.quietN == 1);
  CHECK(!t.quiet);
  CHECK(t.cursorN == 1);
  CHECK(!t.cursor);
  CHECK(t.pointerN == 1);
  CHECK(t.pointer == LG_POINTER_NONE);
  CHECK(t.minimizeN == 1);
  CHECK(t.realignN == 1);

  app_handleFocusEvent(false);
  CHECK(t.releaseN == 1);
  CHECK(t.realignN == 1);

  app_handleFocusEvent(true);
  CHECK(g_state.focused);
  CHECK(t.grabN == 1);
  CHECK(t.grab);
  CHECK(t.realignN == 2);

  g_params.releaseKeysOnFocusLoss = false;
  app_handleFocusEvent(false);
  CHECK(t.releaseN == 1);
  fini();
}

static void testReentrant(void)
{
  init();
  re.handle = app_registerKeybind(LG_KEY_A, replace, &re, "old");
  CHECK(re.handle);
  app_handleKeyPress(LG_KEY_ESC);
  app_handleKeyPress(LG_KEY_A);
  CHECK(re.oldN == 1);
  CHECK(re.newN == 0);
  CHECK(re.handle);
  CHECK(ll_count(g_state.bindings) == 1);

  app_handleKeyPress(LG_KEY_A);
  CHECK(re.oldN == 1);
  CHECK(re.newN == 1);
  CHECK(ll_count(g_state.bindings) == 1);
  fini();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "invalid"      , testInvalid      },
  { "null-callback", testNullCallback },
  { "duplicate"    , testDuplicate    },
  { "normal"       , testNormal       },
  { "dispatch"     , testDispatch     },
  { "release-all"  , testReleaseAll   },
  { "focus"        , testFocus        },
  { "reentrant"    , testReentrant    },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  debug_init();
  for (unsigned int i = 0; i < ARRAY_LENGTH(tests); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
