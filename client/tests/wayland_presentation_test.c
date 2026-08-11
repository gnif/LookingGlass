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

#include "common/debug.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARRAY_LENGTH(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_FEEDBACK 8U
#define MAX_NOTICE   8U
#define SEC          UINT64_C(1000000000)

enum ProxyKind
{
  PROXY_PRESENTATION,
  PROXY_FEEDBACK,
};

struct Proxy
{
  enum ProxyKind kind;
  void (**listener)(void);
  void           * data;
  bool             dead;
};

struct Proto
{
  struct Proxy  presentation;
  struct Proxy  feedback[MAX_FEEDBACK];
  unsigned int  feedbackN;
  unsigned int  feedbackDestroyN;
  unsigned int  presentationDestroyN;
};

struct Notice
{
  uint64_t token;
  uint64_t time;
  bool     valid;
};

struct Log
{
  struct Notice notice[MAX_NOTICE];
  unsigned int  noticeN;
  unsigned int  graphRegisterN;
  unsigned int  graphCompactN;
  unsigned int  graphUnregisterN;
  uint64_t      period;
  bool          havePeriod;
};

struct WaylandDSState wlWm;

static struct Proto proto;
static struct Log   rec;
static uint64_t     now;

const struct wl_interface wp_presentation_feedback_interface =
{
  .name    = "wp_presentation_feedback",
  .version = 1,
};

int clock_gettime(clockid_t id, struct timespec * ts)
{
  CHECK(id == CLOCK_MONOTONIC);
  ts->tv_sec  = now / SEC;
  ts->tv_nsec = now % SEC;
  return 0;
}

void wl_list_init(struct wl_list * list)
{
  list->next = list;
  list->prev = list;
}

void wl_list_insert(struct wl_list * list, struct wl_list * elm)
{
  elm->prev        = list;
  elm->next        = list->next;
  list->next->prev = elm;
  list->next       = elm;
}

void wl_list_remove(struct wl_list * elm)
{
  elm->prev->next = elm->next;
  elm->next->prev = elm->prev;
  elm->next       = NULL;
  elm->prev       = NULL;
}

int wl_list_empty(const struct wl_list * list)
{
  return list->next == list;
}

uint32_t wl_proxy_get_version(struct wl_proxy * proxy)
{
  CHECK(proxy);
  return 1;
}

int wl_proxy_add_listener(struct wl_proxy * proxy,
    void (**listener)(void), void * data)
{
  struct Proxy * p = (struct Proxy *)proxy;
  CHECK(p);
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

  if (p->kind == PROXY_PRESENTATION)
    ++proto.presentationDestroyN;
  else
    ++proto.feedbackDestroyN;
}

struct wl_proxy * wl_proxy_marshal_flags(struct wl_proxy * proxy,
    uint32_t opcode, const struct wl_interface * interface,
    uint32_t version, uint32_t flags, ...)
{
  struct Proxy * p = (struct Proxy *)proxy;
  (void)opcode;
  (void)version;

  CHECK(p == &proto.presentation);
  if (flags & WL_MARSHAL_FLAG_DESTROY)
  {
    CHECK(!interface);
    destroy(p);
    return NULL;
  }

  CHECK(interface == &wp_presentation_feedback_interface);
  CHECK(proto.feedbackN < ARRAY_LENGTH(proto.feedback));
  p = &proto.feedback[proto.feedbackN++];
  p->kind = PROXY_FEEDBACK;
  return (struct wl_proxy *)p;
}

void wl_proxy_destroy(struct wl_proxy * proxy)
{
  destroy((struct Proxy *)proxy);
}

void app_handleFramePresented(uint64_t token, uint64_t time, bool valid)
{
  CHECK(rec.noticeN < ARRAY_LENGTH(rec.notice));
  struct Notice * notice = &rec.notice[rec.noticeN++];
  notice->token = token;
  notice->time  = time;
  notice->valid = valid;
}

GraphHandle app_registerGraph(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn)
{
  CHECK(strcmp(name, "PHOTON") == 0);
  CHECK(buffer);
  CHECK(min == 0.0f);
  CHECK(max == 30.0f);
  CHECK(!formatFn);
  ++rec.graphRegisterN;
  return (GraphHandle)&rec;
}

void app_setGraphCompact(GraphHandle handle, bool compact)
{
  CHECK(handle == (GraphHandle)&rec);
  CHECK(compact);
  ++rec.graphCompactN;
}

void app_unregisterGraph(GraphHandle handle)
{
  CHECK(handle == (GraphHandle)&rec);
  ++rec.graphUnregisterN;
}

bool waylandOutputGetFramePeriod(uint64_t * period)
{
  if (!rec.havePeriod)
    return false;
  *period = rec.period;
  return true;
}

static struct Proxy * feedback(unsigned int no)
{
  CHECK(no < proto.feedbackN);
  struct Proxy * p = &proto.feedback[no];
  CHECK(p->kind == PROXY_FEEDBACK);
  CHECK(p->listener);
  CHECK(!p->dead);
  return p;
}

static void clockReady(void)
{
  struct Proxy * p = &proto.presentation;
  CHECK(p->listener);
  const struct wp_presentation_listener * listener =
    (const struct wp_presentation_listener *)p->listener;
  listener->clock_id(p->data, (struct wp_presentation *)p, CLOCK_MONOTONIC);
}

static void present(unsigned int no, uint64_t time, uint32_t nsec)
{
  struct Proxy * p = feedback(no);
  const struct wp_presentation_feedback_listener * listener =
    (const struct wp_presentation_feedback_listener *)p->listener;
  const uint64_t sec = time / SEC;
  listener->presented(p->data, (struct wp_presentation_feedback *)p,
      sec >> 32, sec, nsec, 0, 0, 0, 0);
}

static void discard(unsigned int no)
{
  struct Proxy * p = feedback(no);
  const struct wp_presentation_feedback_listener * listener =
    (const struct wp_presentation_feedback_listener *)p->listener;
  listener->discarded(p->data, (struct wp_presentation_feedback *)p);
}

static void checkNotice(unsigned int no, uint64_t token,
    uint64_t time, bool valid)
{
  CHECK(no < rec.noticeN);
  CHECK(rec.notice[no].token == token);
  CHECK(rec.notice[no].time == time);
  CHECK(rec.notice[no].valid == valid);
}

static void start(void)
{
  memset(&wlWm, 0, sizeof(wlWm));
  memset(&proto, 0, sizeof(proto));
  memset(&rec, 0, sizeof(rec));
  now                     = 10 * SEC;
  proto.presentation.kind = PROXY_PRESENTATION;
  wlWm.presentation       = (struct wp_presentation *)&proto.presentation;
  wlWm.surface            = (struct wl_surface *)(uintptr_t)1;

  CHECK(waylandPresentationInit());
  CHECK(rec.graphRegisterN == 1);
  CHECK(rec.graphCompactN == 1);
  CHECK(rec.graphUnregisterN == 0);
  CHECK(!waylandPresentationFrame(1));
  CHECK(proto.feedbackN == 0);
  clockReady();
}

static void finish(void)
{
  waylandPresentationFree();
  CHECK(!wlWm.presentation);
  CHECK(proto.presentation.dead);
  CHECK(proto.presentationDestroyN == 1);
  CHECK(rec.graphUnregisterN == 1);
}

static void testFeedbackFirst(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(11);
  CHECK(frame);

  present(0, 10 * SEC, 200000000);
  CHECK(rec.noticeN == 0);
  CHECK(proto.feedbackDestroyN == 1);

  now = 10 * SEC + 100000000;
  bool tracked;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(tracked);
  CHECK(rec.noticeN == 1);
  checkNotice(0, 11, 100000000, true);

  finish();
  CHECK(rec.noticeN == 1);
  CHECK(proto.feedbackDestroyN == 1);
}

static void testSwapFirst(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(12);
  CHECK(frame);

  now = 10 * SEC + 100000000;
  bool tracked;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(tracked);
  CHECK(rec.noticeN == 0);

  present(0, 10 * SEC, 200000000);
  CHECK(rec.noticeN == 1);
  checkNotice(0, 12, 100000000, true);

  finish();
  CHECK(rec.noticeN == 1);
  CHECK(proto.feedbackDestroyN == 1);
}

static void testDiscard(void)
{
  start();
  struct WaylandPresentationFrame * first = waylandPresentationFrame(21);
  CHECK(first);
  now = 10 * SEC + 100000000;
  bool tracked;
  waylandPresentationSwapDone(first, true, &tracked);
  CHECK(tracked);
  discard(0);
  CHECK(rec.noticeN == 1);
  checkNotice(0, 21, 0, false);

  struct WaylandPresentationFrame * second = waylandPresentationFrame(22);
  CHECK(second);
  discard(1);
  CHECK(rec.noticeN == 1);
  waylandPresentationSwapDone(second, true, &tracked);
  CHECK(tracked);
  CHECK(rec.noticeN == 2);
  checkNotice(1, 22, 0, false);

  finish();
  CHECK(rec.noticeN == 2);
  CHECK(proto.feedbackDestroyN == 2);
}

static void testSwapFail(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(31);
  CHECK(frame);

  bool tracked = true;
  waylandPresentationSwapDone(frame, false, &tracked);
  CHECK(!tracked);
  CHECK(rec.noticeN == 1);
  checkNotice(0, 31, 0, false);

  present(0, 10 * SEC, 200000000);
  CHECK(rec.noticeN == 1);
  CHECK(proto.feedbackDestroyN == 1);

  tracked = true;
  waylandPresentationSwapDone(NULL, true, &tracked);
  CHECK(!tracked);
  CHECK(rec.noticeN == 1);

  finish();
  CHECK(rec.noticeN == 1);
}

static void testTokenZero(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(0);
  CHECK(frame);

  now = 10 * SEC + 100000000;
  bool tracked = true;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(!tracked);
  present(0, 10 * SEC, 200000000);
  CHECK(rec.noticeN == 0);

  finish();
  CHECK(rec.noticeN == 0);
  CHECK(proto.feedbackDestroyN == 1);
}

static void testInvalidTime(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(41);
  CHECK(frame);
  now = 10 * SEC + 100000000;
  bool tracked;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(tracked);
  present(0, 10 * SEC, 1000000000);
  CHECK(rec.noticeN == 1);
  checkNotice(0, 41, 0, false);

  frame = waylandPresentationFrame(42);
  CHECK(frame);
  now = 10 * SEC + 200000000;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(tracked);
  present(1, 10 * SEC, 100000000);
  CHECK(rec.noticeN == 2);
  checkNotice(1, 42, 0, false);

  finish();
  CHECK(rec.noticeN == 2);
  CHECK(proto.feedbackDestroyN == 2);
}

static void testFreePending(void)
{
  start();
  struct WaylandPresentationFrame * frame = waylandPresentationFrame(51);
  CHECK(frame);
  now = 10 * SEC + 100000000;
  bool tracked;
  waylandPresentationSwapDone(frame, true, &tracked);
  CHECK(tracked);
  CHECK(rec.noticeN == 0);
  CHECK(proto.feedbackDestroyN == 0);

  finish();
  CHECK(rec.noticeN == 1);
  checkNotice(0, 51, 0, false);
  CHECK(proto.feedbackDestroyN == 1);
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "feedback-first", testFeedbackFirst },
  { "swap-first"    , testSwapFirst     },
  { "discard"       , testDiscard       },
  { "swap-fail"     , testSwapFail      },
  { "token-zero"    , testTokenZero     },
  { "invalid-time"  , testInvalidTime   },
  { "free-pending"  , testFreePending   },
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
