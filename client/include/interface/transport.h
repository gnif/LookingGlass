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

#ifndef _H_LG_CLIENT_TRANSPORT_
#define _H_LG_CLIENT_TRANSPORT_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/framebuffer.h"
#include "common/types.h"
#include "interface/audio.h"
#include "interface/clipboard.h"
#include "interface/input.h"

#define LG_TRANSPORT_MAX_DAMAGE_RECTS LG_MAX_FRAME_DAMAGE_RECTS

typedef struct LG_Transport LG_Transport;
typedef struct LG_RendererInterop LG_RendererInterop;

typedef enum LG_TransportStatus
{
  LG_TRANSPORT_OK,
  LG_TRANSPORT_TIMEOUT,
  LG_TRANSPORT_UNAVAILABLE,
  LG_TRANSPORT_INVALID_VERSION,
  LG_TRANSPORT_DISCONNECTED,
  LG_TRANSPORT_END,
  LG_TRANSPORT_ERROR,
}
LG_TransportStatus;

typedef enum LG_TransportGuestOS
{
  LG_TRANSPORT_OS_LINUX,
  LG_TRANSPORT_OS_BSD,
  LG_TRANSPORT_OS_OSX,
  LG_TRANSPORT_OS_WINDOWS,
  LG_TRANSPORT_OS_OTHER,
}
LG_TransportGuestOS;

enum
{
  LG_TRANSPORT_FEATURE_SET_CURSOR_POS = 0x1,
  LG_TRANSPORT_FEATURE_WINDOW_SIZE    = 0x2,
  LG_TRANSPORT_FEATURE_FRAME_SCHEDULE = 0x4,
  LG_TRANSPORT_FEATURE_INPUT          = 0x8,
};

typedef uint32_t LG_TransportFeatureFlags;

typedef struct LG_TransportSession
{
  char version[32];
  LG_TransportFeatureFlags features;

  bool uuidValid;
  uint8_t uuid[16];

  LG_TransportGuestOS os;
  char osName[64];
  char capture[32];
  char cpuModel[256];
  uint8_t cpus;
  uint8_t cores;
  uint8_t sockets;

  uint32_t remoteVersion;
}
LG_TransportSession;

enum
{
  LG_TRANSPORT_FRAME_BLOCK_SCREENSAVER  = 0x1,
  LG_TRANSPORT_FRAME_REQUEST_ACTIVATION = 0x2,
  LG_TRANSPORT_FRAME_TRUNCATED          = 0x4,
};

typedef uint32_t LG_TransportFrameFlags;

typedef struct LG_TransportFrameTiming
{
  bool     valid; /* producer fields below are available and coherent */
  bool     phaseValid;
  uint32_t scheduleGeneration;
  uint32_t scheduleEpoch;
  uint32_t scheduleDeadlineSerial;
  uint64_t captureTime;
  uint64_t postProcessTime;
  uint64_t copyTime;
  uint64_t readyTime;
  uint64_t holdTime;
  uint64_t readyLeadTime;
}
LG_TransportFrameTiming;

typedef struct LG_TransportFrameFormat
{
  uint32_t version;
  FrameType type;
  uint32_t screenWidth;
  uint32_t screenHeight;
  uint32_t dataWidth;
  uint32_t dataHeight;
  uint32_t frameWidth;
  uint32_t frameHeight;
  FrameRotation rotation;
  uint32_t stride;
  uint32_t pitch;

  bool hdr;
  bool hdrPQ;
  bool hdrMetadata;
  uint16_t hdrDisplayPrimary[3][2];
  uint16_t hdrWhitePoint[2];
  uint32_t hdrMaxDisplayLuminance;
  uint32_t hdrMinDisplayLuminance;
  uint32_t hdrMaxContentLightLevel;
  uint32_t hdrMaxFrameAverageLightLevel;
  uint32_t sdrWhiteLevel;
}
LG_TransportFrameFormat;

typedef struct LG_TransportFrame
{
  uint64_t serial;
  uint64_t timestamp;
  uint32_t scheduleEpoch;
  uint32_t scheduleDeadlineSerial;
  LG_TransportFrameFlags flags;
  bool scheduleOwner;
  // Backend-owned immutable metadata, valid until releaseFrame.
  const LG_TransportFrameFormat * format;

  const FrameBuffer * framebuffer;
  int dmaFD;
  const FrameDamageRect * damageRects;
  uint32_t damageRectsCount;

  /* A transport may keep the frame payload alive asynchronously after
   * onFrame returns. The callback is idempotent and releases that ownership. */
  LG_FrameReleaseFn releaseFn;
  void            * releaseOpaque;
  uint64_t          releaseHandle;
}
LG_TransportFrame;

enum
{
  LG_TRANSPORT_POINTER_POSITION        = 0x1,
  LG_TRANSPORT_POINTER_VISIBLE         = 0x2,
  LG_TRANSPORT_POINTER_SHAPE           = 0x4,
  LG_TRANSPORT_POINTER_COLOR_TRANSFORM = 0x8,
  LG_TRANSPORT_POINTER_VISIBLE_VALID   = 0x10,
};

typedef uint32_t LG_TransportPointerFlags;

typedef struct LG_TransportPointer
{
  LG_TransportPointerFlags flags;
  int16_t x;
  int16_t y;
  CursorType type;
  int8_t hx;
  int8_t hy;
  uint32_t width;
  uint32_t height;
  uint32_t pitch;
  uint32_t sdrWhiteLevel;
  const uint8_t * shape;
  const LGColorTransform * colorTransform;
}
LG_TransportPointer;

typedef enum LG_TransportControlType
{
  LG_TRANSPORT_CONTROL_SET_CURSOR_POS,
  LG_TRANSPORT_CONTROL_WINDOW_SIZE,
  LG_TRANSPORT_CONTROL_FRAME_SCHEDULE,
}
LG_TransportControlType;

enum
{
  LG_TRANSPORT_FRAME_SCHEDULE_ACTIVE    = 0x1,
  LG_TRANSPORT_FRAME_SCHEDULE_RELEASE   = 0x2,
  LG_TRANSPORT_FRAME_SCHEDULE_RESET     = 0x4,
  LG_TRANSPORT_FRAME_SCHEDULE_IMMEDIATE = 0x8,
};

typedef uint32_t LG_TransportFrameScheduleFlags;

typedef struct LG_TransportControl
{
  LG_TransportControlType type;
  union
  {
    struct { int32_t x, y; } cursorPos;
    struct { uint32_t width, height; } windowSize;
    struct
    {
      uint32_t                       generation;
      LG_TransportFrameScheduleFlags flags;
      uint64_t                       period;
      uint64_t                       targetSlack;
      int64_t                        phaseError;
      uint32_t                       feedbackFrameSerial;
      uint32_t                       feedbackScheduleEpoch;
      uint32_t                       feedbackDeadlineSerial;
      uint32_t                       lease;
    }
    frameSchedule;
  };
}
LG_TransportControl;

typedef uint64_t LG_TransportControlToken;

typedef struct LG_TransportOps
{
  const char * name;
  void (*setup)(void);
  bool (*create)(LG_Transport ** transport);
  void (*destroy)(LG_Transport ** transport);

  LG_TransportStatus (*connect)(LG_Transport * transport,
      LG_TransportSession * session);
  void (*disconnect)(LG_Transport * transport);
  bool (*sessionValid)(LG_Transport * transport);
  bool (*supportsDMA)(LG_Transport * transport);
  /* Queried after connect. The returned operations and opaque value remain
   * valid until disconnect; NULL indicates that this session has no input. */
  const LG_InputOps *(*getInputOps)(LG_Transport * transport,
      void ** opaque);
  /* Queried after connect. The returned operations and opaque value remain
   * valid until disconnect; NULL indicates that this session has no audio. */
  const LG_AudioOps *(*getAudioOps)(LG_Transport * transport,
      void ** opaque);
  /* Queried after connect. The returned operations and opaque value remain
   * valid until disconnect; NULL indicates that this session has no
   * clipboard. */
  const LG_ClipboardOps *(*getClipboardOps)(LG_Transport * transport,
      void ** opaque);
  bool (*attachRenderer)(LG_Transport * transport,
      const LG_RendererInterop * interop);
  void (*detachRenderer)(LG_Transport * transport);

  LG_TransportStatus (*nextFrame)(LG_Transport * transport, bool useDMA,
      LG_TransportFrame * frame);
  /* Read producer timings after the renderer has consumed the frame. Some
   * transports publish the frame while its asynchronous copy is in progress,
   * so these values are intentionally sampled late. */
  void (*getFrameTiming)(LG_Transport * transport,
      const LG_TransportFrame * frame, LG_TransportFrameTiming * timing);
  void (*releaseFrame)(LG_Transport * transport, LG_TransportFrame * frame);
  /* Called by the frame consumer as it exits. A backend may release transient
   * stream resources; nextFrame must reacquire them when the consumer restarts. */
  void (*stopFrame)(LG_Transport * transport);

  LG_TransportStatus (*nextPointer)(LG_Transport * transport,
      LG_TransportPointer * pointer);
  void (*releasePointer)(LG_Transport * transport,
      LG_TransportPointer * pointer);
  /* Called by the pointer consumer as it exits. A backend may release transient
   * stream resources; nextPointer must reacquire them when the consumer restarts. */
  void (*stopPointer)(LG_Transport * transport);

  LG_TransportStatus (*sendControl)(LG_Transport * transport,
      const LG_TransportControl * control, LG_TransportControlToken * token);
  LG_TransportStatus (*controlStatus)(LG_Transport * transport,
      LG_TransportControlToken token);
}
LG_TransportOps;

void lgTransport_setup(void);
bool lgTransport_isValid(const char * name);
bool lgTransport_create(const char * name, LG_Transport ** transport,
    const LG_TransportOps ** ops);

#endif
