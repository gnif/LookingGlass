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

static constexpr wchar_t LG_INPUT_PIPE_NAME[] =
  L"\\\\.\\pipe\\LookingGlassIDDInput";

static constexpr uint32_t LG_INPUT_PIPE_MAGIC = 0x5049474c;
static constexpr uint16_t LG_INPUT_PIPE_VERSION = 1;
static constexpr size_t LG_INPUT_PIPE_MAX_REPORT_SIZE = 64;

enum LGInputPipeMessageType : uint16_t
{
  LG_INPUT_PIPE_MESSAGE_REPORT = 1,
};

#pragma pack(push, 1)
struct LGInputPipeMessage
{
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t payloadSize;
  uint64_t sequence;
  uint8_t payload[LG_INPUT_PIPE_MAX_REPORT_SIZE];
};
#pragma pack(pop)

static_assert(sizeof(LGInputPipeMessage) == 84,
  "LGInputPipeMessage wire layout changed");
