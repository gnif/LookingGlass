/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include "common/time.h"
#include "common/debug.h"
#include "common/thread.h"
#include "common/ll.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct LGTimerState
{
  bool              running;
  struct LGThread * thread;
  struct ll       * timers;
};

struct LGTimer
{
  unsigned int   interval;
  unsigned int   count;
  LGTimerFn      fn;
  void         * udata;
};

static struct LGTimerState l_ts = { 0 };

static int timerFn(void * fn)
{
  struct LGTimer * timer;
  struct timespec time;

  clock_gettime(CLOCK_MONOTONIC, &time);

  while(l_ts.running)
  {
    ll_lock(l_ts.timers);
    ll_forEachNL(l_ts.timers, item, timer)
    {
      if (timer->count++ == timer->interval)
      {
        timer->count = 0;
        if (!timer->fn(timer->udata))
          ll_removeNL(l_ts.timers, item);
      }
    }
    ll_unlock(l_ts.timers);

    tsAdd(&time, 1000000);
    while(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL) != 0) {}
  }

  return 0;
}

static inline bool setupTimerThread(void)
{
  if (l_ts.thread)
    return true;

  l_ts.timers  = ll_new();
  l_ts.running = true;
  if (!l_ts.timers)
  {
    DEBUG_ERROR("failed to create linked list");
    goto err;
  }

  if (!lgCreateThread("TimerThread", timerFn, NULL, &l_ts.thread))
  {
    DEBUG_ERROR("failed to create the timer thread");
    goto err_thread;
  }

  return true;

err_thread:
  ll_free(l_ts.timers);

err:
  return false;
}

static void destroyTimerThread(void)
{
  if (ll_count(l_ts.timers))
    return;

  l_ts.running = false;
  lgJoinThread(l_ts.thread, NULL);
  l_ts.thread = NULL;
}

bool lgCreateTimer(const unsigned int intervalMS, LGTimerFn fn,
    void * udata, LGTimer ** result)
{
  struct LGTimer * timer = malloc(sizeof(*timer));
  if (!timer)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  timer->interval = intervalMS;
  timer->count    = 0;
  timer->fn       = fn;
  timer->udata    = udata;

  if (!setupTimerThread())
  {
    DEBUG_ERROR("failed to setup the timer thread");
    goto err_thread;
  }

  ll_push(l_ts.timers, timer);
  *result = timer;
  return true;

err_thread:
  free(timer);
  return false;
}

void lgTimerDestroy(LGTimer * timer)
{
  if (!l_ts.thread)
    return;

  ll_removeData(l_ts.timers, timer);
  free(timer);

  destroyTimerThread();
}
