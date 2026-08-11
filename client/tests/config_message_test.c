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

#include "config.h"
#include "kb.h"
#include "main.h"
#include "message.h"
#include "test.h"

#include "common/debug.h"
#include "common/option.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_CTL 16U
#define DEBOUNCE_US UINT64_C(500000)
#define TEST_ALARM_S 5U

struct LG_Transport
{
  int unused;
};

struct Trace
{
  uint64_t                 now;
  unsigned int             send;
  LG_TransportStatus       status;
  LG_TransportControl      ctl[MAX_CTL];
  LG_TransportControlToken token[MAX_CTL];
};

static struct Trace        t;
static struct LG_Transport tr;

struct AppState    g_state;
struct CursorState g_cursor;
struct AppParams   g_params;

const char * linux_to_str[KEY_MAX] =
{
  [KEY_A]         = "KEY_A",
  [KEY_RIGHTCTRL] = "KEY_RIGHTCTRL",
};

static const char * renAName(void)
{
  return "EGL";
}

static const char * renBName(void)
{
  return "OpenGL";
}

static LG_RendererOps renA =
{
  .getName = renAName,
};

static LG_RendererOps renB =
{
  .getName = renBName,
};

LG_RendererOps * LG_Renderers[] =
{
  &renA,
  &renB,
  NULL,
};

bool lgTransport_isValid(const char * name)
{
  return name &&
    (strcmp(name, "lgmp") == 0 || strcmp(name, "spice") == 0);
}

int clock_gettime(clockid_t id, struct timespec * ts)
{
  const uint64_t ns = t.now * UINT64_C(1000);
  ts->tv_sec         = ns / UINT64_C(1000000000);
  ts->tv_nsec        = ns % UINT64_C(1000000000);
  return 0;
}

static LG_TransportStatus sendCtl(LG_Transport * transport,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  CHECK(transport == &tr);
  CHECK(control);
  CHECK(token);
  CHECK(t.send < MAX_CTL);

  const unsigned int no = t.send++;
  t.ctl[no]              = *control;
  t.token[no]            = no + 1;
  *token                 = t.token[no];
  return t.status;
}

static const LG_TransportOps trOps =
{
  .name        = "fake",
  .sendControl = sendCtl,
};

static void msgInit(void)
{
  t.now                     = UINT64_C(1000000);
  t.status                  = LG_TRANSPORT_OK;
  g_state.transport.handle  = &tr;
  g_state.transport.ops     = &trOps;
  g_state.transportFeatures = LG_TRANSPORT_FEATURE_WINDOW_SIZE;
  g_state.srcSize           = (struct Point) { 1920, 1080 };
  CHECK(lgMessage_init());
}

static void post(unsigned int width, unsigned int height)
{
  const LGMsg msg =
  {
    .type       = LG_MSG_WINDOWSIZE,
    .windowSize =
    {
      .width  = width,
      .height = height,
    },
  };
  lgMessage_post(&msg);
}

static void step(uint64_t us)
{
  t.now += us;
}

static void mature(void)
{
  step(DEBOUNCE_US);
  lgMessage_process();
}

static void checkCtl(unsigned int no, unsigned int width,
    unsigned int height)
{
  CHECK(no < t.send);
  CHECK(t.ctl[no].type == LG_TRANSPORT_CONTROL_WINDOW_SIZE);
  CHECK(t.ctl[no].windowSize.width == width);
  CHECK(t.ctl[no].windowSize.height == height);
  CHECK(t.token[no] == no + 1);
}

static void testDebounce(void)
{
  msgInit();
  post(800, 600);
  lgMessage_process();
  CHECK(t.send == 0);
  step(DEBOUNCE_US - 1);
  lgMessage_process();
  CHECK(t.send == 0);
  step(1);
  lgMessage_process();
  CHECK(t.send == 1);
  checkCtl(0, 800, 600);

  post(800, 600);
  mature();
  CHECK(t.send == 1);
  post(801, 600);
  mature();
  CHECK(t.send == 2);
  checkCtl(1, 801, 600);
  lgMessage_deinit();
}

static void testLatest(void)
{
  msgInit();
  post(800, 600);
  step(100);
  post(1024, 768);
  lgMessage_process();
  CHECK(t.send == 0);

  step(DEBOUNCE_US - 1);
  lgMessage_process();
  CHECK(t.send == 0);
  step(1);
  lgMessage_process();
  CHECK(t.send == 1);
  checkCtl(0, 1024, 768);
  lgMessage_deinit();
}

static void testEqual(void)
{
  msgInit();
  post(800, 600);
  post(1024, 768);
  mature();

  CHECK(t.send == 1);
  /* Posting order decides when timestamps are equal. */
  checkCtl(0, 1024, 768);
  lgMessage_deinit();
}

static void testReinit(void)
{
  msgInit();
  post(640, 480);
  lgMessage_deinit();
  CHECK(lgMessage_init());
  mature();
  CHECK(t.send == 0);

  post(640, 480);
  mature();
  CHECK(t.send == 1);
  lgMessage_deinit();
  CHECK(lgMessage_init());
  post(640, 480);
  mature();

  /* A new message instance must not inherit its old duplicate filter. */
  CHECK(t.send == 2);
  checkCtl(1, 640, 480);
  lgMessage_deinit();
}

static void testControl(void)
{
  msgInit();
  g_state.transportFeatures = 0;
  post(100, 100);
  mature();
  CHECK(t.send == 0);

  g_state.transportFeatures = LG_TRANSPORT_FEATURE_WINDOW_SIZE;
  g_state.transport.handle  = NULL;
  post(200, 200);
  mature();
  CHECK(t.send == 0);

  g_state.transport.handle = &tr;
  g_state.srcSize          = (struct Point) { 300, 300 };
  post(300, 300);
  mature();
  CHECK(t.send == 0);

  t.status = LG_TRANSPORT_ERROR;
  post(400, 400);
  mature();
  CHECK(t.send == 1);
  checkCtl(0, 400, 400);
  step(DEBOUNCE_US);
  lgMessage_process();
  CHECK(t.send == 1);

  t.status = LG_TRANSPORT_UNAVAILABLE;
  post(500, 500);
  mature();
  CHECK(t.send == 2);
  checkCtl(1, 500, 500);

  t.status = LG_TRANSPORT_OK;
  post(600, 600);
  mature();
  CHECK(t.send == 3);
  checkCtl(2, 600, 600);
  lgMessage_deinit();
}

static struct Option * opt(const char * module, const char * name)
{
  struct Option * result = option_get(module, name);
  CHECK(result);
  return result;
}

static void checkStr(char * value, const char * expected)
{
  CHECK(value);
  CHECK(strcmp(value, expected) == 0);
  free(value);
}

static void testCfgValues(void)
{
  config_init();

  struct Option * pos = opt("win", "position");
  CHECK(pos->parser(pos, "center"));
  CHECK(g_params.center);
  checkStr(pos->toString(pos), "center");
  CHECK(pos->parser(pos, "-10x20"));
  CHECK(!g_params.center);
  CHECK(g_params.x == -10);
  CHECK(g_params.y == 20);
  checkStr(pos->toString(pos), "-10x20");

  struct Option * size = opt("win", "size");
  CHECK(size->parser(size, "1920x1080"));
  CHECK(g_params.w == 1920);
  CHECK(g_params.h == 1080);
  checkStr(size->toString(size), "1920x1080");

  struct Option * mic = opt("audio", "micDefault");
  CHECK(mic->parser(mic, "ALLOW"));
  CHECK(g_params.micDefaultState == MIC_DEFAULT_ALLOW);
  checkStr(mic->toString(mic), "allow");
  CHECK(!mic->parser(mic, "sometimes"));

  struct Option * resampler = opt("audio", "resampler");
  CHECK(resampler->parser(resampler, "BACKEND"));
  CHECK(g_params.audioResampler == AUDIO_RESAMPLER_BACKEND);
  checkStr(resampler->toString(resampler), "backend");
  CHECK(!resampler->parser(resampler, "linear"));

  struct Option * renderer = opt("app", "renderer");
  CHECK(renderer->parser(renderer, "OpenGL"));
  CHECK(g_params.forceRenderer);
  CHECK(g_params.forceRendererIndex == 1);
  checkStr(renderer->toString(renderer), "OpenGL");
  CHECK(renderer->parser(renderer, "auto"));
  CHECK(!g_params.forceRenderer);
  checkStr(renderer->toString(renderer), "auto");

  struct Option * key = opt("input", "escapeKey");
  CHECK(key->parser(key, "KEY_A"));
  CHECK(key->value.x_int == KEY_A);
  char * keyName = key->toString(key);
  CHECK(keyName);
  CHECK(strstr(keyName, "KEY_A"));
  free(keyName);

  struct Option * rotate = opt("win", "rotate");
  const char * error = NULL;
  rotate->value.x_int = 270;
  CHECK(rotate->validator(rotate, &error));
  rotate->value.x_int = 45;
  CHECK(!rotate->validator(rotate, &error));
  CHECK(error);

  config_free();
}

static void testCfgPos(void)
{
  config_init();
  struct Option * pos = opt("win", "position");
  CHECK(!pos->parser(pos, NULL));
  CHECK(!pos->parser(pos, "10"));
  CHECK(!pos->parser(pos, "10x20tail"));
  config_free();
}

static void testCfgSize(void)
{
  config_init();
  struct Option * size = opt("win", "size");
  CHECK(!size->parser(size, NULL));
  CHECK(!size->parser(size, "0x20"));
  CHECK(!size->parser(size, "-1x20"));
  CHECK(!size->parser(size, "10x20tail"));
  config_free();
}

static void loadCfg(const char * text)
{
  char path[] = "/tmp/lg-config-test-XXXXXX";
  const int fd = mkstemp(path);
  CHECK(fd >= 0);
  const size_t size = strlen(text);
  CHECK(write(fd, text, size) == (ssize_t)size);
  CHECK(close(fd) == 0);
  CHECK(option_load(path));
  CHECK(unlink(path) == 0);
}

static void testCfgPrecedence(void)
{
  config_init();
  loadCfg("[win]\nsize=800x600\nposition=10x20\n");
  CHECK(g_params.w == 800);
  CHECK(g_params.h == 600);
  CHECK(g_params.x == 10);
  CHECK(g_params.y == 20);

  char * argv[] =
  {
    "test",
    "win:size=1024x768",
    "win:position=center",
  };
  CHECK(option_parse(3, argv));
  CHECK(g_params.w == 1024);
  CHECK(g_params.h == 768);
  CHECK(g_params.center);

  loadCfg("[win]\nsize=1280x720\nposition=30x40\n");
  CHECK(g_params.w == 1280);
  CHECK(g_params.h == 720);
  CHECK(g_params.x == 30);
  CHECK(g_params.y == 40);
  CHECK(!g_params.center);
  CHECK(option_validate());
  config_free();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "debounce"      , testDebounce      },
  { "latest"        , testLatest        },
  { "equal"         , testEqual         },
  { "reinit"        , testReinit        },
  { "control"       , testControl       },
  { "cfg-values"    , testCfgValues     },
  { "cfg-position"  , testCfgPos        },
  { "cfg-size"      , testCfgSize       },
  { "cfg-precedence", testCfgPrecedence },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  alarm(TEST_ALARM_S);
  debug_init();
  for (unsigned int i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
