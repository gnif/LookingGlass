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

#include "common/event.h"
#include "common/thread.h"
#include "common/time.h"

#include <stdatomic.h>
#include <stdint.h>

#define TEST_WAIT_NS UINT64_C(1000000000)

struct SignalState
{
  LGEvent * event;
};

struct TimerState
{
  LGEvent    * event;
  atomic_uint  calls;
};

static int signalThread(void * opaque)
{
  const struct SignalState * state = opaque;
  nsleep(UINT64_C(2000000));
  CHECK(lgSignalEvent(state->event));
  return 0;
}

static bool timerCallback(void * opaque)
{
  struct TimerState * state = opaque;
  const unsigned int calls = atomic_fetch_add_explicit(
      &state->calls, 1, memory_order_relaxed) + 1;
  if (calls < 2)
    return true;

  CHECK(lgSignalEvent(state->event));
  return false;
}

static void testEventWaits(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  CHECK(event);

  const uint64_t timeoutStart = nanotime();
  CHECK(!lgWaitEventNS(event, UINT64_C(1000000)));
  CHECK(nanotime() >= timeoutStart);

  const struct SignalState state = { .event = event };
  LGThread * thread;
  CHECK(lgCreateThread("eventSignal", signalThread, (void *)&state, &thread));
  CHECK(lgWaitEventDeadline(event, nanotime() + TEST_WAIT_NS));
  CHECK(lgJoinThread(thread, NULL));

  CHECK(!lgWaitEvent(event, 0));
  lgFreeEvent(event);
}

static void testManualReset(void)
{
  LGEvent * event = lgCreateEvent(false, 0);
  CHECK(event);
  CHECK(lgSignalEvent(event));
  CHECK(lgWaitEvent(event, 0));
  CHECK(lgWaitEvent(event, 0));
  CHECK(lgResetEvent(event));
  CHECK(!lgWaitEvent(event, 0));
  lgFreeEvent(event);
}

static void testTimer(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  CHECK(event);

  struct TimerState state = { .event = event };
  atomic_init(&state.calls, 0);

  LGTimer * timer;
  CHECK(lgCreateTimer(1, timerCallback, &state, &timer));
  CHECK(lgWaitEventNS(event, TEST_WAIT_NS));
  lgTimerDestroy(timer);
  CHECK(atomic_load_explicit(&state.calls, memory_order_relaxed) == 2);
  lgFreeEvent(event);
}

int main(void)
{
  lgTimeInit();
  testEventWaits();
  testManualReset();
  testTimer();
  return 0;
}
