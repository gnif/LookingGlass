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

#include "frame_scheduler.h"
#include "main.h"
#include "test.h"

#include "common/debug.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CTL 16U
#define NS_MS(x) ((uint64_t)(x) * UINT64_C(1000000))

#define PERIOD_10MS  NS_MS(10)
#define PERIOD_104MS UINT64_C(10400000)
#define PERIOD_11MS  NS_MS(11)
#define PERIOD_60HZ  UINT64_C(16666667)

struct LG_Transport
{
  int unused;
};

struct Trace
{
  uint64_t                 now;
  uint64_t                 period;
  bool                     havePeriod;
  unsigned int             periodN;
  unsigned int             sendN;
  unsigned int             ackN;
  unsigned int             sendRetN;
  unsigned int             ackRetN;
  LG_TransportControl      ctl[MAX_CTL];
  LG_TransportControlToken tok[MAX_CTL];
  uint64_t                 sendAt[MAX_CTL];
  LG_TransportStatus       sendRet[MAX_CTL];
  LG_TransportStatus       ackRet[MAX_CTL];
  LG_TransportControlToken lastTok;
};

static struct Trace        t;
static struct LG_Transport tr;

struct AppState g_state;

int clock_gettime(clockid_t id, struct timespec * ts)
{
  ts->tv_sec  = t.now / UINT64_C(1000000000);
  ts->tv_nsec = t.now % UINT64_C(1000000000);
  return 0;
}

static bool getPeriod(uint64_t * period)
{
  ++t.periodN;
  if (!t.havePeriod)
    return false;
  *period = t.period;
  return true;
}

static LG_TransportStatus sendCtl(LG_Transport * transport,
    const LG_TransportControl * control, LG_TransportControlToken * token)
{
  CHECK(transport == &tr);
  CHECK(control);
  CHECK(token);
  CHECK(t.sendN < MAX_CTL);

  const unsigned int no = t.sendN++;
  t.ctl[no]              = *control;
  t.sendAt[no]           = t.now;
  const LG_TransportStatus status = no < t.sendRetN ?
    t.sendRet[no] : LG_TRANSPORT_OK;
  if (status == LG_TRANSPORT_OK)
  {
    t.lastTok = no + 1;
    t.tok[no] = t.lastTok;
    *token    = t.lastTok;
  }
  return status;
}

static LG_TransportStatus ackCtl(
    LG_Transport * transport, LG_TransportControlToken token)
{
  CHECK(transport == &tr);
  CHECK(token == t.lastTok);

  const unsigned int no = t.ackN++;
  return no < t.ackRetN ? t.ackRet[no] : LG_TRANSPORT_OK;
}

static struct LG_DisplayServerOps dsOps =
{
  .getFramePeriod = getPeriod,
};

static const LG_TransportOps trOps =
{
  .name          = "fake",
  .sendControl   = sendCtl,
  .controlStatus = ackCtl,
};

static void init(LG_TransportFeatureFlags features, uint64_t period)
{
  t.now                    = UINT64_C(1000000000);
  t.period                 = period;
  t.havePeriod             = true;
  g_state.ds               = &dsOps;
  g_state.transport.handle = &tr;
  g_state.transport.ops    = &trOps;
  frameScheduler_init();
  frameScheduler_start(features);
}

static void step(uint64_t ns)
{
  t.now += ns;
}

static const LG_TransportControl * control(unsigned int no)
{
  CHECK(no < t.sendN);
  CHECK(t.ctl[no].type == LG_TRANSPORT_CONTROL_FRAME_SCHEDULE);
  return &t.ctl[no];
}

static void checkCtl(unsigned int no, LG_TransportFrameScheduleFlags flags,
    uint64_t period, uint32_t generation)
{
  const LG_TransportControl * ctl = control(no);
  CHECK(ctl->frameSchedule.flags == flags);
  CHECK(ctl->frameSchedule.period == period);
  CHECK(ctl->frameSchedule.generation == generation);
  CHECK(ctl->frameSchedule.targetSlack == UINT64_C(500000));
  CHECK(ctl->frameSchedule.lease == 1000);
}

static void testAbsent(void)
{
  init(0, PERIOD_60HZ);
  frameScheduler_observeCadence();
  frameScheduler_update();
  frameScheduler_feedback(1, 1, 1, 1, 1);
  frameScheduler_stop();

  CHECK(t.periodN == 0);
  CHECK(t.sendN == 0);
  CHECK(t.ackN == 0);
  frameScheduler_free();
}

static void testStart(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, PERIOD_60HZ);
  frameScheduler_observeCadence();
  frameScheduler_update();

  checkCtl(0, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_RESET |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_60HZ, 2);
  CHECK(t.sendN == 1);
  CHECK(t.ackN == 0);

  step(NS_MS(250));
  frameScheduler_update();
  checkCtl(1, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE, PERIOD_60HZ, 2);
  CHECK(t.ackN == 1);

  frameScheduler_stop();
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 2);
  CHECK(t.ackN == 2);
  frameScheduler_free();
}

static void testEnqueue(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, PERIOD_10MS);
  t.sendRet[0] = LG_TRANSPORT_UNAVAILABLE;
  t.sendRet[1] = LG_TRANSPORT_ERROR;
  t.sendRetN   = 2;

  frameScheduler_observeCadence();
  frameScheduler_update();
  CHECK(t.sendN == 1);

  step(NS_MS(9));
  frameScheduler_update();
  CHECK(t.sendN == 1);
  step(NS_MS(1));
  frameScheduler_update();
  CHECK(t.sendN == 2);

  step(NS_MS(19));
  frameScheduler_update();
  CHECK(t.sendN == 2);
  step(NS_MS(1));
  frameScheduler_update();
  CHECK(t.sendN == 3);
  CHECK(t.sendAt[1] - t.sendAt[0] == NS_MS(10));
  CHECK(t.sendAt[2] - t.sendAt[1] == NS_MS(20));

  frameScheduler_stop();
  checkCtl(3, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 2);
  CHECK(t.ackN == 1);

  /* A failed initial enqueue must not discard the reset request. */
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_RESET |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_10MS, 2);
  frameScheduler_free();
}

static void testAck(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, PERIOD_10MS);
  t.ackRet[0] = LG_TRANSPORT_UNAVAILABLE;
  t.ackRet[1] = LG_TRANSPORT_ERROR;
  t.ackRet[2] = LG_TRANSPORT_OK;
  t.ackRetN   = 3;

  frameScheduler_observeCadence();
  frameScheduler_update();
  CHECK(t.sendN == 1);

  step(NS_MS(250));
  frameScheduler_update();
  CHECK(t.ackN == 1);
  CHECK(t.sendN == 1);
  step(NS_MS(9));
  frameScheduler_update();
  CHECK(t.ackN == 1);
  step(NS_MS(1));
  frameScheduler_update();
  CHECK(t.ackN == 2);

  step(NS_MS(19));
  frameScheduler_update();
  CHECK(t.ackN == 2);
  step(NS_MS(1));
  frameScheduler_update();
  CHECK(t.ackN == 3);
  CHECK(t.sendN == 2);

  frameScheduler_stop();
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 2);
  CHECK(t.ackN == 4);

  /* Recovery from an acknowledgement fault requests an immediate frame. */
  checkCtl(1, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_10MS, 2);
  frameScheduler_free();
}

static void testPeriod(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, NS_MS(1));
  frameScheduler_observeCadence();
  frameScheduler_update();
  CHECK(t.sendN == 0);

  t.period = PERIOD_10MS;
  frameScheduler_update();
  checkCtl(0, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_RESET |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_10MS, 2);

  step(NS_MS(250));
  t.period = PERIOD_104MS;
  frameScheduler_update();
  checkCtl(1, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE,
      UINT64_C(10050000), 2);

  step(NS_MS(250));
  t.period = PERIOD_11MS;
  frameScheduler_update();
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_RESET |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_11MS, 3);

  t.period = UINT64_C(1000000001);
  frameScheduler_update();
  CHECK(t.sendN == 3);

  frameScheduler_stop();
  checkCtl(3, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 3);
  frameScheduler_free();
}

static void testCadence(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, PERIOD_60HZ);
  frameScheduler_observeCadence();
  frameScheduler_update();
  frameScheduler_feedback(20, 2, 7, 9, UINT64_C(100000000));

  step(UINT64_C(500000001));
  frameScheduler_update();
  checkCtl(1, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 2);

  step(NS_MS(10));
  frameScheduler_observeCadence();
  frameScheduler_update();
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE |
      LG_TRANSPORT_FRAME_SCHEDULE_RESET |
      LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE, PERIOD_60HZ, 3);
  CHECK(control(2)->frameSchedule.phaseError == 0);
  CHECK(control(2)->frameSchedule.feedbackFrameSerial == 0);
  CHECK(control(2)->frameSchedule.feedbackScheduleEpoch == 0);
  CHECK(control(2)->frameSchedule.feedbackDeadlineSerial == 0);

  frameScheduler_stop();
  checkCtl(3, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 3);
  frameScheduler_free();
}

static void testFeedback(void)
{
  init(LG_TRANSPORT_FEATURE_FRAME_SCHEDULE, PERIOD_10MS);
  frameScheduler_observeCadence();
  frameScheduler_update();

  frameScheduler_feedback(1, 0, 1, 1, UINT64_C(100000000));
  frameScheduler_feedback(1, 3, 1, 1, UINT64_C(100000000));
  frameScheduler_feedback(20, 2, 7, 9, UINT64_C(100000000));
  step(NS_MS(49));
  frameScheduler_update();
  CHECK(t.sendN == 1);
  step(NS_MS(1));
  frameScheduler_update();
  CHECK(t.sendN == 2);
  checkCtl(1, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE, PERIOD_10MS, 2);
  CHECK(control(1)->frameSchedule.phaseError == INT64_C(5000000));
  CHECK(control(1)->frameSchedule.feedbackFrameSerial == 20);
  CHECK(control(1)->frameSchedule.feedbackScheduleEpoch == 7);
  CHECK(control(1)->frameSchedule.feedbackDeadlineSerial == 9);

  frameScheduler_feedback(20, 2, 7, 9, 0);
  frameScheduler_feedback(21, 3, 7, 10, 0);
  step(NS_MS(50));
  frameScheduler_update();
  CHECK(t.sendN == 2);

  frameScheduler_feedback(21, 2, 8, 10, 0);
  frameScheduler_update();
  CHECK(t.sendN == 3);
  checkCtl(2, LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE, PERIOD_10MS, 2);
  CHECK(control(2)->frameSchedule.phaseError == -INT64_C(500000));
  CHECK(control(2)->frameSchedule.feedbackFrameSerial == 21);
  CHECK(control(2)->frameSchedule.feedbackScheduleEpoch == 8);
  CHECK(control(2)->frameSchedule.feedbackDeadlineSerial == 10);

  frameScheduler_stop();
  checkCtl(3, LG_TRANSPORT_FRAME_SCHEDULE_RELEASE, 0, 2);
  frameScheduler_free();
}

struct Test
{
  const char * name;
  void (*run)(void);
};

static const struct Test tests[] =
{
  { "absent"  , testAbsent   },
  { "start"   , testStart    },
  { "enqueue" , testEnqueue  },
  { "ack"     , testAck      },
  { "period"  , testPeriod   },
  { "cadence" , testCadence  },
  { "feedback", testFeedback },
};

int main(int argc, char ** argv)
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <case>\n", argv[0]);
    return EXIT_FAILURE;
  }

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
