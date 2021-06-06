/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

// decared by the platform
extern HWND MessageHWND;

struct LGTimer
{
  LGTimerFn   fn;
  void      * udata;
  UINT_PTR    handle;
  bool        running;
};

static void TimerProc(HWND Arg1, UINT Arg2, UINT_PTR Arg3, DWORD Arg4)
{
  LGTimer * timer = (LGTimer *)Arg3;
  if (!timer->fn(timer->udata))
  {
    KillTimer(Arg1, timer->handle);
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
  ret->handle  = SetTimer(MessageHWND, (UINT_PTR)ret, intervalMS, TimerProc);

  *result = ret;
  return true;
}

void lgTimerDestroy(LGTimer * timer)
{
  if (timer->running)
  {
    if (!KillTimer(MessageHWND, timer->handle))
      DEBUG_ERROR("failed to destroy the timer");
  }

  free(timer);
}
