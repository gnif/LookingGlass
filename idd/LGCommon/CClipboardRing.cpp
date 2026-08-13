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

#include "CClipboardRing.h"

#include "Atomic.h"

#include <string.h>

void CClipboardRing::Initialize(
  ClipboardMapping& mapping, uint64_t epoch)
{
  memset(&mapping, 0, sizeof(mapping));
  mapping.magic     = LG_CLIPBOARD_MAPPING_MAGIC;
  mapping.version   = LG_CLIPBOARD_MAPPING_VERSION;
  mapping.size      = sizeof(mapping);
  mapping.slotBytes = KVMFR_CLIPBOARD_DATA_BYTES;
  mapping.slotCount = KVMFR_CLIPBOARD_SLOT_COUNT;
  mapping.epoch     = epoch;
}

bool CClipboardRing::Valid(
  const ClipboardMapping& mapping, uint64_t epoch)
{
  if (mapping.magic     != LG_CLIPBOARD_MAPPING_MAGIC   ||
      mapping.version   != LG_CLIPBOARD_MAPPING_VERSION ||
      mapping.size      != sizeof(mapping)               ||
      mapping.slotBytes != KVMFR_CLIPBOARD_DATA_BYTES    ||
      mapping.slotCount != KVMFR_CLIPBOARD_SLOT_COUNT    ||
      !mapping.epoch || mapping.epoch != epoch)
    return false;

  ClipboardRing& helper =
    const_cast<ClipboardRing&>(mapping.helperToIdd);
  ClipboardRing& idd =
    const_cast<ClipboardRing&>(mapping.iddToHelper);
  const uint32_t helperProduced = Atomic::Load(helper.produced);
  const uint32_t helperConsumed = Atomic::Load(helper.consumed);
  const uint32_t iddProduced    = Atomic::Load(idd.produced);
  const uint32_t iddConsumed    = Atomic::Load(idd.consumed);
  return helperProduced - helperConsumed <= KVMFR_CLIPBOARD_SLOT_COUNT &&
    iddProduced - iddConsumed <= KVMFR_CLIPBOARD_SLOT_COUNT;
}

ClipboardRingSlot * CClipboardRing::BeginWrite(
  ClipboardRing& ring, uint32_t& ticket)
{
  const uint32_t produced = Atomic::Load(ring.produced);
  const uint32_t consumed = Atomic::Load(ring.consumed);
  const uint32_t pending = produced - consumed;
  if (pending >= KVMFR_CLIPBOARD_SLOT_COUNT)
    return nullptr;

  ticket = produced;
  return &ring.slots[produced % KVMFR_CLIPBOARD_SLOT_COUNT];
}

bool CClipboardRing::EndWrite(ClipboardRing& ring, uint32_t ticket)
{
  if (Atomic::Load(ring.produced) != ticket)
    return false;

  Atomic::Fence();
  Atomic::Store(ring.produced, ticket + 1U);
  return true;
}

ClipboardRingReadResult CClipboardRing::BeginRead(ClipboardRing& ring,
  uint32_t& ticket, const ClipboardRingSlot *& slot)
{
  slot = nullptr;
  const uint32_t consumed = Atomic::Load(ring.consumed);
  const uint32_t pending = Atomic::Load(ring.produced) - consumed;
  if (!pending)
    return ClipboardRingReadResult::EMPTY;
  if (pending > KVMFR_CLIPBOARD_SLOT_COUNT)
    return ClipboardRingReadResult::CORRUPT;

  Atomic::Fence();
  ticket = consumed;
  slot = &ring.slots[consumed % KVMFR_CLIPBOARD_SLOT_COUNT];
  return ClipboardRingReadResult::READY;
}

bool CClipboardRing::EndRead(ClipboardRing& ring, uint32_t ticket)
{
  if (Atomic::Load(ring.consumed) != ticket)
    return false;

  Atomic::Fence();
  Atomic::Store(ring.consumed, ticket + 1U);
  return true;
}

bool CClipboardRing::Valid(const ClipboardRingSlot& slot)
{
  return slot.header.version == KVMFR_CLIPBOARD_VERSION &&
    slot.header.type >= KVMFR_CLIPBOARD_MESSAGE_CLAIM &&
    slot.header.type <= KVMFR_CLIPBOARD_MESSAGE_GRANT &&
    slot.header.length <= KVMFR_CLIPBOARD_DATA_BYTES &&
    (slot.header.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
      !slot.header.length);
}
