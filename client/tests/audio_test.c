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

#include "interface/audiodev.h"

static struct LG_AudioDevOps dev;
struct LG_AudioDevOps * LG_AudioDevs[] = { &dev, &dev, NULL };

/* Keep the conversion and validation helpers in this unit-test translation
 * unit. The public provider API is used for all lifecycle tests below. */
#include "../src/audio.c"

#include "test.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WAIT_MS 2000U

struct AppState    g_state;
struct AppParams   g_params;
struct CursorState g_cursor;

struct Backend
{
  atomic_uint       initN;
  atomic_uint       freeN;
  atomic_uint       setupN;
  atomic_uint       playStartN;
  atomic_uint       playStopN;
  atomic_uint       recStartN;
  atomic_uint       recStopN;
  atomic_bool       setupOK;
  atomic_bool       playOK;
  atomic_bool       recOK;

  LG_AudioFormat    playFormat;
  LG_AudioPullFn    pull;
  LG_AudioFailureFn playFail;
  uint32_t          playCookie;
  LG_AudioFormat    recFormat;
  LG_AudioPushFn    push;
  LG_AudioFailureFn recFail;
  uint32_t          recCookie;
};

struct Provider
{
  const char             * name;
  bool                     available;
  bool                     attachOK;
  uint32_t                 generation;
  LG_AudioStatusFn         status;
  void                   * statusOpaque;
  const LG_AudioEventOps * events;
  void                   * eventOpaque;
  unsigned int             listenN;
  unsigned int             unlistenN;
  unsigned int             attachN;
  unsigned int             detachN;
  unsigned int             dataN;
  atomic_bool              badDetach;
};

struct UI
{
  MsgBoxConfirmCallback  confirm;
  void                 * opaque;
  MsgBoxHandle           handle;
  unsigned int           confirmN;
  unsigned int           closeN;
  atomic_bool            block;
  atomic_bool            entered;
  atomic_bool            release;
  atomic_bool            inConfirm;
};

static struct Backend b;
static struct UI      ui;

static bool devInit(void)
{
  atomic_fetch_add(&b.initN, 1);
  return true;
}

static void devFree(void)
{
  atomic_fetch_add(&b.freeN, 1);
}

static bool playSetup(const LG_AudioFormat * format,
    int period, bool requestResampler, bool * resamplerEnabled,
    int * maxPeriod, int * startFrames, LG_AudioPullFn pull)
{
  (void)period;
  b.playFormat      = *format;
  b.pull            = pull;
  *resamplerEnabled = requestResampler;
  *maxPeriod        = 256;
  *startFrames      = 0;
  atomic_fetch_add(&b.setupN, 1);
  return atomic_load(&b.setupOK);
}

static bool playStart(LG_AudioFailureFn fail, uint32_t cookie)
{
  b.playFail   = fail;
  b.playCookie = cookie;
  atomic_fetch_add(&b.playStartN, 1);
  return atomic_load(&b.playOK);
}

static void playStop(void)
{
  b.playFail   = NULL;
  b.playCookie = 0;
  atomic_fetch_add(&b.playStopN, 1);
}

static bool playRate(double * ratio)
{
  (void)ratio;
  return true;
}

static uint64_t playLatency(void)
{
  return 0;
}

static bool recStart(const LG_AudioFormat * format, LG_AudioPushFn push,
    LG_AudioFailureFn fail, uint32_t cookie)
{
  b.recFormat = *format;
  b.push      = push;
  b.recFail   = fail;
  b.recCookie = cookie;
  atomic_fetch_add(&b.recStartN, 1);
  return atomic_load(&b.recOK);
}

static void recStop(void)
{
  b.push      = NULL;
  b.recFail   = NULL;
  b.recCookie = 0;
  atomic_fetch_add(&b.recStopN, 1);
}

static void setListener(void * opaque, LG_AudioStatusFn callback,
    void * callbackOpaque)
{
  struct Provider * p = opaque;
  p->status            = callback;
  p->statusOpaque      = callbackOpaque;
  if (!callback)
  {
    ++p->unlistenN;
    return;
  }

  ++p->listenN;
  const LG_AudioStatus status =
  {
    .available  = p->available,
    .generation = p->generation,
  };
  callback(callbackOpaque, &status);
}

static bool attach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  struct Provider * p = opaque;
  ++p->attachN;
  p->events      = events;
  p->eventOpaque = eventOpaque;
  return p->attachOK;
}

static void detach(void * opaque)
{
  struct Provider * p = opaque;
  if (atomic_load(&ui.inConfirm))
    atomic_store(&p->badDetach, true);
  ++p->detachN;
  p->events      = NULL;
  p->eventOpaque = NULL;
}

static bool recordData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * clock)
{
  struct Provider * p = opaque;
  (void)generation;
  (void)data;
  (void)frames;
  (void)clock;
  ++p->dataN;
  return true;
}

static const LG_AudioOps ops =
{
  .name              = "test",
  .setStatusListener = setListener,
  .attach            = attach,
  .detach            = detach,
  .recordData        = recordData,
};

GraphHandle app_registerGraph(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn)
{
  (void)name;
  (void)buffer;
  (void)min;
  (void)max;
  (void)formatFn;
  return NULL;
}

void app_unregisterGraph(GraphHandle handle)
{
  (void)handle;
}

void app_invalidateGraph(GraphHandle handle)
{
  (void)handle;
}

void app_showRecord(bool show)
{
  (void)show;
}

void app_alert(LG_MsgAlert type, const char * format, ...)
{
  (void)type;
  (void)format;
}

MsgBoxHandle app_confirmMsgBox(const char * caption,
    MsgBoxConfirmCallback callback, void * opaque, const char * format, ...)
{
  (void)caption;
  (void)format;
  ui.confirm = callback;
  ui.opaque  = opaque;
  ui.handle  = (MsgBoxHandle)(uintptr_t)(++ui.confirmN);

  if (atomic_load(&ui.block))
  {
    atomic_store(&ui.inConfirm, true);
    atomic_store(&ui.entered, true);
    while (!atomic_load(&ui.release))
      usleep(1000);
    atomic_store(&ui.inConfirm, false);
  }

  return ui.handle;
}

void app_msgBoxClose(MsgBoxHandle handle)
{
  if (handle)
    ++ui.closeN;
}

static void waitFor(atomic_uint * value, unsigned int target)
{
  for (unsigned int i = 0; i < WAIT_MS; ++i)
  {
    if (atomic_load_explicit(value, memory_order_acquire) >= target)
      return;
    usleep(1000);
  }

  fprintf(stderr, "timeout waiting for %u, got %u\n",
      target, atomic_load(value));
  CHECK(atomic_load(value) >= target);
}

static void waitBool(atomic_bool * value)
{
  for (unsigned int i = 0; i < WAIT_MS; ++i)
  {
    if (atomic_load_explicit(value, memory_order_acquire))
      return;
    usleep(1000);
  }

  CHECK(atomic_load(value));
}

static LG_AudioFormat makeFormat(LG_AudioSampleFormat sampleFormat)
{
  return (LG_AudioFormat)
  {
    .sampleFormat = sampleFormat,
    .sampleRate   = 48000,
    .channelCount = 2,
    .channels     =
    {
      LG_AUDIO_CH_FRONT_LEFT,
      LG_AUDIO_CH_FRONT_RIGHT,
    },
  };
}

static void reset(void)
{
  memset(&audio, 0, sizeof(audio));
  memset(&b, 0, sizeof(b));
  memset(&ui, 0, sizeof(ui));
  memset(&g_state, 0, sizeof(g_state));
  memset(&g_params, 0, sizeof(g_params));
  memset(&g_cursor, 0, sizeof(g_cursor));

  atomic_init(&b.initN, 0);
  atomic_init(&b.freeN, 0);
  atomic_init(&b.setupN, 0);
  atomic_init(&b.playStartN, 0);
  atomic_init(&b.playStopN, 0);
  atomic_init(&b.recStartN, 0);
  atomic_init(&b.recStopN, 0);
  atomic_init(&b.setupOK, true);
  atomic_init(&b.playOK, true);
  atomic_init(&b.recOK, true);
  atomic_init(&ui.block, false);
  atomic_init(&ui.entered, false);
  atomic_init(&ui.release, false);
  atomic_init(&ui.inConfirm, false);

  dev = (struct LG_AudioDevOps)
  {
    .name = "test",
    .init = devInit,
    .free = devFree,
    .playback =
    {
      .setup   = playSetup,
      .start   = playStart,
      .stop    = playStop,
      .setRate = playRate,
      .latency = playLatency,
    },
    .record =
    {
      .start = recStart,
      .stop  = recStop,
    },
  };

  g_params.audioResampler = AUDIO_RESAMPLER_BACKEND;
  g_state.micDefaultState = MIC_DEFAULT_PROMPT;
}

static void startAudio(void)
{
  lgAudio_init();
  CHECK(atomic_load(&b.initN) == 1);
  CHECK(lgAudio_supportsPlayback());
  CHECK(lgAudio_supportsRecord());
}

static void stopAudio(void)
{
  lgAudio_free();
  CHECK(atomic_load(&b.freeN) == 1);
}

static void initProvider(struct Provider * p, const char * name,
    bool available)
{
  memset(p, 0, sizeof(*p));
  p->name       = name;
  p->available  = available;
  p->attachOK   = true;
  p->generation = 1;
  atomic_init(&p->badDetach, false);
}

static void setStatus(struct Provider * p, bool available,
    uint32_t generation)
{
  p->available  = available;
  p->generation = generation;
  CHECK(p->status);
  const LG_AudioStatus status =
  {
    .available  = available,
    .generation = generation,
  };
  p->status(p->statusOpaque, &status);
}

static bool near(float a, float expected)
{
  return fabsf(a - expected) < 0.000001f;
}

static void testFormat(void)
{
  LG_AudioFormat format = makeFormat(LG_AUDIO_FMT_S16_LE);
  CHECK(audioFormatValid(&format));
  CHECK(!audioFormatValid(NULL));

  const LG_AudioSampleFormat samples[] =
  {
    LG_AUDIO_FMT_U8,
    LG_AUDIO_FMT_S16_LE,
    LG_AUDIO_FMT_S24_LE,
    LG_AUDIO_FMT_S32_LE,
    LG_AUDIO_FMT_F32_LE,
    LG_AUDIO_FMT_F64_LE,
    LG_AUDIO_FMT_F32_NE,
  };
  for (size_t i = 0; i < ARRAY_LENGTH(samples); ++i)
  {
    format.sampleFormat = samples[i];
    CHECK(audioFormatValid(&format));
  }

  format.sampleFormat = (LG_AudioSampleFormat)-1;
  CHECK(!audioFormatValid(&format));
  format = makeFormat(LG_AUDIO_FMT_S16_LE);
  format.sampleRate = 7999;
  CHECK(!audioFormatValid(&format));
  format.sampleRate = 384001;
  CHECK(!audioFormatValid(&format));
  format = makeFormat(LG_AUDIO_FMT_S16_LE);
  format.channelCount = 0;
  CHECK(!audioFormatValid(&format));
  format.channelCount = LG_AUDIO_MAX_CHANNELS + 1;
  CHECK(!audioFormatValid(&format));
  format = makeFormat(LG_AUDIO_FMT_S16_LE);
  format.channels[1] = (LG_AudioChannel)(LG_AUDIO_CH_TOP_REAR_RIGHT + 1);
  CHECK(!audioFormatValid(&format));

  LG_AudioFormat same = makeFormat(LG_AUDIO_FMT_S16_LE);
  CHECK(audioFormatEqual(&same, &same));
  format = same;
  format.channels[1] = LG_AUDIO_CH_MONO;
  CHECK(!audioFormatEqual(&format, &same));
}

static void testConvert(void)
{
  float out[4];
  const uint8_t u8[] = { 0, 128, 255 };
  CHECK(audioConvertToFloat(out, u8, ARRAY_LENGTH(u8), LG_AUDIO_FMT_U8));
  CHECK(near(out[0], -1.0f));
  CHECK(near(out[1], 0.0f));
  CHECK(near(out[2], 127.0f / 128.0f));

  const uint8_t s16[] =
  {
    0x00, 0x80,
    0xff, 0xff,
    0x00, 0x00,
    0xff, 0x7f,
  };
  CHECK(audioConvertToFloat(out, s16, 4, LG_AUDIO_FMT_S16_LE));
  CHECK(near(out[0], -1.0f));
  CHECK(near(out[1], -1.0f / 32768.0f));
  CHECK(near(out[2], 0.0f));
  CHECK(near(out[3], 32767.0f / 32768.0f));

  const uint8_t s24[] =
  {
    0x00, 0x00, 0x80,
    0xff, 0xff, 0x7f,
  };
  CHECK(audioConvertToFloat(out, s24, 2, LG_AUDIO_FMT_S24_LE));
  CHECK(near(out[0], -1.0f));
  CHECK(near(out[1], 8388607.0f / 8388608.0f));

  const uint8_t s32[] =
  {
    0x00, 0x00, 0x00, 0x80,
    0xff, 0xff, 0xff, 0x7f,
  };
  CHECK(audioConvertToFloat(out, s32, 2, LG_AUDIO_FMT_S32_LE));
  CHECK(near(out[0], -1.0f));
  CHECK(near(out[1], 1.0f));

  const uint8_t f32le[] =
  {
    0x00, 0x00, 0x80, 0xbe,
    0x00, 0x00, 0x40, 0x3f,
  };
  CHECK(audioConvertToFloat(out, f32le, 2, LG_AUDIO_FMT_F32_LE));
  CHECK(near(out[0], -0.25f));
  CHECK(near(out[1], 0.75f));
  const float f32ne[] = { -0.25f, 0.75f };
  CHECK(audioConvertToFloat(out, f32ne, 2, LG_AUDIO_FMT_F32_NE));
  CHECK(near(out[0], -0.25f));
  CHECK(near(out[1], 0.75f));

  const uint8_t f64le[] =
  {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0xbf,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x3f,
  };
  CHECK(audioConvertToFloat(out, f64le, 2, LG_AUDIO_FMT_F64_LE));
  CHECK(near(out[0], -0.5f));
  CHECK(near(out[1], 0.125f));
  CHECK(!audioConvertToFloat(NULL, f32ne, 2, LG_AUDIO_FMT_F32_NE));
  CHECK(!audioConvertToFloat(out, NULL, 2, LG_AUDIO_FMT_F32_NE));
  CHECK(!audioConvertToFloat(out, f32ne, 2, (LG_AudioSampleFormat)-1));
}

static void testProvider(void)
{
  reset();
  startAudio();
  struct Provider fallback;
  struct Provider transport;
  initProvider(&fallback, "fallback", true);
  initProvider(&transport, "transport", false);

  lgAudio_setFallback(&ops, &fallback);
  CHECK(fallback.attachN == 1);
  CHECK(fallback.detachN == 0);
  lgAudio_setTransport(&ops, &transport);
  CHECK(transport.attachN == 0);
  CHECK(fallback.detachN == 0);

  transport.attachOK = false;
  setStatus(&transport, true, 2);
  CHECK(transport.attachN == 1);
  CHECK(transport.detachN == 1);
  CHECK(fallback.detachN == 1);
  CHECK(fallback.attachN == 2);

  transport.attachOK = true;
  setStatus(&transport, true, 3);
  CHECK(transport.attachN == 2);
  CHECK(fallback.detachN == 2);
  setStatus(&transport, false, 4);
  CHECK(transport.detachN == 2);
  CHECK(fallback.attachN == 3);

  const unsigned int detached = fallback.detachN;
  lgAudio_dropFallback();
  CHECK(fallback.detachN == detached);
  stopAudio();
}

static void testPlaybackRetry(void)
{
  reset();
  atomic_store(&b.setupOK, false);
  startAudio();
  struct Provider p;
  initProvider(&p, "provider", true);
  lgAudio_setFallback(&ops, &p);
  CHECK(p.events);

  LG_AudioFormat format = makeFormat(LG_AUDIO_FMT_S16_LE);
  p.events->playbackStart(p.eventOpaque, 11, &format, NULL);
  waitFor(&b.setupN, 2);
  CHECK(atomic_load(&b.playStartN) == 0);

  const unsigned int failed = atomic_load(&b.setupN);
  atomic_store(&b.setupOK, true);
  uint8_t frames[480 * 2 * 2] = { 0 };
  usleep(10000);
  p.events->playbackData(p.eventOpaque, 11, frames, 480, NULL);
  waitFor(&b.setupN, failed + 1);

  for (unsigned int i = 0; i < WAIT_MS; ++i)
  {
    if (atomic_load_explicit(&audio.playback.streamGeneration,
          memory_order_acquire) == 11)
      break;
    usleep(1000);
  }
  CHECK(atomic_load(&audio.playback.streamGeneration) == 11);
  for (unsigned int i = 0; i < 8; ++i)
    p.events->playbackData(p.eventOpaque, 11, frames, 480, NULL);
  waitFor(&b.playStartN, 1);

  LG_AudioFailureFn fail = b.playFail;
  const uint32_t cookie = b.playCookie;
  CHECK(fail);
  const unsigned int stopped = atomic_load(&b.playStopN);
  fail(cookie);
  waitFor(&b.playStopN, stopped + 1);

  const unsigned int retry = atomic_load(&b.setupN);
  usleep(10000);
  p.events->playbackData(p.eventOpaque, 11, frames, 480, NULL);
  waitFor(&b.setupN, retry + 1);

  p.events->playbackStop(p.eventOpaque, 11);
  stopAudio();
}

static void testPlaybackJitter(void)
{
  PlaybackSourceData source = { 0 };
  playbackPrepareMediaClock(&source, NULL);

  bool discontinuity = false;
  playbackMapMediaTime(&source, NULL, 480, 48000, 0,
      &discontinuity);
  source.inputPosition   = 480;
  source.lastArrivalTime = 0;

  discontinuity = true;
  playbackMapMediaTime(&source, NULL, 480, 48000,
      INT64_C(110000000), &discontinuity);
  CHECK(discontinuity);
  CHECK(source.arrivalJitterSec == 0.0);

  memset(&source, 0, sizeof(source));
  playbackPrepareMediaClock(&source, NULL);
  discontinuity = false;
  playbackMapMediaTime(&source, NULL, 480, 48000, 0,
      &discontinuity);
  source.inputPosition   = 480;
  source.lastArrivalTime = 0;
  playbackMapMediaTime(&source, NULL, 480, 48000,
      INT64_C(20000000), &discontinuity);
  CHECK(source.arrivalJitterSec > 0.009);
  CHECK(source.arrivalJitterSec < 0.011);
}

static void testPlaybackRecovery(void)
{
  reset();
  startAudio();

  const LG_AudioFormat format = makeFormat(LG_AUDIO_FMT_S16_LE);
  CHECK(playbackStart(&format, NULL, false, false));
  CHECK(audio.playback.rateControl == PLAYBACK_RATE_BACKEND);
  playbackSetState(STREAM_STATE_RUN);

  PlaybackSourceData * source = &audio.playback.sourceData;
  const int64_t arrival       = nanotime();
  playbackClockReset(&source->deviceClock,
      arrival, 0.0, 1.0 / format.sampleRate);
  source->deviceClockStable       = true;
  source->outputPosition          = 0;
  source->lastRatio               = 0.999;
  source->lastClockRatio          = 1.0002;
  source->sourcePacketDurationSec = 0.010;
  playbackPublishDeviceTiming(16, arrival, 0, 0.0, 0);

  CHECK(ringbuffer_consume(audio.playback.buffer, NULL, 16) == 16);
  CHECK(ringbuffer_getCount(audio.playback.buffer) == -16);
  uint8_t frames[1200 * 2 * 2] = { 0 };
  CHECK(playbackData(frames, 1200, NULL, arrival) ==
      PLAYBACK_DATA_PROCESSED);

  CHECK(source->sourcePacketDurationSec < 0.010);
  CHECK(source->sourcePacketDurationSec > 0.009);
  CHECK(source->sourcePacketRecovering);
  CHECK(source->outputPosition == 1770);
  CHECK(ringbuffer_getCount(audio.playback.buffer) == 1754);
  CHECK(fabs(atomic_load(&audio.playback.backendResampleRatio) -
        source->lastClockRatio) < 0.000001);

  CHECK(playbackData(frames, 1200, NULL, arrival + 1000) ==
      PLAYBACK_DATA_PROCESSED);
  CHECK(source->sourcePacketDurationSec < 0.010);
  CHECK(source->sourcePacketRecovering);

  CHECK(playbackData(frames, 480, NULL,
        arrival + INT64_C(10000000)) == PLAYBACK_DATA_PROCESSED);
  CHECK(!source->sourcePacketRecovering);
  CHECK(fabs(source->sourcePacketDurationSec - 0.010) < 0.000001);

  stopAudio();
}

static void testConsent(void)
{
  reset();
  startAudio();
  struct Provider p;
  initProvider(&p, "provider", true);
  lgAudio_setFallback(&ops, &p);
  LG_AudioFormat format = makeFormat(LG_AUDIO_FMT_S16_LE);

  p.events->recordStart(p.eventOpaque, 20, &format);
  CHECK(ui.confirmN == 1);
  CHECK(ui.confirm);
  MsgBoxConfirmCallback stale = ui.confirm;
  void * staleOpaque = ui.opaque;
  p.events->recordStop(p.eventOpaque, 20);
  CHECK(ui.closeN == 1);
  stale(true, staleOpaque);
  CHECK(atomic_load(&b.recStartN) == 0);

  p.events->recordStart(p.eventOpaque, 21, &format);
  CHECK(ui.confirmN == 2);
  CHECK(ui.confirm);
  stale(true, staleOpaque);
  CHECK(atomic_load(&b.recStartN) == 0);
  ui.confirm(true, ui.opaque);
  waitFor(&b.recStartN, 1);
  CHECK(b.recFormat.sampleRate == format.sampleRate);
  CHECK(b.recFormat.channelCount == format.channelCount);
  CHECK(b.push);

  uint8_t frames[16] = { 0 };
  CHECK(b.push(frames, 4, NULL));
  CHECK(p.dataN == 1);
  p.events->recordStop(p.eventOpaque, 21);
  waitFor(&b.recStopN, 1);
  stopAudio();
}

struct RecordCall
{
  struct Provider * provider;
  LG_AudioFormat    format;
};

static void * callRecord(void * opaque)
{
  struct RecordCall * call = opaque;
  call->provider->events->recordStart(call->provider->eventOpaque,
      30, &call->format);
  return NULL;
}

struct SwitchCall
{
  struct Provider * provider;
};

static void * switchProvider(void * opaque)
{
  struct SwitchCall * call = opaque;
  lgAudio_setTransport(&ops, call->provider);
  return NULL;
}

static void testQuiesce(void)
{
  reset();
  startAudio();
  struct Provider fallback;
  struct Provider transport;
  initProvider(&fallback, "fallback", true);
  initProvider(&transport, "transport", true);
  lgAudio_setFallback(&ops, &fallback);

  atomic_store(&ui.block, true);
  struct RecordCall record =
  {
    .provider = &fallback,
    .format   = makeFormat(LG_AUDIO_FMT_S16_LE),
  };
  pthread_t recordThread;
  CHECK(pthread_create(&recordThread, NULL, callRecord, &record) == 0);
  waitBool(&ui.entered);

  struct SwitchCall change = { .provider = &transport };
  pthread_t switchThread;
  CHECK(pthread_create(&switchThread, NULL, switchProvider, &change) == 0);
  for (unsigned int i = 0; i < WAIT_MS; ++i)
  {
    if (atomic_load_explicit(&audio.activeCallbacks,
          memory_order_acquire) & ACTIVE_CALLBACK_WAITING)
      break;
    usleep(1000);
  }
  CHECK(atomic_load(&audio.activeCallbacks) & ACTIVE_CALLBACK_WAITING);
  CHECK(fallback.detachN == 0);
  CHECK(!atomic_load(&fallback.badDetach));

  MsgBoxConfirmCallback stale = ui.confirm;
  void * staleOpaque = ui.opaque;
  atomic_store(&ui.release, true);
  CHECK(pthread_join(recordThread, NULL) == 0);
  CHECK(pthread_join(switchThread, NULL) == 0);
  CHECK(fallback.detachN == 1);
  CHECK(transport.attachN == 1);
  CHECK(ui.closeN == 1);
  CHECK(!atomic_load(&fallback.badDetach));

  CHECK(stale);
  stale(true, staleOpaque);
  CHECK(atomic_load(&b.recStartN) == 0);
  stopAudio();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "format"        , testFormat        },
  { "convert"       , testConvert       },
  { "provider"      , testProvider      },
  { "playback-retry", testPlaybackRetry },
  { "jitter"        , testPlaybackJitter },
  { "recovery"      , testPlaybackRecovery },
  { "consent"       , testConsent       },
  { "quiesce"       , testQuiesce       },
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
