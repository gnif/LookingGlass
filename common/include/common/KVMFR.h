/*
Looking Glass - KVM FrameRelay (KVMFR)
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/
#pragma once

#include <stdint.h>

#define LGMP_Q_POINTER     1
#define LGMP_Q_FRAME       2

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_BGRA      , // BGRA interleaved: B,G,R,A 32bpp
  FRAME_TYPE_RGBA      , // RGBA interleaved: R,G,B,A 32bpp
  FRAME_TYPE_RGBA10    , // RGBA interleaved: R,G,B,A 10,10,10,2 bpp
  FRAME_TYPE_YUV420    , // YUV420
  FRAME_TYPE_MAX       , // sentinel value
}
FrameType;

enum
{
  CURSOR_FLAG_POSITION = 0x1,
  CURSOR_FLAG_VISIBLE  = 0x2,
  CURSOR_FLAG_SHAPE    = 0x4
};
typedef uint32_t KVMFRCursorFlags;

typedef enum CursorType
{
  CURSOR_TYPE_COLOR       ,
  CURSOR_TYPE_MONOCHROME  ,
  CURSOR_TYPE_MASKED_COLOR
}
CursorType;

#define KVMFR_MAGIC   "KVMFR---"
#define KVMFR_VERSION 3

typedef struct KVMFR
{
  char     magic[8];
  uint32_t version;
  char     hostver[32];
}
KVMFR;

typedef struct KVMFRCursor
{
  int16_t    x, y;        // cursor x & y position
  CursorType type;        // shape buffer data type
  int8_t     hx, hy;      // shape hotspot x & y
  uint32_t   width;       // width of the shape
  uint32_t   height;      // height of the shape
  uint32_t   pitch;       // row length in bytes of the shape
}
KVMFRCursor;

typedef struct KVMFRFrame
{
  FrameType type;        // the frame data type
  uint32_t  width;       // the width
  uint32_t  height;      // the height
  uint32_t  stride;      // the row stride (zero if compressed data)
  uint32_t  pitch;       // the row pitch  (stride in bytes or the compressed frame size)
  uint32_t  offset;      // offset from the start of this header to the FrameBuffer header
}
KVMFRFrame;
