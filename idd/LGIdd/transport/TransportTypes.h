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

#include <stddef.h>
#include <stdint.h>

enum : unsigned
{
  TRANSPORT_FRAME_QUEUE_LENGTH = 2,
  TRANSPORT_FRAME_BUFFER_COUNT = 3,
  TRANSPORT_MAX_CLIENTS        = 8,
};

enum : uint32_t
{
  FRAME_SCHEDULE_ACTIVE    = 0x1,
  FRAME_SCHEDULE_RELEASE   = 0x2,
  FRAME_SCHEDULE_RESET     = 0x4,
  FRAME_SCHEDULE_IMMEDIATE = 0x8,
};

struct FrameScheduleUpdate
{
  uint32_t clientID;
  uint32_t generation;
  uint32_t flags;
  uint64_t period;
  uint64_t targetSlack;
  int64_t  phaseError;
  uint32_t feedbackFrameSerial;
  uint32_t feedbackScheduleEpoch;
  uint32_t feedbackDeadlineSerial;
  uint32_t lease;
};

struct DirectFrameBufferMemory
{
  void   * address = nullptr;
  size_t   size    = 0;

  explicit operator bool() const
  {
    return address && size;
  }
};
