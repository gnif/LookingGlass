#pragma once

#include <tmmintrin.h>
#include <smmintrin.h>
#include <immintrin.h>

#include "types.h"
#include "ttc.h"
#include "ttc_types.h"
#include "ttc_base.h"

typedef enum TranscodeMode {
  TRANSCODE_NONE,
  TRANSCODE_RGB,
  TRANSCODE_DXT1,
  TRANSCODE_DXT5,
  TRANSCODE_ETC2_RGB,
  TRANSCODE_ETC2_RGBA,
  TRANSCODE_MAX
} TranscodeMode;

typedef struct TexConvInfo {
  uint32_t  width;
  uint32_t  height;
  FrameType type;
  void      * ptr;
} TexConvInfo;

static inline size_t getTexConvSize(TexConvInfo * info)
{
  switch (info->type)
  {
    case FRAME_TYPE_RGB:
      return info->width * info->height * 3;
    case FRAME_TYPE_RGBA:
    case FRAME_TYPE_BGRA:
    case FRAME_TYPE_RGBA10:
      return info->width * info->height * 4;
    case FRAME_TYPE_DXT1:
    case FRAME_TYPE_ETC2:
      return info->width * info->height / 2;
    case FRAME_TYPE_DXT5:
    case FRAME_TYPE_ETC2_EAC:
      return info->width * info->height;
    case FRAME_TYPE_RGBA16F:
      return info->width * info->height * 8;
    default:
      return 0;
  }
}

/**
 * Converts a capture format enum into frame type enum.
 * Elements with no converted types return -1.
 */
static inline int captureFormatToFrameFormat(CaptureFormat fmt)
{
  switch (fmt)
  {
    case CAPTURE_FMT_RGB:
      return FRAME_TYPE_RGB;
    case CAPTURE_FMT_BGRA:
      return FRAME_TYPE_BGRA;
    case CAPTURE_FMT_RGBA:
      return FRAME_TYPE_RGBA;
    case CAPTURE_FMT_RGBA10:
      return FRAME_TYPE_RGBA10;
    case CAPTURE_FMT_RGBA16F:
      return FRAME_TYPE_RGBA16F;
    case CAPTURE_FMT_DXT1:
      return FRAME_TYPE_DXT1;
    case CAPTURE_FMT_DXT5:
      return FRAME_TYPE_DXT5;
    case CAPTURE_FMT_ETC2:
      return FRAME_TYPE_ETC2;
    case CAPTURE_FMT_ETC2_EAC:
      return FRAME_TYPE_ETC2_EAC;
    default:
      return -1;
  }
}

/**
 * Converts a frame type enum into ttc Format enum.
 * Elements with no converted types return TTC_FMT_INVALID.
 */
static inline int frameTypeToTtcFormat(FrameType fmt)
{
  switch (fmt)
  {
    case FRAME_TYPE_RGB:
      return TTC_FMT_RGB;
    case FRAME_TYPE_BGRA:
      return TTC_FMT_BGRA;
    case FRAME_TYPE_RGBA:
      return TTC_FMT_RGBA;
    case FRAME_TYPE_DXT1:
      return TTC_FMT_DXT1;
    case FRAME_TYPE_DXT5:
      return TTC_FMT_DXT5;
    case FRAME_TYPE_ETC2:
      return TTC_FMT_ETC2;
    case FRAME_TYPE_ETC2_EAC:
      return TTC_FMT_ETC2_EAC;
    default:
      return TTC_FMT_INVALID;
  }
}

/**
 * Converts a capture format enum into ttc Format enum.
 * Elements with no converted types return TTC_FMT_INVALID.
 */
static inline int captureFormatToTtcFormat(FrameType fmt)
{
  switch (fmt)
  {
    case CAPTURE_FMT_RGB:
      return TTC_FMT_RGB;
    case CAPTURE_FMT_BGRA:
      return TTC_FMT_BGRA;
    case CAPTURE_FMT_RGBA:
      return TTC_FMT_RGBA;
    case CAPTURE_FMT_DXT1:
      return TTC_FMT_DXT1;
    case CAPTURE_FMT_DXT5:
      return TTC_FMT_DXT5;
    case CAPTURE_FMT_ETC2:
      return TTC_FMT_ETC2;
    case CAPTURE_FMT_ETC2_EAC:
      return TTC_FMT_ETC2_EAC;
    default:
      return TTC_FMT_INVALID;
  }
}

// Source buffer is writable and may be used as scratch space
#define XC_SRC_BUF_WRITABLE (1)

static inline void setTexConvParam(CaptureFrame * frame)
{
  if (frame->transcoded.type != FRAME_TYPE_INVALID)
  {
    switch (frame->transcoded.type)
    {
      case  FRAME_TYPE_RGB:
        frame->transcoded.stride = 3;
        frame->transcoded.pitch = frame->transcoded.width * frame->transcoded.stride;
        break;
      case  FRAME_TYPE_RGBA:
      case  FRAME_TYPE_BGRA:
      case  FRAME_TYPE_RGBA10:
        frame->transcoded.stride  = 4;
        frame->transcoded.pitch = frame->transcoded.width  * frame->transcoded.stride;
        break;
      case  FRAME_TYPE_RGBA16F:
        frame->transcoded.stride = 8;
        frame->transcoded.pitch = frame->transcoded.width  * frame->transcoded.stride;
        break;
      case  FRAME_TYPE_DXT1:
      case  FRAME_TYPE_DXT5:
      case  FRAME_TYPE_ETC2:
      case  FRAME_TYPE_ETC2_EAC:
        TexConvInfo info;
        info.width    = frame->frameWidth;
        info.height   = frame->frameHeight;
        info.type     = captureFormatToFrameFormat(frame->format);
        frame->pitch  = getTexConvSize(&info);
        frame->stride = 0;
        break;
      default:
        break;
    }
  }
}



