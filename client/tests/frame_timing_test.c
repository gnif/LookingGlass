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

#define main lgClientMain
#include "../src/main.c"
#undef main

#include <math.h>

#define TEST_ALARM_S 5U

struct Feedback
{
  unsigned int count;
  uint64_t     serial;
  uint32_t     generation;
  uint32_t     epoch;
  uint32_t     deadline;
  uint64_t     phase;
};

static struct Feedback feed;

void frameScheduler_feedback(uint64_t frameSerial, uint32_t generation,
    uint32_t scheduleEpoch, uint32_t deadlineSerial,
    uint64_t measuredPhase)
{
  ++feed.count;
  feed.serial     = frameSerial;
  feed.generation = generation;
  feed.epoch      = scheduleEpoch;
  feed.deadline   = deadlineSerial;
  feed.phase      = measuredPhase;
}

static LG_TransportFrameTiming srcTiming(bool valid, bool phase)
{
  return (LG_TransportFrameTiming)
  {
    .valid                  = valid,
    .phaseValid             = phase,
    .scheduleGeneration     = 2,
    .scheduleEpoch          = 3,
    .scheduleDeadlineSerial = 4,
    .captureTime            = 11,
    .postProcessTime        = 12,
    .copyTime               = 13,
    .readyTime              = 14,
    .holdTime               = 15,
    .readyLeadTime          = 100,
  };
}

static LG_RendererFrameTiming dstTiming(LG_RendererFrameToken token,
    bool tracked)
{
  return (LG_RendererFrameTiming)
  {
    .frameToken     = token,
    .setupTime      = 21,
    .effectsTime    = 22,
    .desktopTime    = 23,
    .composeTime    = 24,
    .swapTime       = 25,
    .presentTracked = tracked,
  };
}

static void start(void)
{
  memset(&g_state, 0, sizeof(g_state));
  memset(&feed, 0, sizeof(feed));
  g_state.frameLatency = ringbuffer_new(16, sizeof(OverlayFrameTiming));
  CHECK(g_state.frameLatency);
  frameTimingInit();
}

static void finish(void)
{
  ringbuffer_free(&g_state.frameLatency);
  LG_LOCK_FREE(l_frameTiming.lock);
}

static OverlayFrameTiming take(void)
{
  OverlayFrameTiming out;
  CHECK(ringbuffer_consume(g_state.frameLatency, &out, 1) == 1);
  return out;
}

static bool near(float value, float expected)
{
  return fabsf(value - expected) < 0.000001f;
}

static void queue(LG_RendererFrameToken token,
    const LG_TransportFrameTiming * timing, uint64_t serial)
{
  frameTimingQueue(token, serial, timing, 10, 5, 100, 150);
}

static void testLifecycle(void)
{
  start();
  CHECK(frameTimingQueuedToken() == LG_RENDERER_FRAME_TOKEN_NONE);

  const LG_TransportFrameTiming timing = srcTiming(true, true);
  const LG_RendererFrameToken   first  = frameTimingReserve();
  CHECK(first == 1);
  queue(first, &timing, 10);
  CHECK(frameTimingQueuedToken() == first);
  CHECK(frameTimingRecord(first)->dispatchTime == 35);
  frameTimingCancel(first);
  app_handleFramePresented(first, 200, true);
  CHECK(frameTimingRecord(first)->token == LG_RENDERER_FRAME_TOKEN_NONE);

  const LG_RendererFrameToken second = frameTimingReserve();
  CHECK(second == 2);
  frameTimingReset();
  CHECK(frameTimingQueuedToken() == LG_RENDERER_FRAME_TOKEN_NONE);
  CHECK(frameTimingRecord(second)->token == LG_RENDERER_FRAME_TOKEN_NONE);
  CHECK(frameTimingReserve() == 3);
  finish();
}

static void testOrder(void)
{
  start();
  const LG_TransportFrameTiming timing = srcTiming(true, true);
  const LG_RendererFrameToken   token  = frameTimingReserve();
  queue(token, &timing, 55);

  app_handleFramePresented(token, 500, true);
  const LG_RendererFrameTiming render = dstTiming(token, true);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 0);

  frameTimingFinishFrame(token, &timing);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  const OverlayFrameTiming out = take();
  CHECK(out.validMask == OVERLAY_FRAME_TIMING_VALID_ALL);
  CHECK(near(out.capture, 0.000011f));
  CHECK(near(out.dispatch, 0.000035f));
  CHECK(near(out.queue, 0.000050f));
  CHECK(near(out.present, 0.000500f));

  frameTimingFinishFrame(token, &timing);
  app_handleFramePresented(token, 600, true);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 0);
  finish();
}

static void testUntracked(void)
{
  start();
  const LG_TransportFrameTiming timing = srcTiming(false, true);
  const LG_RendererFrameToken   token  = frameTimingReserve();
  queue(token, &timing, 66);
  frameTimingFinishFrame(token, &timing);

  const LG_RendererFrameTiming render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  const OverlayFrameTiming out = take();
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_PRODUCER));
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_TRANSPORT));
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_PRESENT));
  CHECK(out.present == 0.0f);
  finish();
}

static void testTimeout(void)
{
  start();
  const LG_TransportFrameTiming timing = srcTiming(true, true);
  const LG_RendererFrameToken   token  = frameTimingReserve();
  queue(token, &timing, 77);
  frameTimingFinishFrame(token, &timing);

  const LG_RendererFrameTiming render = dstTiming(token, true);
  const uint64_t               now    = nanotime();
  frameTimingFinishRender(&render, 200, 7, now, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 0);
  frameTimingRecord(token)->presentDeadline = nanotime();
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  const OverlayFrameTiming out = take();
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_PRESENT));
  CHECK(out.present == 0.0f);
  finish();
}

static void testFifo(void)
{
  start();
  LG_TransportFrameTiming firstTiming  = srcTiming(true, true);
  LG_TransportFrameTiming secondTiming = srcTiming(true, true);
  const LG_RendererFrameToken first  = frameTimingReserve();
  const LG_RendererFrameToken second = frameTimingReserve();
  firstTiming.captureTime  = 81;
  secondTiming.captureTime = 82;
  queue(first, &firstTiming, 81);
  queue(second, &secondTiming, 82);

  const LG_RendererFrameTiming r1 = dstTiming(first, false);
  const LG_RendererFrameTiming r2 = dstTiming(second, false);
  frameTimingFinishRender(&r1, 200, 7, 301, first);
  frameTimingFinishRender(&r2, 210, 8, 302, second);
  frameTimingFinishFrame(second, &secondTiming);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 0);

  frameTimingFinishFrame(first, &firstTiming);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 2);
  const OverlayFrameTiming out1 = take();
  const OverlayFrameTiming out2 = take();
  CHECK(near(out1.capture, 0.000081f));
  CHECK(near(out2.capture, 0.000082f));
  finish();
}

static void testRetire(void)
{
  start();
  const LG_TransportFrameTiming timing = srcTiming(true, true);
  const LG_RendererFrameToken   old    = frameTimingReserve();
  const LG_RendererFrameToken   fresh  = frameTimingReserve();
  queue(old, &timing, 91);
  queue(fresh, &timing, 92);

  const LG_RendererFrameTiming render = dstTiming(fresh, false);
  frameTimingFinishRender(&render, 200, 7, 300, fresh);
  frameTimingFinishFrame(old, &timing);
  CHECK(frameTimingRecord(old)->token == LG_RENDERER_FRAME_TOKEN_NONE);

  frameTimingFinishFrame(fresh, &timing);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  (void)take();
  finish();
}

static void testFeedback(void)
{
  start();
  g_state.jitRender = true;
  const LG_TransportFrameTiming timing = srcTiming(true, true);
  LG_RendererFrameToken         token  = frameTimingReserve();
  queue(token, &timing, 101);
  LG_RendererFrameTiming render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  CHECK(feed.count == 1);
  CHECK(feed.serial == 101);
  CHECK(feed.generation == 2);
  CHECK(feed.epoch == 3);
  CHECK(feed.deadline == 4);
  CHECK(feed.phase == 50);

  token = frameTimingReserve();
  queue(token, &timing, 102);
  render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token - 1);
  CHECK(feed.count == 1);

  LG_TransportFrameTiming invalid = timing;
  invalid.phaseValid = false;
  token = frameTimingReserve();
  queue(token, &invalid, 103);
  render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  CHECK(feed.count == 1);

  invalid                  = timing;
  invalid.scheduleEpoch    = 0;
  token                    = frameTimingReserve();
  queue(token, &invalid, 104);
  render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  CHECK(feed.count == 1);

  token = frameTimingReserve();
  queue(token, &timing, 105);
  render = dstTiming(token, false);
  frameTimingFinishRender(&render, 149, 7, 300, token);
  CHECK(feed.count == 1);
  finish();
}

static void testWrap(void)
{
  start();
  l_frameTiming.nextToken = UINT64_MAX - 1;
  CHECK(frameTimingReserve() == UINT64_MAX);
  CHECK(frameTimingReserve() == 1);
  finish();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "lifecycle", testLifecycle },
  { "order"    , testOrder     },
  { "untracked", testUntracked },
  { "timeout"  , testTimeout   },
  { "fifo"     , testFifo      },
  { "retire"   , testRetire    },
  { "feedback" , testFeedback  },
  { "wrap"     , testWrap      },
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
  for (size_t i = 0; i < ARRAY_LENGTH(tests); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      tests[i].run();
      return 0;
    }

  fprintf(stderr, "unknown test: %s\n", argv[1]);
  return EXIT_FAILURE;
}
