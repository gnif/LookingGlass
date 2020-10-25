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

#include "time.h"

#include <stdatomic.h>

#define LG_LOCK_MODE "Atomic"
typedef atomic_flag LG_Lock;
#define LG_LOCK_INIT(x) atomic_flag_clear(&(x))
#define LG_LOCK(x) \
  while(atomic_flag_test_and_set_explicit(&(x), memory_order_acquire)) { ; }
#define LG_UNLOCK(x) \
  atomic_flag_clear_explicit(&(x), memory_order_release);
#define LG_LOCK_FREE(x)

#define INTERLOCKED_INC(x) atomic_fetch_add((x), 1)
#define INTERLOCKED_DEC(x) atomic_fetch_sub((x), 1)

#define INTERLOCKED_SECTION(lock, x) \
  LG_LOCK(lock) \
  x \
  LG_UNLOCK(lock)
