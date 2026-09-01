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

#ifndef _LG_TYPES_H_
#define _LG_TYPES_H_

#include <stdint.h>

#include <LGProtocol/KVMFRTypes.h>

typedef void (*LG_FrameReleaseFn)(void * opaque, uint64_t handle);

struct Point
{
  int x, y;
};

struct DoublePoint
{
  double x, y;
};

struct Rect
{
  int x, y, w, h;
};

struct DoubleRect
{
  double x, y, w, h;
};

struct Border
{
  int left, top, right, bottom;
};

extern const char * FrameTypeStr[FRAME_TYPE_MAX];

typedef struct StringPair
{
  const char * name;
  const char * value;
}
StringPair;

#endif
