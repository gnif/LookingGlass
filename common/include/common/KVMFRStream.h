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

#ifndef _H_LG_COMMON_KVMFR_STREAM_
#define _H_LG_COMMON_KVMFR_STREAM_

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Transport-neutral wire copy of an exported LGMP stream descriptor. Keep
 * common protocol headers independent of LGMP; the LGMP transports convert
 * this structure field by field at their integration boundary.
 */
typedef struct KVMFRStreamDescriptor
{
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t offset;
  uint32_t regionSize;
  uint32_t direction;
  uint32_t policy;
  uint32_t slotCount;
  uint32_t slotSize;
}
KVMFRStreamDescriptor;

#if defined(__cplusplus)
static_assert(offsetof(KVMFRStreamDescriptor, magic) == 0,
    "KVMFR stream descriptor magic layout changed");
static_assert(offsetof(KVMFRStreamDescriptor, offset) == 8,
    "KVMFR stream descriptor offset layout changed");
static_assert(offsetof(KVMFRStreamDescriptor, slotSize) == 28,
    "KVMFR stream descriptor geometry layout changed");
static_assert(sizeof(KVMFRStreamDescriptor) == 32,
    "KVMFR stream descriptor layout changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(offsetof(KVMFRStreamDescriptor, magic) == 0,
    "KVMFR stream descriptor magic layout changed");
_Static_assert(offsetof(KVMFRStreamDescriptor, offset) == 8,
    "KVMFR stream descriptor offset layout changed");
_Static_assert(offsetof(KVMFRStreamDescriptor, slotSize) == 28,
    "KVMFR stream descriptor geometry layout changed");
_Static_assert(sizeof(KVMFRStreamDescriptor) == 32,
    "KVMFR stream descriptor layout changed");
#endif

#endif
