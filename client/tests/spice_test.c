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

#include "spice.h"
#include "test.h"

#include "common/debug.h"

#include <purespice.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define WAIT_MS 2000U

extern const LG_TransportOps LGT_SPICE;

struct SpiceInput
{
  bool          available;
  unsigned int onN;
  unsigned int offN;
  unsigned int onOrder;
  unsigned int offFirst;
  unsigned int offOrder;
};

struct SpiceClipboard
{
  bool          available;
  unsigned int onN;
  unsigned int offN;
  unsigned int offFirst;
  unsigned int offOrder;
};

struct Pure
{
  PSConfig      config;
  atomic_bool   connectOK;
  atomic_bool   callReady;
  atomic_bool   blockConnect;
  atomic_bool   enterConnect;
  atomic_bool   allowConnect;
  atomic_bool   blockChannelConnect;
  atomic_bool   enterChannelConnect;
  atomic_bool   channelCancel[PS_CHANNEL_MAX];
  atomic_bool   cancel;
  atomic_bool   drop;
  PSStatus      dropStatus;
  bool          infoOK;
  const char  * serverName;
  uint8_t       uuid[16];

  atomic_uint   connectN;
  atomic_uint   disconnectN;
  atomic_uint   processN;
  atomic_uint   mouseModeN;
  atomic_uint   infoN;
  atomic_uint   freeInfoN;
  atomic_uint   order;
  unsigned int  targetSetOrder;
  unsigned int  connectOrder;
  unsigned int  disconnectOrder;

  bool          has[PS_CHANNEL_MAX];
  bool          connected[PS_CHANNEL_MAX];
  bool          connectResult[PS_CHANNEL_MAX];
  bool          disconnectResult[PS_CHANNEL_MAX];
  unsigned int  connectChannelN[PS_CHANNEL_MAX];
  unsigned int  disconnectChannelN[PS_CHANNEL_MAX];
};

struct SurfaceLog
{
  unsigned int       configN;
  unsigned int       destroyN;
  unsigned int       fillN;
  unsigned int       bitmapN;
  unsigned int       pointerN;
  unsigned int       width;
  unsigned int       height;
  int                x;
  int                y;
  int                w;
  int                h;
  int                stride;
  uint32_t           color;
  bool               topDown;
  uint8_t            bitmap[32];
  size_t             bitmapSize;
  LG_TransportPointer pointer;
  uint8_t            shape[32];
  size_t             shapeSize;
  atomic_bool        block;
  atomic_bool        entered;
  atomic_bool        release;
};

struct Fixture
{
  LG_Transport      transport;
  SpiceInput        input;
  SpiceClipboard    clipboard;
  struct SurfaceLog log;
};

static struct Pure ps;
static _Atomic(SpiceClipboard *) cbTarget;

static unsigned int nextOrder(void)
{
  return atomic_fetch_add(&ps.order, 1) + 1;
}

void purespice_init(const PSInit * init)
{
  (void)init;
}

void purespice_beginConnect(void)
{
  atomic_store_explicit(&ps.cancel, false, memory_order_release);
}

bool purespice_connect(const PSConfig * config)
{
  while (!atomic_load_explicit(&ps.allowConnect, memory_order_acquire) &&
      !atomic_load_explicit(&ps.cancel, memory_order_acquire))
    usleep(1000);
  atomic_store_explicit(&ps.enterConnect, true, memory_order_release);

  ps.config       = *config;
  ps.connectOrder = nextOrder();
  atomic_fetch_add(&ps.connectN, 1);

  while (atomic_load(&ps.blockConnect) && !atomic_load(&ps.cancel))
    usleep(1000);

  if (atomic_load(&ps.cancel) || !atomic_load(&ps.connectOK))
    return false;
  if (atomic_load(&ps.callReady) && config->ready)
    config->ready();
  return true;
}

void purespice_cancelConnect(void)
{
  atomic_store_explicit(&ps.cancel, true, memory_order_release);
}

void purespice_beginChannelConnect(PSChannelType channel)
{
  atomic_store_explicit(
      &ps.channelCancel[channel], false, memory_order_release);
}

void purespice_cancelChannelConnect(PSChannelType channel)
{
  atomic_store_explicit(
      &ps.channelCancel[channel], true, memory_order_release);
}

void purespice_disconnect(void)
{
  ps.disconnectOrder = nextOrder();
  atomic_fetch_add(&ps.disconnectN, 1);
}

PSStatus purespice_process(int timeout)
{
  (void)timeout;
  atomic_fetch_add(&ps.processN, 1);
  if (atomic_load(&ps.drop))
    return ps.dropStatus;
  usleep(1000);
  return PS_STATUS_RUN;
}

bool purespice_getServerInfo(PSServerInfo * info)
{
  atomic_fetch_add(&ps.infoN, 1);
  if (!ps.infoOK)
    return false;

  memset(info, 0, sizeof(*info));
  if (ps.serverName)
    info->name = strdup(ps.serverName);
  memcpy(info->uuid, ps.uuid, sizeof(info->uuid));
  return true;
}

void purespice_freeServerInfo(PSServerInfo * info)
{
  free(info->name);
  info->name = NULL;
  atomic_fetch_add(&ps.freeInfoN, 1);
}

bool purespice_hasChannel(PSChannelType channel)
{
  return ps.has[channel];
}

bool purespice_channelConnected(PSChannelType channel)
{
  return ps.connected[channel];
}

bool purespice_connectChannel(PSChannelType channel)
{
  ++ps.connectChannelN[channel];
  atomic_store_explicit(
      &ps.enterChannelConnect, true, memory_order_release);
  while (atomic_load_explicit(
        &ps.blockChannelConnect, memory_order_acquire) &&
      !atomic_load_explicit(&ps.cancel, memory_order_acquire) &&
      !atomic_load_explicit(
        &ps.channelCancel[channel], memory_order_acquire))
    usleep(1000);
  if (atomic_load_explicit(&ps.cancel, memory_order_acquire) ||
      atomic_load_explicit(
        &ps.channelCancel[channel], memory_order_acquire))
    return false;
  if (!ps.connectResult[channel])
    return false;
  ps.connected[channel] = true;
  return true;
}

bool purespice_disconnectChannel(PSChannelType channel)
{
  ++ps.disconnectChannelN[channel];
  if (!ps.disconnectResult[channel])
    return false;
  ps.connected[channel] = false;
  return true;
}

bool purespice_mouseMode(bool server)
{
  (void)server;
  atomic_fetch_add(&ps.mouseModeN, 1);
  return true;
}

bool spiceInput_init(SpiceInput ** input)
{
  *input = calloc(1, sizeof(**input));
  return *input != NULL;
}

void spiceInput_free(SpiceInput ** input)
{
  if (!input)
    return;
  free(*input);
  *input = NULL;
}

const LG_InputOps * spiceInput_getOps(void)
{
  static const LG_InputOps ops = { .name = "test" };
  return &ops;
}

void spiceInput_setAvailable(SpiceInput * input, bool available)
{
  input->available = available;
  if (available)
  {
    ++input->onN;
    input->onOrder = nextOrder();
  }
  else
  {
    ++input->offN;
    input->offOrder = nextOrder();
    if (input->offN == 1)
      input->offFirst = input->offOrder;
  }
}

bool spiceClipboard_init(SpiceClipboard ** clipboard)
{
  *clipboard = calloc(1, sizeof(**clipboard));
  return *clipboard != NULL;
}

void spiceClipboard_free(SpiceClipboard ** clipboard)
{
  if (!clipboard)
    return;
  free(*clipboard);
  *clipboard = NULL;
}

const LG_ClipboardOps * spiceClipboard_getOps(void)
{
  static const LG_ClipboardOps ops = { .name = "test" };
  return &ops;
}

void spiceClipboard_setAvailable(SpiceClipboard * clipboard, bool available)
{
  clipboard->available = available;
  if (available)
    ++clipboard->onN;
  else
  {
    ++clipboard->offN;
    clipboard->offOrder = nextOrder();
    if (clipboard->offN == 1)
      clipboard->offFirst = clipboard->offOrder;
  }
}

void spiceClipboard_setCallbackTarget(SpiceClipboard * clipboard)
{
  atomic_store(&cbTarget, clipboard);
  if (clipboard)
    ps.targetSetOrder = nextOrder();
}

void spiceClipboard_notice(PSDataType type)
{
  (void)type;
}

void spiceClipboard_data(PSDataType type, uint8_t * buffer, uint32_t size)
{
  (void)type;
  (void)buffer;
  (void)size;
}

void spiceClipboard_release(void)
{
}

void spiceClipboard_request(PSDataType type)
{
  (void)type;
}

void spiceClipboard_status(bool available)
{
  SpiceClipboard * clipboard = atomic_load(&cbTarget);
  if (clipboard)
    spiceClipboard_setAvailable(clipboard, available);
}

static void resetPure(void)
{
  memset(&ps, 0, sizeof(ps));
  atomic_init(&ps.connectOK, true);
  atomic_init(&ps.callReady, true);
  atomic_init(&ps.blockConnect, false);
  atomic_init(&ps.enterConnect, false);
  atomic_init(&ps.allowConnect, true);
  atomic_init(&ps.blockChannelConnect, false);
  atomic_init(&ps.enterChannelConnect, false);
  for(unsigned int i = 0; i < PS_CHANNEL_MAX; ++i)
    atomic_init(&ps.channelCancel[i], false);
  atomic_init(&ps.cancel, false);
  atomic_init(&ps.drop, false);
  atomic_init(&ps.connectN, 0);
  atomic_init(&ps.disconnectN, 0);
  atomic_init(&ps.processN, 0);
  atomic_init(&ps.mouseModeN, 0);
  atomic_init(&ps.infoN, 0);
  atomic_init(&ps.freeInfoN, 0);
  atomic_init(&ps.order, 0);
  atomic_store(&cbTarget, NULL);

  ps.dropStatus = PS_STATUS_ERR_READ;
  ps.infoOK     = true;
  ps.serverName = "test-vm";
  ps.uuid[0]    = 0x42;
  for (unsigned int i = 0; i < PS_CHANNEL_MAX; ++i)
  {
    ps.has[i]              = true;
    ps.connectResult[i]    = true;
    ps.disconnectResult[i] = true;
  }
}

static void configure(void * opaque, unsigned int width,
    unsigned int height)
{
  struct SurfaceLog * log = opaque;
  ++log->configN;
  log->width  = width;
  log->height = height;
  if (atomic_load(&log->block))
  {
    atomic_store(&log->entered, true);
    while (!atomic_load(&log->release))
      usleep(1000);
  }
}

static void destroy(void * opaque)
{
  struct SurfaceLog * log = opaque;
  ++log->destroyN;
}

static void drawFill(void * opaque, int x, int y,
    int width, int height, uint32_t color)
{
  struct SurfaceLog * log = opaque;
  ++log->fillN;
  log->x     = x;
  log->y     = y;
  log->w     = width;
  log->h     = height;
  log->color = color;
}

static void drawBitmap(void * opaque, bool topDown,
    int x, int y, int width, int height, int stride, const void * data)
{
  struct SurfaceLog * log = opaque;
  ++log->bitmapN;
  log->topDown    = topDown;
  log->x          = x;
  log->y          = y;
  log->w          = width;
  log->h          = height;
  log->stride     = stride;
  log->bitmapSize = (size_t)height * (size_t)stride;
  CHECK(log->bitmapSize <= sizeof(log->bitmap));
  memcpy(log->bitmap, data, log->bitmapSize);
}

static void pointer(void * opaque, const LG_TransportPointer * pointer)
{
  struct SurfaceLog * log = opaque;
  ++log->pointerN;
  log->pointer = *pointer;
  if (pointer->shape)
  {
    log->shapeSize = (size_t)pointer->height * pointer->pitch;
    CHECK(log->shapeSize <= sizeof(log->shape));
    memcpy(log->shape, pointer->shape, log->shapeSize);
    log->pointer.shape = log->shape;
  }
}

static const LG_SwSurfaceEventOps surfaceEvents =
{
  .configure  = configure,
  .destroy    = destroy,
  .drawFill   = drawFill,
  .drawBitmap = drawBitmap,
  .pointer    = pointer,
};

static void initFixture(struct Fixture * f)
{
  memset(f, 0, sizeof(*f));
  f->transport.host             = "test";
  f->transport.port             = 5900;
  f->transport.inputEnabled     = true;
  f->transport.clipboardEnabled = true;
  f->transport.input            = &f->input;
  f->transport.clipboard        = &f->clipboard;
  CHECK(spiceSurface_init(&f->transport.surface));
  f->transport.connectEvent = lgCreateEvent(false, 0);
  CHECK(f->transport.connectEvent);
  atomic_init(&f->transport.stop, false);
  atomic_init(&f->transport.sessionValid, false);
  atomic_init(&f->log.block, false);
  atomic_init(&f->log.entered, false);
  atomic_init(&f->log.release, false);
}

static void freeFixture(struct Fixture * f)
{
  LGT_SPICE.disconnect(&f->transport);
  lgFreeEvent(f->transport.connectEvent);
  f->transport.connectEvent = NULL;
  spiceSurface_free(&f->transport.surface);
}

static void waitInvalid(LG_Transport * transport)
{
  for (unsigned int i = 0; i < WAIT_MS; ++i)
  {
    if (!LGT_SPICE.sessionValid(transport))
      return;
    usleep(1000);
  }

  CHECK(!LGT_SPICE.sessionValid(transport));
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

static bool cancel(void * opaque)
{
  unsigned int * calls = opaque;
  ++*calls;
  return true;
}

static bool cancelBeforeEntry(void * opaque)
{
  unsigned int * calls = opaque;
  ++*calls;
  return !atomic_load_explicit(&ps.enterConnect, memory_order_acquire);
}

static void testConnectFail(void)
{
  resetPure();
  atomic_store(&ps.connectOK, false);
  struct Fixture f;
  initFixture(&f);

  LG_TransportSession session;
  CHECK(LGT_SPICE.connect(&f.transport, &session) == LG_TRANSPORT_ERROR);
  CHECK(!f.transport.thread);
  CHECK(!LGT_SPICE.sessionValid(&f.transport));
  CHECK(atomic_load(&ps.connectN) == 1);
  CHECK(atomic_load(&ps.disconnectN) == 0);
  CHECK(f.input.onN == 0);
  CHECK(f.input.offN == 1);
  CHECK(f.clipboard.offN == 1);
  CHECK(!atomic_load(&cbTarget));
  CHECK(ps.targetSetOrder < ps.connectOrder);

  ps.config.ready();
  CHECK(f.input.onN == 0);
  CHECK(!LGT_SPICE.sessionValid(&f.transport));
  freeFixture(&f);
}

static void testPreReadyDrop(void)
{
  resetPure();
  atomic_store(&ps.callReady, false);
  atomic_store(&ps.drop, true);
  ps.dropStatus = PS_STATUS_SHUTDOWN;
  struct Fixture f;
  initFixture(&f);

  LG_TransportSession session;
  CHECK(LGT_SPICE.connect(&f.transport, &session) == LG_TRANSPORT_ERROR);
  CHECK(!f.transport.thread);
  CHECK(!LGT_SPICE.sessionValid(&f.transport));
  CHECK(atomic_load(&ps.connectN) == 1);
  CHECK(atomic_load(&ps.processN) == 1);
  CHECK(atomic_load(&ps.disconnectN) == 1);
  CHECK(f.input.onN == 0);
  CHECK(f.input.offN == 2);
  CHECK(f.clipboard.offN == 2);
  CHECK(f.input.offFirst < ps.disconnectOrder);
  CHECK(f.clipboard.offFirst < ps.disconnectOrder);
  CHECK(!atomic_load(&cbTarget));
  freeFixture(&f);
}

static void testCancel(void)
{
  resetPure();
  atomic_store(&ps.blockConnect, true);
  struct Fixture f;
  initFixture(&f);
  unsigned int cancelN = 0;
  LG_TransportSession session;

  CHECK(LGT_SPICE.connectCancellable(&f.transport, &session,
        cancel, &cancelN) == LG_TRANSPORT_DISCONNECTED);
  CHECK(cancelN == 1);
  CHECK(!f.transport.thread);
  CHECK(!LGT_SPICE.sessionValid(&f.transport));
  CHECK(atomic_load(&ps.connectN) == 1);
  CHECK(atomic_load(&ps.disconnectN) == 0);
  CHECK(f.input.onN == 0);
  CHECK(f.input.offN == 1);
  CHECK(f.clipboard.offN == 1);
  CHECK(!atomic_load(&cbTarget));
  freeFixture(&f);

  resetPure();
  atomic_store_explicit(&ps.allowConnect, false, memory_order_release);
  atomic_store_explicit(&ps.enterConnect, false, memory_order_release);
  initFixture(&f);
  cancelN = 0;
  CHECK(LGT_SPICE.connectCancellable(&f.transport, &session,
        cancelBeforeEntry, &cancelN) == LG_TRANSPORT_DISCONNECTED);
  CHECK(cancelN == 1);
  CHECK(!f.transport.thread);
  CHECK(!LGT_SPICE.sessionValid(&f.transport));
  CHECK(atomic_load_explicit(&ps.cancel, memory_order_acquire));
  freeFixture(&f);
}

static void testReadyDrop(void)
{
  resetPure();
  struct Fixture f;
  initFixture(&f);
  const LG_VideoOps * video = LGT_SPICE.getVideoOps(&f.transport);
  CHECK(video && video->type == LG_VIDEO_TYPE_SW_SURFACE);
  CHECK(video->swSurface->attach(
        &f.transport, &surfaceEvents, &f.log));

  LG_TransportSession session;
  CHECK(LGT_SPICE.connect(&f.transport, &session) == LG_TRANSPORT_OK);
  CHECK(LGT_SPICE.sessionValid(&f.transport));
  CHECK(strcmp(session.version, "SPICE") == 0);
  CHECK(strcmp(session.name, "test-vm") == 0);
  CHECK(session.uuidValid);
  CHECK(session.uuid[0] == 0x42);
  CHECK(session.os == LG_TRANSPORT_OS_OTHER);
  CHECK(f.input.available);
  CHECK(f.input.onN == 1);
  CHECK(atomic_load(&ps.mouseModeN) == 1);
  CHECK(atomic_load(&ps.infoN) == 1);
  CHECK(atomic_load(&ps.freeInfoN) == 1);
  CHECK(ps.targetSetOrder < ps.connectOrder);
  CHECK(ps.connectOrder < f.input.onOrder);

  ps.config.clipboard.status(true);
  CHECK(f.clipboard.available);
  ps.config.display.surfaceCreate(
      0, PS_SURFACE_FMT_32_xRGB, 640, 480);
  CHECK(f.log.configN == 1);
  CHECK(video->swSurface->setActive(&f.transport, true));

  atomic_store(&ps.drop, true);
  waitInvalid(&f.transport);
  LGT_SPICE.disconnect(&f.transport);
  CHECK(!f.transport.thread);
  CHECK(!f.input.available);
  CHECK(!f.clipboard.available);
  CHECK(f.input.offN >= 1);
  CHECK(f.clipboard.offN >= 1);
  CHECK(atomic_load(&ps.disconnectN) == 1);
  CHECK(f.input.offFirst < ps.disconnectOrder);
  CHECK(f.clipboard.offFirst < ps.disconnectOrder);
  CHECK(!atomic_load(&cbTarget));
  CHECK(LGT_SPICE.sendControl(&f.transport, NULL, NULL) ==
      LG_TRANSPORT_DISCONNECTED);

  const unsigned int inputOn = f.input.onN;
  const unsigned int configN = f.log.configN;
  ps.config.ready();
  ps.config.display.surfaceCreate(
      0, PS_SURFACE_FMT_32_xRGB, 800, 600);
  CHECK(f.input.onN == inputOn);
  CHECK(f.log.configN == configN);
  video->swSurface->detach(&f.transport);
  freeFixture(&f);
}

static void testSessionOrder(void)
{
  resetPure();
  struct Fixture first;
  struct Fixture second;
  initFixture(&first);
  initFixture(&second);

  LG_TransportSession session;
  CHECK(LGT_SPICE.connect(&first.transport, &session) == LG_TRANSPORT_OK);
  CHECK(LGT_SPICE.connect(&second.transport, &session) ==
      LG_TRANSPORT_ERROR);
  CHECK(!second.transport.thread);
  CHECK(atomic_load(&ps.connectN) == 1);
  CHECK(LGT_SPICE.sessionValid(&first.transport));
  CHECK(!LGT_SPICE.sessionValid(&second.transport));

  LGT_SPICE.disconnect(&first.transport);
  CHECK(!LGT_SPICE.sessionValid(&first.transport));
  CHECK(LGT_SPICE.connect(&second.transport, &session) == LG_TRANSPORT_OK);
  CHECK(atomic_load(&ps.connectN) == 2);
  CHECK(LGT_SPICE.sessionValid(&second.transport));
  LGT_SPICE.disconnect(&second.transport);
  freeFixture(&second);
  freeFixture(&first);
}

static void testSurfaceActive(void)
{
  resetPure();
  struct Fixture f;
  initFixture(&f);
  const LG_SwSurfaceOps * sw =
    spiceSurface_getVideoOps()->swSurface;

  LG_SwSurfaceEventOps bad = surfaceEvents;
  bad.pointer = NULL;
  CHECK(!sw->attach(&f.transport, &bad, &f.log));
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));
  CHECK(!sw->setActive(&f.transport, true));
  atomic_store(&f.transport.sessionValid, true);

  ps.has[PS_CHANNEL_DISPLAY] = false;
  CHECK(!sw->setActive(&f.transport, true));
  ps.has[PS_CHANNEL_DISPLAY] = true;
  ps.connectResult[PS_CHANNEL_DISPLAY] = false;
  CHECK(!sw->setActive(&f.transport, true));
  CHECK(ps.connectChannelN[PS_CHANNEL_DISPLAY] == 1);
  CHECK(ps.connectChannelN[PS_CHANNEL_CURSOR] == 0);

  ps.connectResult[PS_CHANNEL_DISPLAY] = true;
  ps.connectResult[PS_CHANNEL_CURSOR]  = false;
  CHECK(!sw->setActive(&f.transport, true));
  CHECK(ps.disconnectChannelN[PS_CHANNEL_DISPLAY] == 1);
  CHECK(!ps.connected[PS_CHANNEL_DISPLAY]);

  ps.connectResult[PS_CHANNEL_CURSOR] = true;
  CHECK(sw->setActive(&f.transport, true));
  const unsigned int displayConnect =
    ps.connectChannelN[PS_CHANNEL_DISPLAY];
  const unsigned int cursorConnect =
    ps.connectChannelN[PS_CHANNEL_CURSOR];
  CHECK(sw->setActive(&f.transport, true));
  CHECK(ps.connectChannelN[PS_CHANNEL_DISPLAY] == displayConnect);
  CHECK(ps.connectChannelN[PS_CHANNEL_CURSOR] == cursorConnect);

  ps.disconnectResult[PS_CHANNEL_DISPLAY] = false;
  CHECK(!sw->setActive(&f.transport, false));
  CHECK(ps.disconnectChannelN[PS_CHANNEL_CURSOR] == 0);
  ps.disconnectResult[PS_CHANNEL_DISPLAY] = true;
  ps.disconnectResult[PS_CHANNEL_CURSOR]  = false;
  CHECK(!sw->setActive(&f.transport, false));
  CHECK(ps.connected[PS_CHANNEL_DISPLAY]);
  CHECK(ps.connectChannelN[PS_CHANNEL_DISPLAY] == displayConnect + 1);

  ps.disconnectResult[PS_CHANNEL_CURSOR] = true;
  CHECK(sw->setActive(&f.transport, false));
  CHECK(!ps.connected[PS_CHANNEL_DISPLAY]);
  CHECK(!ps.connected[PS_CHANNEL_CURSOR]);
  sw->detach(&f.transport);
  freeFixture(&f);
}

struct ActiveCall
{
  const LG_SwSurfaceOps * ops;
  LG_Transport          * transport;
  atomic_bool             done;
  bool                    result;
};

static void * activateSurface(void * opaque)
{
  struct ActiveCall * call = opaque;
  call->result = call->ops->setActive(call->transport, true);
  atomic_store_explicit(&call->done, true, memory_order_release);
  return NULL;
}

static void * holdActivationLock(void * opaque)
{
  LG_Transport * transport = opaque;
  spiceSurface_testLockActivation(transport->surface);
  atomic_store_explicit(
      &ps.enterChannelConnect, true, memory_order_release);
  while (atomic_load_explicit(
        &ps.blockChannelConnect, memory_order_acquire))
    usleep(1000);
  spiceSurface_testUnlockActivation(transport->surface);
  return NULL;
}

static void testSurfaceCancel(void)
{
  resetPure();
  struct Fixture f;
  initFixture(&f);
  const LG_SwSurfaceOps * sw = spiceSurface_getVideoOps()->swSurface;
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));
  atomic_store_explicit(
      &f.transport.sessionValid, true, memory_order_release);
  atomic_store_explicit(
      &ps.blockChannelConnect, true, memory_order_release);

  struct ActiveCall call =
  {
    .ops       = sw,
    .transport = &f.transport,
  };
  atomic_init(&call.done, false);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, activateSurface, &call) == 0);
  waitBool(&ps.enterChannelConnect);

  sw->cancelPending(&f.transport);
  waitBool(&call.done);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(!call.result);
  CHECK(atomic_load_explicit(
        &ps.channelCancel[PS_CHANNEL_DISPLAY], memory_order_acquire));
  CHECK(!atomic_load_explicit(&ps.cancel, memory_order_acquire));
  CHECK(ps.connectChannelN[PS_CHANNEL_DISPLAY] == 1);
  CHECK(ps.connectChannelN[PS_CHANNEL_CURSOR] == 0);
  CHECK(LGT_SPICE.sessionValid(&f.transport));

  atomic_store_explicit(
      &ps.blockChannelConnect, false, memory_order_release);
  CHECK(sw->setActive(&f.transport, true));
  CHECK(LGT_SPICE.sessionValid(&f.transport));
  CHECK(sw->setActive(&f.transport, false));

  sw->detach(&f.transport);
  freeFixture(&f);

  resetPure();
  initFixture(&f);
  sw = spiceSurface_getVideoOps()->swSurface;
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));
  atomic_store_explicit(
      &f.transport.sessionValid, true, memory_order_release);
  atomic_store_explicit(
      &ps.blockChannelConnect, true, memory_order_release);

  pthread_t holdThread;
  CHECK(pthread_create(
        &holdThread, NULL, holdActivationLock, &f.transport) == 0);
  waitBool(&ps.enterChannelConnect);

  call = (struct ActiveCall)
  {
    .ops       = sw,
    .transport = &f.transport,
  };
  atomic_init(&call.done, false);
  CHECK(pthread_create(&thread, NULL, activateSurface, &call) == 0);
  usleep(10000);
  sw->cancelPending(&f.transport);
  atomic_store_explicit(
      &ps.blockChannelConnect, false, memory_order_release);
  CHECK(pthread_join(holdThread, NULL) == 0);
  waitBool(&call.done);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(!call.result);
  CHECK(ps.connectChannelN[PS_CHANNEL_DISPLAY] == 0);

  sw->detach(&f.transport);
  freeFixture(&f);

  resetPure();
  initFixture(&f);
  sw = spiceSurface_getVideoOps()->swSurface;
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));
  atomic_store_explicit(
      &f.transport.sessionValid, true, memory_order_release);
  atomic_store_explicit(
      &ps.blockChannelConnect, true, memory_order_release);

  call = (struct ActiveCall)
  {
    .ops       = sw,
    .transport = &f.transport,
  };
  atomic_init(&call.done, false);
  CHECK(pthread_create(&thread, NULL, activateSurface, &call) == 0);
  waitBool(&ps.enterChannelConnect);

  atomic_store_explicit(
      &f.transport.sessionValid, false, memory_order_release);
  spiceSurface_sessionStopped(f.transport.surface);
  waitBool(&call.done);
  CHECK(pthread_join(thread, NULL) == 0);
  CHECK(!call.result);
  CHECK(atomic_load_explicit(
        &ps.channelCancel[PS_CHANNEL_DISPLAY], memory_order_acquire));
  CHECK(atomic_load_explicit(
        &ps.channelCancel[PS_CHANNEL_CURSOR], memory_order_acquire));
  CHECK(!atomic_load_explicit(&ps.cancel, memory_order_acquire));
  CHECK(sw->setActive(&f.transport, false));

  atomic_store_explicit(
      &ps.blockChannelConnect, false, memory_order_release);
  sw->detach(&f.transport);
  freeFixture(&f);
}

static void testSurfaceEvents(void)
{
  resetPure();
  struct Fixture f;
  initFixture(&f);
  const LG_SwSurfaceOps * sw =
    spiceSurface_getVideoOps()->swSurface;
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));

  spiceSurface_create(f.transport.surface,
      1, PS_SURFACE_FMT_32_xRGB, 20, 10);
  CHECK(f.log.configN == 0);
  spiceSurface_create(f.transport.surface,
      0, PS_SURFACE_FMT_32_xRGB, 2, 1);
  CHECK(f.log.configN == 1);
  CHECK(f.log.width == 2);
  CHECK(f.log.height == 1);
  CHECK(f.log.fillN == 0);

  spiceSurface_drawFill(
      f.transport.surface, 0, 1, 2, 3, 4, 0xaabbccdd);
  CHECK(f.log.fillN == 1);
  CHECK(f.log.x == 1);
  CHECK(f.log.y == 2);
  CHECK(f.log.w == 3);
  CHECK(f.log.h == 4);
  CHECK(f.log.color == 0xaabbccdd);

  uint8_t bitmap[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  spiceSurface_drawBitmap(f.transport.surface, 0,
      PS_BITMAP_FMT_24BIT, true, 0, 0, 2, 1, 8, bitmap);
  CHECK(f.log.bitmapN == 0);
  spiceSurface_drawBitmap(f.transport.surface, 0,
      PS_BITMAP_FMT_RGBA, true, 4, 5, 2, 1, 8, bitmap);
  CHECK(f.log.bitmapN == 1);
  CHECK(f.log.topDown);
  CHECK(f.log.x == 4);
  CHECK(f.log.y == 5);
  CHECK(f.log.bitmapSize == sizeof(bitmap));
  CHECK(memcmp(f.log.bitmap, bitmap, sizeof(bitmap)) == 0);

  const uint8_t rgba[] =
  {
    1, 2, 3, 4,
    5, 6, 7, 8,
  };
  const uint8_t bgra[] =
  {
    3, 2, 1, 4,
    7, 6, 5, 8,
  };
  spiceSurface_setRGBAImage(
      f.transport.surface, 2, 1, 6, 7, rgba);
  CHECK(f.log.pointerN == 1);
  CHECK(f.log.pointer.flags == LG_TRANSPORT_POINTER_SHAPE);
  CHECK(f.log.pointer.type == CURSOR_TYPE_COLOR);
  CHECK(f.log.pointer.hx == 6);
  CHECK(f.log.pointer.hy == 7);
  CHECK(f.log.shapeSize == sizeof(bgra));
  CHECK(memcmp(f.log.shape, bgra, sizeof(bgra)) == 0);

  spiceSurface_setPointerState(f.transport.surface, true, 8, 9);
  CHECK(f.log.pointerN == 2);
  CHECK(f.log.pointer.flags & LG_TRANSPORT_POINTER_VISIBLE);
  CHECK(f.log.pointer.x == 8);
  CHECK(f.log.pointer.y == 9);
  CHECK(f.log.pointer.hx == 6);
  CHECK(f.log.pointer.hy == 7);

  spiceSurface_destroy(f.transport.surface, 0);
  CHECK(f.log.destroyN == 1);
  spiceSurface_drawFill(f.transport.surface, 0, 0, 0, 1, 1, 0);
  CHECK(f.log.fillN == 1);
  sw->detach(&f.transport);
  freeFixture(&f);
}

struct CreateCall
{
  SpiceSurface * surface;
};

static void * createSurface(void * opaque)
{
  struct CreateCall * call = opaque;
  spiceSurface_create(
      call->surface, 0, PS_SURFACE_FMT_32_xRGB, 640, 480);
  return NULL;
}

struct DetachCall
{
  LG_Transport           * transport;
  const LG_SwSurfaceOps  * ops;
  atomic_bool              started;
  atomic_bool              done;
};

static void * detachSurface(void * opaque)
{
  struct DetachCall * call = opaque;
  atomic_store(&call->started, true);
  call->ops->detach(call->transport);
  atomic_store(&call->done, true);
  return NULL;
}

static void testSurfaceDetach(void)
{
  resetPure();
  struct Fixture f;
  initFixture(&f);
  const LG_SwSurfaceOps * sw =
    spiceSurface_getVideoOps()->swSurface;
  CHECK(sw->attach(&f.transport, &surfaceEvents, &f.log));
  atomic_store(&f.log.block, true);

  struct CreateCall create = { .surface = f.transport.surface };
  pthread_t createThread;
  CHECK(pthread_create(&createThread, NULL, createSurface, &create) == 0);
  waitBool(&f.log.entered);

  struct DetachCall detach =
  {
    .transport = &f.transport,
    .ops       = sw,
  };
  atomic_init(&detach.started, false);
  atomic_init(&detach.done, false);
  pthread_t detachThread;
  CHECK(pthread_create(&detachThread, NULL, detachSurface, &detach) == 0);
  waitBool(&detach.started);
  usleep(10000);
  CHECK(!atomic_load(&detach.done));

  atomic_store(&f.log.release, true);
  CHECK(pthread_join(createThread, NULL) == 0);
  CHECK(pthread_join(detachThread, NULL) == 0);
  CHECK(atomic_load(&detach.done));
  CHECK(f.log.configN == 1);
  CHECK(f.log.fillN == 0);
  spiceSurface_drawFill(f.transport.surface, 0, 0, 0, 1, 1, 0);
  CHECK(f.log.fillN == 0);
  freeFixture(&f);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "connect-fail"  , testConnectFail   },
  { "pre-ready-drop", testPreReadyDrop  },
  { "cancel"        , testCancel        },
  { "ready-drop"    , testReadyDrop     },
  { "session-order" , testSessionOrder  },
  { "surface-active", testSurfaceActive },
  { "surface-cancel", testSurfaceCancel },
  { "surface-events", testSurfaceEvents },
  { "surface-detach", testSurfaceDetach },
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
