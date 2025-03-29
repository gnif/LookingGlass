/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#define LG_PIPE_NAME "\\\\.\\pipe\\LookingGlassIDD"

struct LGPipeMsg
{
  unsigned size;
  enum
  {
    SETCURSORPOS,
    SETDISPLAYMODE
  }
  type;
  union
  {
    struct
    {
      uint32_t x;
      uint32_t y;
    }
    curorPos;

    struct
    {
      uint32_t width;
      uint32_t height;
    }
    displayMode;
  };
};