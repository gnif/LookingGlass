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

#include "main.h"
#include "render_queue.h"
#include "test.h"

#include "common/debug.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))

struct AppState g_state;

struct Fake
{
  LG_Renderer                 renderer;
  struct LG_DisplayServerOps  ds;

  bool                 prepResult;
  unsigned int         prepCount;
  RenderQueueSource    prepSource;
  uint64_t             prepGeneration;
  uint64_t             prepSerial;

  unsigned int         appliedCount;
  RenderQueueSource    appliedSource;
  uint64_t             appliedGeneration;
  uint64_t             appliedSerial;
  bool                 appliedSwSurface;

  unsigned int         invalidates;
  unsigned int         showCount;
  bool                 show;
  unsigned int         configCount;
  int                  configWidth;
  int                  configHeight;
  unsigned int         fillCount;
  int                  fillX;
  int                  fillY;
  int                  fillWidth;
  int                  fillHeight;
  uint32_t             fillColor;
  unsigned int         bitmapCount;
  int                  bitmapX;
  int                  bitmapY;
  int                  bitmapWidth;
  int                  bitmapHeight;
  int                  bitmapStride;
  bool                 bitmapTopDown;
  uint8_t              bitmap[64];
  size_t               bitmapSize;

  unsigned int          shapeCount;
  LG_RendererCursor     shapeType;
  int                   shapeWidth;
  int                   shapeHeight;
  int                   shapePitch;
  uint8_t               shape[64];
  size_t                shapeSize;
  unsigned int          cursorCount;
  bool                  cursorVisible;
  int                   cursorX;
  int                   cursorY;
  int                   cursorHX;
  int                   cursorHY;
  unsigned int          colorCount;
  LGColorTransformFlags colorFlags;
  float                 colorScalar;
  float                 colorMatrix;
  float                 colorLut;
  unsigned int          whiteCount;
  uint32_t              whiteLevel;

  unsigned int         hdrCount;
  LG_RendererFormat    hdr[8];
};

static struct Fake f;

void app_invalidateWindow(bool full)
{
  (void)full;
  ++f.invalidates;
}

static bool prep(void * opaque, RenderQueueSource source,
    uint64_t generation, uint64_t serial)
{
  struct Fake * fake   = opaque;
  fake->prepCount++;
  fake->prepSource     = source;
  fake->prepGeneration = generation;
  fake->prepSerial     = serial;
  return fake->prepResult;
}

static void applied(void * opaque, RenderQueueSource source,
    uint64_t generation, uint64_t serial, bool swSurface)
{
  struct Fake * fake      = opaque;
  fake->appliedCount++;
  fake->appliedSource     = source;
  fake->appliedGeneration = generation;
  fake->appliedSerial     = serial;
  fake->appliedSwSurface  = swSurface;
}

static void swShow(LG_Renderer * renderer, bool show)
{
  (void)renderer;
  ++f.showCount;
  f.show = show;
}

static void swConfig(LG_Renderer * renderer, int width, int height)
{
  (void)renderer;
  ++f.configCount;
  f.configWidth  = width;
  f.configHeight = height;
}

static void swFill(LG_Renderer * renderer, int x, int y,
    int width, int height, uint32_t color)
{
  (void)renderer;
  ++f.fillCount;
  f.fillX      = x;
  f.fillY      = y;
  f.fillWidth  = width;
  f.fillHeight = height;
  f.fillColor  = color;
}

static void swBitmap(LG_Renderer * renderer, int x, int y,
    int width, int height, int stride, uint8_t * data, bool topDown)
{
  (void)renderer;
  ++f.bitmapCount;
  f.bitmapX       = x;
  f.bitmapY       = y;
  f.bitmapWidth   = width;
  f.bitmapHeight  = height;
  f.bitmapStride  = stride;
  f.bitmapTopDown = topDown;
  f.bitmapSize    = (size_t)height * (size_t)stride;
  CHECK(f.bitmapSize <= sizeof(f.bitmap));
  memcpy(f.bitmap, data, f.bitmapSize);
}

static bool mouseShape(LG_Renderer * renderer, LG_RendererCursor type,
    int width, int height, int pitch, const uint8_t * data)
{
  (void)renderer;
  ++f.shapeCount;
  f.shapeType   = type;
  f.shapeWidth  = width;
  f.shapeHeight = height;
  f.shapePitch  = pitch;
  f.shapeSize   = (size_t)height * (size_t)pitch;
  CHECK(f.shapeSize <= sizeof(f.shape));
  memcpy(f.shape, data, f.shapeSize);
  return true;
}

static bool mouseEvent(LG_Renderer * renderer, bool visible,
    int x, int y, int hx, int hy)
{
  (void)renderer;
  ++f.cursorCount;
  f.cursorVisible = visible;
  f.cursorX       = x;
  f.cursorY       = y;
  f.cursorHX      = hx;
  f.cursorHY      = hy;
  return true;
}

static void mouseColor(LG_Renderer * renderer,
    const LGColorTransform * color)
{
  (void)renderer;
  ++f.colorCount;
  f.colorFlags  = color->flags;
  f.colorScalar = color->scalar;
  f.colorMatrix = color->matrix[0][0];
  f.colorLut    = color->lut[0][0];
}

static void mouseWhite(LG_Renderer * renderer, uint32_t level)
{
  (void)renderer;
  ++f.whiteCount;
  f.whiteLevel = level;
}

static bool setHDR(const void * data)
{
  CHECK(f.hdrCount < ARRAY_LENGTH(f.hdr));
  f.hdr[f.hdrCount++] = *(const LG_RendererFormat *)data;
  return true;
}

static void start(void)
{
  memset(&f, 0, sizeof(f));
  memset(&g_state, 0, sizeof(g_state));

  f.prepResult = true;
  f.renderer.ops.onMouseShape          = mouseShape;
  f.renderer.ops.onMouseEvent          = mouseEvent;
  f.renderer.ops.onMouseColorTransform = mouseColor;
  f.renderer.ops.onMouseWhiteLevel     = mouseWhite;
  f.renderer.ops.swSurfaceConfigure    = swConfig;
  f.renderer.ops.swSurfaceDrawFill     = swFill;
  f.renderer.ops.swSurfaceDrawBitmap   = swBitmap;
  f.renderer.ops.swSurfaceShow         = swShow;
  f.ds.setHDRImageDesc                 = setHDR;
  g_state.lgr                          = &f.renderer;
  g_state.ds                           = &f.ds;

  renderQueue_init();
  renderQueue_setSourceFns(prep, applied, &f);
}

static void stop(void)
{
  renderQueue_setSourceFns(NULL, NULL, NULL);
  renderQueue_free();
}

static void testGeneration(void)
{
  start();

  const uint64_t generation =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  CHECK(generation != 0);

  renderQueue_sourceSwSurfaceDrawFill(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 1, 2, 3, 4, 5);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        generation, true, NULL) != 0);
  renderQueue_sourceInvalidate(RENDER_QUEUE_SOURCE_PRIMARY, generation);
  renderQueue_process();
  renderQueue_presented();

  CHECK(f.fillCount == 0);
  CHECK(f.prepCount == 0);
  CHECK(f.appliedCount == 0);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        generation, false, NULL) == 0);

  const uint64_t current =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  CHECK(current != 0);
  CHECK(current != generation);
  renderQueue_sourceInvalidate(RENDER_QUEUE_SOURCE_PRIMARY, generation);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        current, false, NULL) != 0);
  renderQueue_process();
  CHECK(f.prepCount == 1);
  CHECK(f.prepGeneration == current);

  stop();
}

static void testLatest(void)
{
  start();

  const uint64_t primary =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  const uint64_t fallback =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_FALLBACK);
  atomic_uint_least64_t published;
  atomic_init(&published, 0);

  const uint64_t old = renderQueue_sourceTransition(
      RENDER_QUEUE_SOURCE_PRIMARY, primary, false, &published);
  CHECK(old != 0);
  CHECK(atomic_load(&published) == old);
  const uint64_t latest = renderQueue_sourceTransition(
      RENDER_QUEUE_SOURCE_FALLBACK, fallback, true, &published);
  CHECK(latest > old);
  CHECK(atomic_load(&published) == latest);

  renderQueue_process();
  CHECK(f.prepCount == 1);
  CHECK(f.prepSource == RENDER_QUEUE_SOURCE_FALLBACK);
  CHECK(f.prepGeneration == fallback);
  CHECK(f.prepSerial == latest);
  CHECK(f.showCount == 1);
  CHECK(f.show);

  renderQueue_presented();
  CHECK(f.appliedCount == 1);
  CHECK(f.appliedSource == RENDER_QUEUE_SOURCE_FALLBACK);
  CHECK(f.appliedGeneration == fallback);
  CHECK(f.appliedSerial == latest);
  CHECK(f.appliedSwSurface);

  stop();
}

static void testPrepare(void)
{
  start();

  const uint64_t generation =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  f.prepResult = false;
  CHECK(renderQueue_sourceSwSurfaceConfigureTransition(
        RENDER_QUEUE_SOURCE_PRIMARY, generation, 640, 480, NULL) != 0);
  renderQueue_process();
  renderQueue_presented();

  CHECK(f.prepCount == 1);
  CHECK(f.configCount == 0);
  CHECK(f.fillCount == 0);
  CHECK(f.showCount == 0);
  CHECK(f.appliedCount == 0);

  f.prepResult = true;
  const uint64_t serial = renderQueue_sourceSwSurfaceConfigureTransition(
      RENDER_QUEUE_SOURCE_PRIMARY, generation, 320, 200, NULL);
  CHECK(serial != 0);
  renderQueue_process();
  CHECK(f.configCount == 1);
  CHECK(f.configWidth == 320);
  CHECK(f.configHeight == 200);
  CHECK(f.fillCount == 1);
  CHECK(f.fillX == 0);
  CHECK(f.fillY == 0);
  CHECK(f.fillWidth == 320);
  CHECK(f.fillHeight == 200);
  CHECK(f.fillColor == 0);
  CHECK(f.showCount == 1);
  CHECK(f.show);

  renderQueue_presented();
  CHECK(f.appliedCount == 1);
  CHECK(f.appliedSerial == serial);
  CHECK(f.appliedSwSurface);

  stop();
}

static void testPresented(void)
{
  start();

  const uint64_t primary =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  const uint64_t fallback =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_FALLBACK);
  const uint64_t first = renderQueue_sourceTransition(
      RENDER_QUEUE_SOURCE_PRIMARY, primary, false, NULL);
  CHECK(first != 0);
  renderQueue_process();
  CHECK(f.appliedCount == 0);

  const uint64_t latest = renderQueue_sourceTransition(
      RENDER_QUEUE_SOURCE_FALLBACK, fallback, false, NULL);
  CHECK(latest > first);
  renderQueue_process();
  CHECK(f.appliedCount == 0);

  renderQueue_presented();
  CHECK(f.appliedCount == 1);
  CHECK(f.appliedSource == RENDER_QUEUE_SOURCE_FALLBACK);
  CHECK(f.appliedGeneration == fallback);
  CHECK(f.appliedSerial == latest);
  renderQueue_presented();
  CHECK(f.appliedCount == 1);

  stop();
}

static void testPayload(void)
{
  start();

  const uint64_t generation =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  uint8_t bitmap[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  const uint8_t bitmapExpected[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  uint8_t shape[] = { 11, 12, 13, 14, 15, 16, 17, 18 };
  const uint8_t shapeExpected[] = { 11, 12, 13, 14, 15, 16, 17, 18 };
  LGColorTransform color =
  {
    .flags        = LG_COLOR_TRANSFORM_MATRIX | LG_COLOR_TRANSFORM_LUT,
    .matrix[0][0] = 2.0f,
    .scalar       = 3.0f,
    .lut[0][0]    = 4.0f,
  };

  renderQueue_sourceSwSurfaceDrawBitmap(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 5, 6, 2, 2, 4, bitmap, true);
  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 2, 2, 4, shape);
  renderQueue_sourceCursorColorTransform(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, &color);
  renderQueue_sourceCursorState(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      true, 20, 21, 1, 2);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        generation, false, NULL) != 0);

  memset(bitmap, 0, sizeof(bitmap));
  memset(shape, 0, sizeof(shape));
  color.flags        = 0;
  color.matrix[0][0] = 0.0f;
  color.scalar       = 0.0f;
  color.lut[0][0]    = 0.0f;
  renderQueue_process();

  CHECK(f.bitmapCount == 1);
  CHECK(f.bitmapX == 5);
  CHECK(f.bitmapY == 6);
  CHECK(f.bitmapWidth == 2);
  CHECK(f.bitmapHeight == 2);
  CHECK(f.bitmapStride == 4);
  CHECK(f.bitmapTopDown);
  CHECK(f.bitmapSize == sizeof(bitmapExpected));
  CHECK(memcmp(f.bitmap, bitmapExpected, sizeof(bitmapExpected)) == 0);
  CHECK(f.shapeCount == 1);
  CHECK(f.shapeType == LG_CURSOR_COLOR);
  CHECK(f.shapeWidth == 2);
  CHECK(f.shapeHeight == 2);
  CHECK(f.shapePitch == 4);
  CHECK(f.shapeSize == sizeof(shapeExpected));
  CHECK(memcmp(f.shape, shapeExpected, sizeof(shapeExpected)) == 0);
  CHECK(f.colorCount == 1);
  CHECK(f.colorFlags ==
      (LG_COLOR_TRANSFORM_MATRIX | LG_COLOR_TRANSFORM_LUT));
  CHECK(f.colorMatrix == 2.0f);
  CHECK(f.colorScalar == 3.0f);
  CHECK(f.colorLut == 4.0f);
  CHECK(f.cursorCount == 1);
  CHECK(f.cursorVisible);
  CHECK(f.cursorX == 20);
  CHECK(f.cursorY == 21);
  CHECK(f.cursorHX == 1);
  CHECK(f.cursorHY == 2);

  stop();
}

static void testValidation(void)
{
  start();

  const uint64_t generation =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  uint8_t data[16] = { 0 };

  renderQueue_sourceSwSurfaceDrawBitmap(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 0, 0, 0, 1, 4, data, false);
  renderQueue_sourceSwSurfaceDrawBitmap(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 0, 0, 1, 0, 4, data, false);
  renderQueue_sourceSwSurfaceDrawBitmap(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 0, 0, 1, 1, 0, data, false);
  renderQueue_sourceSwSurfaceDrawBitmap(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 0, 0, 1, 1, 4, NULL, false);
  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 0, 1, 4, data);
  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 1, 0, 4, data);
  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 1, 1, 0, data);
  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 1, 1, 4, NULL);
  renderQueue_sourceCursorState(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      true, 1, 2, 3, 4);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        generation, false, NULL) != 0);
  renderQueue_process();

  CHECK(f.bitmapCount == 0);
  CHECK(f.shapeCount == 0);
  CHECK(f.cursorCount == 1);
  CHECK(!f.cursorVisible);

  stop();
}

static void testCache(void)
{
  start();

  const uint64_t generation =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  const uint8_t shape[] = { 1, 2, 3, 4 };
  LGColorTransform color =
  {
    .flags        = LG_COLOR_TRANSFORM_MATRIX,
    .matrix[0][0] = 1.5f,
    .scalar       = 2.5f,
  };
  LG_RendererFormat format =
  {
    .type          = FRAME_TYPE_RGBA,
    .hdr           = true,
    .hdrPQ         = true,
    .hdrMetadata   = true,
    .screenWidth   = 1920,
    .screenHeight  = 1080,
    .sdrWhiteLevel = 250,
  };

  renderQueue_sourceCursorImage(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      LG_CURSOR_COLOR, 1, 1, 4, shape);
  renderQueue_sourceCursorState(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      true, 30, 31, 2, 3);
  renderQueue_sourceCursorColorTransform(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, &color);
  renderQueue_sourceCursorWhiteLevel(RENDER_QUEUE_SOURCE_PRIMARY,
      generation, 250);
  renderQueue_sourceSurfaceFormat(RENDER_QUEUE_SOURCE_PRIMARY, generation,
      format, true);
  renderQueue_process();
  CHECK(f.shapeCount == 0);
  CHECK(f.cursorCount == 0);
  CHECK(f.colorCount == 0);
  CHECK(f.whiteCount == 0);
  CHECK(f.hdrCount == 0);

  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        generation, false, NULL) != 0);
  renderQueue_process();
  CHECK(f.shapeCount == 1);
  CHECK(f.cursorCount == 1);
  CHECK(f.cursorVisible);
  CHECK(f.colorCount == 1);
  CHECK(f.colorScalar == 2.5f);
  CHECK(f.whiteCount == 1);
  CHECK(f.whiteLevel == 250);
  CHECK(f.hdrCount == 1);
  CHECK(f.hdr[0].type == FRAME_TYPE_RGBA);
  CHECK(f.hdr[0].hdr);
  CHECK(f.hdr[0].hdrPQ);
  CHECK(f.hdr[0].screenWidth == 1920);

  renderQueue_sourceInvalidate(RENDER_QUEUE_SOURCE_PRIMARY, generation);
  const uint64_t current =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  CHECK(current != generation);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        current, false, NULL) != 0);
  renderQueue_process();
  CHECK(f.shapeCount == 2);
  CHECK(f.cursorCount == 2);
  CHECK(f.cursorVisible);
  CHECK(f.cursorX == 30);
  CHECK(f.cursorY == 31);
  CHECK(f.cursorHX == 2);
  CHECK(f.cursorHY == 3);
  CHECK(f.colorCount == 2);
  CHECK(f.colorScalar == 2.5f);
  CHECK(f.whiteCount == 2);
  CHECK(f.whiteLevel == 250);
  CHECK(f.hdrCount == 2);
  CHECK(f.hdr[1].type == 0);
  CHECK(!f.hdr[1].hdr);

  renderQueue_sourceClearCursor(RENDER_QUEUE_SOURCE_PRIMARY);
  const uint64_t clear =
    renderQueue_sourceBegin(RENDER_QUEUE_SOURCE_PRIMARY);
  CHECK(renderQueue_sourceTransition(RENDER_QUEUE_SOURCE_PRIMARY,
        clear, false, NULL) != 0);
  renderQueue_process();
  CHECK(f.shapeCount == 2);
  CHECK(f.cursorCount == 3);
  CHECK(!f.cursorVisible);
  CHECK(f.colorCount == 3);
  CHECK(f.colorFlags == 0);
  CHECK(f.colorScalar == 0.0f);
  CHECK(f.whiteCount == 3);
  CHECK(f.whiteLevel == LG_SDR_WHITE_LEVEL_DEFAULT);

  stop();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "generation", testGeneration },
  { "latest"    , testLatest     },
  { "prepare"   , testPrepare    },
  { "presented" , testPresented  },
  { "payload"   , testPayload    },
  { "validation", testValidation },
  { "cache"     , testCache      },
};

int main(int argc, char ** argv)
{
  debug_init();

  if (argc == 2)
  {
    for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
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

  for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
    tests[i].run();
  return 0;
}
