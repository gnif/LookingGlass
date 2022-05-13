/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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
#include "common/framebuffer.h"
#include "common/KVMFR.h"

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
  CAPTURE_FMT_BGRA      ,
  CAPTURE_FMT_RGBA      ,
  CAPTURE_FMT_RGBA10    ,
  CAPTURE_FMT_RGBA16F   ,
  CAPTURE_FMT_RGB       ,
  CAPTURE_FMT_DXT1      ,
  CAPTURE_FMT_DXT5      ,
  CAPTURE_FMT_ETC2      ,
  CAPTURE_FMT_ETC2_EAC  ,
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

typedef struct TranscodeFrame
{
  unsigned int    width;
  unsigned int    height;
  unsigned int    pitch;
  unsigned int    stride;
  FrameType       type; // No transcode requested => FRAME_TYPE_INVALID
  CaptureRotation rotation;
} TranscodeFrame;

typedef struct CaptureFrame
{
  unsigned int    formatVer;
  unsigned int    screenWidth;
  unsigned int    screenHeight;
  unsigned int    frameWidth;
  unsigned int    frameHeight;
  bool            truncated;
  unsigned int    pitch;
  unsigned int    stride;
  CaptureFormat   format;
  CaptureRotation rotation;
  TranscodeFrame  transcoded;
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
  unsigned int  hx, hy;
  unsigned int  width, height;
  unsigned int  pitch;
}
CapturePointer;

typedef bool (*CaptureGetPointerBuffer )(void ** data, uint32_t * size);
typedef void (*CapturePostPointerBuffer)(CapturePointer pointer);

typedef struct CaptureInterface
{
  const char * shortName;
  const bool   asyncCapture;
  const char * (*getName        )(void);
  void         (*initOptions    )(void);

  bool(*create)(
    CaptureGetPointerBuffer  getPointerBufferFn,
    CapturePostPointerBuffer postPointerBufferFn
  );

  bool          (*init         )(void);
  bool          (*start        )(void);
  void          (*stop         )(void);
  bool          (*deinit       )(void);
  void          (*free         )(void);

  CaptureResult (*capture   )(void);
  CaptureResult (*waitFrame )(CaptureFrame * frame, const size_t maxFrameSize);
  CaptureResult (*getFrame  )(FrameBuffer  * frame, const unsigned int height, int frameIndex);
}
CaptureInterface;
