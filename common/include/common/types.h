/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

struct Border
{
  int left, top, right, bottom;
};

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_BGRA      , // BGRA interleaved: B,G,R,A 32bpp
  FRAME_TYPE_RGBA      , // RGBA interleaved: R,G,B,A 32bpp
  FRAME_TYPE_RGBA10    , // RGBA interleaved: R,G,B,A 10,10,10,2 bpp
  FRAME_TYPE_RGBA16F   , // RGBA interleaved: R,G,B,A 16,16,16,16 bpp float
  FRAME_TYPE_MAX       , // sentinel value
}
FrameType;

typedef enum FrameRotation
{
  FRAME_ROT_0,
  FRAME_ROT_90,
  FRAME_ROT_180,
  FRAME_ROT_270
}
FrameRotation;

extern const char * FrameTypeStr[FRAME_TYPE_MAX];

typedef enum CursorType
{
  CURSOR_TYPE_COLOR       ,
  CURSOR_TYPE_MONOCHROME  ,
  CURSOR_TYPE_MASKED_COLOR
}
CursorType;

#endif
