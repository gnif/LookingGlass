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

enum HIDReportId : uint8_t
{
  HID_REPORT_ID_MOUSE_ABSOLUTE = 1,
  HID_REPORT_ID_MOUSE_RELATIVE = 2,
  HID_REPORT_ID_KEYBOARD       = 3,
};

#pragma pack(push, 1)

struct HIDMouseAbsoluteReport
{
  uint8_t  reportId;
  uint32_t buttons;
  uint16_t x;
  uint16_t y;
  int8_t   wheel;
};

struct HIDMouseRelativeReport
{
  uint8_t reportId;
  uint32_t buttons;
  int16_t x;
  int16_t y;
  int8_t  wheel;
};

struct HIDKeyboardReport
{
  uint8_t reportId;
  uint8_t modifiers;
  uint8_t reserved;
  uint8_t keys[6];
};

struct HIDKeyboardLedsReport
{
  uint8_t reportId;
  uint8_t leds;
};

#pragma pack(pop)

static_assert(sizeof(HIDMouseAbsoluteReport) == 10);
static_assert(sizeof(HIDMouseRelativeReport) == 10);
static_assert(sizeof(HIDKeyboardReport)     == 9);
static_assert(sizeof(HIDKeyboardLedsReport) == 2);

static constexpr size_t HID_MAX_INPUT_REPORT_SIZE =
  sizeof(HIDMouseAbsoluteReport);

const uint8_t * HIDGetReportDescriptor();
size_t HIDGetReportDescriptorSize();
size_t HIDGetInputReportSize(uint8_t reportId);
