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

#include "wayland.h"
#include "test.h"
#include "../src/clipboard.h"

#include "common/debug.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_ABORT    8U
#define MAX_DATA     4U
#define MAX_MIME    12U
#define MAX_NOTICE   8U
#define MAX_OFFER    8U
#define MAX_POLL     8U
#define MAX_SOURCE   4U
#define MAX_TRANSFER 8192U

enum ProxyKind
{
  PROXY_MANAGER,
  PROXY_DEVICE,
  PROXY_OFFER,
  PROXY_SOURCE,
};

struct Proxy
{
  enum ProxyKind kind;
  void (**listener)(void);
  void           * data;
  void           * user;
  bool             dead;
  char             mime[MAX_MIME][80];
  unsigned int     mimeN;
};

struct Proto
{
  struct Proxy   manager;
  struct Proxy   device;
  struct Proxy   offer[MAX_OFFER];
  struct Proxy   source[MAX_SOURCE];
  unsigned int   offerN;
  unsigned int   sourceN;
  unsigned int   offerDestroyN;
  unsigned int   sourceDestroyN;
  unsigned int   deviceReleaseN;
  unsigned int   dirtyDestroyN;
  unsigned int   receiveN;
  unsigned int   setActionsN;
  unsigned int   selectionSetN;
  char           receiveMime[80];
  const void   * receiveData;
  size_t         receiveSize;
  int            receiveFd;
  bool           keepReceiveOpen;
  struct Proxy * selection;
  uint32_t       serial;
};

struct Poll
{
  int                 fd;
  WaylandPollCallback callback;
  WaylandPollCleanup  cleanup;
  void              * opaque;
  uint32_t            events;
  bool                active;
  bool                retired;
};

struct TypeNotice
{
  LG_ClipboardData type[LG_CLIPBOARD_DATA_NONE];
  size_t           count;
};

struct DataNotice
{
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  uint8_t             data[MAX_TRANSFER];
  size_t              size;
};

struct StreamNotice
{
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  uint64_t            sizeHint;
  uint64_t            finalSize;
  uint8_t             data[MAX_TRANSFER];
  size_t              size;
  unsigned int        beginN;
  unsigned int        chunkN;
  unsigned int        endN;
};

struct Log
{
  struct Poll         poll[MAX_POLL];
  struct TypeNotice   notice[MAX_NOTICE];
  struct DataNotice   data[MAX_DATA];
  struct StreamNotice streamData;
  LG_ClipboardRequest abort[MAX_ABORT];
  unsigned int        pollN;
  unsigned int        pollUnregisterN;
  unsigned int        pollCleanupN;
  unsigned int        noticeN;
  unsigned int        dataN;
  unsigned int        abortN;
  unsigned int        releaseN;
  unsigned int        requestN;
  unsigned int        requestReadyN;
  LG_ClipboardData    requestType;
  const LG_ClipboardStreamOps * requestStream;
  void               * requestOpaque;
  LG_ClipboardRequest requestId;
  LG_ClipboardResult  beginResult;
  LG_ClipboardResult  chunkResult;
  LG_ClipboardResult  endResult;
  LG_ClipboardResult  requestBeginResult;
  bool                 readyDuringChunk;
  bool                 readyDuringEnd;
  bool                 pollOK;
  bool                 requestOK;
  bool                 requestCancelSync;
  bool                 requestBeginSync;
  bool                 expectReleaseLocked;
  bool                 firing;
  unsigned int         releaseLockedN;
};

struct WaylandDSState wlWm;
struct WCBState       wlCb;

static struct Proto proto;
static struct Log   rec;

const struct wl_interface wl_data_device_interface =
{
  .name    = "wl_data_device",
  .version = 3,
};

const struct wl_interface wl_data_source_interface =
{
  .name    = "wl_data_source",
  .version = 3,
};

uint32_t wl_proxy_get_version(struct wl_proxy * proxy)
{
  CHECK(!((struct Proxy *)proxy)->dead);
  return 3;
}

void wl_proxy_set_user_data(struct wl_proxy * proxy, void * data)
{
  struct Proxy * p = (struct Proxy *)proxy;
  CHECK(!p->dead);
  p->user = data;
}

void * wl_proxy_get_user_data(struct wl_proxy * proxy)
{
  struct Proxy * p = (struct Proxy *)proxy;
  CHECK(!p->dead);
  return p->user;
}

int wl_proxy_add_listener(struct wl_proxy * proxy,
    void (**listener)(void), void * data)
{
  struct Proxy * p = (struct Proxy *)proxy;
  CHECK(!p->dead);
  CHECK(!p->listener);
  p->listener = listener;
  p->data     = data;
  return 0;
}

static void destroy(struct Proxy * p)
{
  CHECK(p);
  CHECK(!p->dead);
  p->dead = true;
  if (p->user)
    ++proto.dirtyDestroyN;

  switch (p->kind)
  {
    case PROXY_OFFER:
      ++proto.offerDestroyN;
      break;
    case PROXY_SOURCE:
      ++proto.sourceDestroyN;
      break;
    case PROXY_DEVICE:
      ++proto.deviceReleaseN;
      break;
    default:
      CHECK(false);
  }
}

static void copyMime(struct Proxy * p, const char * mime)
{
  CHECK(p->mimeN < ARRAY_LENGTH(p->mime));
  const int n = snprintf(p->mime[p->mimeN], sizeof(p->mime[p->mimeN]),
      "%s", mime);
  CHECK(n >= 0);
  CHECK((size_t)n < sizeof(p->mime[p->mimeN]));
  ++p->mimeN;
}

static void sendData(int fd, const void * data, size_t size)
{
  const uint8_t * pos = data;
  while (size)
  {
    const ssize_t n = write(fd, pos, size);
    CHECK(n > 0);
    pos  += n;
    size -= n;
  }
}

struct wl_proxy * wl_proxy_marshal_flags(struct wl_proxy * proxy,
    uint32_t opcode, const struct wl_interface * interface,
    uint32_t version, uint32_t flags, ...)
{
  struct Proxy * p = (struct Proxy *)proxy;
  CHECK(p);
  CHECK(!p->dead);
  CHECK(version == 3);

  if (flags & WL_MARSHAL_FLAG_DESTROY)
  {
    CHECK(!interface);
    switch (p->kind)
    {
      case PROXY_DEVICE:
        CHECK(opcode == WL_DATA_DEVICE_RELEASE);
        break;
      case PROXY_OFFER:
        CHECK(opcode == WL_DATA_OFFER_DESTROY);
        break;
      case PROXY_SOURCE:
        CHECK(opcode == WL_DATA_SOURCE_DESTROY);
        break;
      default:
        CHECK(false);
    }
    destroy(p);
    return NULL;
  }

  va_list ap;
  va_start(ap, flags);
  switch (p->kind)
  {
    case PROXY_MANAGER:
      if (opcode == WL_DATA_DEVICE_MANAGER_GET_DATA_DEVICE)
      {
        CHECK(interface == &wl_data_device_interface);
        p = &proto.device;
      }
      else
      {
        CHECK(opcode == WL_DATA_DEVICE_MANAGER_CREATE_DATA_SOURCE);
        CHECK(interface == &wl_data_source_interface);
        CHECK(proto.sourceN < ARRAY_LENGTH(proto.source));
        p       = &proto.source[proto.sourceN++];
        p->kind = PROXY_SOURCE;
      }
      break;

    case PROXY_DEVICE:
      CHECK(opcode == WL_DATA_DEVICE_SET_SELECTION);
      CHECK(!interface);
      proto.selection = (struct Proxy *)va_arg(ap, struct wl_data_source *);
      proto.serial    = va_arg(ap, unsigned int);
      ++proto.selectionSetN;
      p               = NULL;
      break;

    case PROXY_OFFER:
      CHECK(!interface);
      if (opcode == WL_DATA_OFFER_RECEIVE)
      {
        copyMime(p, va_arg(ap, const char *));
        const int fd = va_arg(ap, int);
        ++proto.receiveN;
        snprintf(proto.receiveMime, sizeof(proto.receiveMime),
            "%s", p->mime[p->mimeN - 1]);
        if (proto.keepReceiveOpen)
        {
          CHECK(proto.receiveFd < 0);
          proto.receiveFd = dup(fd);
          CHECK(proto.receiveFd >= 0);
        }
        if (proto.receiveSize)
          sendData(fd, proto.receiveData, proto.receiveSize);
      }
      else
      {
        CHECK(opcode == WL_DATA_OFFER_SET_ACTIONS);
        CHECK(va_arg(ap, unsigned int) ==
            WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
        CHECK(va_arg(ap, unsigned int) ==
            WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
        ++proto.setActionsN;
      }
      p = NULL;
      break;

    case PROXY_SOURCE:
      CHECK(opcode == WL_DATA_SOURCE_OFFER);
      CHECK(!interface);
      copyMime(p, va_arg(ap, const char *));
      p = NULL;
      break;
  }
  va_end(ap);
  return (struct wl_proxy *)p;
}

static void pollCleanup(struct Poll * poll)
{
  CHECK(poll->retired);
  poll->retired = false;
  ++rec.pollCleanupN;
  if (poll->cleanup)
    poll->cleanup(poll->opaque);
  poll->cleanup = NULL;
  poll->opaque  = NULL;
}

bool waylandPollRegisterWithCleanup(int fd, WaylandPollCallback callback,
    void * opaque, WaylandPollCleanup cleanup, uint32_t events)
{
  if (!rec.pollOK)
    return false;

  CHECK(rec.pollN < ARRAY_LENGTH(rec.poll));
  struct Poll * poll = &rec.poll[rec.pollN++];
  poll->fd       = fd;
  poll->callback = callback;
  poll->cleanup  = cleanup;
  poll->opaque   = opaque;
  poll->events   = events;
  poll->active   = true;
  return true;
}

bool waylandPollUnregister(int fd)
{
  for (unsigned int i = 0; i < rec.pollN; ++i)
  {
    struct Poll * poll = &rec.poll[i];
    if (!poll->active || poll->fd != fd)
      continue;

    poll->active  = false;
    poll->retired = true;
    ++rec.pollUnregisterN;
    if (!rec.firing)
      pollCleanup(poll);
    return true;
  }
  return false;
}

bool waylandPollUpdate(int fd, uint32_t events)
{
  for (unsigned int i = 0; i < rec.pollN; ++i)
  {
    struct Poll * poll = &rec.poll[i];
    if (!poll->active || poll->fd != fd)
      continue;

    poll->events = events;
    return true;
  }
  return false;
}

static void pollFire(unsigned int no, uint32_t events)
{
  CHECK(no < rec.pollN);
  struct Poll * poll = &rec.poll[no];
  CHECK(poll->active);
  CHECK(events & (poll->events | EPOLLERR));

  rec.firing = true;
  poll->callback(events, poll->opaque);
  rec.firing = false;
  for (unsigned int i = 0; i < rec.pollN; ++i)
    if (rec.poll[i].retired)
      pollCleanup(&rec.poll[i]);
}

void lgClipboard_notifyTypes(
    const LG_ClipboardData types[], size_t count)
{
  CHECK(rec.noticeN < ARRAY_LENGTH(rec.notice));
  CHECK(count <= ARRAY_LENGTH(rec.notice[0].type));
  struct TypeNotice * notice = &rec.notice[rec.noticeN++];
  memcpy(notice->type, types, count * sizeof(*types));
  notice->count = count;
}

void lgClipboard_release(void)
{
  if (rec.expectReleaseLocked)
  {
    CHECK(atomic_flag_test_and_set_explicit(
        &wlCb.lock, memory_order_acquire));
    ++rec.releaseLockedN;
  }
  ++rec.releaseN;
}

void lgClipboard_abort(LG_ClipboardRequest request)
{
  CHECK(rec.abortN < ARRAY_LENGTH(rec.abort));
  rec.abort[rec.abortN++] = request;
}

void lgClipboard_data(LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  CHECK(rec.dataN < ARRAY_LENGTH(rec.data));
  CHECK(size <= MAX_TRANSFER);
  struct DataNotice * notice = &rec.data[rec.dataN++];
  notice->request = request;
  notice->type    = type;
  notice->size    = size;
  memcpy(notice->data, data, size);
}

LG_ClipboardResult lgClipboard_dataBegin(LG_ClipboardRequest request,
    LG_ClipboardData type, uint64_t sizeHint)
{
  struct StreamNotice * stream = &rec.streamData;
  ++stream->beginN;
  stream->request  = request;
  stream->type     = type;
  stream->sizeHint = sizeHint;
  return rec.beginResult;
}

LG_ClipboardResult lgClipboard_dataChunk(LG_ClipboardRequest request,
    uint64_t offset, const void * data, size_t size)
{
  const LG_ClipboardResult result = rec.chunkResult;
  struct StreamNotice * stream = &rec.streamData;
  ++stream->chunkN;
  CHECK(request == stream->request);
  CHECK(offset == stream->size);
  CHECK(data);
  CHECK(size);
  CHECK(size <= sizeof(stream->data) - stream->size);
  if (rec.readyDuringChunk)
  {
    rec.readyDuringChunk = false;
    rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
    waylandCBRequestReady(request);
  }
  if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    memcpy(stream->data + stream->size, data, size);
    stream->size += size;
  }
  return result;
}

LG_ClipboardResult lgClipboard_dataEnd(LG_ClipboardRequest request,
    uint64_t finalSize)
{
  const LG_ClipboardResult result = rec.endResult;
  struct StreamNotice * stream = &rec.streamData;
  ++stream->endN;
  CHECK(request == stream->request);
  stream->finalSize = finalSize;
  if (rec.readyDuringEnd)
  {
    rec.readyDuringEnd = false;
    rec.endResult = LG_CLIPBOARD_RESULT_ACCEPTED;
    waylandCBRequestReady(request);
  }
  return result;
}

bool lgClipboard_requestStream(LG_ClipboardData type,
    const LG_ClipboardStreamOps * stream, void * opaque,
    LG_ClipboardRequest * request)
{
  ++rec.requestN;
  rec.requestType = type;
  rec.requestStream = stream;
  rec.requestOpaque = opaque;
  rec.requestId = 9000 + rec.requestN;
  if (request)
    *request = rec.requestId;
  if (rec.requestBeginSync)
  {
    rec.requestBeginResult = stream->begin(
        opaque, type, LG_CLIPBOARD_SIZE_UNKNOWN);
    const uint8_t byte = 0xa5;
    CHECK(stream->chunk(opaque, 0, &byte, 1) ==
        LG_CLIPBOARD_RESULT_ACCEPTED);
    CHECK(stream->chunk(opaque, 1, &byte, 1) ==
        LG_CLIPBOARD_RESULT_BLOCKED);
    pollFire(rec.pollN - 1, EPOLLOUT);
  }
  if (rec.requestCancelSync)
    stream->cancel(opaque, LG_CLIPBOARD_CANCEL_REPLACED);
  return rec.requestOK;
}

bool lgClipboard_requestReady(LG_ClipboardRequest request)
{
  CHECK(request == rec.requestId);
  ++rec.requestReadyN;
  return true;
}

static const struct wl_data_device_listener * deviceListener(void)
{
  CHECK(proto.device.listener);
  return (const struct wl_data_device_listener *)proto.device.listener;
}

static struct Proxy * newOffer(void)
{
  CHECK(proto.offerN < ARRAY_LENGTH(proto.offer));
  struct Proxy * offer = &proto.offer[proto.offerN++];
  offer->kind = PROXY_OFFER;
  deviceListener()->data_offer(proto.device.data,
      (struct wl_data_device *)&proto.device,
      (struct wl_data_offer *)offer);
  CHECK(offer->listener);
  CHECK(offer->user);
  return offer;
}

static struct Proxy * rawOffer(void)
{
  CHECK(proto.offerN < ARRAY_LENGTH(proto.offer));
  struct Proxy * offer = &proto.offer[proto.offerN++];
  offer->kind = PROXY_OFFER;
  return offer;
}

static void offerMime(struct Proxy * offer, const char * mime)
{
  const struct wl_data_offer_listener * listener =
    (const struct wl_data_offer_listener *)offer->listener;
  listener->offer(offer->data, (struct wl_data_offer *)offer, mime);
}

static void selectOffer(struct Proxy * offer)
{
  proto.selection = offer;
  deviceListener()->selection(proto.device.data,
      (struct wl_data_device *)&proto.device,
      (struct wl_data_offer *)offer);
}

static const struct wl_data_source_listener * sourceListener(
    struct Proxy * source)
{
  CHECK(source);
  CHECK(source->listener);
  return (const struct wl_data_source_listener *)source->listener;
}

static void start(void)
{
  memset(&wlWm, 0, sizeof(wlWm));
  memset(&wlCb, 0, sizeof(wlCb));
  memset(&proto, 0, sizeof(proto));
  memset(&rec, 0, sizeof(rec));
  proto.receiveFd = -1;
  LG_LOCK_INIT(wlCb.lock);

  rec.pollOK             = true;
  rec.requestOK          = true;
  rec.beginResult        = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.chunkResult        = LG_CLIPBOARD_RESULT_ACCEPTED;
  rec.endResult          = LG_CLIPBOARD_RESULT_ACCEPTED;
  proto.manager.kind     = PROXY_MANAGER;
  proto.device.kind      = PROXY_DEVICE;
  wlWm.dataDeviceManager =
    (struct wl_data_device_manager *)&proto.manager;
  wlWm.seat                = (struct wl_seat *)(uintptr_t)1;
  wlWm.keyboardEnterSerial = 77;

  CHECK(waylandCBInit());
  CHECK(wlCb.dataDevice == (struct wl_data_device *)&proto.device);
  CHECK(proto.device.listener);
  CHECK(strstr(wlCb.lgMimetype, "application/x-looking-glass-copy;pid=") ==
      wlCb.lgMimetype);
}

static void finish(void)
{
  if (proto.receiveFd >= 0)
  {
    CHECK(close(proto.receiveFd) == 0);
    proto.receiveFd = -1;
  }
  waylandCBFree();
  CHECK(!wlCb.dataDevice);
  CHECK(!wlCb.offer);
  CHECK(!wlCb.dndOffer);
  CHECK(!wlCb.currentRead);
  CHECK(proto.device.dead);
  CHECK(proto.deviceReleaseN == 1);
  for (unsigned int i = 0; i < rec.pollN; ++i)
    CHECK(!rec.poll[i].active);
}

static struct Proxy * textOffer(void)
{
  struct Proxy * offer = newOffer();
  offerMime(offer, "text/plain;charset=utf-8");
  selectOffer(offer);
  return offer;
}

static void checkClosed(int fd)
{
  errno = 0;
  CHECK(fcntl(fd, F_GETFD) == -1);
  CHECK(errno == EBADF);
}

static void testMime(void)
{
  start();
  struct Proxy * offer = newOffer();
  offerMime(offer, "text/html");
  offerMime(offer, "text/ico");
  offerMime(offer, "image/png");
  offerMime(offer, "application/xhtml+xml");
  offerMime(offer, "text/plain;charset=utf-8");
  offerMime(offer, "image/x-MS-bmp");
  offerMime(offer, "image/bmp");
  selectOffer(offer);

  CHECK(rec.noticeN == 1);
  CHECK(rec.notice[0].count == 3);
  CHECK(rec.notice[0].type[0] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.notice[0].type[1] == LG_CLIPBOARD_DATA_PNG);
  CHECK(rec.notice[0].type[2] == LG_CLIPBOARD_DATA_BMP);
  CHECK(strcmp(wlCb.mimetypes[LG_CLIPBOARD_DATA_TEXT],
      "text/plain;charset=utf-8") == 0);
  CHECK(strcmp(wlCb.mimetypes[LG_CLIPBOARD_DATA_PNG], "image/png") == 0);
  CHECK(strcmp(wlCb.mimetypes[LG_CLIPBOARD_DATA_BMP],
      "image/x-MS-bmp") == 0);
  CHECK(!offer->user);

  finish();
  CHECK(proto.offerDestroyN == 1);
  CHECK(proto.dirtyDestroyN == 0);
}

static void testSelfCopy(void)
{
  start();
  struct Proxy * local = textOffer();
  CHECK(rec.noticeN == 1);

  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);
  CHECK(!local->dead);

  /* A remote notice and its self echo must not erase the Wayland offer for a
   * local clipboard generation that Windows can still request. */
  const uint8_t localData[] = "local clipboard";
  proto.receiveData = localData;
  proto.receiveSize = sizeof(localData) - 1;

  /* Losing focus before the compositor echoes our source must not release
   * the remote clipboard. */
  waylandCBInvalidate();
  CHECK(rec.releaseN == 0);
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);
  CHECK(!local->dead);

  struct Proxy * echo = newOffer();
  offerMime(echo, "text/plain");
  offerMime(echo, wlCb.lgMimetype);
  selectOffer(echo);

  CHECK(rec.noticeN == 1);
  CHECK(rec.releaseN == 0);
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);
  CHECK(wlCb.mimetypes[LG_CLIPBOARD_DATA_TEXT]);
  CHECK(!local->dead);
  CHECK(echo->dead);
  CHECK(proto.offerDestroyN == 1);
  CHECK(!echo->user);

  waylandCBInvalidate();
  CHECK(rec.releaseN == 0);
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);

  waylandCBRequest(102, LG_CLIPBOARD_DATA_TEXT);
  CHECK(proto.receiveN == 1);
  CHECK(rec.pollN == 1);
  pollFire(0, EPOLLIN);
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.request == 102);
  CHECK(rec.streamData.size == sizeof(localData) - 1);
  CHECK(rec.streamData.endN == 1);
  CHECK(memcmp(rec.streamData.data, localData,
      sizeof(localData) - 1) == 0);

  int fds[2];
  CHECK(pipe(fds) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "text/plain", fds[1]);
  CHECK(rec.requestN == 1);
  CHECK(rec.requestType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.requestStream);
  CHECK(rec.requestOpaque);
  CHECK(rec.requestStream->begin(rec.requestOpaque,
      LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestStream->end(rec.requestOpaque, 0) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  uint8_t value;
  CHECK(read(fds[0], &value, 1) == 0);
  CHECK(close(fds[0]) == 0);
  checkClosed(fds[1]);

  selectOffer(NULL);
  CHECK(rec.releaseN == 1);
  CHECK(!wlCb.selectionSource);
  CHECK(local->dead);
  CHECK(proto.offerDestroyN == 2);

  CHECK(proto.dirtyDestroyN == 0);
  finish();
}

static void testLateSelfCopy(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);

  struct Proxy * local = textOffer();
  CHECK(rec.noticeN == 1);
  CHECK(!wlCb.selectionSource);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);
  CHECK(!local->dead);

  /* The compositor may deliver our marked echo after a newer external
   * selection. The stale echo must not discard that local selection. */
  struct Proxy * echo = newOffer();
  offerMime(echo, "text/plain");
  offerMime(echo, wlCb.lgMimetype);
  selectOffer(echo);

  CHECK(rec.noticeN == 1);
  CHECK(rec.releaseN == 0);
  CHECK(!wlCb.selectionSource);
  CHECK(wlCb.offer == (struct wl_data_offer *)local);
  CHECK(wlCb.mimetypes[LG_CLIPBOARD_DATA_TEXT]);
  CHECK(!local->dead);
  CHECK(echo->dead);

  const uint8_t data[] = "new local selection";
  proto.receiveData = data;
  proto.receiveSize = sizeof(data) - 1;
  waylandCBRequest(102, LG_CLIPBOARD_DATA_TEXT);
  CHECK(proto.receiveN == 1);
  CHECK(rec.pollN == 1);
  pollFire(0, EPOLLIN);
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.request == 102);
  CHECK(rec.streamData.size == sizeof(data) - 1);
  CHECK(rec.streamData.endN == 1);
  CHECK(memcmp(rec.streamData.data, data, sizeof(data) - 1) == 0);

  CHECK(proto.dirtyDestroyN == 0);
  finish();
}

static void testIgnoredOffer(void)
{
  start();
  struct Proxy * offer = newOffer();
  offerMime(offer, "application/x-unsupported");
  selectOffer(offer);

  CHECK(rec.noticeN == 0);
  CHECK(rec.releaseN == 1);
  CHECK(offer->dead);
  CHECK(proto.offerDestroyN == 1);
  CHECK(!offer->user);
  CHECK(proto.dirtyDestroyN == 0);
  finish();
}

static void testInvalidateSerialization(void)
{
  start();
  struct Proxy * offer = textOffer();
  rec.expectReleaseLocked = true;

  waylandCBInvalidate();

  CHECK(rec.releaseN == 1);
  CHECK(rec.releaseLockedN == 1);
  CHECK(offer->dead);
  CHECK(!wlCb.offer);
  finish();
}

static void testReplace(void)
{
  start();
  struct Proxy * first = textOffer();
  waylandCBRequest(101, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  const int fd = rec.poll[0].fd;

  struct Proxy * second = newOffer();
  offerMime(second, "image/png");
  selectOffer(second);
  CHECK(first->dead);
  CHECK(!second->dead);
  CHECK(rec.abortN == 1);
  CHECK(rec.abort[0] == 101);
  CHECK(rec.pollUnregisterN == 1);
  CHECK(rec.pollCleanupN == 1);
  CHECK(!rec.poll[0].active);
  checkClosed(fd);
  CHECK(rec.noticeN == 2);
  CHECK(rec.notice[1].count == 1);
  CHECK(rec.notice[1].type[0] == LG_CLIPBOARD_DATA_PNG);

  selectOffer(NULL);
  CHECK(second->dead);
  CHECK(rec.releaseN == 1);
  CHECK(proto.offerDestroyN == 2);
  finish();
  CHECK(proto.dirtyDestroyN == 0);
}

static void testReadEOF(void)
{
  start();
  textOffer();
  uint8_t data[4096];
  for (size_t i = 0; i < sizeof(data); ++i)
    data[i] = i;
  proto.receiveData = data;
  proto.receiveSize = sizeof(data);

  waylandCBRequest(201, LG_CLIPBOARD_DATA_TEXT);
  CHECK(proto.receiveN == 1);
  CHECK(strcmp(proto.receiveMime, "text/plain;charset=utf-8") == 0);
  CHECK(rec.pollN == 1);
  CHECK(rec.poll[0].events == EPOLLIN);
  const int fd = rec.poll[0].fd;

  pollFire(0, EPOLLIN);
  CHECK(rec.poll[0].active);
  CHECK(rec.streamData.beginN == 1);
  CHECK(rec.streamData.chunkN == 1);
  CHECK(rec.streamData.endN == 0);
  pollFire(0, EPOLLIN);
  CHECK(!rec.poll[0].active);
  CHECK(rec.pollCleanupN == 1);
  CHECK(rec.streamData.request == 201);
  CHECK(rec.streamData.type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.streamData.sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN);
  CHECK(rec.streamData.size == sizeof(data));
  CHECK(rec.streamData.finalSize == sizeof(data));
  CHECK(rec.streamData.endN == 1);
  CHECK(memcmp(rec.streamData.data, data, sizeof(data)) == 0);
  checkClosed(fd);

  finish();
  CHECK(rec.abortN == 0);
}

static void testReadBlocked(void)
{
  start();
  textOffer();
  const uint8_t data[] = "blocked stream";
  proto.receiveData = data;
  proto.receiveSize = sizeof(data) - 1;
  rec.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;

  waylandCBRequest(202, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.beginN == 1);
  CHECK(rec.streamData.chunkN == 1);
  CHECK(rec.streamData.size == 0);
  CHECK(rec.poll[0].events == 0);

  rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  waylandCBRequestReady(202);
  CHECK(rec.streamData.chunkN == 2);
  CHECK(rec.streamData.size == sizeof(data) - 1);
  CHECK(rec.streamData.endN == 1);
  CHECK(rec.streamData.finalSize == sizeof(data) - 1);
  CHECK(memcmp(rec.streamData.data, data, sizeof(data) - 1) == 0);
  CHECK(!rec.poll[0].active);
  CHECK(rec.abortN == 0);
  finish();
}

static void testReadBlockedEmpty(void)
{
  start();
  textOffer();
  const uint8_t data[] = "buffered while source stays open";
  proto.receiveData     = data;
  proto.receiveSize     = sizeof(data) - 1;
  proto.keepReceiveOpen = true;
  rec.chunkResult       = LG_CLIPBOARD_RESULT_BLOCKED;

  waylandCBRequest(204, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.chunkN == 1);
  CHECK(rec.streamData.size == 0);
  CHECK(rec.poll[0].events == 0);

  /* The read drained the pipe, whose duplicated write end remains open.
   * Ready must retry the buffered chunk without another poll callback. */
  rec.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  waylandCBRequestReady(204);
  CHECK(rec.streamData.chunkN == 2);
  CHECK(rec.streamData.size == sizeof(data) - 1);
  CHECK(memcmp(rec.streamData.data, data, sizeof(data) - 1) == 0);
  CHECK(rec.streamData.endN == 0);
  CHECK(rec.poll[0].active);
  CHECK(rec.poll[0].events == EPOLLIN);

  CHECK(close(proto.receiveFd) == 0);
  proto.receiveFd = -1;
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.endN == 1);
  CHECK(rec.streamData.finalSize == sizeof(data) - 1);
  CHECK(!rec.poll[0].active);
  CHECK(rec.abortN == 0);
  finish();
}

static void testReadReadyRace(void)
{
  start();
  textOffer();
  const uint8_t data[] = "ready while returning blocked";
  proto.receiveData = data;
  proto.receiveSize = sizeof(data) - 1;
  rec.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;
  rec.readyDuringChunk = true;

  waylandCBRequest(203, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.beginN == 1);
  CHECK(rec.streamData.chunkN == 2);
  CHECK(rec.streamData.size == sizeof(data) - 1);
  CHECK(rec.poll[0].events == EPOLLIN);

  rec.endResult = LG_CLIPBOARD_RESULT_BLOCKED;
  rec.readyDuringEnd = true;
  pollFire(0, EPOLLIN);
  CHECK(rec.streamData.endN == 2);
  CHECK(rec.streamData.finalSize == sizeof(data) - 1);
  CHECK(!rec.poll[0].active);
  CHECK(rec.abortN == 0);
  finish();
}

static void testReadError(void)
{
  start();
  textOffer();
  waylandCBRequest(301, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  const int firstFd = rec.poll[0].fd;
  CHECK(close(firstFd) == 0);
  pollFire(0, EPOLLIN);
  CHECK(rec.abortN == 1);
  CHECK(rec.abort[0] == 301);
  CHECK(rec.pollCleanupN == 1);
  checkClosed(firstFd);

  waylandCBRequest(302, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 2);
  const int secondFd = rec.poll[1].fd;
  pollFire(1, EPOLLERR);
  CHECK(rec.abortN == 2);
  CHECK(rec.abort[1] == 302);
  CHECK(rec.pollCleanupN == 2);
  checkClosed(secondFd);

  finish();
}

static void testSource(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  CHECK(proto.sourceN == 1);
  struct Proxy * source = &proto.source[0];
  CHECK(proto.selection == source);
  CHECK(proto.serial == 77);
  CHECK(source->mimeN == 6);
  CHECK(strcmp(source->mime[0], "text/plain") == 0);
  CHECK(strcmp(source->mime[1], "text/plain;charset=utf-8") == 0);
  CHECK(strcmp(source->mime[4], "UTF8_STRING") == 0);
  CHECK(strcmp(source->mime[5], wlCb.lgMimetype) == 0);

  int bad[2];
  CHECK(pipe(bad) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "image/png", bad[1]);
  CHECK(rec.pollN == 0);
  uint8_t value;
  CHECK(read(bad[0], &value, 1) == 0);
  CHECK(close(bad[0]) == 0);

  int good[2];
  CHECK(pipe(good) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "text/plain", good[1]);
  CHECK(rec.pollN == 1);
  CHECK(rec.poll[0].fd == good[1]);
  CHECK(rec.poll[0].events == 0);
  CHECK(rec.requestN == 1);
  CHECK(rec.requestType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.requestStream);
  CHECK(rec.requestOpaque);

  const uint8_t first[] = "source ";
  const uint8_t second[] = "data";
  CHECK(rec.requestStream->begin(rec.requestOpaque,
      LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestStream->chunk(rec.requestOpaque, 0,
      first, sizeof(first) - 1) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.poll[0].events == EPOLLOUT);
  CHECK(rec.requestStream->chunk(rec.requestOpaque, sizeof(first) - 1,
      second, sizeof(second) - 1) == LG_CLIPBOARD_RESULT_BLOCKED);
  pollFire(0, EPOLLOUT);
  CHECK(rec.requestReadyN == 1);
  CHECK(rec.poll[0].events == 0);

  CHECK(rec.requestStream->chunk(rec.requestOpaque, sizeof(first) - 1,
      second, sizeof(second) - 1) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestStream->end(rec.requestOpaque,
      sizeof(first) + sizeof(second) - 2) == LG_CLIPBOARD_RESULT_BLOCKED);
  pollFire(0, EPOLLOUT);
  CHECK(rec.requestReadyN == 2);
  CHECK(rec.requestStream->end(rec.requestOpaque,
      sizeof(first) + sizeof(second) - 2) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(!rec.poll[0].active);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  CHECK(source->dead);
  CHECK(proto.sourceDestroyN == 1);
  CHECK(rec.pollCleanupN == 1);
  uint8_t actual[sizeof(first) + sizeof(second)] = {};
  const size_t expected = sizeof(first) + sizeof(second) - 2;
  CHECK(read(good[0], actual, sizeof(actual)) == (ssize_t)expected);
  CHECK(memcmp(actual, "source data", expected) == 0);
  CHECK(read(good[0], &value, 1) == 0);
  CHECK(close(good[0]) == 0);
  checkClosed(good[1]);

  finish();
}

static void testSourceRelease(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  CHECK(wlCb.selectionSource == (struct wl_data_source *)source);
  CHECK(wlCb.sources);
  CHECK(proto.selection == source);
  CHECK(proto.selectionSetN == 1);

  waylandCBRelease();
  CHECK(!wlCb.selectionSource);
  CHECK(wlCb.sources);
  CHECK(!proto.selection);
  CHECK(proto.selectionSetN == 2);

  waylandCBRelease();
  CHECK(proto.selectionSetN == 2);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  CHECK(source->dead);
  CHECK(!wlCb.sources);
  finish();
}

static void testSourceExternalOwner(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  CHECK(proto.selectionSetN == 1);

  struct Proxy * external = newOffer();
  offerMime(external, "image/png");
  selectOffer(external);
  CHECK(!wlCb.selectionSource);
  CHECK(proto.selection == external);

  waylandCBRelease();
  CHECK(proto.selectionSetN == 1);
  CHECK(proto.selection == external);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  CHECK(source->dead);
  finish();
}

static void testSourceReplacement(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * first = &proto.source[0];
  waylandCBNotice(LG_CLIPBOARD_DATA_PNG);
  struct Proxy * second = &proto.source[1];
  CHECK(proto.selection == second);
  CHECK(proto.selectionSetN == 2);
  CHECK(wlCb.selectionSource == (struct wl_data_source *)second);

  sourceListener(first)->cancelled(first->data,
      (struct wl_data_source *)first);
  CHECK(first->dead);
  CHECK(wlCb.selectionSource == (struct wl_data_source *)second);

  waylandCBRelease();
  CHECK(!proto.selection);
  CHECK(proto.selectionSetN == 3);
  CHECK(!wlCb.selectionSource);

  sourceListener(second)->cancelled(second->data,
      (struct wl_data_source *)second);
  CHECK(second->dead);
  CHECK(!wlCb.sources);
  finish();
}

static void testSourceCancelDuringRequest(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  rec.requestCancelSync = true;

  int fds[2];
  CHECK(pipe(fds) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "text/plain", fds[1]);
  CHECK(rec.requestN == 1);
  CHECK(rec.pollN == 1);
  CHECK(!rec.poll[0].active);
  CHECK(rec.pollCleanupN == 1);
  checkClosed(fds[1]);
  uint8_t value;
  CHECK(read(fds[0], &value, 1) == 0);
  CHECK(close(fds[0]) == 0);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  finish();
}

static void testSourceBeginDuringRequest(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  rec.requestBeginSync = true;

  int fds[2];
  CHECK(pipe(fds) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "text/plain", fds[1]);
  CHECK(rec.requestN == 1);
  CHECK(rec.requestBeginResult == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestOpaque);
  CHECK(rec.requestReadyN == 1);

  CHECK(rec.requestStream->end(rec.requestOpaque, 1) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.pollCleanupN == 1);
  uint8_t value = 0;
  CHECK(read(fds[0], &value, 1) == 1);
  CHECK(value == 0xa5);
  CHECK(read(fds[0], &value, 1) == 0);
  CHECK(close(fds[0]) == 0);
  checkClosed(fds[1]);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  finish();
}

static void testSourceError(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];

  int fds[2];
  CHECK(pipe(fds) == 0);
  sourceListener(source)->send(source->data,
      (struct wl_data_source *)source, "text/plain", fds[1]);
  CHECK(rec.requestStream->begin(rec.requestOpaque,
      LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);

  const uint8_t first[] = "first";
  const uint8_t second[] = "second";
  CHECK(rec.requestStream->chunk(rec.requestOpaque, 0,
      first, sizeof(first) - 1) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestStream->chunk(rec.requestOpaque, sizeof(first) - 1,
      second, sizeof(second) - 1) == LG_CLIPBOARD_RESULT_BLOCKED);
  CHECK(close(fds[0]) == 0);
  pollFire(0, EPOLLERR);
  CHECK(rec.requestReadyN == 1);
  CHECK(!rec.poll[0].active);
  CHECK(rec.pollCleanupN == 1);

  CHECK(rec.requestStream->chunk(rec.requestOpaque, sizeof(first) - 1,
      second, sizeof(second) - 1) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(rec.requestStream->end(rec.requestOpaque,
      sizeof(first) + sizeof(second) - 2) == LG_CLIPBOARD_RESULT_ACCEPTED);
  checkClosed(fds[1]);

  sourceListener(source)->cancelled(source->data,
      (struct wl_data_source *)source);
  finish();
}

static void testTeardown(void)
{
  start();
  struct Proxy * offer = textOffer();
  waylandCBRequest(401, LG_CLIPBOARD_DATA_TEXT);
  CHECK(rec.pollN == 1);
  const int fd = rec.poll[0].fd;
  struct Proxy * dnd = rawOffer();
  wlCb.dndOffer = (struct wl_data_offer *)dnd;
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];

  finish();
  CHECK(offer->dead);
  CHECK(dnd->dead);
  CHECK(source->dead);
  CHECK(proto.offerDestroyN == 2);
  CHECK(proto.sourceDestroyN == 1);
  CHECK(proto.dirtyDestroyN == 0);
  CHECK(proto.selectionSetN == 2);
  CHECK(!proto.selection);
  CHECK(rec.pollUnregisterN == 1);
  CHECK(rec.pollCleanupN == 1);
  checkClosed(fd);
}

static void testTeardownExternalOwner(void)
{
  start();
  waylandCBNotice(LG_CLIPBOARD_DATA_TEXT);
  struct Proxy * source = &proto.source[0];
  struct Proxy * external = newOffer();
  offerMime(external, "text/plain");
  selectOffer(external);
  CHECK(!wlCb.selectionSource);

  finish();
  CHECK(source->dead);
  CHECK(external->dead);
  CHECK(proto.selectionSetN == 1);
  CHECK(proto.selection == external);
}

static void testDnd(void)
{
  start();
  struct Proxy * leave = rawOffer();
  wlCb.dndOffer = (struct wl_data_offer *)leave;
  deviceListener()->leave(proto.device.data,
      (struct wl_data_device *)&proto.device);
  CHECK(!wlCb.dndOffer);
  CHECK(leave->dead);
  CHECK(proto.offerDestroyN == 1);

  struct Proxy * drop = rawOffer();
  wlCb.dndOffer = (struct wl_data_offer *)drop;
  deviceListener()->drop(proto.device.data,
      (struct wl_data_device *)&proto.device);
  CHECK(!wlCb.dndOffer);
  CHECK(drop->dead);
  CHECK(proto.offerDestroyN == 2);

  struct Proxy * enter = newOffer();
  offerMime(enter, "text/plain");
  deviceListener()->enter(proto.device.data,
      (struct wl_data_device *)&proto.device, 1, wlWm.surface, 0, 0,
      (struct wl_data_offer *)enter);
  CHECK(wlCb.dndOffer == (struct wl_data_offer *)enter);
  CHECK(!enter->user);
  CHECK(proto.setActionsN == 1);

  deviceListener()->leave(proto.device.data,
      (struct wl_data_device *)&proto.device);
  CHECK(!wlCb.dndOffer);
  CHECK(enter->dead);
  CHECK(proto.offerDestroyN == 3);
  finish();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "mime"      , testMime      },
  { "replace"   , testReplace   },
  { "read-eof"  , testReadEOF   },
  { "read-block", testReadBlocked },
  { "read-empty", testReadBlockedEmpty },
  { "read-ready", testReadReadyRace },
  { "read-error", testReadError },
  { "source"    , testSource    },
  { "source-release", testSourceRelease },
  { "source-external", testSourceExternalOwner },
  { "source-replace", testSourceReplacement },
  { "source-cancel", testSourceCancelDuringRequest },
  { "source-early", testSourceBeginDuringRequest },
  { "source-error", testSourceError },
  { "teardown"  , testTeardown  },
  { "teardown-external", testTeardownExternalOwner },
  { "self-copy" , testSelfCopy  },
  { "self-copy-late", testLateSelfCopy },
  { "ignored"   , testIgnoredOffer },
  { "invalidate-lock", testInvalidateSerialization },
  { "dnd"       , testDnd       },
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
