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

#include "ClipboardRing.h"

enum class ClipboardRingReadResult
{
  EMPTY,
  READY,
  CORRUPT,
};

class CClipboardRing
{
public:
  static void Initialize(ClipboardMapping& mapping, uint64_t epoch,
    const uint64_t (&authorityId)[2]);
  static bool Valid(const ClipboardMapping& mapping, uint64_t epoch);

  static ClipboardRingSlot * BeginWrite(
    ClipboardRing& ring, uint32_t& ticket);
  static bool EndWrite(ClipboardRing& ring, uint32_t ticket);

  static ClipboardRingReadResult BeginRead(ClipboardRing& ring,
    uint32_t& ticket, const ClipboardRingSlot *& slot);
  static bool EndRead(ClipboardRing& ring, uint32_t ticket);

  static bool Valid(const ClipboardRingSlot& slot);
};
