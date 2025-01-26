/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#include <stdbool.h>
#include <stdint.h>
#include "common/KVMFR.h"

#ifdef __cplusplus
/* using common/framebuffer.h breaks compatibillity with C++ due to it's usage
 * of stdatomic.h, so we need to forward declare the structure here */
typedef struct stFrameBuffer FrameBuffer;
#else
#include "common/framebuffer.h"
#endif

typedef enum CaptureResult
{
  CAPTURE_RESULT_OK     ,
  CAPTURE_RESULT_REINIT ,
  CAPTURE_RESULT_TIMEOUT,
  CAPTURE_RESULT_ERROR
}
CaptureResult;

typedef enum CaptureFormat
{
  // frame formats
  CAPTURE_FMT_BGRA   ,
  CAPTURE_FMT_RGBA   ,
  CAPTURE_FMT_RGBA10 ,
  CAPTURE_FMT_RGBA16F,
  CAPTURE_FMT_BGR_32 ,
  CAPTURE_FMT_RGB_24 ,

  // pointer formats
  CAPTURE_FMT_COLOR ,
  CAPTURE_FMT_MONO  ,
  CAPTURE_FMT_MASKED,

  CAPTURE_FMT_MAX
}
CaptureFormat;

typedef enum CaptureRotation
{
  CAPTURE_ROT_0,
  CAPTURE_ROT_90,
  CAPTURE_ROT_180,
  CAPTURE_ROT_270
}
CaptureRotation;

typedef struct CaptureFrame
{
  unsigned        formatVer;
  unsigned        screenWidth;   // actual screen width
  unsigned        screenHeight;  // actual screen height
  unsigned        dataWidth;     // the width of the packed frame data
  unsigned        dataHeight;    // the height of the packed frame data
  unsigned        frameWidth;    // width of the frame image
  unsigned        frameHeight;   // height of the frame image
  unsigned        pitch;         // total width of one row of data in bytes
  unsigned        stride;        // total width of one row of data in pixels
  CaptureFormat   format;        // the data format of the frame
  bool            truncated;     // true if the frame data is truncated
  bool            hdr;           // true if the frame format is HDR
  bool            hdrPQ;         // true if the frame format is PQ transformed
  CaptureRotation rotation;      // output rotation of the frame
  ColorMetadata   colorMetadata; // display color metadata (mainly for HDR)

  uint32_t        damageRectsCount;
  FrameDamageRect damageRects[KVMFR_MAX_DAMAGE_RECTS];
}
CaptureFrame;

typedef struct CapturePointer
{
  bool          positionUpdate;
  int           x, y;
  bool          visible;

  bool          shapeUpdate;
  CaptureFormat format;
  unsigned      hx, hy;
  unsigned      width, height;
  unsigned      pitch;
}
CapturePointer;

typedef bool (*CaptureGetPointerBuffer )(void ** data, uint32_t * size);
typedef void (*CapturePostPointerBuffer)(const CapturePointer * pointer);

typedef struct CaptureInterface
{
  const char * shortName;
  const bool   asyncCapture;
  const bool   deprecated;

  const char * (*getName        )(void);
  void         (*initOptions    )(void);

  bool(*create)(
    CaptureGetPointerBuffer  getPointerBufferFn,
    CapturePostPointerBuffer postPointerBufferFn,
    unsigned                 frameBuffers
  );

  bool          (*init         )(void * ivshmemBase, unsigned * alignSize);
  bool          (*start        )(void);
  void          (*stop         )(void);
  bool          (*deinit       )(void);
  void          (*free         )(void);

  CaptureResult (*capture   )(
    unsigned frameBufferIndex,
    FrameBuffer * frame);
  CaptureResult (*waitFrame )(
    unsigned frameBufferIndex,
    CaptureFrame * frame,
    const size_t maxFrameSize);
  CaptureResult (*getFrame  )(
    unsigned frameBufferIndex,
    FrameBuffer  * frame,
    const size_t maxFrameSize);
}
CaptureInterface;
