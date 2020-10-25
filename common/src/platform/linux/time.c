/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2020-2020 Max Sistemich <maximilian.sistemich@rwth-aachen.de>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "common/time.h"
#include "common/debug.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct LGTimer
{
  LGTimerFn   fn;
  void      * udata;
  timer_t     id;
  bool        running;
};

static void TimerProc(union sigval arg)
{
  LGTimer * timer = (LGTimer *)arg.sival_ptr;
  if (!timer->fn(timer->udata))
  {
    if (timer_delete(timer->id))
      DEBUG_ERROR("failed to destroy the timer: %s", strerror(errno));
    timer->running = false;
  }
}

bool lgCreateTimer(const unsigned int intervalMS, LGTimerFn fn,
    void * udata, LGTimer ** result)
{
  LGTimer * ret = malloc(sizeof(LGTimer));

  if (!ret)
  {
    DEBUG_ERROR("failed to malloc LGTimer struct");
    return false;
  }

  ret->fn      = fn;
  ret->udata   = udata;
  ret->running = true;

  struct sigevent sev =
  {
    .sigev_notify = SIGEV_THREAD,
    .sigev_notify_function = &TimerProc,
    .sigev_value.sival_ptr = ret,
  };

  if (timer_create(CLOCK_MONOTONIC, &sev, &ret->id))
  {
    DEBUG_ERROR("failed to create timer: %s", strerror(errno));
    free(ret);
    return false;
  }

  struct timespec interval =
  {
    .tv_sec = 0,
    .tv_nsec = intervalMS * 1000 * 1000,
  };
  struct itimerspec spec =
  {
    .it_interval = interval,
    .it_value = interval,
  };

  if (timer_settime(ret->id, 0, &spec, NULL))
  {
    DEBUG_ERROR("failed to set timer: %s", strerror(errno));
    timer_delete(ret->id);
    free(ret);
    return false;
  }

  *result = ret;
  return true;
}

void lgTimerDestroy(LGTimer * timer)
{
  if (timer->running)
  {
    if (timer_delete(timer->id))
      DEBUG_ERROR("failed to destroy the timer: %s", strerror(errno));
  }

  free(timer);
}
