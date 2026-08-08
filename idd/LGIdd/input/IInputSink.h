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

#include <stdint.h>

class IInputSink
{
public:
  virtual ~IInputSink() = default;

  virtual bool IsAvailable() const = 0;
  virtual uint64_t GetGeneration() const = 0;
  virtual bool SendMouseRelative(int32_t deltaX, int32_t deltaY,
    int32_t wheel, uint32_t buttons) = 0;
  virtual bool SendMouseAbsolute(uint16_t x, uint16_t y,
    int32_t wheel, uint32_t buttons) = 0;
  virtual bool SendKeyboard(
    uint8_t modifiers, const uint8_t * keys) = 0;
  virtual bool Reset() = 0;
};
