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

#include <stdbool.h>
#include <stdint.h>

struct IVSHMEM
{
  unsigned int   size;
  void         * mem;

  // internal use
  void * opaque;
};

void ivshmemOptionsInit();
bool ivshmemInit(struct IVSHMEM * dev);
bool ivshmemOpen(struct IVSHMEM * dev);
bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDev);
void ivshmemClose(struct IVSHMEM * dev);
void ivshmemFree(struct IVSHMEM * dev);

/* Linux KVMFR support only for now (VM->VM) */
bool ivshmemHasDMA   (struct IVSHMEM * dev);
int  ivshmemGetDMABuf(struct IVSHMEM * dev, uint64_t offset, uint64_t size);
