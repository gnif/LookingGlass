#pragma once
/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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

#include <stdint.h>

#define KVMGFX_HEADER_MAGIC   "[[KVMGFXHeader]]"
#define KVMGFX_HEADER_VERSION 3

typedef enum FrameType
{
  FRAME_TYPE_INVALID   ,
  FRAME_TYPE_ARGB      , // ABGR interleaved: A,R,G,B 32bpp
  FRAME_TYPE_RGB       , // RGB interleaved :   R,G,B 24bpp
  FRAME_TYPE_XOR       , // xor of the previous frame: R, G, B
  FRAME_TYPE_YUV444P   , // YUV444 planar
  FRAME_TYPE_YUV420P   , // YUV420 12bpp
  FRAME_TYPE_ARGB10    , // rgb 10 bit packed, a2 b10 r10
  FRAME_TYPE_MAX       , // sentinel value
} FrameType;

struct KVMGFXHeader
{
  char      magic[sizeof(KVMGFX_HEADER_MAGIC)];
  uint32_t  version;     // version of this structure
  uint16_t  hostID;      // the host ivshmem client id
  uint16_t  guestID;     // the guest ivshmem client id
  FrameType frameType;   // the frame type
  uint32_t  width;       // the width
  uint32_t  height;      // the height
  uint32_t  stride;      // the row stride
  int32_t   mouseX;      // the initial mouse X position
  int32_t   mouseY;      // the initial mouse Y position
  uint64_t  dataLen;     // total lengh of the data after this header
  uint64_t  dataPos;     // offset to the frame
};

#pragma pack(push,1)
struct RLEHeader
{
  uint8_t  magic[3];
  uint16_t length;
};
#pragma pack(pop)