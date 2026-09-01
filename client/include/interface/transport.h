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

#include <limits.h>
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

typedef bool (*LG_TransportCancelledFn)(void * opaque);

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
  LG_TRANSPORT_FEATURE_CLIPBOARD      = 0x10,
};

typedef uint32_t LG_TransportFeatureFlags;

typedef struct LG_VersionMismatch
{
  bool     valid;
  char     component[16];
  uint32_t expectedVersion;
  uint32_t currentVersion;
}
LG_VersionMismatch;

#define LG_RECOVERY_MAX_VERSIONS 4

enum
{
  LG_RECOVERY_CAP_DISPLAY = 0x1,
};

typedef uint32_t LG_RecoveryCaps;

typedef enum LG_RecoveryRequest
{
  LG_RECOVERY_REQ_NONE,
  LG_RECOVERY_REQ_NORMAL,
  LG_RECOVERY_REQ_RECOVERY,
}
LG_RecoveryRequest;

typedef enum LG_RecoveryState
{
  LG_RECOVERY_STATE_UNKNOWN,
  LG_RECOVERY_STATE_NORMAL,
  LG_RECOVERY_STATE_SWITCHING,
  LG_RECOVERY_STATE_ACTIVE,
  LG_RECOVERY_STATE_FAILED,
}
LG_RecoveryState;

typedef enum LG_RecoveryError
{
  LG_RECOVERY_ERR_NONE,
  LG_RECOVERY_ERR_UNSUPPORTED,
  LG_RECOVERY_ERR_HELPER_UNAVAILABLE,
  LG_RECOVERY_ERR_TOPOLOGY_FAILED,
  LG_RECOVERY_ERR_NO_FALLBACK_DISPLAY,
  LG_RECOVERY_ERR_BUSY,
  LG_RECOVERY_ERR_CAPACITY,
}
LG_RecoveryError;

typedef struct LG_RecoveryVersion
{
  char     component[16];
  uint32_t version;
}
LG_RecoveryVersion;

typedef struct LG_RecoveryInfo
{
  uint32_t            abiVersion;
  LG_RecoveryCaps     capabilities;
  uint64_t            instance;
  uint32_t            heartbeat;
  bool                uuidValid;
  uint8_t             uuid[16];
  char                producerVersion[64];
  uint32_t            versionCount;
  LG_RecoveryVersion  versions[LG_RECOVERY_MAX_VERSIONS];
  uint32_t            requestSerial;
  LG_RecoveryRequest  request;
  uint32_t            ackSerial;
  LG_RecoveryRequest  ackRequest;
  LG_RecoveryState    state;
  LG_RecoveryError    error;
}
LG_RecoveryInfo;

typedef struct LG_TransportSession
{
  char version[32];
  char name[256];
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

  LG_VersionMismatch versionMismatch;
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
  /* The producer fields through readyLeadTime are available and coherent. */
  bool     valid;
  /* receiveTime and prepareTime are available and coherent. */
  bool     providerValid;
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
  /* Non-overlapping work performed while acquiring and preparing the returned
   * payload. These do not include status callbacks or renderer import time. */
  uint64_t receiveTime;
  uint64_t prepareTime;
}
LG_TransportFrameTiming;

typedef struct LG_TransportFrameFormat
{
  uint32_t           version;
  KVMFRFrameType     type;
  uint32_t           screenWidth;
  uint32_t           screenHeight;
  uint32_t           dataWidth;
  uint32_t           dataHeight;
  uint32_t           frameWidth;
  uint32_t           frameHeight;
  KVMFRFrameRotation rotation;
  uint32_t           stride;
  uint32_t           pitch;

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

/*
 * Validate the common packed-frame layout used by renderers. The optional
 * dataSize result is the validated byte capacity required for the frame data.
 */
static inline bool lgTransport_validateFrameFormat(
    const LG_TransportFrameFormat * format, LG_TransportFrameFlags flags,
    size_t * dataSize)
{
  if (!format)
    return false;

  uint32_t storageBpp;
  switch (format->type)
  {
    case FRAME_TYPE_BGRA:
    case FRAME_TYPE_RGBA:
    case FRAME_TYPE_RGBA10:
    case FRAME_TYPE_BGR_32:
      storageBpp = 4;
      break;

    case FRAME_TYPE_RGBA16F:
      storageBpp = 8;
      break;

    case FRAME_TYPE_RGB_24:
      storageBpp = 3;
      break;

    default:
      return false;
  }

  switch (format->rotation)
  {
    case FRAME_ROT_0:
    case FRAME_ROT_90:
    case FRAME_ROT_180:
    case FRAME_ROT_270:
      break;

    default:
      return false;
  }

  if (!format->screenWidth || !format->screenHeight ||
      !format->dataWidth   || !format->dataHeight   ||
      !format->frameWidth  || !format->frameHeight  ||
      !format->stride      || !format->pitch        ||
      format->screenWidth > INT_MAX || format->screenHeight > INT_MAX ||
      format->dataWidth   > INT_MAX || format->dataHeight   > INT_MAX ||
      format->frameWidth  > INT_MAX || format->frameHeight  > INT_MAX ||
      format->stride      > INT_MAX || format->pitch        > INT_MAX)
    return false;

  const uint64_t rowBytes = (uint64_t)format->stride * storageBpp;
  const uint64_t size     = (uint64_t)format->dataHeight * format->pitch;
  if (format->dataWidth > format->stride || rowBytes != format->pitch ||
      size > INT_MAX || format->dataHeight > format->frameHeight)
    return false;

  if (format->type == FRAME_TYPE_BGR_32)
  {
    /* Packed BGR rows are stored in a reshaped 32-bpp surface, so its storage
     * height may be shorter than the logical frame without truncation. */
    if (format->dataWidth != format->stride ||
        (uint64_t)format->frameWidth * 3U > format->pitch)
      return false;
  }
  else
  {
    if (format->dataWidth != format->frameWidth ||
        (format->dataHeight < format->frameHeight &&
         !(flags & LG_TRANSPORT_FRAME_TRUNCATED)))
      return false;
  }

  if (dataSize)
    *dataSize = (size_t)size;
  return true;
}

static inline bool lgTransport_frameLayoutMatches(
    const LG_TransportFrameFormat * left,
    const LG_TransportFrameFormat * right)
{
  return left && right &&
    left->type         == right->type         &&
    left->screenWidth  == right->screenWidth  &&
    left->screenHeight == right->screenHeight &&
    left->dataWidth    == right->dataWidth    &&
    left->dataHeight   == right->dataHeight   &&
    left->frameWidth   == right->frameWidth   &&
    left->frameHeight  == right->frameHeight  &&
    left->rotation     == right->rotation     &&
    left->stride       == right->stride       &&
    left->pitch        == right->pitch;
}

typedef struct LG_TransportFrame
{
  uint64_t serial;
  /* When setStatusListener is present, matches LG_VideoComponentStatus::epoch
   * for the frame endpoint which returned this payload; otherwise may be 0. */
  uint64_t epoch;
  uint64_t timestamp;
  uint32_t scheduleEpoch;
  uint32_t scheduleDeadlineSerial;
  LG_TransportFrameFlags flags;
  bool scheduleOwner;
  // Backend-owned immutable metadata, valid until releaseFrame.
  const LG_TransportFrameFormat * format;

  const KVMFRFrameBuffer     * framebuffer;
  int                          dmaFD;
  const KVMFRFrameDamageRect * damageRects;
  uint32_t                     damageRectsCount;

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
  /* When setStatusListener is present, matches LG_VideoComponentStatus::epoch
   * for the pointer endpoint which returned this payload; otherwise may be 0.
   */
  uint64_t                    epoch;
  LG_TransportPointerFlags    flags;
  int16_t                     x;
  int16_t                     y;
  KVMFRCursorType             type;
  int16_t                     hx;
  int16_t                     hy;
  uint32_t                    width;
  uint32_t                    height;
  uint32_t                    pitch;
  uint32_t                    sdrWhiteLevel;
  const uint8_t             * shape;
  const KVMFRColorTransform * colorTransform;
}
LG_TransportPointer;

typedef struct LG_VideoComponentStatus
{
  bool     available;
  /* Nonzero while available and changed whenever the endpoint is replaced or
   * changes availability. */
  uint64_t epoch;
  /* LG_TRANSPORT_OK while available; otherwise the latest observed cause. */
  LG_TransportStatus reason;
}
LG_VideoComponentStatus;

typedef struct LG_VideoStatus
{
  LG_VideoComponentStatus frame;
  LG_VideoComponentStatus pointer;
}
LG_VideoStatus;

typedef void (*LG_VideoStatusFn)(void * opaque,
    const LG_VideoStatus * status);

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

typedef struct LG_FrameOps
{
  /* Registration is scoped to a connected session and must synchronously
   * report the current status after releasing component state locks. Frame and
   * pointer epochs advance independently whenever their endpoint is
   * replaced or changes availability. Passing NULL unregisters the listener
   * and synchronously quiesces its callbacks. Callbacks must be serialized.
   * Providers without a listener are assumed to keep every component they
   * expose available for the session. */
  void (*setStatusListener)(LG_Transport * transport,
      LG_VideoStatusFn callback, void * callbackOpaque);

  bool (*supportsDMA)(LG_Transport * transport);
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
  /* Required, thread-safe cancellation of a blocking nextFrame call. This
   * does not release a frame already returned to the consumer. */
  void (*cancelFrameWait)(LG_Transport * transport);
  /* Called by the frame consumer as it exits. A backend may release transient
   * stream resources; nextFrame must reacquire them when the consumer
   * restarts. */
  void (*stopFrame)(LG_Transport * transport);

  /* Pointer operations are optional. When absent, pointer.available must be
   * false for providers implementing the status listener. */
  LG_TransportStatus (*nextPointer)(LG_Transport * transport,
      LG_TransportPointer * pointer);
  void (*releasePointer)(LG_Transport * transport,
      LG_TransportPointer * pointer);
  /* Required whenever nextPointer is present; thread-safe cancellation of a
   * blocking nextPointer call. */
  void (*cancelPointerWait)(LG_Transport * transport);
  /* Called by the pointer consumer as it exits. A backend may release
   * transient stream resources; nextPointer must reacquire them when the
   * consumer restarts. */
  void (*stopPointer)(LG_Transport * transport);
}
LG_FrameOps;

typedef struct LG_SwSurfaceEventOps
{
  void (*configure)(void * opaque, unsigned int width, unsigned int height);
  void (*destroy)(void * opaque);
  void (*drawFill)(void * opaque, int x, int y, int width, int height,
      uint32_t color);
  /* Bitmap data is BGRA32 and remains valid only for the callback. */
  void (*drawBitmap)(void * opaque, bool topDown, int x, int y,
      int width, int height, int stride, const void * data);
  /* Pointer payloads remain valid only for the callback. */
  void (*pointer)(void * opaque, const LG_TransportPointer * pointer);
}
LG_SwSurfaceEventOps;

typedef struct LG_SwSurfaceOps
{
  /* detach synchronously quiesces all event callbacks. */
  bool (*attach)(LG_Transport * transport,
      const LG_SwSurfaceEventOps * events, void * opaque);
  void (*detach)(LG_Transport * transport);
  /* Activation may wait while the requested component becomes available.
   * Deactivation must complete promptly without cancellation. */
  bool (*setActive)(LG_Transport * transport, bool active);
  /* Required. Interrupts a pending setActive call and causes it to return
   * promptly. Safe to invoke repeatedly from another thread. */
  void (*cancelPending)(LG_Transport * transport);
}
LG_SwSurfaceOps;

typedef enum LG_VideoType
{
  LG_VIDEO_TYPE_FRAME,
  LG_VIDEO_TYPE_SW_SURFACE,
}
LG_VideoType;

typedef struct LG_VideoOps
{
  const char       * name;
  LG_VideoType       type;
  union
  {
    const LG_FrameOps     * frame;
    const LG_SwSurfaceOps * swSurface;
  };
}
LG_VideoOps;

typedef struct LG_TransportOps
{
  const char * name;
  void (*setup)(void);
  bool (*create)(LG_Transport ** transport);
  void (*destroy)(LG_Transport ** transport);

  LG_TransportStatus (*connect)(LG_Transport * transport,
      LG_TransportSession * session);
  /* Required session entry point. The callback must be observed promptly
   * while establishing a session, and cancellation must bound this call. */
  LG_TransportStatus (*connectCancellable)(LG_Transport * transport,
      LG_TransportSession * session, LG_TransportCancelledFn cancelled,
      void * opaque);
  void (*disconnect)(LG_Transport * transport);
  bool (*sessionValid)(LG_Transport * transport);
  /* Queried after create. The returned operations remain valid until the
   * transport is destroyed. NULL indicates that this transport has no video. */
  const LG_VideoOps *(*getVideoOps)(LG_Transport * transport);
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

  /* Recovery operations are independent of a transport session and may be
   * used after create whenever the backing transport advertises them. */
  LG_TransportStatus (*getRecoveryInfo)(LG_Transport * transport,
      LG_RecoveryInfo * info);
  LG_TransportStatus (*requestRecovery)(LG_Transport * transport,
      LG_RecoveryRequest request, uint64_t * instance, uint32_t * serial);

  LG_TransportStatus (*sendControl)(LG_Transport * transport,
      const LG_TransportControl * control, LG_TransportControlToken * token);
  LG_TransportStatus (*controlStatus)(LG_Transport * transport,
      LG_TransportControlToken token);
}
LG_TransportOps;

typedef struct LG_TransportInstance
{
  LG_Transport          * handle;
  const LG_TransportOps * ops;
}
LG_TransportInstance;

void lgTransport_setup(void);
bool lgTransport_isValid(const char * name);
bool lgTransport_create(const char * name, LG_TransportInstance * instance);
void lgTransport_destroy(LG_TransportInstance * instance);

#endif
