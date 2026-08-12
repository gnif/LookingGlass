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

struct FrameMode
{
  uint32_t width          = 0;
  uint32_t height         = 0;
  uint32_t refreshMilliHz = 0;
};

class FrameCaps
{
public:
  virtual ~FrameCaps() = default;

  // A rejecting implementation may provide a minimum configured-capacity
  // hint. Zero means that no useful size recommendation is available.
  virtual bool CanUseMode(const FrameMode& mode,
    uint32_t * requiredSizeMiB = nullptr) const = 0;
};
