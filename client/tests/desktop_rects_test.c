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

#include "desktop_rects.h"
#include "test.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

static void checkRect(const struct Rect * rect,
    int x, int y, int w, int h)
{
  CHECK(rect->x == x);
  CHECK(rect->y == y);
  CHECK(rect->w == w);
  CHECK(rect->h == h);
}

static void testRotations(void)
{
  static const struct Rect expected[] =
  {
    { .x =  20, .y = 70, .w = 40, .h = 20 },
    { .x = 140, .y = 70, .w = 40, .h = 20 },
    { .x = 140, .y = 10, .w = 40, .h = 20 },
    { .x =  20, .y = 10, .w = 40, .h = 20 },
  };
  const FrameDamageRect damage =
  {
    .x      = 10,
    .y      = 5,
    .width  = 20,
    .height = 10,
  };
  const FrameDamageRect full =
  {
    .width  = 100,
    .height = 50,
  };

  for (unsigned int i = 0; i < LG_ROTATE_MAX; ++i)
  {
    double matrix[6];
    egl_desktopToScreenMatrix(matrix, 100, 50, 0.0, 0.0, 1.0, 1.0,
        (LG_RendererRotate)i, 200.0, 100.0);

    const struct Rect rect = egl_desktopToScreen(matrix, &damage);
    checkRect(&rect, expected[i].x, expected[i].y,
        expected[i].w, expected[i].h);

    const struct Rect screen = egl_desktopToScreen(matrix, &full);
    checkRect(&screen, 0, 0, 200, 100);
  }
}

static void testFractional(void)
{
  double matrix[6];
  egl_desktopToScreenMatrix(matrix, 3, 2, 0.0, 0.0, 1.0, 1.0,
      LG_ROTATE_0, 10.0, 7.0);
  const FrameDamageRect damage =
  {
    .x      = 1,
    .width  = 1,
    .height = 1,
  };
  const struct Rect rect = egl_desktopToScreen(matrix, &damage);
  checkRect(&rect, 3, 3, 4, 4);

  egl_desktopToScreenMatrix(matrix, 100, 50, 0.1, -0.2, 0.5, 0.75,
      LG_ROTATE_0, 10.0, 7.0);
  const FrameDamageRect full =
  {
    .width  = 100,
    .height = 50,
  };
  const struct Rect scaled = egl_desktopToScreen(matrix, &full);
  checkRect(&scaled, 3, 0, 5, 6);
}

static void testRoundTrip(void)
{
  const FrameDamageRect damage =
  {
    .x      = 10,
    .y      = 5,
    .width  = 20,
    .height = 10,
  };

  for (unsigned int i = 0; i < LG_ROTATE_MAX; ++i)
  {
    double forward[6];
    double inverse[6];
    egl_desktopToScreenMatrix(forward, 100, 50, 0.0, 0.0, 1.0, 1.0,
        (LG_RendererRotate)i, 200.0, 100.0);
    egl_screenToDesktopMatrix(inverse, 100, 50, 0.0, 0.0, 1.0, 1.0,
        (LG_RendererRotate)i, 200.0, 100.0);

    const struct Rect screen = egl_desktopToScreen(forward, &damage);
    FrameDamageRect result;
    CHECK(egl_screenToDesktop(&result, inverse, &screen, 100, 50));
    CHECK(result.x <= damage.x);
    CHECK(result.y <= damage.y);
    CHECK(result.x + result.width >= damage.x + damage.width);
    CHECK(result.y + result.height >= damage.y + damage.height);
  }
}

static void testClipping(void)
{
  double matrix[6];
  egl_screenToDesktopMatrix(matrix, 100, 50, 0.0, 0.0, 1.0, 1.0,
      LG_ROTATE_0, 200.0, 100.0);

  const struct Rect outside =
  {
    .x = 300,
    .y = 40,
    .w = 10,
    .h = 10,
  };
  FrameDamageRect result;
  CHECK(!egl_screenToDesktop(&result, matrix, &outside, 100, 50));

  const struct Rect edge =
  {
    .x = 198,
    .y = 40,
    .w = 10,
    .h = 10,
  };
  CHECK(egl_screenToDesktop(&result, matrix, &edge, 100, 50));
  CHECK(result.x == 98);
  CHECK(result.y == 24);
  CHECK(result.width == 2);
  CHECK(result.height == 7);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "rotations" , testRotations  },
  { "fractional", testFractional },
  { "round-trip", testRoundTrip  },
  { "clipping"  , testClipping   },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

  for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
