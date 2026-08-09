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
#include "input.h"
#include "main.h"
#include "test.h"
#include "util.h"

#include "common/debug.h"

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
  {
    m.conf.on = false;
    core_handleGrabEvent(false);
  }
}

static bool grabbed(void)
{
  return m.conf.on;
}

static void capture(void)
{
  if (m.support == LG_DS_WARP_NONE)
  {
    grab();
    return;
  }

  m.conf.req = false;
  if (!m.conf.hold)
    m.conf.on = false;
  m.lock.req = true;
  push(EV_CAPTURE, 0, 0, false);
}

static void uncapture(void)
{
  if (m.support == LG_DS_WARP_NONE)
  {
    ungrab();
    return;
  }

  push(EV_UNCAPTURE, 0, 0, false);
  m.lock.req = false;
  if (!m.lock.hold)
    m.lock.on = false;
}

static bool captured(void)
{
  return m.support == LG_DS_WARP_NONE ? m.conf.on : m.lock.on;
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
  .isPointerGrabbed    = grabbed,
  .capturePointer      = capture,
  .uncapturePointer    = uncapture,
  .isPointerCaptured   = captured,
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

static bool inputSupports(void * opaque, LG_InputSupport support)
{
  (void)opaque;
  (void)support;
  return false;
}

static bool inputKey(void * opaque, int key)
{
  (void)opaque;
  (void)key;
  return true;
}

static bool inputMouseMotion(void * opaque, int32_t x, int32_t y)
{
  (void)opaque;
  push(EV_MOTION, x, y, false);
  return true;
}

static bool inputMouseButton(void * opaque, unsigned int button)
{
  (void)opaque;
  (void)button;
  return true;
}

static const LG_InputOps inputOps = {
  .name         = "test",
  .supports     = inputSupports,
  .keyDown      = inputKey,
  .keyUp        = inputKey,
  .mouseMotion  = inputMouseMotion,
  .mousePress   = inputMouseButton,
  .mouseRelease = inputMouseButton,
};

static void reset(void)
{
  memset(&m, 0, sizeof(m));
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_cursor, 0, sizeof(g_cursor));
  memset(&g_params, 0, sizeof(g_params));

  m.support  = LG_DS_WARP_SURFACE;
  m.valid    = true;
  m.conf.req = true;
  m.conf.on  = true;

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

  g_params.scaleMouseInput = true;

  g_cursor.inWindow    = true;
  g_cursor.inView      = true;
  g_cursor.viewReq     = true;
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

static bool near(double a, double b)
{
  return fabs(a - b) < 0.000001;
}

static void testInsetExit(void)
{
  reset();
  setLocal(10, 20);
  g_cursor.pos = (struct DoublePoint) { 9.75, 19.75 };

  core_handleMouseNormal(-0.25, -0.25);

  CHECK(count(EV_UNGRAB) == 0);
  CHECK(count(EV_WARP) == 0);
  CHECK(near(g_cursor.exitPos.x, 9.75));
  CHECK(near(g_cursor.exitPos.y, 19.75));
  CHECK(g_cursor.exit);
  CHECK(!g_cursor.inView);
  CHECK(!g_cursor.viewReq);

  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 0);
}

static void setConf(bool active)
{
  m.conf.on = active;
  core_handleGrabEvent(active);
}

static void setSurfaceEdge(double x, double y)
{
  reset();
  g_state.windowW = 100;
  g_state.windowH = 80;
  g_state.dstRect = (LG_RendererRect) {
    .valid = true,
    .x     = 0,
    .y     = 0,
    .w     = 100,
    .h     = 80,
  };
  setLocal(x, y);
}

static void testSurfaceExit(void)
{
  setSurfaceEdge(99, 40);

  core_handleMouseNormal(1, 0);

  const int drop = first(EV_UNGRAB);
  const int warp = first(EV_WARP);
  const int move = first(EV_MOTION);
  CHECK(drop >= 0);
  CHECK(warp > drop);
  CHECK(move > warp);
  CHECK(m.ev[warp].x == 100);
  CHECK(m.ev[warp].y == 40);
  CHECK(m.ev[move].x == 1);
  CHECK(m.ev[move].y == 0);
  CHECK(g_cursor.surfaceExit);
  CHECK(g_cursor.inWindow);
  CHECK(g_cursor.inView);
  CHECK(g_cursor.viewReq);

  core_handleMouseNormal(1, 0);
  CHECK(count(EV_MOTION) == 2);
  CHECK(g_cursor.surfaceExit);
  CHECK(g_cursor.inWindow);
  CHECK(g_cursor.inView);
  CHECK(g_cursor.viewReq);

  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 1);

  setLocal(99, 40);
  core_handleMouseNormal(-1, 0);
  CHECK(m.conf.req);
  CHECK(g_cursor.surfaceExit);

  setConf(false);
  CHECK(g_cursor.inView);
  CHECK(g_cursor.surfaceExit);

  setConf(true);
  CHECK(g_cursor.inView);
  CHECK(!g_cursor.surfaceExit);

  setSurfaceEdge(0, 79);
  core_handleMouseNormal(-1, 1);
  CHECK(g_cursor.surfaceExit);
  CHECK(g_cursor.inWindow);
  CHECK(g_cursor.inView);
  CHECK(g_cursor.viewReq);
  CHECK(count(EV_MOTION) == 1);

  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 1);

  g_cursor.inWindow = false;
  core_setCursorInView(false);
  CHECK(!g_cursor.surfaceExit);
  CHECK(!g_cursor.inView);
  CHECK(!g_cursor.viewReq);
}

static void startExit(void)
{
  reset();
  setLocal(109, 50);

  core_handleMouseNormal(2, 0);
}

static void testExitImmediate(void)
{
  startExit();
  CHECK(count(EV_UNGRAB) == 0);
  CHECK(count(EV_WARP) == 0);
  CHECK(near(g_cursor.exitPos.x, 111));
  CHECK(near(g_cursor.exitPos.y, 50));
  CHECK(!g_cursor.inView);
  CHECK(!g_cursor.viewReq);
  CHECK(g_cursor.exit);
}

static void testExitGuest(void)
{
  startExit();
  g_cursor.guest.x = 99;
  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 0);
}

static void testExitReentry(void)
{
  startExit();
  CHECK(g_cursor.exit);
  CHECK(!g_cursor.inView);
  CHECK(!g_cursor.viewReq);

  setLocal(50, 50);
  core_handleMouseNormal(0, 0);
  CHECK(!g_cursor.exit);
  CHECK(g_cursor.viewReq);
  CHECK(g_cursor.inView);
  CHECK(count(EV_GRAB) == 0);
  CHECK(count(EV_UNGRAB) == 0);
  CHECK(count(EV_WARP) == 0);

  startExit();
  g_state.focused = false;
  core_setCursorInView(false);
  CHECK(!g_cursor.exit);
}

static void testViewImmediate(void)
{
  reset();
  setLocal(50, 50);
  m.conf.req       = false;
  m.conf.on        = false;
  g_cursor.inView  = false;
  g_cursor.viewReq = false;

  core_setCursorInView(true);
  CHECK(!m.conf.req);
  CHECK(!m.conf.on);
  CHECK(count(EV_GRAB) == 0);
  CHECK(count(EV_UNGRAB) == 0);
  CHECK(g_cursor.viewReq);
  CHECK(g_cursor.inView);

  core_handleGuestMouseUpdate();
  CHECK(count(EV_GUEST) == 1);
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

static void testCapAlign(void)
{
  reset();
  setLocal(50, 50);
  g_params.hideMouse = true;
  core_setGrabQuiet(true);

  m.lock.on = true;
  g_cursor.guest.x += 10;
  g_cursor.warpState = WARP_STATE_ON;
  m.count = 0;
  core_setGrabQuiet(false);

  const int drop = first(EV_UNCAPTURE);
  const int warp = first(EV_WARP);
  CHECK(drop >= 0);
  CHECK(warp > drop);
  CHECK(m.ev[warp].x == 60);
  CHECK(m.ev[warp].y == 50);
  CHECK(!m.ev[warp].exit);
}

static void testCapFallback(void)
{
  reset();
  setLocal(50, 50);
  m.support = LG_DS_WARP_NONE;
  g_params.captureInputOnly = true;
  core_setGrabQuiet(true);

  m.conf.on = true;
  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 1);

  m.conf.on = false;
  core_handleMouseGrabbed(1, 0);
  CHECK(count(EV_MOTION) == 1);
}

static void testRotateScale(void)
{
  reset();
  g_state.dstRect = (LG_RendererRect) {
    .valid = true,
    .x     = 10,
    .y     = 20,
    .w     = 200,
    .h     = 100,
  };
  g_cursor.scale   = (struct DoublePoint) { 2, 4 };
  g_cursor.guest.x = 40;
  g_cursor.guest.y = 20;

  for (int rot = 0; rot < LG_ROTATE_MAX; ++rot)
  {
    g_state.rotate = rot;

    struct DoublePoint local;
    CHECK(util_guestCurToLocal(&local));
    g_cursor.pos = local;

    struct DoublePoint guest;
    util_localCurToGuest(&guest);
    CHECK(near(guest.x, 40));
    CHECK(near(guest.y, 20));
  }
}

static void testGeometry(void)
{
  static const struct DoublePoint expected[] = {
    {  30,  25 },
    { 205,  40 },
    { 190, 115 },
    {  15, 100 },
  };
  static const struct DoublePoint rotated[] = {
    {  1,  2 },
    {  2, -1 },
    { -1, -2 },
    { -2,  1 },
  };

  reset();
  g_state.dstRect = (LG_RendererRect) {
    .valid = true,
    .x     = 10,
    .y     = 20,
    .w     = 200,
    .h     = 100,
  };
  g_cursor.scale   = (struct DoublePoint) { 2, 4 };
  g_cursor.guest.x = 40;
  g_cursor.guest.y = 20;

  for (int rot = 0; rot < LG_ROTATE_MAX; ++rot)
  {
    g_state.rotate = rot;

    struct DoublePoint local;
    CHECK(util_guestCurToLocal(&local));
    CHECK(near(local.x, expected[rot].x));
    CHECK(near(local.y, expected[rot].y));

    struct DoublePoint delta = { 1, 2 };
    util_rotatePoint(&delta);
    CHECK(near(delta.x, rotated[rot].x));
    CHECK(near(delta.y, rotated[rot].y));
  }

  static const double scales[] = { 0.5, 1.0, 2.0 };
  for (unsigned int i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i)
    for (int rot = 0; rot < LG_ROTATE_MAX; ++rot)
    {
      g_state.rotate    = rot;
      g_cursor.scale.x = scales[i];
      g_cursor.scale.y = scales[i];
      g_cursor.guest.x = 40;
      g_cursor.guest.y = 20;

      struct DoublePoint local;
      CHECK(util_guestCurToLocal(&local));
      g_cursor.pos = local;

      struct DoublePoint round;
      util_localCurToGuest(&round);
      CHECK(near(round.x, 40));
      CHECK(near(round.y, 20));
    }

  g_cursor.guest.valid = false;
  struct DoublePoint local = { -1, -2 };
  CHECK(!util_guestCurToLocal(&local));
  CHECK(near(local.x, -1));
  CHECK(near(local.y, -2));
}

static struct DoublePoint inputFor(int rot, double x, double y)
{
  switch (rot)
  {
    case LG_ROTATE_0:
      return (struct DoublePoint) { x, y };
    case LG_ROTATE_90:
      return (struct DoublePoint) { -y, x };
    case LG_ROTATE_180:
      return (struct DoublePoint) { -x, -y };
    case LG_ROTATE_270:
      return (struct DoublePoint) { y, -x };
  }

  CHECK(false);
  return (struct DoublePoint) { 0, 0 };
}

static void testEdges(void)
{
  static const struct
  {
    struct Point pos;
    struct Point delta;
    struct Point target;
  }
  cases[] = {
    { {  10, 60 }, { -1,  0 }, {   9,  60 } },
    { { 109, 60 }, {  1,  0 }, { 110,  60 } },
    { {  60, 20 }, {  0, -1 }, {  60,  19 } },
    { {  60, 99 }, {  0,  1 }, {  60, 100 } },
    { {  10, 20 }, { -1, -1 }, {   9,  19 } },
    { { 109, 20 }, {  1, -1 }, { 110,  19 } },
    { {  10, 99 }, { -1,  1 }, {   9, 100 } },
    { { 109, 99 }, {  1,  1 }, { 110, 100 } },
  };

  for (int rot = 0; rot < LG_ROTATE_MAX; ++rot)
    for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
    {
      reset();
      g_state.rotate = rot;
      setLocal(cases[i].pos.x, cases[i].pos.y);
      const struct DoublePoint input = inputFor(rot,
          cases[i].delta.x, cases[i].delta.y);

      core_handleMouseNormal(input.x, input.y);

      CHECK(count(EV_UNGRAB) == 0);
      CHECK(count(EV_WARP) == 0);
      CHECK(g_cursor.exit);
      CHECK(!g_cursor.inView);
      CHECK(!g_cursor.viewReq);
      CHECK(near(g_cursor.exitPos.x, cases[i].target.x));
      CHECK(near(g_cursor.exitPos.y, cases[i].target.y));
    }
}

static void testScales(void)
{
  static const struct
  {
    double scale;
    double pos;
    double delta;
    int    target;
  }
  cases[] = {
    { 0.5, 108, 4, 110 },
    { 1.0, 109, 1, 110 },
    { 2.0, 109, 1, 111 },
  };

  for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    reset();
    g_cursor.useScale = true;
    g_cursor.scale.x  = cases[i].scale;
    g_cursor.scale.y  = cases[i].scale;
    setLocal(cases[i].pos, 50);

    core_handleMouseNormal(cases[i].delta, 0);

    CHECK(count(EV_UNGRAB) == 0);
    CHECK(count(EV_WARP) == 0);
    CHECK(g_cursor.exit);
    CHECK(!g_cursor.inView);
    CHECK(!g_cursor.viewReq);
    CHECK(near(g_cursor.exitPos.x, cases[i].target));
    CHECK(near(g_cursor.exitPos.y, 50));
  }
}

static void testBorderExit(void)
{
  static const struct
  {
    struct Border border;
    struct Point  valid;
  }
  cases[] = {
    { { 8, 24, 8, 8 }, { 219, 274 } },
    { { 0,  0, 0, 0 }, { 211, 250 } },
  };

  for (unsigned int i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
  {
    reset();
    m.support         = LG_DS_WARP_SCREEN;
    g_state.windowPos = (struct Point) { 100, 200 };
    g_state.border    = cases[i].border;
    setLocal(109, 50);

    core_handleMouseNormal(2, 0);

    const int check = first(EV_VALID);
    CHECK(check >= 0);
    CHECK(m.ev[check].x == cases[i].valid.x);
    CHECK(m.ev[check].y == cases[i].valid.y);
    CHECK(count(EV_UNGRAB) == 0);
    CHECK(count(EV_WARP) == 0);
    CHECK(g_cursor.exit);
    CHECK(!g_cursor.inView);
    CHECK(!g_cursor.viewReq);
    CHECK(near(g_cursor.exitPos.x, 111));
    CHECK(near(g_cursor.exitPos.y, 50));
  }
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] = {
  { "inset-exit"      , testInsetExit    },
  { "surface-exit"    , testSurfaceExit  },
  { "exit-immediate"  , testExitImmediate},
  { "exit-guest"      , testExitGuest    },
  { "exit-reentry"    , testExitReentry  },
  { "view-immediate"  , testViewImmediate},
  { "capture-pending" , testCapWait      },
  { "capture-revoked" , testCapRevoke    },
  { "capture-release" , testCapRelease   },
  { "capture-align"   , testCapAlign     },
  { "capture-fallback", testCapFallback  },
  { "rotate-scale"    , testRotateScale  },
  { "geometry"        , testGeometry     },
  { "edges"           , testEdges        },
  { "scales"          , testScales       },
  { "border-exit"     , testBorderExit   },
};

int main(int argc, char ** argv)
{
  debug_init();
  lgInput_init();
  lgInput_setFallback(&inputOps, NULL);

  if (argc == 2)
  {
    for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
      if (strcmp(argv[1], tests[i].name) == 0)
      {
        tests[i].run();
        lgInput_free();
        return 0;
      }

    fprintf(stderr, "unknown test: %s\n", argv[1]);
    lgInput_free();
    return EXIT_FAILURE;
  }

  if (argc != 1)
  {
    lgInput_free();
    return EXIT_FAILURE;
  }

  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    tests[i].run();

  lgInput_free();
  return 0;
}
