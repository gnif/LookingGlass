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
#define KVMFR_HEADER_VERSION 2
#define KVMFR_CURSOR_BUFFER  (32*32*4)

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_ARGB      , // ABGR interleaved: A,R,G,B 32bpp
  FRAME_TYPE_RGB       , // RGB interleaved :   R,G,B 24bpp
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

#define KVMFR_CURSOR_FLAG_VISIBLE 1 // cursor is visible
#define KVMFR_CURSOR_FLAG_SHAPE   2 // shape updated
#define KVMFR_CURSOR_FLAG_POS     4 // position updated

typedef struct KVMFRCursor
{
  uint8_t    flags;       // KVMFR_CURSOR_FLAGS
  int16_t    x, y;        // cursor x & y position
  CursorType type;        // shape buffer data type
  uint8_t    w, h;        // shape width and height
  uint8_t    pitch;       // shape row length in bytes
  uint8_t    shape[KVMFR_CURSOR_BUFFER];
}
KVMFRCursor;

typedef struct KVMFRFrame
{
  FrameType   type;        // the frame data type
  uint32_t    width;       // the width
  uint32_t    height;      // the height
  uint32_t    stride;      // the row stride
  uint64_t    dataPos;     // offset to the frame
}
KVMFRFrame;

#define KVMFR_HEADER_FLAG_FRAME   1 // frame update available
#define KVMFR_HEADER_FLAG_CURSOR  2 // cursor update available
#define KVMFR_HEADER_FLAG_RESTART 4 // restart signal from client
#define KVMFR_HEADER_FLAG_READY   8 // ready signal from client

typedef struct KVMFRHeader
{
  char        magic[sizeof(KVMFR_HEADER_MAGIC)];
  uint32_t    version;     // version of this structure
  uint16_t    hostID;      // the host ivshmem client id
  uint16_t    guestID;     // the guest ivshmem client id
  uint32_t    updateCount; // updated each change
  uint8_t     flags;       // KVMFR_HEADER_FLAGS

  KVMFRFrame  frame;    // the frame information
  KVMFRCursor cursor;   // the cursor information
}
KVMFRHeader;