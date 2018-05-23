/*
Looking Glass - KVM FrameRelay (KVMFR)
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#define KVMFR_HEADER_MAGIC   "[[KVMFR]]"
#define KVMFR_HEADER_VERSION 6

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_ARGB      , // ABGR interleaved: A,R,G,B 32bpp
  FRAME_TYPE_H264      , // H264 compressed
  FRAME_TYPE_MAX       , // sentinel value
}
FrameType;

typedef enum CursorType
{
  CURSOR_TYPE_COLOR       ,
  CURSOR_TYPE_MONOCHROME  ,
  CURSOR_TYPE_MASKED_COLOR
}
CursorType;

#define KVMFR_CURSOR_FLAG_UPDATE  1 // cursor update available
#define KVMFR_CURSOR_FLAG_VISIBLE 2 // cursor is visible
#define KVMFR_CURSOR_FLAG_SHAPE   4 // shape updated
#define KVMFR_CURSOR_FLAG_POS     8 // position updated

typedef struct KVMFRCursor
{
  uint8_t    flags;       // KVMFR_CURSOR_FLAGS
  int16_t    x, y;        // cursor x & y position

  uint32_t   version;     // shape version
  CursorType type;        // shape buffer data type
  uint32_t   width;       // width of the shape
  uint32_t   height;      // height of the shape
  uint32_t   pitch;       // row length in bytes of the shape
  uint64_t   dataPos;     // offset to the shape data
}
KVMFRCursor;

#define KVMFR_FRAME_FLAG_UPDATE 1 // frame update available

typedef struct KVMFRFrame
{
  uint8_t     flags;       // KVMFR_FRAME_FLAGS
  FrameType   type;        // the frame data type
  uint32_t    width;       // the width
  uint32_t    height;      // the height
  uint32_t    stride;      // the row stride (zero if compressed data)
  uint32_t    pitch;       // the row pitch  (stride in bytes or the compressed frame size)
  uint64_t    dataPos;     // offset to the frame
}
KVMFRFrame;

#define KVMFR_HEADER_FLAG_RESTART 1 // restart signal from client
#define KVMFR_HEADER_FLAG_READY   2 // ready signal from client
#define KVMFR_HEADER_FLAG_PAUSED  4 // capture has been paused by the host

typedef struct KVMFRHeader
{
  char        magic[sizeof(KVMFR_HEADER_MAGIC)];
  uint32_t    version;     // version of this structure
  uint8_t     flags;       // KVMFR_HEADER_FLAGS
  KVMFRFrame  frame;       // the frame information
  KVMFRCursor cursor;      // the cursor information
}
KVMFRHeader;