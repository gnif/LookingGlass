/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include <time.h>
#include <stdint.h>

static inline uint64_t microtime()
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time);
  return ((uint64_t)time.tv_sec * 1000000) + (time.tv_nsec / 1000);
}

static inline uint64_t nanotime()
{
  struct timespec time;
  clock_gettime(CLOCK_MONOTONIC_RAW, &time);
  return ((uint64_t)time.tv_sec * 1e9) + time.tv_nsec;
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

#ifdef ATOMIC_LOCKING
  #define LG_LOCK_MODE    "Atomic"
  typedef volatile int LG_Lock;
  #define LG_LOCK_INIT(x) (x) = 0
  #define LG_LOCK(x)      while(__sync_lock_test_and_set(&(x), 1)) {nsleep(100);}
  #define LG_UNLOCK(x)    __sync_lock_release(&x)
  #define LG_LOCK_FREE(x)
#else
  #define LG_LOCK_MODE    "Mutex"
  typedef SDL_mutex * LG_Lock;
  #define LG_LOCK_INIT(x) (x = SDL_CreateMutex())
  #define LG_LOCK(x)      SDL_LockMutex(x)
  #define LG_UNLOCK(x)    SDL_UnlockMutex(x)
  #define LG_LOCK_FREE(x) SDL_DestroyMutex(x)
#endif