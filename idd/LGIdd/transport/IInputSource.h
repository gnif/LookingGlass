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

struct InputSourceId
{
  uint32_t client     = 0;
  uint32_t generation = 0;
};

struct InputTargetState
{
  uint64_t      state = 0;
  InputSourceId owner;
  bool          available = false;
  bool          owned     = false;
};

enum class InputResult
{
  ACCEPTED,
  BUSY,
  UNAVAILABLE,
  STALE,
};

class IInputTarget
{
public:
  virtual ~IInputTarget() = default;

  virtual InputTargetState GetState(const InputSourceId& source) = 0;
  virtual void Failed() = 0;
  virtual InputResult Claim(const InputSourceId& source) = 0;
  virtual InputResult Touch(const InputSourceId& source) = 0;
  virtual InputResult Release(
    const InputSourceId& source, bool reset) = 0;
  virtual InputResult SendMouseRelative(const InputSourceId& source,
    int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons) = 0;
  virtual InputResult SendMouseAbsolute(const InputSourceId& source,
    uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons) = 0;
  virtual InputResult SendKeyboard(const InputSourceId& source,
    uint8_t modifiers, const uint8_t * keys) = 0;
  virtual InputResult Reset(const InputSourceId& source) = 0;
};

class IInputSource
{
public:
  virtual ~IInputSource() = default;

  // Start may issue target calls before it returns. Stop is a callback-
  // quiescence barrier and leaves no work that can access the target.
  virtual bool Start(IInputTarget& target) = 0;
  virtual void Stop() = 0;
};
