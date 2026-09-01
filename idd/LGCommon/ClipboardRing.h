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

#pragma once

#include <LGProtocol/KVMFRClipboard.h>

#include <stddef.h>
#include <stdint.h>

static constexpr uint32_t LG_CLIPBOARD_MAPPING_MAGIC = 0x4c474342U;
static constexpr uint32_t LG_CLIPBOARD_MAPPING_VERSION = 5U;

struct ClipboardRingSlot
{
  KVMFRClipboardSlotHeader header;
  uint8_t data[KVMFR_CLIPBOARD_DATA_BYTES];
};

struct alignas(64) ClipboardRing
{
  uint32_t produced;
  uint32_t consumed;
  uint32_t reserved[14];
  ClipboardRingSlot slots[KVMFR_CLIPBOARD_SLOT_COUNT];
};

struct alignas(64) ClipboardMapping
{
  uint32_t magic;
  uint32_t version;
  uint32_t size;
  uint32_t slotBytes;
  uint32_t slotCount;
  uint32_t reserved0;
  uint64_t epoch;
  uint64_t authorityId[2];
  uint8_t reserved1[16];
  ClipboardRing helperToIdd;
  ClipboardRing iddToHelper;
};

static_assert(sizeof(KVMFRClipboardSlotHeader) == 64,
  "clipboard slot header alignment changed");
static_assert(offsetof(ClipboardMapping, helperToIdd) == 64,
  "clipboard mapping header layout changed");
static_assert(offsetof(ClipboardMapping, iddToHelper) % 64 == 0,
  "clipboard rings must remain cache-line aligned");
