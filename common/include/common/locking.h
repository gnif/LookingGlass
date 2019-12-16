/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#if defined(__GCC__) || defined(__GNUC__)
#define INTERLOCKED_AND8        __sync_fetch_and_and
#define INTERLOCKED_OR8         __sync_fetch_and_or
#define INTERLOCKED_INC(x)      __sync_fetch_and_add((x), 1)
#define INTERLOCKED_DEC(x)      __sync_fetch_and_sub((x), 1)
#define INTERLOCKED_GET(x)      __sync_fetch_and_add((x), 0)
#define INTERLOCKED_CE(x, c, v) __sync_val_compare_and_swap((x), (c), (v))

#define INTERLOCKED_SECTION(lock, x) \
  while(__sync_lock_test_and_set(&(lock), 1)) while((lock)); \
  x\
  __sync_lock_release(&(lock));

#else
#define INTERLOCKED_OR8         InterlockedOr8
#define INTERLOCKED_AND8        InterlockedAnd8
#define INTERLOCKED_INC         InterlockedIncrement
#define INTERLOCKED_DEC         InterlockedDecrement
#define INTERLOCKED_GET(x)      InterlockedAdd((x), 0)
#define INTERLOCKED_CE(x, c, v) InterlockedCompareExchange((x), (v), (c))
#endif