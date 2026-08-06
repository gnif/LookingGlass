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
#include "core.h"
#include "main.h"
#include "test.h"
#include "util.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

enum EvType
{
  EV_GRAB,
  EV_UNGRAB,
  EV_CAPTURE,
  EV_UNCAPTURE,
  EV_WARP,
  EV_GUEST,
  EV_MOTION,
  EV_VALID,
};

struct Ev
{
  enum EvType type;
  double      x;
  double      y;
  bool        exit;
  bool        conf;
  bool        lock;
};

struct Trap
{
  bool req;
  bool on;
  bool hold;
};

static struct
{
  enum LG_DSWarpSupport support;
  struct Trap           conf;
  struct Trap           lock;
  bool                  valid;
  struct Ev             ev[256];
  unsigned int          count;
}
m;

struct AppState    g_state;
struct CursorState g_cursor;
struct AppParams   g_params;

static void push(enum EvType type, double x, double y, bool leaving)
{
  CHECK(m.count < sizeof(m.ev) / sizeof(m.ev[0]));
  m.ev[m.count++] = (struct Ev) {
    .type = type,
    .x    = x,
    .y    = y,
    .exit = leaving,
    .conf = m.conf.on,
    .lock = m.lock.on,
  };
}

static unsigned int count(enum EvType type)
{
  unsigned int result = 0;
  for (unsigned int i = 0; i < m.count; ++i)
    if (m.ev[i].type == type)
      ++result;
  return result;
}

static int first(enum EvType type)
{
  for (unsigned int i = 0; i < m.count; ++i)
    if (m.ev[i].type == type)
      return i;
  return -1;
}

static void guest(double x, double y, double localX, double localY)
{
  (void)x;
  (void)y;
  push(EV_GUEST, localX, localY, false);
}

static void setPointer(LG_DSPointer pointer)
{
  (void)pointer;
}

static void keyNoop(void)
{
}

static void grab(void)
{
  m.conf.req = true;
  push(EV_GRAB, 0, 0, false);
}

static void ungrab(void)
{
  push(EV_UNGRAB, 0, 0, false);
  m.conf.req = false;
  if (!m.conf.hold)
    m.conf.on = false;
}

static void capture(void)
{
  m.conf.req = false;
  if (!m.conf.hold)
    m.conf.on = false;
  m.lock.req = true;
  push(EV_CAPTURE, 0, 0, false);
}

static void uncapture(void)
{
  push(EV_UNCAPTURE, 0, 0, false);
  m.lock.req = false;
  if (!m.lock.hold)
    m.lock.on = false;
}

static void warp(int x, int y, bool exiting)
{
  push(EV_WARP, x, y, exiting);
}

static bool valid(int x, int y)
{
  push(EV_VALID, x, y, false);
  return m.valid;
}

static struct LG_DisplayServerOps ds = {
  .name                = "test",
  .guestPointerUpdated = guest,
  .setPointer          = setPointer,
  .grabKeyboard        = keyNoop,
  .ungrabKeyboard      = keyNoop,
  .grabPointer         = grab,
  .ungrabPointer       = ungrab,
  .capturePointer      = capture,
  .uncapturePointer    = uncapture,
  .warpPointer         = warp,
  .isValidPointerPos   = valid,
};

bool app_getProp(LG_DSProperty prop, void * ret)
{
  if (prop != LG_DS_WARP_SUPPORT)
    return false;

  *(enum LG_DSWarpSupport *)ret = m.support;
  return true;
}

bool app_guestIsWindows(void)
{
  return false;
}

bool app_isOverlayMode(void)
{
  return false;
}

bool app_isRunning(void)
{
  return false;
}

uint64_t app_mouseSeq(void)
{
  return 0;
}

void app_mouseTrace(const char * file, unsigned int line,
    const char * function, uint64_t seq, const char * format, ...)
{
  (void)file;
  (void)line;
  (void)function;
  (void)seq;
  (void)format;
}

bool purespice_mouseMotion(int32_t x, int32_t y)
{
  push(EV_MOTION, x, y, false);
  return true;
}

static void reset(void)
{
  memset(&m, 0, sizeof(m));
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_cursor, 0, sizeof(g_cursor));
  memset(&g_params, 0, sizeof(g_params));

  m.support = LG_DS_WARP_SURFACE;
  m.valid   = true;

  g_state.ds           = &ds;
  g_state.focused      = true;
  g_state.haveSrcSize  = true;
  g_state.posInfoValid = true;
  g_state.formatValid  = true;
  g_state.windowW      = 300;
  g_state.windowH      = 200;
  g_state.srcSize      = (struct Point) { 100, 80 };
  g_state.dstRect      = (LG_RendererRect) {
    .valid = true,
    .x     = 10,
    .y     = 20,
    .w     = 100,
    .h     = 80,
  };

  g_params.useSpiceInput     = true;
  g_params.scaleMouseInput   = true;

  g_cursor.inWindow    = true;
  g_cursor.inView      = true;
  g_cursor.warpState   = WARP_STATE_ON;
  g_cursor.guest.valid = true;
  g_cursor.scale       = (struct DoublePoint) { 1.0, 1.0 };
}

static void setLocal(double x, double y)
{
  g_cursor.pos = (struct DoublePoint) { x, y };

  struct DoublePoint guest;
  util_localCurToGuest(&guest);
  g_cursor.guest.x = lround(guest.x);
  g_cursor.guest.y = lround(guest.y);
}

static void testInsetExit(void)
{
  reset();
  setLocal(10, 20);
  g_cursor.pos = (struct DoublePoint) { 9.75, 19.75 };

  core_handleMouseNormal(-0.25, -0.25);

  const int drop = first(EV_UNGRAB);
  const int move = first(EV_WARP);
  CHECK(drop >= 0);
  CHECK(count(EV_WARP) == 1);
  CHECK(move > drop);
  CHECK(m.ev[move].x == 9);
  CHECK(m.ev[move].y == 19);
  CHECK(m.ev[move].exit);
  CHECK(!g_cursor.inView);

  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 0);
}

static void startExit(void)
{
  reset();
  setLocal(109, 50);
  m.conf.req  = true;
  m.conf.on   = true;
  m.conf.hold = true;

  core_handleMouseNormal(2, 0);
}

static void testExitWait(void)
{
  startExit();
  const int drop = first(EV_UNGRAB);
  CHECK(drop >= 0);
  CHECK(count(EV_WARP) == 0);
  CHECK(g_cursor.inView);
}

static void testExitGuest(void)
{
  startExit();
  g_cursor.guest.x = 99;
  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 0);
}

static void startConf(void)
{
  reset();
  setLocal(50, 50);
  g_cursor.inView = false;

  core_handleMouseNormal(0, 0);
}

static void testConfWait(void)
{
  startConf();
  CHECK(m.conf.req);
  CHECK(!m.conf.on);
  CHECK(!g_cursor.inView);
}

static void testConfGuest(void)
{
  startConf();
  core_handleGuestMouseUpdate();
  CHECK(m.conf.req);
  CHECK(count(EV_GUEST) == 0);
}

static void testCapWait(void)
{
  reset();
  setLocal(50, 50);
  g_params.captureInputOnly = true;

  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 0);

  core_setGrabQuiet(true);
  CHECK(g_cursor.grab);
  CHECK(m.lock.req);
  CHECK(!m.lock.on);

  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 0);
}

static void testCapRevoke(void)
{
  reset();
  setLocal(50, 50);
  g_params.captureInputOnly = true;
  core_setGrabQuiet(true);

  m.lock.on = true;
  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 1);
  CHECK(m.ev[first(EV_MOTION)].lock);

  m.lock.on = false;
  core_handleMouseGrabbed(1, 0);
  CHECK(g_cursor.grab);
  CHECK(count(EV_MOTION) == 1);
}

static void testCapRelease(void)
{
  reset();
  setLocal(50, 50);
  g_params.captureInputOnly = true;
  core_setGrabQuiet(true);

  m.lock.on = true;
  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 1);

  m.lock.hold = true;
  core_setGrabQuiet(false);
  CHECK(!g_cursor.grab);
  CHECK(!m.lock.req);
  CHECK(m.lock.on);

  const int drop = first(EV_UNCAPTURE);
  CHECK(drop >= 0);
  CHECK(m.ev[drop].lock);

  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 1);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] = {
  { "inset-exit"     , testInsetExit   },
  { "exit-delay"     , testExitWait    },
  { "exit-guest"     , testExitGuest   },
  { "confine-pending", testConfWait    },
  { "confine-guest"  , testConfGuest   },
  { "capture-pending", testCapWait     },
  { "capture-revoked", testCapRevoke   },
  { "capture-release", testCapRelease  },
};

int main(int argc, char ** argv)
{
  if (argc == 2)
  {
    for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
      if (strcmp(argv[1], tests[i].name) == 0)
      {
        tests[i].run();
        return 0;
      }

    fprintf(stderr, "unknown test: %s\n", argv[1]);
    return EXIT_FAILURE;
  }

  if (argc != 1)
    return EXIT_FAILURE;

  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    tests[i].run();
  return 0;
}
