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

#include "test.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/array.h"

#include "../src/keybind.c"

#define MAX_BIND 64U
#define MAX_KEY  64U
#define MAX_OP   128U

struct Bind
{
  struct KeybindHandle handle;
  bool                 active;
};

struct KeyEvent
{
  int  key;
  bool down;
};

struct Trace
{
  struct Bind     bind[MAX_BIND];
  struct KeyEvent key[MAX_KEY];
  int             op[MAX_OP];
  unsigned int    bindN;
  unsigned int    tryN;
  unsigned int    activeN;
  unsigned int    collisionN;
  unsigned int    releaseN;
  unsigned int    keyN;
  unsigned int    opN;
  unsigned int    posN;
  unsigned int    cursorN;
  unsigned int    realignN;
  unsigned int    alertN;
  unsigned int    msgN;
  unsigned int    videoN;
  unsigned int    overlayN;
  bool            isLinux;
  bool            fullscreen;
  bool            cursorInView;
  LGMsg           msg;
  char            alert[64];
};

static struct Trace t;
static struct LG_DisplayServerOps ds;

struct AppState    g_state;
struct CursorState g_cursor;
struct AppParams   g_params;
_Atomic(enum RunState) p_appState;

enum RunState app_getState(void)
{
  return atomic_load(&p_appState);
}

void app_setState(enum RunState state)
{
  atomic_store(&p_appState, state);
}

static const int fKeys[] =
{
  KEY_F1 , KEY_F2 , KEY_F3 , KEY_F4,
  KEY_F5 , KEY_F6 , KEY_F7 , KEY_F8,
  KEY_F9 , KEY_F10, KEY_F11, KEY_F12,
};

static void realign(void)
{
  ++t.realignN;
}

static void reset(void)
{
  memset(&t, 0, sizeof(t));
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_cursor, 0, sizeof(g_cursor));
  memset(&g_params, 0, sizeof(g_params));
  memset(&ds, 0, sizeof(ds));
  ds.realignPointer = realign;
  g_state.ds        = &ds;
  atomic_store(&p_appState, APP_STATE_RUNNING);
}

static struct Bind * find(int sc)
{
  for (unsigned int i = 0; i < t.bindN; ++i)
    if (t.bind[i].active && t.bind[i].handle.sc == sc)
      return &t.bind[i];
  return NULL;
}

static void call(int sc)
{
  struct Bind * bind = find(sc);
  CHECK(bind);
  CHECK(bind->handle.callback);
  bind->handle.callback(sc, bind->handle.opaque);
}

KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque,
    const char * description)
{
  CHECK(sc > KEY_RESERVED && sc < KEY_MAX);
  CHECK(callback);
  CHECK(description);
  ++t.tryN;

  if (find(sc))
  {
    ++t.collisionN;
    return NULL;
  }

  CHECK(t.bindN < MAX_BIND);
  CHECK(t.opN < MAX_OP);
  struct Bind * bind         = &t.bind[t.bindN++];
  bind->active               = true;
  bind->handle.sc            = sc;
  bind->handle.callback      = callback;
  bind->handle.description   = description;
  bind->handle.opaque        = opaque;
  t.op[t.opN++]              = sc;
  ++t.activeN;
  return &bind->handle;
}

void app_releaseKeybind(KeybindHandle * handle)
{
  CHECK(handle);
  if (!*handle)
    return;

  struct Bind * bind = NULL;
  for (unsigned int i = 0; i < t.bindN; ++i)
    if (&t.bind[i].handle == *handle)
    {
      bind = &t.bind[i];
      break;
    }

  CHECK(bind);
  CHECK(bind->active);
  CHECK(t.opN < MAX_OP);
  bind->active  = false;
  t.op[t.opN++] = -bind->handle.sc;
  --t.activeN;
  ++t.releaseN;
  *handle = NULL;
}

bool app_guestIsLinux(void)
{
  return t.isLinux;
}

void app_setFullscreen(bool fullscreen)
{
  t.fullscreen = fullscreen;
}

bool app_getFullscreen(void)
{
  return t.fullscreen;
}

void app_stopVideo(bool stop)
{
  ++t.videoN;
  g_state.stopVideo = stop;
}

void app_setOverlay(bool enable)
{
  ++t.overlayN;
  g_state.overlayInput = enable;
}

void app_alert(LG_MsgAlert type, const char * format, ...)
{
  CHECK(type == LG_ALERT_INFO);
  va_list args;
  va_start(args, format);
  vsnprintf(t.alert, sizeof(t.alert), format, args);
  va_end(args);
  ++t.alertN;
}

void core_updatePositionInfo(void)
{
  ++t.posN;
}

void core_setCursorInView(bool enable)
{
  ++t.cursorN;
  t.cursorInView = enable;
}

static bool key(bool down, int sc)
{
  CHECK(t.keyN < MAX_KEY);
  t.key[t.keyN].key    = sc;
  t.key[t.keyN++].down = down;
  return true;
}

bool lgInput_keyDown(int sc)
{
  return key(true, sc);
}

bool lgInput_keyUp(int sc)
{
  return key(false, sc);
}

void lgMessage_post(const LGMsg * msg)
{
  CHECK(msg);
  t.msg = *msg;
  ++t.msgN;
}

static void nop(int sc, void * opaque)
{
}

static void checkBind(unsigned int index, int sc, const char * description)
{
  CHECK(index < t.bindN);
  CHECK(t.bind[index].active);
  CHECK(t.bind[index].handle.sc == sc);
  CHECK(strcmp(t.bind[index].handle.description, description) == 0);
}

static void checkKey(unsigned int index, bool down, int sc)
{
  CHECK(index < t.keyN);
  CHECK(t.key[index].down == down);
  CHECK(t.key[index].key == sc);
}

static void testCommon(void)
{
  reset();
  keybind_commonRegister();

  CHECK(t.tryN == 6);
  CHECK(t.activeN == 6);
  CHECK(t.collisionN == 0);
  checkBind(0, KEY_F, "Full screen toggle");
  checkBind(1, KEY_V, "Video stream toggle");
  checkBind(2, KEY_R, "Rotate the output clockwise by 90° increments");
  checkBind(3, KEY_Q, "Quit");
  checkBind(4, KEY_O, "Toggle overlay");
  checkBind(5, KEY_EQUAL,
      "Set guest resolution to match window size");

  keybind_commonRegister();
  CHECK(t.tryN == 12);
  CHECK(t.activeN == 6);
  CHECK(t.collisionN == 6);
}

static void testOS(void)
{
  reset();
  CHECK(app_registerKeybind(KEY_F1, nop, NULL, "external"));
  t.isLinux = true;
  keybind_inputRegister();

  CHECK(t.tryN == 21);
  CHECK(t.activeN == 20);
  CHECK(t.collisionN == 1);
  CHECK(find(KEY_I));
  CHECK(find(KEY_LEFTMETA));
  CHECK(find(KEY_RIGHTMETA));
  for (unsigned int i = 0; i < ARRAY_LENGTH(fKeys); ++i)
    CHECK(find(fKeys[i]));

  struct Bind * fixed = find(KEY_I);
  struct Bind * oldF2 = find(KEY_F2);
  const unsigned int op = t.opN;
  keybind_inputRegister();
  CHECK(t.activeN == 20);
  CHECK(t.releaseN == 11);
  CHECK(t.collisionN == 2);
  CHECK(find(KEY_I) == fixed);
  CHECK(find(KEY_F2) != oldF2);
  CHECK(!oldF2->active);
  CHECK(t.opN == op + 22);
  for (unsigned int i = 0; i < 11; ++i)
  {
    CHECK(t.op[op + i] < 0);
    CHECK(t.op[op + 11 + i] > 0);
  }

  t.isLinux = false;
  keybind_inputRegister();
  CHECK(t.activeN == 9);
  CHECK(t.releaseN == 22);
  CHECK(find(KEY_I) == fixed);
  CHECK(strcmp(find(KEY_F1)->handle.description, "external") == 0);
  for (unsigned int i = 1; i < ARRAY_LENGTH(fKeys); ++i)
    CHECK(!find(fKeys[i]));

  t.isLinux = true;
  keybind_inputRegister();
  CHECK(t.activeN == 20);
  CHECK(t.collisionN == 3);
}

static void testOrder(void)
{
  reset();
  t.isLinux = true;
  keybind_inputRegister();
  call(KEY_F5);

  CHECK(t.keyN == 6);
  checkKey(0, true , KEY_LEFTCTRL);
  checkKey(1, true , KEY_LEFTALT );
  checkKey(2, true , KEY_F5      );
  checkKey(3, false, KEY_F5      );
  checkKey(4, false, KEY_LEFTALT );
  checkKey(5, false, KEY_LEFTCTRL);
}

static void testRepeat(void)
{
  reset();
  keybind_inputRegister();
  call(KEY_LEFTMETA);
  call(KEY_LEFTMETA);
  call(KEY_UP);
  call(KEY_UP);

  CHECK(t.keyN == 8);
  checkKey(0, true , KEY_LEFTMETA);
  checkKey(1, false, KEY_LEFTMETA);
  checkKey(2, true , KEY_LEFTMETA);
  checkKey(3, false, KEY_LEFTMETA);
  checkKey(4, true , KEY_VOLUMEUP);
  checkKey(5, false, KEY_VOLUMEUP);
  checkKey(6, true , KEY_VOLUMEUP);
  checkKey(7, false, KEY_VOLUMEUP);
}

static void testActions(void)
{
  reset();
  keybind_commonRegister();
  keybind_inputRegister();

  call(KEY_F);
  CHECK(t.fullscreen);
  call(KEY_F);
  CHECK(!t.fullscreen);

  call(KEY_V);
  CHECK(g_state.stopVideo);
  call(KEY_V);
  CHECK(!g_state.stopVideo);
  CHECK(t.videoN == 2);

  g_params.winRotate = LG_ROTATE_MAX - 1;
  call(KEY_R);
  CHECK(g_params.winRotate == 0);
  call(KEY_R);
  CHECK(g_params.winRotate == 1);
  CHECK(t.posN == 2);

  call(KEY_O);
  CHECK(g_state.overlayInput);
  call(KEY_O);
  CHECK(!g_state.overlayInput);
  CHECK(t.overlayN == 2);

  g_state.ignoreInput = false;
  call(KEY_I);
  CHECK(g_state.ignoreInput);
  CHECK(t.cursorN == 1);
  CHECK(!t.cursorInView);
  CHECK(strcmp(t.alert, "Input Disabled") == 0);
  call(KEY_I);
  CHECK(!g_state.ignoreInput);
  CHECK(t.realignN == 1);
  CHECK(strcmp(t.alert, "Input Enabled") == 0);

  g_cursor.sens = 8;
  call(KEY_INSERT);
  call(KEY_INSERT);
  CHECK(g_cursor.sens == 9);
  CHECK(strcmp(t.alert, "Sensitivity: +9") == 0);
  g_cursor.sens = -8;
  call(KEY_DELETE);
  call(KEY_DELETE);
  CHECK(g_cursor.sens == -9);
  CHECK(strcmp(t.alert, "Sensitivity: -9") == 0);

  g_state.windowW = 1920;
  g_state.windowH = 1080;
  call(KEY_EQUAL);
  CHECK(t.msgN == 0);
  CHECK(strcmp(t.alert,
        "The guest doesn't support this feature") == 0);
  g_state.transportFeatures = LG_TRANSPORT_FEATURE_WINDOW_SIZE;
  call(KEY_EQUAL);
  CHECK(t.msgN == 1);
  CHECK(t.msg.type == LG_MSG_WINDOWSIZE);
  CHECK(t.msg.windowSize.width == 1920);
  CHECK(t.msg.windowSize.height == 1080);

  call(KEY_Q);
  CHECK(app_getState() == APP_STATE_SHUTDOWN);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "common" , testCommon  },
  { "os"     , testOS      },
  { "order"  , testOrder   },
  { "repeat" , testRepeat  },
  { "actions", testActions },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  for (unsigned int i = 0; i < ARRAY_LENGTH(tests); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
