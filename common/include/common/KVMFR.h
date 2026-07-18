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

#ifndef _H_LG_COMMON_KVMFR_
#define _H_LG_COMMON_KVMFR_

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "types.h"

#define KVMFR_MAGIC   "KVMFR---"
#define KVMFR_VERSION 23

// Fallback used by producers that cannot report the source display's SDR
// white level. IDD frames override this with IDDCX_METADATA2::SdrWhiteLevel.
#define KVMFR_SDR_WHITE_LEVEL_DEFAULT 203

#define KVMFR_MAX_DAMAGE_RECTS 64

#define LGMP_Q_POINTER     1
#define LGMP_Q_FRAME       2

#define LGMP_Q_FRAME_LEN   2
#define LGMP_Q_POINTER_LEN 32


#ifdef _MSC_VER
 // don't warn on zero length arrays
#pragma warning(push)
#pragma warning(disable: 4200)
#endif

enum
{
  CURSOR_FLAG_POSITION        = 0x1,
  CURSOR_FLAG_VISIBLE         = 0x2,
  CURSOR_FLAG_SHAPE           = 0x4,
  CURSOR_FLAG_COLOR_TRANSFORM = 0x8,
  // CURSOR_FLAG_VISIBLE contains a new visibility state. This distinguishes
  // an invisible cursor update from messages carrying unrelated cursor data.
  CURSOR_FLAG_VISIBLE_VALID   = 0x10
};

typedef uint32_t KVMFRCursorFlags;

enum
{
  KVMFR_FEATURE_SETCURSORPOS = 0x1,
  KVMFR_FEATURE_WINDOWSIZE   = 0x2
};

typedef uint32_t KVMFRFeatureFlags;

enum
{
  KVMFR_MESSAGE_SETCURSORPOS,
  KVMFR_MESSAGE_WINDOWSIZE
};

typedef uint32_t KVMFRMessageType;

typedef struct KVMFR
{
  char              magic[8];
  uint32_t          version;
  char              hostver[32];
  KVMFRFeatureFlags features;
  //KVMFRRecords start here if there are any
}
KVMFR;

typedef struct KVMFRRecord
{
  uint8_t  type;
  uint32_t size;
  uint8_t  data[];
}
KVMFRRecord;

enum
{
  KVMFR_RECORD_VMINFO = 1,
  KVMFR_RECORD_OSINFO
};

typedef enum
{
  KVMFR_OS_LINUX,
  KVMFR_OS_BSD,
  KVMFR_OS_OSX,
  KVMFR_OS_WINDOWS,
  KVMFR_OS_OTHER
}
KVMFROS;

typedef struct KVMFRRecord_VMInfo
{
  uint8_t uuid   [16]; // the guest's UUID
  char    capture[32]; // the capture device in use
  uint8_t cpus;        // number of CPUs
  uint8_t cores;       // number of CPU cores
  uint8_t sockets;     // number of CPU sockets
  char    model[];
}
KVMFRRecord_VMInfo;

typedef struct KVMFRRecord_OSInfo
{
  uint8_t os;     // KVMFR_OS_*
  char    name[]; // friendly name
}
KVMFRRecord_OSInfo;

typedef struct KVMFRCursor
{
  int16_t    x, y;        // cursor x & y position
  CursorType type;        // shape buffer data type
  int8_t     hx, hy;      // shape hotspot x & y
  uint32_t   width;       // width of the shape
  uint32_t   height;      // height of the shape
  uint32_t   pitch;       // row length in bytes of the shape
  uint32_t   sdrWhiteLevel; // cursor white level in nits for HDR composition
}
KVMFRCursor;

enum
{
  KVMFR_COLOR_TRANSFORM_MATRIX = 0x1,
  KVMFR_COLOR_TRANSFORM_LUT    = 0x2,
};

typedef uint32_t KVMFRColorTransformFlags;

// Optional payload appended to KVMFRCursor when
// CURSOR_FLAG_COLOR_TRANSFORM is present. The matrix is an XYZ-to-XYZ
// adjustment; the LUT is applied after encoding to the active wire transfer
// function. Four LUT components keep the payload directly uploadable as an
// RGBA32F texture, with alpha reserved and set to 1.0.
typedef struct KVMFRColorTransform
{
  KVMFRColorTransformFlags flags;
  float matrix[3][4];
  float scalar;
  float lut[4096][4];
}
KVMFRColorTransform;

enum
{
  FRAME_FLAG_BLOCK_SCREENSAVER  = 0x1 ,
  FRAME_FLAG_REQUEST_ACTIVATION = 0x2 ,
  FRAME_FLAG_TRUNCATED          = 0x4 , // ivshmem was too small for the frame
  FRAME_FLAG_HDR                = 0x8 , // RGBA10 may not be HDR
  FRAME_FLAG_HDR_PQ             = 0x10, // HDR PQ has been applied to the frame
  FRAME_FLAG_HDR_METADATA       = 0x20  // HDR static metadata fields are valid
};

typedef uint32_t KVMFRFrameFlags;

typedef struct KVMFRFrame
{
  uint32_t        formatVer;          // the frame format version number
  uint32_t        frameSerial;        // the unique frame number
  FrameType       type;               // the frame data type
  uint32_t        screenWidth;        // the client's screen width
  uint32_t        screenHeight;       // the client's screen height
  uint32_t        dataWidth;          // the packed frame width
  uint32_t        dataHeight;         // the packed frame height
  uint32_t        frameWidth;         // the unpacked frame width
  uint32_t        frameHeight;        // the unpacked frame height
  FrameRotation   rotation;           // the frame rotation
  uint32_t        stride;             // the row stride (zero if compressed data)
  uint32_t        pitch;              // the row pitch  (stride in bytes or the compressed frame size)
  uint32_t        offset;             // offset from the start of this header to the FrameBuffer header
  uint32_t        damageRectsCount;   // the number of damage rectangles (zero for full-frame damage)
  FrameDamageRect damageRects[KVMFR_MAX_DAMAGE_RECTS];
  KVMFRFrameFlags flags;              // bit field combination of FRAME_FLAG_*

  // HDR static metadata (valid when FRAME_FLAG_HDR_METADATA is set)
  // Display color primaries in 0.00002 units (SMPTE ST 2086 format)
  uint16_t hdrDisplayPrimary[3][2];      // Rx,Ry, Gx,Gy, Bx,By
  uint16_t hdrWhitePoint[2];             // Wx, Wy
  // Mastering display luminances follow SMPTE ST 2086 units: the maximum is
  // in whole cd/m², the minimum in 0.0001 cd/m². (Note: the DXGI docs
  // describe MaxMasteringLuminance as 0.0001 cd/m², but IddCx/ST 2086 provide
  // it in whole cd/m².)
  uint32_t hdrMaxDisplayLuminance;       // Max mastering display luminance (cd/m²)
  uint32_t hdrMinDisplayLuminance;       // Min mastering display luminance (0.0001 cd/m²)
  uint32_t hdrMaxContentLightLevel;      // MaxCLL (cd/m²)
  uint32_t hdrMaxFrameAverageLightLevel; // MaxFALL (cd/m²)

  // White level in nits for SDR content composited into an HDR frame, such as
  // the hardware cursor. Valid for all frames; SDR modes normally report 80.
  uint32_t sdrWhiteLevel;
}
KVMFRFrame;

typedef struct KVMFRMessage
{
  KVMFRMessageType type;
}
KVMFRMessage;

typedef struct KVMFRSetCursorPos
{
  KVMFRMessage msg;
  int32_t x, y;
}
KVMFRSetCursorPos;

typedef struct KVMFRWindowSize
{
  KVMFRMessage msg;
  uint32_t w, h;
}
KVMFRWindowSize;

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
