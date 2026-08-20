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

#include "common/KVMFRInput.h"

#include <stddef.h>
#include <stdint.h>

static constexpr wchar_t LG_INPUT_PIPE_NAME[] =
  L"\\\\.\\pipe\\LookingGlassIDDInput";

static constexpr uint32_t LG_INPUT_PIPE_MAGIC                  = 0x5049474c;
static constexpr uint16_t LG_INPUT_PIPE_VERSION                = 4;
static constexpr size_t   LG_INPUT_PIPE_MAX_PAYLOAD_SIZE       = 64;
static constexpr uint16_t LG_INPUT_MOUSE_ABSOLUTE_MAX          =
  KVMFR_INPUT_MOUSE_ABSOLUTE_MAX;
static constexpr int8_t   LG_INPUT_MOUSE_WHEEL_MIN             = -127;
static constexpr int32_t  LG_INPUT_MOUSE_DELTA_MAX             = INT16_MAX * 4;
static constexpr int32_t  LG_INPUT_MOUSE_DELTA_MIN             = INT16_MIN * 4;
static constexpr int32_t  LG_INPUT_MOUSE_WHEEL_MAX             = INT8_MAX * 4;
static constexpr int32_t  LG_INPUT_MOUSE_WHEEL_MIN_TOTAL       =
  LG_INPUT_MOUSE_WHEEL_MIN * 4;
static constexpr size_t   LG_INPUT_KEYBOARD_KEY_COUNT          =
  KVMFR_INPUT_KEYBOARD_KEY_COUNT;
static constexpr uint8_t  LG_INPUT_KEYBOARD_USAGE_MAX          =
  KVMFR_INPUT_KEYBOARD_USAGE_MAX;

enum LGInputMouseButton : uint32_t
{
  LG_INPUT_MOUSE_BUTTON_LEFT    = 1 << 0,
  LG_INPUT_MOUSE_BUTTON_RIGHT   = 1 << 1,
  LG_INPUT_MOUSE_BUTTON_MIDDLE  = 1 << 2,
  LG_INPUT_MOUSE_BUTTON_BACK    = 1 << 3,
  LG_INPUT_MOUSE_BUTTON_FORWARD = 1 << 4,
};

enum LGInputKeyboardModifier : uint8_t
{
  LG_INPUT_KEYBOARD_MODIFIER_LEFT_CONTROL  = 1 << 0,
  LG_INPUT_KEYBOARD_MODIFIER_LEFT_SHIFT    = 1 << 1,
  LG_INPUT_KEYBOARD_MODIFIER_LEFT_ALT      = 1 << 2,
  LG_INPUT_KEYBOARD_MODIFIER_LEFT_GUI      = 1 << 3,
  LG_INPUT_KEYBOARD_MODIFIER_RIGHT_CONTROL = 1 << 4,
  LG_INPUT_KEYBOARD_MODIFIER_RIGHT_SHIFT   = 1 << 5,
  LG_INPUT_KEYBOARD_MODIFIER_RIGHT_ALT     = 1 << 6,
  LG_INPUT_KEYBOARD_MODIFIER_RIGHT_GUI     = 1 << 7,
};

enum LGInputPipeMessageType : uint16_t
{
  // LGIdd to LGInput.
  LG_INPUT_PIPE_MESSAGE_MOUSE_ABSOLUTE = 1,
  LG_INPUT_PIPE_MESSAGE_MOUSE_RELATIVE = 2,
  LG_INPUT_PIPE_MESSAGE_KEYBOARD       = 3,
  // LGInput to LGIdd.
  LG_INPUT_PIPE_MESSAGE_KEYBOARD_LEDS  = 4,
};

#pragma pack(push, 1)
struct LGInputPipeMessage
{
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint32_t payloadSize;
  uint64_t sequence;
  uint8_t payload[LG_INPUT_PIPE_MAX_PAYLOAD_SIZE];
};
#pragma pack(pop)

using LGInputPipeMouseRelative = KVMFRInputMouseRelative;
using LGInputPipeMouseAbsolute = KVMFRInputMouseAbsolute;
using LGInputPipeKeyboard      = KVMFRInputKeyboard;

struct LGInputPipeKeyboardLEDs
{
  KVMFRInputKeyboardLEDFlags leds;
};

static_assert(sizeof(LGInputPipeMouseRelative) == 16,
  "LGInputPipeMouseRelative wire layout changed");
static_assert(sizeof(LGInputPipeMouseAbsolute) == 16,
  "LGInputPipeMouseAbsolute wire layout changed");
static_assert(sizeof(LGInputPipeKeyboard) == 16,
  "LGInputPipeKeyboard wire layout changed");
static_assert(sizeof(LGInputPipeKeyboardLEDs) == 1,
  "LGInputPipeKeyboardLEDs wire layout changed");
static_assert(sizeof(LGInputPipeMessage) == 84,
  "LGInputPipeMessage wire layout changed");
