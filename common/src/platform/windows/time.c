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

#include "common/time.h"
#include "common/debug.h"
#include "common/windebug.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <windows.h>

NTSYSCALLAPI NTSTATUS NTAPI NtDelayExecution(
  _In_ BOOLEAN Alertable,
  _In_opt_ PLARGE_INTEGER DelayInterval
);

struct LGTimer
{
  LGTimerFn   fn;
  void      * udata;
  HANDLE      handle;
  atomic_bool running;
};

static uint64_t performanceFrequency(void)
{
  static atomic_uint_fast64_t cached;

  uint64_t result = atomic_load_explicit(&cached, memory_order_acquire);
  if (result)
    return result;

  LARGE_INTEGER value;
  if (!QueryPerformanceFrequency(&value) || value.QuadPart <= 0)
    abort();

  result = value.QuadPart;
  atomic_store_explicit(&cached, result, memory_order_release);
  return result;
}

static VOID CALLBACK timerProc(PVOID opaque, BOOLEAN fired)
{
  (void)fired;

  LGTimer * timer = opaque;
  if (!atomic_load_explicit(&timer->running, memory_order_acquire))
    return;

  if (!timer->fn(timer->udata))
  {
    atomic_store_explicit(&timer->running, false, memory_order_release);
    if (!ChangeTimerQueueTimer(NULL, timer->handle, INFINITE, 0))
      DEBUG_WINERROR("Failed to stop timer", GetLastError());
  }
}

uint64_t microtime(void)
{
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  const uint64_t frequency = performanceFrequency();
  const uint64_t seconds   = (uint64_t)value.QuadPart / frequency;
  const uint64_t ticks     = (uint64_t)value.QuadPart % frequency;
  return seconds * UINT64_C(1000000) +
    ticks * UINT64_C(1000000) / frequency;
}

uint64_t nanotime(void)
{
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  const uint64_t frequency = performanceFrequency();
  const uint64_t seconds   = (uint64_t)value.QuadPart / frequency;
  const uint64_t ticks     = (uint64_t)value.QuadPart % frequency;
  return seconds * UINT64_C(1000000000) +
    ticks * UINT64_C(1000000000) / frequency;
}

void nsleep(uint64_t ns)
{
  if (!ns)
    return;

  LARGE_INTEGER interval =
  {
    .QuadPart = -(int64_t)((ns + UINT64_C(99)) / UINT64_C(100)),
  };
  NtDelayExecution(FALSE, &interval);
}

bool lgCreateTimer(const unsigned int intervalMS, LGTimerFn fn,
    void * udata, LGTimer ** result)
{
  LGTimer * ret = malloc(sizeof(*ret));
  if (!ret)
  {
    DEBUG_ERROR("failed to malloc LGTimer struct");
    return false;
  }

  ret->fn     = fn;
  ret->udata  = udata;
  ret->handle = NULL;
  atomic_init(&ret->running, true);

  if (!CreateTimerQueueTimer(&ret->handle, NULL, timerProc, ret,
        intervalMS, intervalMS, WT_EXECUTEDEFAULT))
  {
    DEBUG_WINERROR("Failed to create timer", GetLastError());
    free(ret);
    return false;
  }

  *result = ret;
  return true;
}

void lgTimerDestroy(LGTimer * timer)
{
  atomic_store_explicit(&timer->running, false, memory_order_release);
  if (timer->handle &&
      !DeleteTimerQueueTimer(NULL, timer->handle, INVALID_HANDLE_VALUE))
  {
    const DWORD error = GetLastError();
    if (error != ERROR_IO_PENDING)
      DEBUG_WINERROR("Failed to destroy timer", error);
  }

  free(timer);
}

NTSYSCALLAPI NTSTATUS NTAPI NtSetTimerResolution(
  _In_ ULONG DesiredTime,
  _In_ BOOLEAN SetResolution,
  _Out_ PULONG ActualTime
);

void lgTimeInit(void)
{
  ULONG actualResolution;
  NtSetTimerResolution(1, true, &actualResolution);
  DEBUG_INFO("System timer resolution: %.1f μs", actualResolution / 10.0);
}
