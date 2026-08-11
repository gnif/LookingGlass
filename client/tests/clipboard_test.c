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
  unsigned int                 request;
  LG_ClipboardData             noticeTypes[LG_CLIPBOARD_DATA_NONE];
  size_t                       noticeCount;
  LG_ClipboardRequest          reqId[MAX_CALL];
  LG_ClipboardData             reqType[MAX_CALL];
  LG_ClipboardRequest          dataId[MAX_CALL];
  LG_ClipboardData             dataType[MAX_CALL];
  size_t                       dataSize[MAX_CALL];
  uint8_t                      dataBuf[MAX_CALL][MAX_DATA];
};

struct Display
{
  unsigned int        notice;
  unsigned int        release;
  unsigned int        request;
  LG_ClipboardData    noticeType[MAX_CALL];
  LG_ClipboardRequest reqId[MAX_CALL];
  LG_ClipboardData    reqType[MAX_CALL];
  bool                autoData;
  LG_ClipboardData    autoType;
  const void        * autoBuf;
  size_t              autoSize;
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

static struct Provider p;
static struct Provider q;
static struct Provider r;
static struct Display  d;

struct AppState  g_state;
struct AppParams g_params;

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

static bool request(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type)
{
  struct Provider * provider = opaque;
  CHECK(provider->request < MAX_CALL);
  const unsigned int no = provider->request++;
  provider->reqId[no]    = id;
  provider->reqType[no]  = type;
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

static struct LG_DisplayServerOps dsOps =
{
  .cbNotice  = dsNotice,
  .cbRelease = dsRelease,
  .cbRequest = dsRequest,
};

static void initProvider(struct Provider * provider)
{
  provider->avail    = true;
  provider->attachOK = true;
  provider->callOK   = true;
  provider->reqOK    = true;
  provider->gen      = 1;
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
