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

#include "clipboard.h"
#include "main.h"
#include "test.h"

#include "common/debug.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CALL       16U
#define MAX_DATA       32U
#define TEST_ALARM_S    5U

struct Provider
{
  bool                         avail;
  bool                         attachOK;
  bool                         callOK;
  bool                         reqOK;
  uint32_t                     gen;
  LG_ClipboardStatusFn         statFn;
  void                       * statCtx;
  const LG_ClipboardEventOps * ev;
  void                       * evCtx;
  const LG_ClipboardEventOps * oldEv;
  void                       * oldCtx;
  unsigned int                 statSet;
  unsigned int                 statClear;
  unsigned int                 attach;
  unsigned int                 detach;
  unsigned int                 release;
  unsigned int                 notice;
  unsigned int                 data;
  unsigned int                 begin;
  unsigned int                 chunk;
  unsigned int                 end;
  unsigned int                 cancel;
  unsigned int                 ready;
  unsigned int                 request;
  LG_ClipboardData             noticeTypes[LG_CLIPBOARD_DATA_NONE];
  size_t                       noticeCount;
  LG_ClipboardRequest          reqId[MAX_CALL];
  LG_ClipboardData             reqType[MAX_CALL];
  LG_ClipboardRequest          dataId[MAX_CALL];
  LG_ClipboardData             dataType[MAX_CALL];
  size_t                       dataSize[MAX_CALL];
  uint8_t                      dataBuf[MAX_CALL][MAX_DATA];
  LG_ClipboardResult           beginResult;
  LG_ClipboardResult           chunkResult;
  LG_ClipboardResult           endResult;
  LG_ClipboardRequest          streamId;
  LG_ClipboardData             streamType;
  uint64_t                     sizeHint;
  uint64_t                     chunkOffset[MAX_CALL];
  size_t                       chunkSize[MAX_CALL];
  uint8_t                      chunkBuf[MAX_CALL][MAX_DATA];
  uint64_t                     finalSize;
  LG_ClipboardCancelReason     cancelReason;
  bool                         earlyBegin;
  LG_ClipboardResult           earlyResult;
};

struct Display
{
  unsigned int        notice;
  unsigned int        release;
  unsigned int        request;
  unsigned int        ready;
  unsigned int        cancel;
  LG_ClipboardData    noticeType[MAX_CALL];
  LG_ClipboardRequest reqId[MAX_CALL];
  LG_ClipboardData    reqType[MAX_CALL];
  bool                autoData;
  LG_ClipboardData    autoType;
  const void        * autoBuf;
  size_t              autoSize;
  bool                autoBegin;
  LG_ClipboardResult  readyResult;
  LG_ClipboardCancelReason cancelReason;
};

struct Reply
{
  unsigned int     count;
  LG_ClipboardData type;
  uint32_t         size;
  uint8_t          data[MAX_DATA];
  struct Reply   * next;
  bool             nested;
};

struct StreamSink
{
  unsigned int             begin;
  unsigned int             chunk;
  unsigned int             end;
  unsigned int             cancel;
  LG_ClipboardResult       beginResult;
  LG_ClipboardResult       chunkResult;
  LG_ClipboardResult       endResult;
  LG_ClipboardData         type;
  uint64_t                 sizeHint;
  uint64_t                 offset;
  uint64_t                 finalSize;
  LG_ClipboardCancelReason reason;
  LG_ClipboardRequest          request;
  bool                         requireRequest;
  size_t                   size;
  uint8_t                  data[MAX_DATA];
};

static struct Provider p;
static struct Provider q;
static struct Provider r;
static struct Display  d;

struct AppState  g_state;
struct AppParams g_params;

static LG_ClipboardResult sinkBegin(void * opaque,
    LG_ClipboardData type, uint64_t sizeHint)
{
  struct StreamSink * sink = opaque;
  if (sink->requireRequest)
    CHECK(sink->request != LG_CLIPBOARD_REQUEST_INVALID);
  ++sink->begin;
  sink->type     = type;
  sink->sizeHint = sizeHint;
  return sink->beginResult;
}

static LG_ClipboardResult sinkChunk(void * opaque, uint64_t offset,
    const void * data, size_t size)
{
  struct StreamSink * sink = opaque;
  ++sink->chunk;
  sink->offset = offset;
  if (sink->chunkResult == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    CHECK(offset + size <= MAX_DATA);
    memcpy(sink->data + offset, data, size);
    sink->size = offset + size;
  }
  return sink->chunkResult;
}

static LG_ClipboardResult sinkEnd(void * opaque, uint64_t finalSize)
{
  struct StreamSink * sink = opaque;
  ++sink->end;
  sink->finalSize = finalSize;
  return sink->endResult;
}

static void sinkCancel(void * opaque, LG_ClipboardCancelReason reason)
{
  struct StreamSink * sink = opaque;
  ++sink->cancel;
  sink->reason = reason;
}

static const LG_ClipboardStreamOps sinkOps =
{
  .begin  = sinkBegin,
  .chunk  = sinkChunk,
  .end    = sinkEnd,
  .cancel = sinkCancel,
};

static void initSink(struct StreamSink * sink)
{
  sink->beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  sink->chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  sink->endResult   = LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void setStat(void * opaque, LG_ClipboardStatusFn callback,
    void * callbackOpaque)
{
  struct Provider * provider = opaque;
  provider->statFn  = callback;
  provider->statCtx = callbackOpaque;
  if (!callback)
  {
    ++provider->statClear;
    return;
  }

  ++provider->statSet;
  const LG_ClipboardStatus status =
  {
    .available  = provider->avail,
    .generation = provider->gen,
  };
  callback(callbackOpaque, &status);
}

static bool attach(void * opaque, const LG_ClipboardEventOps * events,
    void * eventOpaque)
{
  struct Provider * provider = opaque;
  ++provider->attach;
  provider->ev    = events;
  provider->evCtx = eventOpaque;
  return provider->attachOK;
}

static void detach(void * opaque)
{
  struct Provider * provider = opaque;
  ++provider->detach;
  provider->oldEv  = provider->ev;
  provider->oldCtx = provider->evCtx;
  provider->ev     = NULL;
  provider->evCtx  = NULL;
}

static bool release(void * opaque)
{
  struct Provider * provider = opaque;
  ++provider->release;
  return provider->callOK;
}

static bool notify(void * opaque, const LG_ClipboardData types[],
    size_t count)
{
  struct Provider * provider = opaque;
  CHECK(types);
  CHECK(count <= LG_CLIPBOARD_DATA_NONE);
  ++provider->notice;
  provider->noticeCount = count;
  memcpy(provider->noticeTypes, types, count * sizeof(*types));
  return provider->callOK;
}

static bool data(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type, const void * buf, size_t size)
{
  struct Provider * provider = opaque;
  CHECK(provider->data < MAX_CALL);
  const unsigned int no = provider->data++;
  provider->dataId[no]   = id;
  provider->dataType[no] = type;
  provider->dataSize[no] = size;
  if (size)
  {
    CHECK(buf);
    CHECK(size <= MAX_DATA);
    memcpy(provider->dataBuf[no], buf, size);
  }
  return provider->callOK;
}

static LG_ClipboardResult dataBegin(void * opaque,
    LG_ClipboardRequest id, LG_ClipboardData type, uint64_t sizeHint)
{
  struct Provider * provider = opaque;
  ++provider->begin;
  provider->streamId   = id;
  provider->streamType = type;
  provider->sizeHint   = sizeHint;
  return provider->beginResult;
}

static LG_ClipboardResult dataChunk(void * opaque,
    LG_ClipboardRequest id, uint64_t offset, const void * buf, size_t size)
{
  struct Provider * provider = opaque;
  CHECK(provider->chunk < MAX_CALL);
  CHECK(id == provider->streamId);
  const unsigned int no = provider->chunk++;
  provider->chunkOffset[no] = offset;
  provider->chunkSize[no]   = size;
  if (size <= MAX_DATA)
    memcpy(provider->chunkBuf[no], buf, size);
  return provider->chunkResult;
}

static LG_ClipboardResult dataEnd(void * opaque,
    LG_ClipboardRequest id, uint64_t finalSize)
{
  struct Provider * provider = opaque;
  CHECK(id == provider->streamId);
  ++provider->end;
  provider->finalSize = finalSize;
  return provider->endResult;
}

static bool dataCancel(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  struct Provider * provider = opaque;
  ++provider->cancel;
  provider->streamId    = id;
  provider->cancelReason = reason;
  return provider->callOK;
}

static bool dataReady(void * opaque, LG_ClipboardRequest id)
{
  struct Provider * provider = opaque;
  ++provider->ready;
  provider->streamId = id;
  return provider->callOK;
}

struct EarlyRequest
{
  struct Provider     * provider;
  LG_ClipboardRequest   request;
  LG_ClipboardData      type;
};

static void * earlyRequestThread(void * opaque)
{
  struct EarlyRequest * early = opaque;
  early->provider->earlyResult = early->provider->ev->dataBegin(
      early->provider->evCtx, early->request, early->type,
      LG_CLIPBOARD_SIZE_UNKNOWN);
  return NULL;
}

static bool request(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type)
{
  struct Provider * provider = opaque;
  CHECK(provider->request < MAX_CALL);
  const unsigned int no = provider->request++;
  provider->reqId[no]    = id;
  provider->reqType[no]  = type;
  if (provider->earlyBegin)
  {
    struct EarlyRequest early =
    {
      .provider = provider,
      .request  = id,
      .type     = type,
    };
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, earlyRequestThread, &early) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
  }
  return provider->reqOK;
}

static const LG_ClipboardOps plainOps =
{
  .name        = "plain",
  .attach      = attach,
  .detach      = detach,
  .release     = release,
  .notifyTypes = notify,
  .data        = data,
  .request     = request,
};

static const LG_ClipboardOps statOps =
{
  .name              = "status",
  .setStatusListener = setStat,
  .attach            = attach,
  .detach            = detach,
  .release           = release,
  .notifyTypes       = notify,
  .data              = data,
  .request           = request,
};

static const LG_ClipboardOps streamOps =
{
  .name        = "stream",
  .attach      = attach,
  .detach      = detach,
  .release     = release,
  .notifyTypes = notify,
  .dataBegin   = dataBegin,
  .dataChunk   = dataChunk,
  .dataEnd     = dataEnd,
  .dataCancel  = dataCancel,
  .dataReady   = dataReady,
  .request     = request,
};

static void dsNotice(LG_ClipboardData type)
{
  CHECK(d.notice < MAX_CALL);
  d.noticeType[d.notice++] = type;
}

static void dsRelease(void)
{
  ++d.release;
}

static void dsRequest(LG_ClipboardRequest id, LG_ClipboardData type)
{
  CHECK(d.request < MAX_CALL);
  const unsigned int no = d.request++;
  d.reqId[no]            = id;
  d.reqType[no]          = type;
  if (d.autoData)
    lgClipboard_data(id, d.autoType, d.autoBuf, d.autoSize);
}

static void dsRequestReady(LG_ClipboardRequest id)
{
  ++d.ready;
  if (d.autoBegin)
    d.readyResult = lgClipboard_dataBegin(
        id, LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_SIZE_UNKNOWN);
}

static void dsRequestCancel(LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  (void)id;
  ++d.cancel;
  d.cancelReason = reason;
}

static struct LG_DisplayServerOps dsOps =
{
  .cbNotice  = dsNotice,
  .cbRelease = dsRelease,
  .cbRequest = dsRequest,
  .cbRequestReady  = dsRequestReady,
  .cbRequestCancel = dsRequestCancel,
};

static void initProvider(struct Provider * provider)
{
  provider->avail    = true;
  provider->attachOK = true;
  provider->callOK   = true;
  provider->reqOK    = true;
  provider->gen      = 1;
  provider->beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  provider->chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  provider->endResult   = LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void init(void)
{
  initProvider(&p);
  initProvider(&q);
  initProvider(&r);
  g_state.ds                  = &dsOps;
  g_params.clipboardToVM      = true;
  g_params.clipboardToLocal   = true;
  lgClipboard_init();
  lgClipboard_setLocalAvailable(true);
}

static void bind(struct Provider * provider)
{
  lgClipboard_setFallback(&plainOps, provider);
  CHECK(provider->attach == 1);
}

static void setStatus(struct Provider * provider, bool avail, uint32_t gen)
{
  CHECK(provider->statFn);
  provider->avail = avail;
  provider->gen   = gen;
  const LG_ClipboardStatus status =
  {
    .available  = avail,
    .generation = gen,
  };
  provider->statFn(provider->statCtx, &status);
}

static void notice(struct Provider * provider,
    const LG_ClipboardData types[], size_t count)
{
  CHECK(provider->ev);
  provider->ev->notice(provider->evCtx, types, count);
}

static void remoteData(struct Provider * provider, LG_ClipboardRequest id,
    LG_ClipboardData type, const void * buf, size_t size)
{
  CHECK(provider->ev);
  provider->ev->data(provider->evCtx, id, type, buf, size);
}

static void onReply(void * opaque, LG_ClipboardData type,
    const uint8_t * buf, uint32_t size)
{
  struct Reply * reply = opaque;
  ++reply->count;
  reply->type = type;
  reply->size = size;
  CHECK(size <= MAX_DATA);
  if (size)
  {
    CHECK(buf);
    memcpy(reply->data, buf, size);
  }
  else
    CHECK(!buf);

  if (type != LG_CLIPBOARD_DATA_NONE && reply->next)
    reply->nested = lgClipboard_request(
        LG_CLIPBOARD_DATA_TEXT, onReply, reply->next);
}

static void checkNone(const struct Reply * reply)
{
  CHECK(reply->count == 1);
  CHECK(reply->type == LG_CLIPBOARD_DATA_NONE);
  CHECK(reply->size == 0);
}

static void testPreference(void)
{
  init();
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);

  lgClipboard_setFallback(&statOps, &p);
  CHECK(p.attach == 1);
  CHECK(p.notice == 1);

  lgClipboard_setTransport(&statOps, &q);
  CHECK(p.release == 1);
  CHECK(p.detach == 1);
  CHECK(q.attach == 1);
  CHECK(q.notice == 1);

  CHECK(p.oldEv);
  p.oldEv->notice(p.oldCtx, types, 1);
  p.oldEv->release(p.oldCtx);
  CHECK(!p.oldEv->request(p.oldCtx, 1, LG_CLIPBOARD_DATA_TEXT));
  CHECK(d.notice == 0);
  CHECK(d.release == 0);

  const LG_ClipboardStatusFn staleFn = q.statFn;
  void * staleCtx = q.statCtx;
  CHECK(staleFn);
  lgClipboard_dropTransport();
  CHECK(q.release == 0);
  CHECK(q.detach == 0);
  CHECK(p.attach == 2);

  const LG_ClipboardStatus stale =
  {
    .available  = false,
    .generation = 2,
  };
  staleFn(staleCtx, &stale);
  CHECK(p.attach == 2);

  lgClipboard_setTransport(&statOps, &r);
  CHECK(p.release == 2);
  CHECK(p.detach == 2);
  CHECK(r.attach == 1);
  lgClipboard_setTransport(NULL, NULL);
  CHECK(r.statClear == 1);
  CHECK(r.release == 1);
  CHECK(r.detach == 1);
  CHECK(p.attach == 3);

  lgClipboard_free();
}

static void testRequest(void)
{
  init();
  bind(&p);
  const LG_ClipboardData types[] =
    { LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_DATA_PNG };
  notice(&p, types, 2);
  CHECK(d.notice == 1);
  CHECK(d.noticeType[0] == LG_CLIPBOARD_DATA_TEXT);

  struct Reply a = { 0 };
  struct Reply b = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &a));
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &b));
  CHECK(p.request == 2);
  CHECK(p.reqId[0] != LG_CLIPBOARD_REQUEST_INVALID);
  CHECK(p.reqId[0] != p.reqId[1]);

  const uint8_t one[] = { 1, 2, 3 };
  const uint8_t two[] = { 4, 5 };
  remoteData(&p, p.reqId[1] + 100, LG_CLIPBOARD_DATA_TEXT,
      one, sizeof(one));
  CHECK(a.count == 0);
  CHECK(b.count == 0);
  remoteData(&p, p.reqId[1], LG_CLIPBOARD_DATA_TEXT, two, sizeof(two));
  remoteData(&p, p.reqId[0], LG_CLIPBOARD_DATA_TEXT, one, sizeof(one));
  CHECK(a.count == 1);
  CHECK(a.type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(a.size == sizeof(one));
  CHECK(memcmp(a.data, one, sizeof(one)) == 0);
  CHECK(b.count == 1);
  CHECK(b.type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(b.size == sizeof(two));
  CHECK(memcmp(b.data, two, sizeof(two)) == 0);

  lgClipboard_free();
}

static void testInvalid(void)
{
  init();
  bind(&p);
  const LG_ClipboardData bad[] = { LG_CLIPBOARD_DATA_NONE };
  p.ev->notice(p.evCtx, NULL, 1);
  p.ev->notice(p.evCtx, bad, 1);
  p.ev->notice(p.evCtx, bad, 0);
  CHECK(d.notice == 0);

  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  notice(&p, types, 1);
  CHECK(!lgClipboard_request(
      LG_CLIPBOARD_DATA_NONE, onReply, &(struct Reply) { 0 }));
  CHECK(!lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, NULL, NULL));

  struct Reply empty = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &empty));
  remoteData(&p, LG_CLIPBOARD_REQUEST_INVALID,
      LG_CLIPBOARD_DATA_TEXT, NULL, 0);
  CHECK(empty.count == 0);
  remoteData(&p, p.reqId[p.request - 1],
      LG_CLIPBOARD_DATA_TEXT, NULL, 0);
  CHECK(empty.count == 1);
  CHECK(empty.type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(empty.size == 0);

  struct Reply wrong = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &wrong));
  remoteData(&p, p.reqId[p.request - 1],
      LG_CLIPBOARD_DATA_PNG, NULL, 0);
  checkNone(&wrong);

  struct Reply null = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &null));
  remoteData(&p, p.reqId[p.request - 1],
      LG_CLIPBOARD_DATA_TEXT, NULL, 1);
  checkNone(&null);

#if SIZE_MAX > UINT32_MAX
  struct Reply large = { 0 };
  const uint8_t byte = 1;
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &large));
  remoteData(&p, p.reqId[p.request - 1], LG_CLIPBOARD_DATA_TEXT,
      &byte, (size_t)UINT32_MAX + 1);
  checkNone(&large);
#endif

  p.reqOK = false;
  struct Reply failed = { 0 };
  CHECK(!lgClipboard_request(
      LG_CLIPBOARD_DATA_TEXT, onReply, &failed));
  CHECK(failed.count == 0);

  lgClipboard_free();
}

static void testCancel(void)
{
  init();
  bind(&p);
  const LG_ClipboardData text[] = { LG_CLIPBOARD_DATA_TEXT };
  const LG_ClipboardData png[]  = { LG_CLIPBOARD_DATA_PNG };

  notice(&p, text, 1);
  struct Reply newer = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &newer));
  const LG_ClipboardRequest newerId = p.reqId[p.request - 1];
  notice(&p, png, 1);
  checkNone(&newer);
  remoteData(&p, newerId, LG_CLIPBOARD_DATA_TEXT, "x", 1);
  CHECK(newer.count == 1);

  struct Reply released = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_PNG, onReply, &released));
  const LG_ClipboardRequest releaseId = p.reqId[p.request - 1];
  p.ev->release(p.evCtx);
  checkNone(&released);
  remoteData(&p, releaseId, LG_CLIPBOARD_DATA_PNG, "x", 1);
  CHECK(released.count == 1);

  notice(&p, text, 1);
  struct Reply unavailable = { 0 };
  CHECK(lgClipboard_request(
      LG_CLIPBOARD_DATA_TEXT, onReply, &unavailable));
  const LG_ClipboardRequest unavailableId = p.reqId[p.request - 1];
  lgClipboard_setLocalAvailable(false);
  checkNone(&unavailable);
  remoteData(&p, unavailableId, LG_CLIPBOARD_DATA_TEXT, "x", 1);
  CHECK(unavailable.count == 1);

  lgClipboard_setLocalAvailable(true);
  notice(&p, text, 1);
  struct Reply switched = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &switched));
  const LG_ClipboardRequest switchedId = p.reqId[p.request - 1];
  lgClipboard_setTransport(&plainOps, &q);
  checkNone(&switched);
  CHECK(p.oldEv);
  p.oldEv->data(p.oldCtx, switchedId, LG_CLIPBOARD_DATA_TEXT, "x", 1);
  CHECK(switched.count == 1);
  CHECK(d.release == 3);

  lgClipboard_free();
}

static void testGeneration(void)
{
  init();
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);
  lgClipboard_setFallback(&statOps, &p);
  notice(&p, types, 1);

  struct Reply old = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &old));
  const LG_ClipboardRequest oldId = p.reqId[p.request - 1];
  setStatus(&p, true, 2);
  checkNone(&old);
  CHECK(p.attach == 2);
  CHECK(p.detach == 1);
  CHECK(d.release == 1);

  remoteData(&p, oldId, LG_CLIPBOARD_DATA_TEXT, "old", 3);
  CHECK(old.count == 1);
  notice(&p, types, 1);
  struct Reply next = { 0 };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &next));
  remoteData(&p, p.reqId[p.request - 1],
      LG_CLIPBOARD_DATA_TEXT, "new", 3);
  CHECK(next.count == 1);
  CHECK(next.type == LG_CLIPBOARD_DATA_TEXT);
  CHECK(next.size == 3);
  CHECK(memcmp(next.data, "new", 3) == 0);

  lgClipboard_free();
}

static void testLocal(void)
{
  init();
  bind(&p);
  const LG_ClipboardData bad[] = { LG_CLIPBOARD_DATA_NONE };
  const LG_ClipboardData many[LG_CLIPBOARD_DATA_NONE + 1] = { 0 };
  const LG_ClipboardData types[] =
    { LG_CLIPBOARD_DATA_TEXT, LG_CLIPBOARD_DATA_PNG };
  lgClipboard_notifyTypes(NULL, 1);
  lgClipboard_notifyTypes(bad, 1);
  lgClipboard_notifyTypes(many, LG_CLIPBOARD_DATA_NONE + 1);
  CHECK(p.notice == 0);
  lgClipboard_notifyTypes(types, 2);
  CHECK(p.notice == 1);

  CHECK(!p.ev->request(
      p.evCtx, LG_CLIPBOARD_REQUEST_INVALID, LG_CLIPBOARD_DATA_TEXT));
  CHECK(!p.ev->request(p.evCtx, 1, LG_CLIPBOARD_DATA_NONE));
  CHECK(p.ev->request(p.evCtx, 40, LG_CLIPBOARD_DATA_TEXT));
  CHECK(d.request == 1);
  CHECK(!p.ev->request(p.evCtx, 41, LG_CLIPBOARD_DATA_PNG));

  const LG_ClipboardRequest first = d.reqId[0];
  lgClipboard_data(first + 100, LG_CLIPBOARD_DATA_TEXT, "x", 1);
  CHECK(p.data == 0);
  lgClipboard_data(first, LG_CLIPBOARD_DATA_PNG, "x", 1);
  CHECK(p.data == 1);
  CHECK(p.dataId[0] == 40);
  CHECK(p.dataType[0] == LG_CLIPBOARD_DATA_NONE);
  CHECK(p.dataSize[0] == 0);

  CHECK(p.ev->request(p.evCtx, 41, LG_CLIPBOARD_DATA_PNG));
  const LG_ClipboardRequest second = d.reqId[1];
  lgClipboard_abort(second + 100);
  CHECK(p.data == 1);
  lgClipboard_abort(second);
  CHECK(p.data == 2);
  CHECK(p.dataId[1] == 41);
  CHECK(p.dataType[1] == LG_CLIPBOARD_DATA_NONE);

  CHECK(p.ev->request(p.evCtx, 42, LG_CLIPBOARD_DATA_TEXT));
  const LG_ClipboardRequest third = d.reqId[2];
  const uint8_t buf[] = { 5, 6, 7 };
  lgClipboard_data(third, LG_CLIPBOARD_DATA_TEXT, buf, sizeof(buf));
  CHECK(p.data == 3);
  CHECK(p.dataId[2] == 42);
  CHECK(p.dataType[2] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(p.dataSize[2] == sizeof(buf));
  CHECK(memcmp(p.dataBuf[2], buf, sizeof(buf)) == 0);
  lgClipboard_data(third, LG_CLIPBOARD_DATA_TEXT, buf, sizeof(buf));
  CHECK(p.data == 3);

  lgClipboard_free();
}

static void testReentrant(void)
{
  init();
  bind(&p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  notice(&p, types, 1);

  struct Reply second = { 0 };
  struct Reply first  = { .next = &second };
  CHECK(lgClipboard_request(LG_CLIPBOARD_DATA_TEXT, onReply, &first));
  remoteData(&p, p.reqId[0], LG_CLIPBOARD_DATA_TEXT, "a", 1);
  CHECK(first.count == 1);
  CHECK(first.nested);
  CHECK(p.request == 2);
  remoteData(&p, p.reqId[1], LG_CLIPBOARD_DATA_TEXT, "b", 1);
  CHECK(second.count == 1);

  lgClipboard_notifyTypes(types, 1);
  d.autoData = true;
  d.autoType = LG_CLIPBOARD_DATA_TEXT;
  d.autoBuf  = "local";
  d.autoSize = 5;
  CHECK(p.ev->request(p.evCtx, 70, LG_CLIPBOARD_DATA_TEXT));
  CHECK(p.data == 1);
  CHECK(p.dataId[0] == 70);
  CHECK(p.dataType[0] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(p.dataSize[0] == 5);
  CHECK(memcmp(p.dataBuf[0], "local", 5) == 0);

  lgClipboard_free();
}

static void testStreamLocal(void)
{
  init();
  lgClipboard_setFallback(&streamOps, &p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);

  CHECK(p.ev->request(p.evCtx, 40, LG_CLIPBOARD_DATA_TEXT));
  const LG_ClipboardRequest transfer = d.reqId[0];
  CHECK(lgClipboard_dataBegin(transfer, LG_CLIPBOARD_DATA_TEXT,
      LG_CLIPBOARD_SIZE_UNKNOWN) == LG_CLIPBOARD_RESULT_ACCEPTED);
  const uint8_t first[] = { 1, 2 };
  const uint8_t second[] = { 3, 4, 5 };
  CHECK(lgClipboard_dataChunk(transfer, 0, first, sizeof(first)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(transfer, sizeof(first),
      second, sizeof(second)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataEnd(transfer,
      sizeof(first) + sizeof(second)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(p.begin == 1);
  CHECK(p.streamId == 40);
  CHECK(p.streamType == LG_CLIPBOARD_DATA_TEXT);
  CHECK(p.sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN);
  CHECK(p.chunk == 2);
  CHECK(p.chunkOffset[0] == 0);
  CHECK(p.chunkOffset[1] == sizeof(first));
  CHECK(p.finalSize == sizeof(first) + sizeof(second));

  CHECK(p.ev->request(p.evCtx, 41, LG_CLIPBOARD_DATA_TEXT));
  const LG_ClipboardRequest invalid = d.reqId[1];
  CHECK(lgClipboard_dataBegin(invalid, LG_CLIPBOARD_DATA_TEXT, 1) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(invalid, 1, first, 1) ==
      LG_CLIPBOARD_RESULT_FAILED);
  CHECK(p.cancel == 1);
  CHECK(p.streamId == 41);
  CHECK(p.cancelReason == LG_CLIPBOARD_CANCEL_INVALID);

  CHECK(p.ev->request(p.evCtx, 42, LG_CLIPBOARD_DATA_TEXT));
  p.ev->requestCancel(p.evCtx, 42, LG_CLIPBOARD_CANCEL_REPLACED);
  CHECK(d.cancel == 1);
  CHECK(d.cancelReason == LG_CLIPBOARD_CANCEL_REPLACED);

  lgClipboard_free();
}

static void testStreamBlocked(void)
{
  init();
  lgClipboard_setFallback(&streamOps, &p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);
  CHECK(p.ev->request(p.evCtx, 50, LG_CLIPBOARD_DATA_TEXT));

  p.beginResult = LG_CLIPBOARD_RESULT_BLOCKED;
  uint8_t data[] = { 7, 8, 9 };
  lgClipboard_data(d.reqId[0], LG_CLIPBOARD_DATA_TEXT,
      data, sizeof(data));
  CHECK(p.begin == 1);
  CHECK(p.chunk == 0);
  CHECK(p.end == 0);
  data[0] = 0;

  p.beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  p.ev->dataReady(p.evCtx, 50);
  CHECK(p.begin == 2);
  CHECK(p.chunk == 1);
  CHECK(p.end == 1);
  CHECK(p.chunkSize[0] == 3);
  CHECK(p.chunkBuf[0][0] == 7);
  CHECK(p.chunkBuf[0][1] == 8);
  CHECK(p.chunkBuf[0][2] == 9);
  CHECK(p.finalSize == 3);

  CHECK(p.ev->request(p.evCtx, 51, LG_CLIPBOARD_DATA_TEXT));
  const LG_ClipboardRequest transfer = d.reqId[1];
  p.beginResult = LG_CLIPBOARD_RESULT_BLOCKED;
  CHECK(lgClipboard_dataBegin(transfer, LG_CLIPBOARD_DATA_TEXT,
      LG_CLIPBOARD_SIZE_UNKNOWN) == LG_CLIPBOARD_RESULT_BLOCKED);
  p.beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  d.autoBegin = true;
  p.ev->dataReady(p.evCtx, 51);
  CHECK(d.ready == 1);
  CHECK(d.readyResult == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataEnd(transfer, 0) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);

  lgClipboard_free();
}

static void testStreamRemote(void)
{
  init();
  lgClipboard_setFallback(&streamOps, &p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  notice(&p, types, 1);

  struct StreamSink sink = { 0 };
  initSink(&sink);
  LG_ClipboardRequest id;
  CHECK(lgClipboard_requestStream(
      LG_CLIPBOARD_DATA_TEXT, &sinkOps, &sink, &id));
  CHECK(id == p.reqId[0]);
  CHECK(p.ev->dataBegin(p.evCtx, id, LG_CLIPBOARD_DATA_TEXT,
      LG_CLIPBOARD_SIZE_UNKNOWN) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(sink.begin == 1);
  CHECK(sink.sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN);

  const uint8_t data[] = { 1, 3, 5, 7 };
  sink.chunkResult = LG_CLIPBOARD_RESULT_BLOCKED;
  CHECK(p.ev->dataChunk(p.evCtx, id, 0, data, sizeof(data)) ==
      LG_CLIPBOARD_RESULT_BLOCKED);
  CHECK(sink.chunk == 1);
  sink.chunkResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  CHECK(lgClipboard_requestReady(id));
  CHECK(p.ready == 1);
  CHECK(p.streamId == id);
  CHECK(p.ev->dataChunk(p.evCtx, id, 0, data, sizeof(data)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(p.ev->dataEnd(p.evCtx, id, sizeof(data)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(sink.chunk == 2);
  CHECK(sink.end == 1);
  CHECK(sink.finalSize == sizeof(data));
  CHECK(memcmp(sink.data, data, sizeof(data)) == 0);

  struct StreamSink cancel = { 0 };
  initSink(&cancel);
  CHECK(lgClipboard_requestStream(
      LG_CLIPBOARD_DATA_TEXT, &sinkOps, &cancel, &id));
  p.ev->dataCancel(p.evCtx, id, LG_CLIPBOARD_CANCEL_ABORTED);
  CHECK(cancel.cancel == 1);
  CHECK(cancel.reason == LG_CLIPBOARD_CANCEL_ABORTED);

  lgClipboard_free();
}

static void testRequestPublication(void)
{
  init();
  lgClipboard_setFallback(&streamOps, &p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  notice(&p, types, 1);

  struct StreamSink sink = { 0 };
  initSink(&sink);
  sink.requireRequest = true;
  p.earlyBegin = true;
  CHECK(lgClipboard_requestStream(LG_CLIPBOARD_DATA_TEXT,
      &sinkOps, &sink, &sink.request));
  CHECK(sink.request == p.reqId[0]);
  CHECK(p.earlyResult == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(sink.begin == 1);
  CHECK(p.ev->dataEnd(p.evCtx, sink.request, 0) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(sink.end == 1);

  lgClipboard_free();
}

static void testStreamLegacy(void)
{
  init();
  bind(&p);
  const LG_ClipboardData types[] = { LG_CLIPBOARD_DATA_TEXT };
  lgClipboard_notifyTypes(types, 1);

  CHECK(p.ev->request(p.evCtx, 60, LG_CLIPBOARD_DATA_TEXT));
  const LG_ClipboardRequest transfer = d.reqId[0];
  const uint8_t first[] = { 2, 4 };
  const uint8_t second[] = { 6, 8 };
  CHECK(lgClipboard_dataBegin(transfer, LG_CLIPBOARD_DATA_TEXT,
      LG_CLIPBOARD_SIZE_UNKNOWN) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(transfer, 0, first, sizeof(first)) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataChunk(transfer, sizeof(first), second,
      sizeof(second)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(lgClipboard_dataEnd(transfer, 4) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(p.data == 1);
  CHECK(p.dataId[0] == 60);
  CHECK(p.dataType[0] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(p.dataSize[0] == 4);
  CHECK(memcmp(p.dataBuf[0], "\x02\x04\x06\x08", 4) == 0);

  notice(&p, types, 1);
  struct StreamSink sink = { 0 };
  initSink(&sink);
  sink.beginResult = LG_CLIPBOARD_RESULT_BLOCKED;
  LG_ClipboardRequest id;
  CHECK(lgClipboard_requestStream(
      LG_CLIPBOARD_DATA_TEXT, &sinkOps, &sink, &id));
  const uint8_t remote[] = { 9, 8, 7 };
  remoteData(&p, id, LG_CLIPBOARD_DATA_TEXT, remote, sizeof(remote));
  CHECK(sink.begin == 1);
  CHECK(sink.chunk == 0);

  sink.beginResult = LG_CLIPBOARD_RESULT_ACCEPTED;
  CHECK(lgClipboard_requestReady(id));
  CHECK(p.ready == 0);
  CHECK(sink.begin == 2);
  CHECK(sink.chunk == 1);
  CHECK(sink.end == 1);
  CHECK(sink.size == sizeof(remote));
  CHECK(memcmp(sink.data, remote, sizeof(remote)) == 0);

  lgClipboard_free();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "preference", testPreference },
  { "request"   , testRequest    },
  { "invalid"   , testInvalid    },
  { "cancel"    , testCancel     },
  { "generation", testGeneration },
  { "local"     , testLocal      },
  { "reentrant" , testReentrant  },
  { "stream-local"  , testStreamLocal   },
  { "stream-blocked", testStreamBlocked },
  { "stream-remote" , testStreamRemote  },
  { "request-publish", testRequestPublication },
  { "stream-legacy" , testStreamLegacy  },
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
