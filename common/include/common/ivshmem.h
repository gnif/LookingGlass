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

#ifndef _H_LG_COMMON_IVSHMEM_
#define _H_LG_COMMON_IVSHMEM_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum IVSHMEMCacheMode
{
  /* Producers normally use write-combined mappings; consumers use cached.
   * Platforms without selectable cache modes ignore this policy. */
  IVSHMEM_CACHE_MODE_NONCACHED     = 0,
  IVSHMEM_CACHE_MODE_CACHED        = 1,
  IVSHMEM_CACHE_MODE_WRITECOMBINED = 2,
}
IVSHMEMCacheMode;

struct IVSHMEM
{
  size_t   size;
  void   * mem;

  // internal use
  void   * opaque;
};

void ivshmemOptionsInit(void);
bool ivshmemInit(struct IVSHMEM * dev);
bool ivshmemOpen(struct IVSHMEM * dev, IVSHMEMCacheMode cacheMode);
bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDev,
    IVSHMEMCacheMode cacheMode);
void ivshmemClose(struct IVSHMEM * dev);
void ivshmemFree(struct IVSHMEM * dev);

#endif
