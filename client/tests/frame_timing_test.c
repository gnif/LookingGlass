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
#include <pthread.h>

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
static unsigned int rawConnectCalls;
static unsigned int cancellableConnectCalls;
static bool         connectSawCancellation;

uint64_t renderQueue_sourceBegin(RenderQueueSource source)
{
  (void)source;
  return 1;
}

void renderQueue_sourceInvalidate(RenderQueueSource source,
    uint64_t generation)
{
  (void)source;
  (void)generation;
}

void renderQueue_sourceClearCursor(RenderQueueSource source)
{
  (void)source;
}

bool renderQueue_sourceSwSurfaceConfigure(RenderQueueSource source,
    uint64_t generation, int width, int height)
{
  (void)source;
  (void)generation;
  (void)width;
  (void)height;
  return true;
}

void renderQueue_sourceSwSurfaceDrawFill(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height,
    uint32_t color)
{
  (void)source;
  (void)generation;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  (void)color;
}

void renderQueue_sourceSwSurfaceDrawBitmap(RenderQueueSource source,
    uint64_t generation, int x, int y, int width, int height, int stride,
    const void * data, bool topDown)
{
  (void)source;
  (void)generation;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  (void)stride;
  (void)data;
  (void)topDown;
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

void renderQueue_sourceCursorImage(RenderQueueSource source,
    uint64_t generation, LG_RendererCursor type,
    int width, int height, int pitch, const void * data)
{
  (void)source;
  (void)generation;
  (void)type;
  (void)width;
  (void)height;
  (void)pitch;
  (void)data;
}

void renderQueue_sourceCursorColorTransform(RenderQueueSource source,
    uint64_t generation, const KVMFRColorTransform * transform)
{
  (void)source;
  (void)generation;
  (void)transform;
}

void renderQueue_sourceCursorWhiteLevel(RenderQueueSource source,
    uint64_t generation, uint32_t sdrWhiteLevel)
{
  (void)source;
  (void)generation;
  (void)sdrWhiteLevel;
}

void overlaySplash_show(bool show)
{
  (void)show;
}

bool lgTransportFallback_usable(const LG_TransportFallback * fallback)
{
  (void)fallback;
  return false;
}

bool lgTransportFallback_ready(const LG_TransportFallback * fallback)
{
  (void)fallback;
  return false;
}

bool lgTransportFallback_acceptsVideoEvents(
    const LG_TransportFallback * fallback)
{
  (void)fallback;
  return false;
}

bool lgTransportFallback_videoRequested(LG_TransportFallback * fallback)
{
  (void)fallback;
  return false;
}

void lgTransportFallback_requestVideoActive(
    LG_TransportFallback * fallback, bool active)
{
  (void)fallback;
  (void)active;
}

bool lgInput_available(void)
{
  return false;
}

bool core_inputEnabled(void)
{
  return false;
}

void core_alignToGuest(void) {}
void core_handleGuestMouseUpdate(void) {}
void core_setTitle(const char * title) { (void)title; }

enum RunState app_getState(void)
{
  return atomic_load_explicit(&p_appState, memory_order_acquire);
}

void app_setState(enum RunState state)
{
  atomic_store_explicit(&p_appState, state, memory_order_release);
}

void app_invalidateWindow(bool full)
{
  (void)full;
}

void app_refreshVideoSource(void) {}
void app_updateMouseState(void) {}

static bool cancelled(void * opaque)
{
  (void)opaque;
  return true;
}

static LG_TransportStatus rawConnect(LG_Transport * transport,
    LG_TransportSession * session)
{
  (void)transport;
  (void)session;
  ++rawConnectCalls;
  return LG_TRANSPORT_ERROR;
}

static LG_TransportStatus cancellableConnect(LG_Transport * transport,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * opaque)
{
  (void)transport;
  (void)session;
  ++cancellableConnectCalls;
  connectSawCancellation = cancelled && cancelled(opaque);
  return connectSawCancellation ? LG_TRANSPORT_DISCONNECTED :
    LG_TRANSPORT_OK;
}

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
    .providerValid          = true,
    .phaseValid             = phase,
    .scheduleGeneration     = 2,
    .scheduleEpoch          = 3,
    .scheduleDeadlineSerial = 4,
    .captureTime            = 11,
    .postProcessTime        = 12,
    .copyTime               = 13,
    .readyTime              = 14,
    .holdTime               = 15,
    .readyLeadTime          = 110,
    .receiveTime            = 2,
    .prepareTime            = 3,
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
  main_framePresented(first, 200, true);
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

  main_framePresented(token, 500, true);
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
  CHECK(near(out.transport, 0.000010f));
  CHECK(near(out.receive, 0.000002f));
  CHECK(near(out.providerPrepare, 0.000003f));
  CHECK(near(out.dispatch, 0.000035f));
  CHECK(near(out.queue, 0.000050f));
  CHECK(near(out.present, 0.000500f));

  frameTimingFinishFrame(token, &timing);
  main_framePresented(token, 600, true);
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
  CHECK((out.validMask & OVERLAY_FRAME_TIMING_VALID_PROVIDER) ==
      OVERLAY_FRAME_TIMING_VALID_PROVIDER);
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_PRESENT));
  CHECK(near(out.receive, 0.000002f));
  CHECK(near(out.providerPrepare, 0.000003f));
  CHECK(out.present == 0.0f);
  finish();
}

static void testAccounting(void)
{
  start();
  LG_TransportFrameTiming timing = srcTiming(true, true);
  timing.readyLeadTime = 1;
  timing.receiveTime   = UINT64_MAX;
  timing.prepareTime   = UINT64_MAX;
  const LG_RendererFrameToken token = frameTimingReserve();
  frameTimingQueue(token, 67, &timing, UINT64_MAX, UINT64_MAX, 100, 150);
  frameTimingFinishFrame(token, &timing);
  const LG_RendererFrameTiming render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  const OverlayFrameTiming out = take();
  CHECK(out.validMask & OVERLAY_FRAME_TIMING_VALID_TRANSPORT);
  CHECK((out.validMask & OVERLAY_FRAME_TIMING_VALID_PROVIDER) ==
      OVERLAY_FRAME_TIMING_VALID_PROVIDER);
  CHECK(out.transport == 0.0f);
  CHECK(out.import ==
      (float)frameTimingAdd(UINT64_MAX, UINT64_MAX) * 1e-6f);
  finish();
}

static void testProviderUnavailable(void)
{
  start();
  LG_TransportFrameTiming timing = srcTiming(true, true);
  timing.providerValid = false;
  const LG_RendererFrameToken token = frameTimingReserve();
  queue(token, &timing, 68);
  frameTimingFinishFrame(token, &timing);
  const LG_RendererFrameTiming render = dstTiming(token, false);
  frameTimingFinishRender(&render, 200, 7, 300, token);
  frameTimingPublishReady();
  CHECK(ringbuffer_getCount(g_state.frameLatency) == 1);
  const OverlayFrameTiming out = take();
  CHECK(out.validMask & OVERLAY_FRAME_TIMING_VALID_PRODUCER);
  CHECK(out.validMask & OVERLAY_FRAME_TIMING_VALID_TRANSPORT);
  CHECK(!(out.validMask & OVERLAY_FRAME_TIMING_VALID_PROVIDER));
  CHECK(near(out.transport, 0.000015f));
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

static void testComponentEpoch(void)
{
  atomic_bool available = true;
  atomic_uint_least64_t epoch = 7;
  atomic_int reason = LG_TRANSPORT_OK;
  CHECK(videoComponentPayloadCurrent(&available, &epoch, 7, true));
  CHECK(!videoComponentPayloadCurrent(&available, &epoch, 6, true));
  CHECK(videoComponentPayloadCurrent(&available, &epoch, 0, false));

  const LG_VideoComponentStatus unavailable =
  {
    .available = false,
    .epoch     = 8,
    .reason    = LG_TRANSPORT_ERROR,
  };
  CHECK(videoComponentStatusChanged(
        &available, &epoch, &reason, &unavailable));
  CHECK(!videoComponentPayloadCurrent(&available, &epoch, 7, true));
  CHECK(!videoComponentPayloadCurrent(&available, &epoch, 0, false));
  CHECK(atomic_load(&reason) == LG_TRANSPORT_ERROR);

  const LG_VideoComponentStatus replacement =
  {
    .available = true,
    .epoch     = 9,
    .reason    = LG_TRANSPORT_OK,
  };
  CHECK(videoComponentStatusChanged(
        &available, &epoch, &reason, &replacement));
  CHECK(!videoComponentPayloadCurrent(&available, &epoch, 7, true));
  CHECK(videoComponentPayloadCurrent(&available, &epoch, 9, true));
}

struct PayloadGateTrace
{
  const atomic_bool          * available;
  const atomic_uint_least64_t * epoch;
  uint64_t                     payloadEpoch;
  uint64_t                     sourceGeneration;
  atomic_bool                  entered;
  atomic_bool                  release;
  atomic_bool                  accepted;
  atomic_bool                  changed;
};

static void * holdPayload(void * opaque)
{
  struct PayloadGateTrace * trace = opaque;
  const bool accepted = videoPayloadAccept(trace->available, trace->epoch,
      trace->payloadEpoch, true, trace->sourceGeneration);
  atomic_store_explicit(&trace->accepted, accepted, memory_order_release);
  if (!accepted)
    return NULL;

  atomic_store_explicit(&trace->entered, true, memory_order_release);
  while (!atomic_load_explicit(&trace->release, memory_order_acquire))
    usleep(1000);
  videoPayloadRelease();
  return NULL;
}

static void * changePayload(void * opaque)
{
  struct PayloadGateTrace * trace = opaque;
  LG_LOCK(g_state.videoPayloadLock);
  atomic_store_explicit((atomic_bool *)trace->available,
      false, memory_order_release);
  atomic_store_explicit((atomic_uint_least64_t *)trace->epoch,
      trace->payloadEpoch + 1, memory_order_release);
  LG_UNLOCK(g_state.videoPayloadLock);
  atomic_store_explicit(&trace->changed, true, memory_order_release);
  return NULL;
}

static void testPayloadGate(void)
{
  memset(&g_state, 0, sizeof(g_state));
  LG_LOCK_INIT(g_state.videoPayloadLock);
  atomic_store_explicit(
      &g_state.videoSource[LG_VIDEO_SOURCE_PRIMARY].generation,
      5, memory_order_relaxed);
  atomic_bool available = true;
  atomic_uint_least64_t epoch = 7;
  struct PayloadGateTrace trace =
  {
    .available        = &available,
    .epoch            = &epoch,
    .payloadEpoch     = 7,
    .sourceGeneration = 5,
  };
  pthread_t payloadThread;
  pthread_t statusThread;
  CHECK(pthread_create(&payloadThread, NULL, holdPayload, &trace) == 0);
  while (!atomic_load_explicit(&trace.entered, memory_order_acquire))
    usleep(1000);
  CHECK(pthread_create(&statusThread, NULL, changePayload, &trace) == 0);
  usleep(10000);
  CHECK(!atomic_load_explicit(&trace.changed, memory_order_acquire));
  atomic_store_explicit(&trace.release, true, memory_order_release);
  CHECK(pthread_join(payloadThread, NULL) == 0);
  CHECK(pthread_join(statusThread, NULL) == 0);
  CHECK(atomic_load_explicit(&trace.accepted, memory_order_acquire));
  CHECK(atomic_load_explicit(&trace.changed, memory_order_acquire));
  CHECK(!videoPayloadAccept(&available, &epoch, 7, true, 5));
  LG_LOCK_FREE(g_state.videoPayloadLock);
}

static void testFramePayloadBookkeeping(void)
{
  memset(&g_state, 0, sizeof(g_state));
  LG_LOCK_INIT(g_state.videoPayloadLock);
  atomic_store_explicit(&g_state.videoFrameAvailable,
      true, memory_order_relaxed);
  atomic_store_explicit(&g_state.videoFrameEpoch, 7, memory_order_relaxed);
  atomic_store_explicit(
      &g_state.videoSource[LG_VIDEO_SOURCE_PRIMARY].generation,
      5, memory_order_relaxed);
  g_state.formatValid = true;

  uint64_t sourceGeneration = 4;
  uint64_t frameSerial       = 41;
  uint32_t formatVersion     = 3;
  bool sourceChanged         = false;
  LG_TransportFrame frame =
  {
    .serial = 42,
    .epoch  = 6,
  };

  CHECK(!videoFramePayloadAccept(&frame, 5, true,
        &sourceGeneration, &frameSerial, &formatVersion, &sourceChanged));
  CHECK(sourceGeneration == 4);
  CHECK(frameSerial == 41);
  CHECK(formatVersion == 3);

  frame.epoch = 7;
  CHECK(videoFramePayloadAccept(&frame, 5, true,
        &sourceGeneration, &frameSerial, &formatVersion, &sourceChanged));
  CHECK(sourceChanged);
  CHECK(sourceGeneration == 5);
  CHECK(frameSerial == 42);
  CHECK(formatVersion == 0);
  videoPayloadRelease();

  CHECK(!videoFramePayloadAccept(&frame, 5, true,
        &sourceGeneration, &frameSerial, &formatVersion, &sourceChanged));
  LG_LOCK_FREE(g_state.videoPayloadLock);
}

static void testConnectCancellation(void)
{
  rawConnectCalls         = 0;
  cancellableConnectCalls = 0;
  connectSawCancellation  = false;
  const LG_TransportOps ops =
  {
    .connect            = rawConnect,
    .connectCancellable = cancellableConnect,
  };
  struct TransportSessionProbe probe =
  {
    .ops       = &ops,
    .cancelled = cancelled,
    .done      = false,
  };
  CHECK(transportSessionProbe(&probe) == 0);
  CHECK(atomic_load_explicit(&probe.done, memory_order_acquire));
  CHECK(probe.status == LG_TRANSPORT_DISCONNECTED);
  CHECK(rawConnectCalls == 0);
  CHECK(cancellableConnectCalls == 1);
  CHECK(connectSawCancellation);
}

static void testRecoveryErrors(void)
{
  CHECK(strcmp(recoveryErrorText(LG_RECOVERY_ERR_BUSY),
        "A conflicting recovery request is already in progress") == 0);
  CHECK(strcmp(recoveryErrorText(LG_RECOVERY_ERR_CAPACITY),
        "Too many recovery requests are pending") == 0);
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
  { "accounting", testAccounting },
  { "provider-unavailable", testProviderUnavailable },
  { "timeout"  , testTimeout   },
  { "fifo"     , testFifo      },
  { "retire"   , testRetire    },
  { "feedback" , testFeedback  },
  { "wrap"     , testWrap      },
  { "component-epoch", testComponentEpoch },
  { "payload-gate", testPayloadGate },
  { "frame-payload-bookkeeping", testFramePayloadBookkeeping },
  { "connect-cancel", testConnectCancellation },
  { "recovery-errors", testRecoveryErrors },
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
