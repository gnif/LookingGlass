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

#include "transport_fallback.h"
#include "test.h"

#include "common/debug.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_WAIT_MS 3000U
#define TEST_ALARM_S    5U

struct LG_Transport
{
  unsigned int id;
};

struct ProviderTrace
{
  atomic_uint set;
  atomic_uint clear;
  atomic_uint drop;
};

struct TransportTrace
{
  atomic_uint          make;
  atomic_uint          free;
  atomic_uint          conn;
  atomic_uint          disc;
  atomic_uint          attach;
  atomic_uint          detach;
  atomic_uint          on;
  atomic_uint          off;
  atomic_uint          getInput;
  atomic_uint          getAudio;
  atomic_uint          getClipboard;
  atomic_bool          alive;
  atomic_bool          hold;
  atomic_bool          release;
  atomic_bool          cancel;
  atomic_bool          badProvider;
  atomic_bool          noVideo;
  atomic_bool          failAttach;
  atomic_bool          failActive;
  atomic_bool          blockActive;
  atomic_bool          activeEntered;
  atomic_bool          activeCancelled;
  atomic_bool          holdActiveReturn;
  atomic_uint          cancelPending;
  const LG_SwSurfaceEventOps * surfaceEvents;
  void                       * surfaceOpaque;
  atomic_uint_fast64_t connTime[4];
  unsigned int         failConn;
  bool                 uuidValid;
  uint8_t              uuid[16];
};

struct EventTrace
{
  atomic_uint         conn;
  atomic_uint         disc;
  atomic_uint         mismatch;
  atomic_uint         videoReady;
  atomic_uint         videoUnavailable;
  atomic_uint         configureAccepted;
  atomic_uint         surfaceCleanup;
  atomic_bool         surfaceRetained;
  LG_TransportSession session;
  uint8_t             primary[16];
  uint8_t             fallback[16];
};

static struct TransportTrace t;
static struct EventTrace     e;
static struct ProviderTrace  pInput;
#ifdef ENABLE_AUDIO
static struct ProviderTrace  pAudio;
#endif
static struct ProviderTrace  pClipboard;
static _Atomic(LG_TransportFallback *) surfaceFallback;

static const uint8_t UUID_A[16] =
  { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f };
static const uint8_t UUID_B[16] =
  { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f };

static uint64_t nowNS(void)
{
  struct timespec ts;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + ts.tv_nsec;
}

static bool waitCount(const atomic_uint * count, unsigned int value)
{
  for (unsigned int i = 0; i < TEST_WAIT_MS; ++i)
  {
    if (atomic_load(count) >= value)
      return true;
    usleep(1000);
  }

  return false;
}

static bool waitReady(const LG_TransportFallback * fallback)
{
  for (unsigned int i = 0; i < TEST_WAIT_MS; ++i)
  {
    if (lgTransportFallback_ready(fallback))
      return true;
    usleep(1000);
  }

  return false;
}

static bool waitUsable(const LG_TransportFallback * fallback)
{
  for (unsigned int i = 0; i < TEST_WAIT_MS; ++i)
  {
    if (lgTransportFallback_usable(fallback))
      return true;
    usleep(1000);
  }

  return false;
}

static void setRemote(const uint8_t uuid[16])
{
  t.uuidValid = true;
  memcpy(t.uuid, uuid, sizeof(t.uuid));
}

static bool fakeCreate(LG_Transport ** transport)
{
  LG_Transport * result = calloc(1, sizeof(*result));
  if (!result)
    return false;

  result->id = atomic_fetch_add(&t.make, 1) + 1;
  *transport = result;
  return true;
}

static void fakeDestroy(LG_Transport ** transport)
{
  CHECK(transport);
  CHECK(*transport);
  atomic_fetch_add(&t.free, 1);
  free(*transport);
  *transport = NULL;
}

static LG_TransportStatus fakeConnect(LG_Transport * transport,
    LG_TransportSession * session, LG_TransportCancelledFn cancelled,
    void * opaque)
{
  CHECK(transport);
  CHECK(session);

  const unsigned int no = atomic_load(&t.conn);
  if (no < 4)
    atomic_store(&t.connTime[no], nowNS());
  atomic_fetch_add(&t.conn, 1);

  while (atomic_load(&t.hold) && !atomic_load(&t.release))
  {
    if (cancelled && cancelled(opaque))
    {
      atomic_store(&t.cancel, true);
      return LG_TRANSPORT_UNAVAILABLE;
    }
    usleep(1000);
  }

  if (no < t.failConn)
    return LG_TRANSPORT_UNAVAILABLE;

  *session = (LG_TransportSession) { 0 };
  memcpy(session->version, "fallback-test", sizeof("fallback-test"));
  memcpy(session->name, "fallback", sizeof("fallback"));
  session->uuidValid = t.uuidValid;
  memcpy(session->uuid, t.uuid, sizeof(session->uuid));
  atomic_store(&t.alive, true);
  return LG_TRANSPORT_OK;
}

static void fakeDisconnect(LG_Transport * transport)
{
  CHECK(transport);
  atomic_store(&t.alive, false);
  atomic_fetch_add(&t.disc, 1);
}

static bool fakeValid(LG_Transport * transport)
{
  CHECK(transport);
  return atomic_load(&t.alive);
}

static bool fakeAttach(LG_Transport * transport,
    const LG_SwSurfaceEventOps * events, void * opaque)
{
  CHECK(transport);
  CHECK(events);
  t.surfaceEvents = events;
  t.surfaceOpaque = opaque;
  atomic_fetch_add(&t.attach, 1);
  return !atomic_load(&t.failAttach);
}

static void fakeDetach(LG_Transport * transport)
{
  CHECK(transport);
  atomic_fetch_add(&t.detach, 1);
}

static bool fakeActive(LG_Transport * transport, bool active)
{
  CHECK(transport);
  atomic_fetch_add(active ? &t.on : &t.off, 1);
  atomic_store(&t.activeEntered, true);
  while (atomic_load(&t.blockActive) &&
      !atomic_load(&t.activeCancelled))
    usleep(1000);
  while (active && atomic_load(&t.holdActiveReturn))
    usleep(1000);
  return !atomic_load(&t.failActive);
}

static void fakeCancelPending(LG_Transport * transport)
{
  CHECK(transport);
  atomic_fetch_add(&t.cancelPending, 1);
  atomic_store(&t.activeCancelled, true);
}

static const LG_InputOps inputOps =
{
  .name = "fake",
};

static const LG_AudioOps audioOps =
{
  .name = "fake",
};

static const LG_ClipboardOps clipboardOps =
{
  .name = "fake",
};

static const LG_InputOps * fakeGetInput(
    LG_Transport * transport, void ** opaque)
{
  atomic_fetch_add(&t.getInput, 1);
  *opaque = transport;
  return &inputOps;
}

static const LG_AudioOps * fakeGetAudio(
    LG_Transport * transport, void ** opaque)
{
  atomic_fetch_add(&t.getAudio, 1);
  *opaque = transport;
  return &audioOps;
}

static const LG_ClipboardOps * fakeGetClipboard(
    LG_Transport * transport, void ** opaque)
{
  atomic_fetch_add(&t.getClipboard, 1);
  *opaque = transport;
  return &clipboardOps;
}

static const LG_SwSurfaceOps surfaceOps =
{
  .attach    = fakeAttach,
  .detach    = fakeDetach,
  .setActive = fakeActive,
  .cancelPending = fakeCancelPending,
};

static const LG_VideoOps videoOps =
{
  .name      = "fake",
  .type      = LG_VIDEO_TYPE_SW_SURFACE,
  .swSurface = &surfaceOps,
};

static const LG_VideoOps * fakeGetVideo(LG_Transport * transport)
{
  CHECK(transport);
  return atomic_load(&t.noVideo) ? NULL : &videoOps;
}

static const LG_TransportOps transportOps =
{
  .name               = "fake",
  .create             = fakeCreate,
  .destroy            = fakeDestroy,
  .connectCancellable = fakeConnect,
  .disconnect         = fakeDisconnect,
  .sessionValid       = fakeValid,
  .getVideoOps        = fakeGetVideo,
  .getInputOps        = fakeGetInput,
  .getAudioOps        = fakeGetAudio,
  .getClipboardOps    = fakeGetClipboard,
};

bool lgTransport_isValid(const char * name)
{
  return name && strcmp(name, transportOps.name) == 0;
}

bool lgTransport_create(const char * name, LG_TransportInstance * instance)
{
  if (!instance || !lgTransport_isValid(name))
    return false;

  *instance = (LG_TransportInstance) { 0 };
  if (!transportOps.create(&instance->handle))
    return false;
  instance->ops = &transportOps;
  return true;
}

void lgTransport_destroy(LG_TransportInstance * instance)
{
  CHECK(instance);
  if (instance->ops)
    instance->ops->destroy(&instance->handle);
  *instance = (LG_TransportInstance) { 0 };
}

static void setProvider(struct ProviderTrace * trace, const void * ops,
    const void * expected, void * opaque)
{
  if (ops)
  {
    atomic_fetch_add(&trace->set, 1);
    if (ops != expected || !opaque)
      atomic_store(&t.badProvider, true);
    return;
  }

  atomic_fetch_add(&trace->clear, 1);
  if (opaque)
    atomic_store(&t.badProvider, true);
}

void lgInput_setFallback(const LG_InputOps * ops, void * opaque)
{
  setProvider(&pInput, ops, &inputOps, opaque);
}

void lgInput_dropFallback(void)
{
  atomic_fetch_add(&pInput.drop, 1);
}

#ifdef ENABLE_AUDIO
void lgAudio_setFallback(const LG_AudioOps * ops, void * opaque)
{
  setProvider(&pAudio, ops, &audioOps, opaque);
}

void lgAudio_dropFallback(void)
{
  atomic_fetch_add(&pAudio.drop, 1);
}
#endif

void clipboard_setFallback(const LG_ClipboardOps * ops, void * opaque)
{
  setProvider(&pClipboard, ops, &clipboardOps, opaque);
}

void clipboard_dropFallback(void)
{
  atomic_fetch_add(&pClipboard.drop, 1);
}

static void onConnected(void * opaque,
    const LG_TransportSession * session)
{
  CHECK(opaque == &e);
  e.session = *session;
  atomic_fetch_add(&e.conn, 1);
}

static void onDisconnected(void * opaque)
{
  CHECK(opaque == &e);
  atomic_fetch_add(&e.disc, 1);
}

static void onMismatch(void * opaque, const uint8_t primary[16],
    const uint8_t fallback[16])
{
  CHECK(opaque == &e);
  memcpy(e.primary, primary, sizeof(e.primary));
  memcpy(e.fallback, fallback, sizeof(e.fallback));
  atomic_fetch_add(&e.mismatch, 1);
}

static void onVideoState(void * opaque, bool ready)
{
  CHECK(opaque == &e);
  atomic_fetch_add(ready ? &e.videoReady : &e.videoUnavailable, 1);
  if (!ready && atomic_exchange(&e.surfaceRetained, false))
    atomic_fetch_add(&e.surfaceCleanup, 1);
}

static void onSurfaceConfigure(void * opaque,
    unsigned int width, unsigned int height)
{
  CHECK(opaque == &e);
  CHECK(width == 1920);
  CHECK(height == 1080);
  LG_TransportFallback * fallback = atomic_load(&surfaceFallback);
  CHECK(fallback);
  if (!lgTransportFallback_acceptsVideoEvents(fallback))
    return;

  atomic_store(&e.surfaceRetained, true);
  atomic_fetch_add(&e.configureAccepted, 1);
}

static LG_TransportFallback * start(const uint8_t primary[16])
{
  static const LG_SwSurfaceEventOps surfaceEvents =
  {
    .configure = onSurfaceConfigure,
  };
  static const LG_TransportFallbackEventOps eventOps =
  {
    .connected    = onConnected,
    .disconnected = onDisconnected,
    .videoStateChanged = onVideoState,
    .endpointMismatch = onMismatch,
  };

  LG_TransportFallback * fallback = NULL;
  CHECK(lgTransportFallback_start("fake", &surfaceEvents, &e,
        &eventOps, &e, primary, &fallback));
  CHECK(fallback);
  atomic_store(&surfaceFallback, fallback);
  return fallback;
}

static void checkPublished(unsigned int set, unsigned int clear,
    unsigned int drop)
{
  CHECK(atomic_load(&t.getInput) == set);
  CHECK(atomic_load(&t.getAudio) == set);
  CHECK(atomic_load(&t.getClipboard) == set);
  CHECK(atomic_load(&pInput.set) == set);
  CHECK(atomic_load(&pInput.clear) == clear);
  CHECK(atomic_load(&pInput.drop) == drop);
#ifdef ENABLE_AUDIO
  CHECK(atomic_load(&pAudio.set) == set);
  CHECK(atomic_load(&pAudio.clear) == clear);
  CHECK(atomic_load(&pAudio.drop) == drop);
#endif
  CHECK(atomic_load(&pClipboard.set) == set);
  CHECK(atomic_load(&pClipboard.clear) == clear);
  CHECK(atomic_load(&pClipboard.drop) == drop);
  CHECK(!atomic_load(&t.badProvider));
}

static void testGraceful(void)
{
  setRemote(UUID_A);
  atomic_store(&t.hold, true);

  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&t.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(lgTransportFallback_videoRequested(fallback));
  atomic_store(&t.release, true);
  CHECK(waitReady(fallback));
  CHECK(waitCount(&e.conn, 1));
  CHECK(waitCount(&e.videoReady, 1));

  CHECK(lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&t.on) == 1);
  CHECK(memcmp(e.session.uuid, UUID_A, sizeof(UUID_A)) == 0);
  checkPublished(1, 0, 0);

  lgTransportFallback_stop(&fallback);
  CHECK(!fallback);
  CHECK(atomic_load(&e.conn) == 1);
  CHECK(atomic_load(&e.disc) == 1);
  CHECK(atomic_load(&t.off) == 1);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
  checkPublished(1, 1, 0);
}

static void testDead(void)
{
  setRemote(UUID_A);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));

  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&t.on, 1));
  CHECK(waitReady(fallback));
  atomic_store(&t.alive, false);
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&e.disc, 1));

  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(!lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&t.off) == 0);
  checkPublished(1, 0, 1);

  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&e.disc) == 1);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
}

static void testMismatch(void)
{
  setRemote(UUID_B);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&t.free, 2));

  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(!lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&e.conn) == 0);
  CHECK(atomic_load(&e.disc) == 0);
  CHECK(atomic_load(&e.mismatch) == 1);
  CHECK(memcmp(e.primary, UUID_A, sizeof(UUID_A)) == 0);
  CHECK(memcmp(e.fallback, UUID_B, sizeof(UUID_B)) == 0);
  checkPublished(0, 0, 0);

  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&t.conn) == 2);
  CHECK(atomic_load(&t.attach) == 2);
  CHECK(atomic_load(&t.detach) == 2);
  CHECK(atomic_load(&t.disc) == 2);
  CHECK(atomic_load(&t.free) == 2);
}

static void testMissingUUID(void)
{
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));
  CHECK(lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&e.mismatch) == 0);
  lgTransportFallback_stop(&fallback);

  setRemote(UUID_B);
  fallback = start(NULL);
  CHECK(waitCount(&e.conn, 2));
  CHECK(lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&e.mismatch) == 0);
  lgTransportFallback_stop(&fallback);
  checkPublished(2, 2, 0);
}

static void testLateMismatch(void)
{
  setRemote(UUID_A);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));

  lgTransportFallback_setPrimaryUUID(fallback, UUID_B);
  lgTransportFallback_setPrimaryUUID(fallback, UUID_B);
  CHECK(waitCount(&t.free, 2));

  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(!lgTransportFallback_usable(fallback));
  CHECK(atomic_load(&e.conn) == 1);
  CHECK(atomic_load(&e.disc) == 1);
  CHECK(atomic_load(&e.mismatch) == 1);
  CHECK(memcmp(e.primary, UUID_B, sizeof(UUID_B)) == 0);
  CHECK(memcmp(e.fallback, UUID_A, sizeof(UUID_A)) == 0);
  checkPublished(1, 0, 1);

  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&t.conn) == 2);
  CHECK(atomic_load(&t.detach) == 2);
  CHECK(atomic_load(&t.disc) == 2);
  CHECK(atomic_load(&t.free) == 2);
}

static void testRetry(void)
{
  setRemote(UUID_A);
  t.failConn = 2;
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));

  const uint64_t first  = atomic_load(&t.connTime[0]);
  const uint64_t second = atomic_load(&t.connTime[1]);
  const uint64_t third  = atomic_load(&t.connTime[2]);
  CHECK(second - first >= UINT64_C(200000000));
  CHECK(third - second >= UINT64_C(400000000));
  CHECK(atomic_load(&t.conn) == 3);
  CHECK(atomic_load(&t.free) == 2);

  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&e.conn) == 1);
  CHECK(atomic_load(&e.disc) == 1);
  CHECK(atomic_load(&t.attach) == 1);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 3);
  CHECK(atomic_load(&t.free) == 3);
  checkPublished(1, 1, 0);
}

static void testCancel(void)
{
  setRemote(UUID_A);
  atomic_store(&t.hold, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&t.conn, 1));

  lgTransportFallback_stop(&fallback);
  CHECK(!fallback);
  CHECK(atomic_load(&t.cancel));
  CHECK(atomic_load(&e.conn) == 0);
  CHECK(atomic_load(&e.disc) == 0);
  CHECK(atomic_load(&t.attach) == 0);
  CHECK(atomic_load(&t.detach) == 0);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
  checkPublished(0, 0, 0);
}

static void testWithoutVideo(void)
{
  setRemote(UUID_A);
  atomic_store(&t.noVideo, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitUsable(fallback));
  CHECK(waitCount(&e.conn, 1));
  CHECK(!lgTransportFallback_ready(fallback));
  checkPublished(1, 0, 0);

  lgTransportFallback_requestVideoActive(fallback, true);
  usleep(50000);
  CHECK(atomic_load(&t.on) == 0);
  CHECK(lgTransportFallback_usable(fallback));

  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&t.attach) == 0);
  CHECK(atomic_load(&t.detach) == 0);
  checkPublished(1, 1, 0);
}

static void testAttachFailure(void)
{
  setRemote(UUID_A);
  atomic_store(&t.failAttach, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitUsable(fallback));
  CHECK(waitCount(&e.conn, 1));
  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(atomic_load(&t.attach) == 1);
  CHECK(atomic_load(&t.detach) == 0);
  checkPublished(1, 0, 0);
  lgTransportFallback_stop(&fallback);
  checkPublished(1, 1, 0);
}

static void testActiveFailure(void)
{
  setRemote(UUID_A);
  atomic_store(&t.failActive, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&t.on, 1));
  CHECK(waitCount(&e.videoUnavailable, 1));
  CHECK(lgTransportFallback_usable(fallback));
  CHECK(!lgTransportFallback_ready(fallback));
  checkPublished(1, 0, 0);
  lgTransportFallback_stop(&fallback);
  CHECK(atomic_load(&t.detach) == 1);
  checkPublished(1, 1, 0);
}

static void testActivationCancel(void)
{
  setRemote(UUID_A);
  atomic_store(&t.blockActive, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&t.on, 1));
  CHECK(atomic_load(&t.activeEntered));
  lgTransportFallback_stop(&fallback);
  CHECK(!fallback);
  CHECK(atomic_load(&t.cancelPending) > 0);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
}

static void testActivationRequestCancel(void)
{
  setRemote(UUID_A);
  atomic_store(&t.blockActive, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));

  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&t.on, 1));
  CHECK(atomic_load(&t.activeEntered));
  CHECK(lgTransportFallback_acceptsVideoEvents(fallback));
  CHECK(t.surfaceEvents);
  CHECK(t.surfaceEvents->configure);
  t.surfaceEvents->configure(t.surfaceOpaque, 1920, 1080);
  CHECK(atomic_load(&e.configureAccepted) == 1);
  CHECK(atomic_load(&e.surfaceRetained));

  lgTransportFallback_requestVideoActive(fallback, false);
  CHECK(waitCount(&t.cancelPending, 1));
  CHECK(waitCount(&t.off, 1));
  CHECK(waitCount(&e.videoUnavailable, 1));
  usleep(250000);

  CHECK(atomic_load(&e.videoUnavailable) == 1);
  CHECK(atomic_load(&e.videoReady) == 0);
  CHECK(atomic_load(&e.configureAccepted) == 1);
  CHECK(atomic_load(&e.surfaceCleanup) == 1);
  CHECK(!atomic_load(&e.surfaceRetained));
  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(!lgTransportFallback_acceptsVideoEvents(fallback));
  CHECK(lgTransportFallback_usable(fallback));
  checkPublished(1, 0, 0);

  lgTransportFallback_stop(&fallback);
  CHECK(!fallback);
  CHECK(atomic_load(&t.off) == 1);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
  checkPublished(1, 1, 0);
}

static void testDeactivationCompletes(void)
{
  setRemote(UUID_A);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitReady(fallback));
  atomic_store(&t.activeEntered, false);
  lgTransportFallback_stop(&fallback);
  CHECK(!fallback);
  CHECK(atomic_load(&t.off) == 1);
  CHECK(atomic_load(&t.cancelPending) == 0);
  CHECK(atomic_load(&t.detach) == 1);
  CHECK(atomic_load(&t.disc) == 1);
  CHECK(atomic_load(&t.free) == 1);
}

static void testMismatchDuringActivation(void)
{
  setRemote(UUID_A);
  atomic_store(&t.holdActiveReturn, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&e.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  CHECK(waitCount(&t.on, 1));
  CHECK(atomic_load(&t.activeEntered));
  lgTransportFallback_setPrimaryUUID(fallback, UUID_B);
  CHECK(waitCount(&e.disc, 1));
  atomic_store(&t.holdActiveReturn, false);
  CHECK(waitCount(&t.free, 1));
  CHECK(!lgTransportFallback_ready(fallback));
  CHECK(!lgTransportFallback_usable(fallback));
  CHECK(!lgTransportFallback_acceptsVideoEvents(fallback));
  CHECK(atomic_load(&e.videoReady) == 0);
  lgTransportFallback_stop(&fallback);
}

static void testPreRequestedActivation(void)
{
  setRemote(UUID_A);
  atomic_store(&t.hold, true);
  atomic_store(&t.blockActive, true);
  LG_TransportFallback * fallback = start(UUID_A);
  CHECK(waitCount(&t.conn, 1));
  lgTransportFallback_requestVideoActive(fallback, true);
  atomic_store(&t.release, true);
  CHECK(waitCount(&e.conn, 1));
  CHECK(waitCount(&t.on, 1));
  CHECK(atomic_load(&t.activeEntered));
  CHECK(!lgTransportFallback_ready(fallback));

  atomic_store(&t.blockActive, false);
  CHECK(waitReady(fallback));
  CHECK(waitCount(&e.videoReady, 1));
  lgTransportFallback_setPrimaryUUID(fallback, UUID_B);
  CHECK(waitCount(&e.disc, 1));
  CHECK(waitCount(&t.free, 1));
  CHECK(atomic_load(&e.conn) == 1);
  CHECK(atomic_load(&e.disc) == 1);
  CHECK(atomic_load(&e.mismatch) == 1);
  CHECK(!lgTransportFallback_usable(fallback));
  CHECK(!lgTransportFallback_ready(fallback));
  lgTransportFallback_stop(&fallback);
}


struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "graceful", testGraceful },
  { "dead"    , testDead     },
  { "mismatch", testMismatch },
  { "missing-uuid", testMissingUUID },
  { "late-mismatch", testLateMismatch },
  { "retry"   , testRetry    },
  { "cancel"  , testCancel   },
  { "no-video", testWithoutVideo },
  { "attach-fail", testAttachFailure },
  { "active-fail", testActiveFailure },
  { "activation-cancel", testActivationCancel },
  { "activation-request-cancel", testActivationRequestCancel },
  { "deactivation-completes", testDeactivationCompletes },
  { "activation-mismatch", testMismatchDuringActivation },
  { "pre-requested-activation", testPreRequestedActivation },
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
