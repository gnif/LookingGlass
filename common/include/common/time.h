/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#include <stdint.h>
#endif

typedef struct LGTimer LGTimer;

static inline uint64_t microtime(void)
{
#if defined(_WIN32)
  static LARGE_INTEGER freq = { 0 };
  if (!freq.QuadPart)
    QueryPerformanceFrequency(&freq);

  LARGE_INTEGER time;
  QueryPerformanceCounter(&time);
  return time.QuadPart / (freq.QuadPart / 1000000LL);
#else
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000000LL + time.tv_nsec / 1000LL;
#endif
}

#if !defined(_WIN32)
//FIXME: make win32 versions
static inline uint64_t nanotime(void)
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time);
  return ((uint64_t)time.tv_sec * 1000000000LL) + time.tv_nsec;
}

static inline void nsleep(uint64_t ns)
{
  const struct timespec ts =
  {
    .tv_sec  = ns / 1e9,
    .tv_nsec = ns - ((ns / 1e9) * 1e9)
  };
  nanosleep(&ts, NULL);
}

static inline void tsDiff(struct timespec *diff, const struct timespec *left,
              const struct timespec *right)
{
  diff->tv_sec = left->tv_sec - right->tv_sec;
  diff->tv_nsec = left->tv_nsec - right->tv_nsec;
  if (diff->tv_nsec < 0)
  {
    --diff->tv_sec;
    diff->tv_nsec += 1000000000;
  }
}

static inline uint32_t __iter_div_u64_rem(uint64_t dividend, uint32_t divisor, uint64_t *remainder)
{
  uint32_t ret = 0;

  while (dividend >= divisor) {
    /* The following asm() prevents the compiler from
       optimising this loop into a modulo operation.  */
    asm("" : "+rm"(dividend));

    dividend -= divisor;
    ret++;
  }

  *remainder = dividend;

  return ret;
}

static inline void tsAdd(struct timespec *a, uint64_t ns)
{
  a->tv_sec  += __iter_div_u64_rem(a->tv_nsec + ns, 1000000000L, &ns);
  a->tv_nsec = ns;
}
#endif

typedef bool (*LGTimerFn)(void * udata);

bool lgCreateTimer(const unsigned int intervalMS, LGTimerFn fn,
    void * udata, LGTimer ** result);

void lgTimerDestroy(LGTimer * timer);
