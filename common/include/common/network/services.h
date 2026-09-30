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

#ifndef _H_LG_COMMON_NETWORK_SERVICES_
#define _H_LG_COMMON_NETWORK_SERVICES_

#include "common/network/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Variable-length decoders return borrowed views into their input buffer. The
 * caller must keep that buffer alive while using the decoded payload. */

/* Fixed boundaries must not move when a service's current version changes. */
#define LG_NET_VIDEO_PYROWAVE_INTRODUCED_SERVICE_VERSION  1U
#define LG_NET_CURSOR_SHAPE_INTRODUCED_SERVICE_VERSION    1U
#define LG_NET_INPUT_LEDS_INTRODUCED_SERVICE_VERSION      1U
#define LG_NET_AUDIO_PCM_INTRODUCED_SERVICE_VERSION       1U
#define LG_NET_CLIPBOARD_CHUNK_INTRODUCED_SERVICE_VERSION 1U
#define LG_NET_FILE_CHUNK_INTRODUCED_SERVICE_VERSION      1U
#define LG_NET_USB_RESERVED_INTRODUCED_SERVICE_VERSION    1U

#define LG_NET_VIDEO_MAX_WIDTH                    32768U
#define LG_NET_VIDEO_MAX_HEIGHT                   32768U
#define LG_NET_VIDEO_MAX_PLANES                   4U
#define LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE      60U
#define LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE       56U
#define LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE    40U
#define LG_NET_VIDEO_FEEDBACK_WIRE_SIZE           56U
#define LG_NET_VIDEO_MAX_FRAME_LENGTH             \
  (LG_NET_MAX_PAYLOAD_LENGTH - LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE)
#define LG_NET_VIDEO_MAX_FRAGMENT_LENGTH          65535U
#define LG_NET_VIDEO_MAX_FRAGMENTS                65535U
#define LG_NET_VIDEO_MAX_BLOCKS                   65535U

#define LG_NET_CURSOR_MAX_WIDTH                   4096U
#define LG_NET_CURSOR_MAX_HEIGHT                  4096U
#define LG_NET_CURSOR_MAX_SHAPE_LENGTH            (16U * 1024U * 1024U)
#define LG_NET_CURSOR_POSITION_WIRE_SIZE          40U
#define LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE      40U

#define LG_NET_INPUT_CLAIM_WIRE_SIZE              24U
#define LG_NET_INPUT_STATUS_WIRE_SIZE             32U
#define LG_NET_INPUT_RELATIVE_WIRE_SIZE           40U
#define LG_NET_INPUT_ABSOLUTE_WIRE_SIZE           48U
#define LG_NET_INPUT_KEYBOARD_WIRE_SIZE           32U
#define LG_NET_INPUT_LEDS_WIRE_SIZE               16U
#define LG_NET_INPUT_ABSOLUTE_MAX_WIDTH           32768U
#define LG_NET_INPUT_ABSOLUTE_MAX_HEIGHT          32768U
#define LG_NET_INPUT_KEYBOARD_MODIFIER_MASK       UINT32_C(0x000000FF)

#define LG_NET_AUDIO_MAX_CHANNELS                 32U
#define LG_NET_AUDIO_MAX_SAMPLE_RATE              768000U
#define LG_NET_AUDIO_MAX_PACKET_FRAMES            16384U
#define LG_NET_AUDIO_MAX_DATA_LENGTH              (4U * 1024U * 1024U)
#define LG_NET_AUDIO_FORMAT_WIRE_SIZE             40U
#define LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE        48U
#define LG_NET_AUDIO_STATE_WIRE_SIZE              32U
#define LG_NET_AUDIO_CLOCK_FEEDBACK_WIRE_SIZE     48U

#define LG_NET_CLIPBOARD_MAX_MIME_LENGTH          255U
#define LG_NET_CLIPBOARD_MAX_FORMATS              256U
#define LG_NET_CLIPBOARD_MAX_CHUNK_LENGTH         (4U * 1024U * 1024U)
#define LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE          24U
#define LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE   40U
#define LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE        32U
#define LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE   40U
#define LG_NET_CLIPBOARD_STATUS_WIRE_SIZE         32U

#define LG_NET_FILE_MAX_LABEL_LENGTH              1024U
#define LG_NET_FILE_MAX_PATH_LENGTH               4096U
#define LG_NET_FILE_MAX_LINK_TARGET_LENGTH        4096U
#define LG_NET_FILE_MAX_ENTRIES                   1048576U
#define LG_NET_FILE_MAX_CHUNK_LENGTH              (4U * 1024U * 1024U)
#define LG_NET_FILE_OFFER_HEADER_WIRE_SIZE        32U
#define LG_NET_FILE_ENTRY_HEADER_WIRE_SIZE        56U
#define LG_NET_FILE_REQUEST_WIRE_SIZE             40U
#define LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE        48U
#define LG_NET_FILE_STATUS_WIRE_SIZE              40U

#define LG_NET_USB_RESERVED_HEADER_WIRE_SIZE      32U
#define LG_NET_USB_MAX_RESERVED_PAYLOAD_LENGTH    \
  (LG_NET_MAX_PAYLOAD_LENGTH - LG_NET_USB_RESERVED_HEADER_WIRE_SIZE)

typedef uint16_t LGNetVideoCodec;

enum
{
  LG_NET_VIDEO_CODEC_PYROWAVE = 1,
};

typedef uint16_t LGNetVideoPixelFormat;

enum
{
  LG_NET_VIDEO_PIXEL_FORMAT_NV12       = 1,
  LG_NET_VIDEO_PIXEL_FORMAT_P010       = 2,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV420P8   = 3,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV420P10  = 4,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV422P8   = 5,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV422P10  = 6,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV444P8   = 7,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV444P10  = 8,
  LG_NET_VIDEO_PIXEL_FORMAT_YUV444P16F = 9,
};

typedef uint8_t LGNetVideoChromaSubsampling;

enum
{
  LG_NET_VIDEO_CHROMA_420 = 1,
  LG_NET_VIDEO_CHROMA_422 = 2,
  LG_NET_VIDEO_CHROMA_444 = 3,
};

typedef uint8_t LGNetColorPrimaries;

enum
{
  LG_NET_COLOR_PRIMARIES_BT601  = 1,
  LG_NET_COLOR_PRIMARIES_BT709  = 2,
  LG_NET_COLOR_PRIMARIES_BT2020 = 3,
};

typedef uint8_t LGNetColorTransfer;

enum
{
  LG_NET_COLOR_TRANSFER_LINEAR = 1,
  LG_NET_COLOR_TRANSFER_SRGB   = 2,
  LG_NET_COLOR_TRANSFER_BT709  = 3,
  LG_NET_COLOR_TRANSFER_PQ     = 4,
  LG_NET_COLOR_TRANSFER_HLG    = 5,
};

typedef uint8_t LGNetColorMatrix;

enum
{
  LG_NET_COLOR_MATRIX_BT601  = 1,
  LG_NET_COLOR_MATRIX_BT709  = 2,
  LG_NET_COLOR_MATRIX_BT2020 = 3,
  LG_NET_COLOR_MATRIX_RGB    = 4,
};

typedef uint8_t LGNetColorRange;

enum
{
  LG_NET_COLOR_RANGE_LIMITED = 1,
  LG_NET_COLOR_RANGE_FULL    = 2,
};

typedef uint16_t LGNetVideoStreamFlags;

enum
{
  LG_NET_VIDEO_STREAM_HDR             = 1U << 0,
  LG_NET_VIDEO_STREAM_ALPHA           = 1U << 1,
  LG_NET_VIDEO_STREAM_GPU_PLANES_ONLY = 1U << 2,
  LG_NET_VIDEO_STREAM_DATAGRAMS       = 1U << 3,
};

typedef struct LGNetVideoStreamConfig
{
  uint32_t                    streamID;
  LGNetVideoCodec             codec;
  uint16_t                    codecVersion;
  uint64_t                    configEpoch;
  uint32_t                    width;
  uint32_t                    height;
  uint32_t                    refreshNumerator;
  uint32_t                    refreshDenominator;
  LGNetVideoPixelFormat       pixelFormat;
  LGNetVideoChromaSubsampling chromaSubsampling;
  LGNetColorPrimaries         colorPrimaries;
  LGNetColorTransfer          colorTransfer;
  LGNetColorMatrix            colorMatrix;
  LGNetColorRange             colorRange;
  uint8_t                     planeCount;
  uint8_t                     bitDepth;
  LGNetVideoStreamFlags       flags;
  uint32_t                    maxFrameLength;
  uint32_t                    maxFragmentLength;
  uint32_t                    maxFrameLatencyMs;
}
LGNetVideoStreamConfig;

typedef uint16_t LGNetVideoFrameFlags;

enum
{
  LG_NET_VIDEO_FRAME_KEYFRAME       = 1U << 0,
  LG_NET_VIDEO_FRAME_DISCONTINUITY  = 1U << 1,
  LG_NET_VIDEO_FRAME_END_OF_STREAM  = 1U << 2,
  LG_NET_VIDEO_FRAME_HAS_CHECKSUM   = 1U << 3,
};

/* checksum is CRC-32C (Castagnoli), reflected polynomial 0x82f63b78, with
 * initial and final XOR 0xffffffff, over exactly encodedLength bytes of data.
 * It is zero when LG_NET_VIDEO_FRAME_HAS_CHECKSUM is clear. */
typedef struct LGNetVideoFrame
{
  uint32_t             streamID;
  LGNetVideoCodec      codec;
  LGNetVideoFrameFlags flags;
  uint64_t             configEpoch;
  uint64_t             frameID;
  uint64_t             captureTimestampNs;
  uint64_t             presentationTimestampNs;
  uint32_t             encodedLength;
  uint16_t             blockCount;
  uint16_t             fragmentCount;
  uint32_t             deadlineMs;
  uint32_t             checksum;
  const uint8_t *      data;
}
LGNetVideoFrame;

typedef uint16_t LGNetVideoFragmentFlags;

enum
{
  LG_NET_VIDEO_FRAGMENT_BLOCK_START = 1U << 0,
  LG_NET_VIDEO_FRAGMENT_BLOCK_END   = 1U << 1,
  LG_NET_VIDEO_FRAGMENT_FRAME_END   = 1U << 2,
  LG_NET_VIDEO_FRAGMENT_RECOVERY    = 1U << 3,
};

typedef struct LGNetVideoFragment
{
  uint32_t                streamID;
  uint64_t                configEpoch;
  uint64_t                frameID;
  uint32_t                frameLength;
  uint32_t                offset;
  uint16_t                fragmentIndex;
  uint16_t                fragmentCount;
  uint16_t                blockIndex;
  uint16_t                blockCount;
  uint16_t                payloadLength;
  LGNetVideoFragmentFlags flags;
  const uint8_t *         payload;
}
LGNetVideoFragment;

typedef uint32_t LGNetVideoFeedbackFlags;

enum
{
  LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME = 1U << 0,
  LG_NET_VIDEO_FEEDBACK_CONGESTED        = 1U << 1,
  LG_NET_VIDEO_FEEDBACK_DECODER_STALLED  = 1U << 2,
};

typedef struct LGNetVideoFeedback
{
  uint32_t                streamID;
  LGNetVideoFeedbackFlags flags;
  uint64_t                lastCompleteFrameID;
  uint64_t                highestSeenFrameID;
  uint64_t                lastDecodedFrameID;
  uint32_t                lostFrames;
  uint32_t                lostFragments;
  uint32_t                receiveQueueUs;
  uint32_t                decodeQueueUs;
  uint32_t                roundTripUs;
}
LGNetVideoFeedback;

typedef uint32_t LGNetCursorPositionFlags;

enum
{
  LG_NET_CURSOR_POSITION_VISIBLE  = 1U << 0,
  LG_NET_CURSOR_POSITION_RELATIVE = 1U << 1,
};

typedef struct LGNetCursorPosition
{
  uint64_t                 updateID;
  uint64_t                 timestampNs;
  int32_t                  x;
  int32_t                  y;
  uint32_t                 desktopWidth;
  uint32_t                 desktopHeight;
  LGNetCursorPositionFlags flags;
  uint32_t                 displayID;
}
LGNetCursorPosition;

typedef uint16_t LGNetCursorFormat;

enum
{
  LG_NET_CURSOR_FORMAT_BGRA8_PREMULTIPLIED = 1,
  LG_NET_CURSOR_FORMAT_RGBA8_PREMULTIPLIED = 2,
  LG_NET_CURSOR_FORMAT_MASKED_COLOR        = 3,
  LG_NET_CURSOR_FORMAT_MONOCHROME          = 4,
};

typedef uint16_t LGNetCursorShapeFlags;

enum
{
  LG_NET_CURSOR_SHAPE_HIDDEN   = 1U << 0,
  LG_NET_CURSOR_SHAPE_ANIMATED = 1U << 1,
};

typedef struct LGNetCursorShape
{
  uint64_t              shapeID;
  uint32_t              width;
  uint32_t              height;
  uint32_t              pitch;
  uint32_t              dataLength;
  LGNetCursorFormat     format;
  LGNetCursorShapeFlags flags;
  uint32_t              hotspotX;
  uint32_t              hotspotY;
  uint32_t              frameDurationMs;
  const uint8_t *       data;
}
LGNetCursorShape;

typedef uint32_t LGNetInputClaimFlags;

enum
{
  LG_NET_INPUT_CLAIM_KEYBOARD  = 1U << 0,
  LG_NET_INPUT_CLAIM_POINTER   = 1U << 1,
  LG_NET_INPUT_CLAIM_EXCLUSIVE = 1U << 2,
};

typedef struct LGNetInputClaim
{
  uint64_t             claimantID;
  uint64_t             claimEpoch;
  uint32_t             leaseMs;
  LGNetInputClaimFlags flags;
}
LGNetInputClaim;

typedef uint32_t LGNetInputStatusCode;

enum
{
  LG_NET_INPUT_STATUS_ACCEPTED    = 1,
  LG_NET_INPUT_STATUS_REJECTED    = 2,
  LG_NET_INPUT_STATUS_RELEASED    = 3,
  LG_NET_INPUT_STATUS_EXPIRED     = 4,
  LG_NET_INPUT_STATUS_UNSUPPORTED = 5,
  LG_NET_INPUT_STATUS_ERROR       = 6,
};

typedef struct LGNetInputStatus
{
  uint64_t             claimEpoch;
  uint64_t             lastSequence;
  LGNetInputStatusCode status;
  LGNetInputClaimFlags flags;
  uint32_t             leaseRemainingMs;
  uint32_t             detail;
}
LGNetInputStatus;

typedef uint32_t LGNetPointerButtons;

enum
{
  LG_NET_POINTER_BUTTON_LEFT    = 1U << 0,
  LG_NET_POINTER_BUTTON_RIGHT   = 1U << 1,
  LG_NET_POINTER_BUTTON_MIDDLE  = 1U << 2,
  LG_NET_POINTER_BUTTON_BACK    = 1U << 3,
  LG_NET_POINTER_BUTTON_FORWARD = 1U << 4,
};

typedef struct LGNetInputRelative
{
  uint64_t            sequence;
  uint64_t            timestampNs;
  int32_t             deltaX;
  int32_t             deltaY;
  int32_t             wheelX;
  int32_t             wheelY;
  LGNetPointerButtons buttons;
  LGNetPointerButtons changedButtons;
}
LGNetInputRelative;

typedef struct LGNetInputAbsolute
{
  uint64_t            sequence;
  uint64_t            timestampNs;
  int32_t             x;
  int32_t             y;
  uint32_t            width;
  uint32_t            height;
  int32_t             wheelX;
  int32_t             wheelY;
  LGNetPointerButtons buttons;
  LGNetPointerButtons changedButtons;
}
LGNetInputAbsolute;

typedef uint16_t LGNetKeyboardFlags;

enum
{
  LG_NET_KEYBOARD_DOWN     = 1U << 0,
  LG_NET_KEYBOARD_REPEAT   = 1U << 1,
  LG_NET_KEYBOARD_EXTENDED = 1U << 2,
  LG_NET_KEYBOARD_E1       = 1U << 3,
};

/* modifiers uses the USB HID keyboard modifier byte in its low eight bits;
 * all other bits are reserved and must be zero. */
typedef struct LGNetInputKeyboard
{
  uint64_t           sequence;
  uint64_t           timestampNs;
  uint16_t           usagePage;
  uint16_t           usage;
  uint16_t           scanCode;
  LGNetKeyboardFlags flags;
  uint32_t           modifiers;
}
LGNetInputKeyboard;

typedef uint32_t LGNetKeyboardLEDs;

enum
{
  LG_NET_KEYBOARD_LED_NUM_LOCK    = 1U << 0,
  LG_NET_KEYBOARD_LED_CAPS_LOCK   = 1U << 1,
  LG_NET_KEYBOARD_LED_SCROLL_LOCK = 1U << 2,
  LG_NET_KEYBOARD_LED_COMPOSE     = 1U << 3,
  LG_NET_KEYBOARD_LED_KANA        = 1U << 4,
};

typedef struct LGNetInputLEDs
{
  uint64_t          sequence;
  LGNetKeyboardLEDs state;
  LGNetKeyboardLEDs validMask;
}
LGNetInputLEDs;

typedef uint16_t LGNetAudioDirection;

enum
{
  LG_NET_AUDIO_DIRECTION_PLAYBACK = 1,
  LG_NET_AUDIO_DIRECTION_CAPTURE  = 2,
};

typedef uint16_t LGNetAudioSampleFormat;

enum
{
  LG_NET_AUDIO_SAMPLE_S16_LE = 1,
  LG_NET_AUDIO_SAMPLE_S24_LE = 2,
  LG_NET_AUDIO_SAMPLE_S32_LE = 3,
  LG_NET_AUDIO_SAMPLE_F32_LE = 4,
};

typedef uint32_t LGNetAudioFormatFlags;

enum
{
  LG_NET_AUDIO_FORMAT_INTERLEAVED = 1U << 0,
};

typedef struct LGNetAudioFormat
{
  uint32_t               streamID;
  LGNetAudioDirection    direction;
  LGNetAudioSampleFormat sampleFormat;
  uint64_t               formatEpoch;
  uint32_t               sampleRate;
  uint16_t               channels;
  uint16_t               framesPerPacket;
  uint32_t               channelMask;
  LGNetAudioFormatFlags  flags;
  uint32_t               maxPacketFrames;
}
LGNetAudioFormat;

typedef uint16_t LGNetAudioDataFlags;

enum
{
  LG_NET_AUDIO_DATA_DISCONTINUITY = 1U << 0,
  LG_NET_AUDIO_DATA_SILENT        = 1U << 1,
  LG_NET_AUDIO_DATA_END_OF_STREAM = 1U << 2,
};

typedef struct LGNetAudioData
{
  uint32_t            streamID;
  LGNetAudioDirection direction;
  LGNetAudioDataFlags flags;
  uint64_t            formatEpoch;
  uint64_t            packetID;
  uint64_t            timestampNs;
  uint64_t            startFrame;
  uint32_t            frameCount;
  uint32_t            dataLength;
  const uint8_t *     data;
}
LGNetAudioData;

typedef uint16_t LGNetAudioStateCode;

enum
{
  LG_NET_AUDIO_STATE_STOPPED = 1,
  LG_NET_AUDIO_STATE_RUNNING = 2,
  LG_NET_AUDIO_STATE_PAUSED  = 3,
  LG_NET_AUDIO_STATE_ERROR   = 4,
};

typedef uint32_t LGNetAudioStateFlags;

enum
{
  LG_NET_AUDIO_STATE_MUTED         = 1U << 0,
  LG_NET_AUDIO_STATE_FORMAT_ACTIVE = 1U << 1,
};

typedef struct LGNetAudioState
{
  uint32_t             streamID;
  LGNetAudioDirection  direction;
  LGNetAudioStateCode  state;
  uint64_t             stateSequence;
  uint64_t             formatEpoch;
  int32_t              volumeMillibels;
  LGNetAudioStateFlags flags;
}
LGNetAudioState;

typedef struct LGNetAudioClockFeedback
{
  uint32_t            streamID;
  LGNetAudioDirection direction;
  uint64_t            formatEpoch;
  uint64_t            packetID;
  uint64_t            framePosition;
  uint64_t            timestampNs;
  int32_t             queuedFrames;
  int32_t             driftPpm;
}
LGNetAudioClockFeedback;

typedef uint32_t LGNetClipboardClaimFlags;

enum
{
  LG_NET_CLIPBOARD_CLAIM_READ  = 1U << 0,
  LG_NET_CLIPBOARD_CLAIM_WRITE = 1U << 1,
};

typedef struct LGNetClipboardClaim
{
  uint64_t                 ownerID;
  uint64_t                 claimEpoch;
  uint32_t                 leaseMs;
  LGNetClipboardClaimFlags flags;
}
LGNetClipboardClaim;

typedef uint16_t LGNetClipboardOfferFlags;

enum
{
  LG_NET_CLIPBOARD_OFFER_LAZY      = 1U << 0,
  LG_NET_CLIPBOARD_OFFER_SENSITIVE = 1U << 1,
};

typedef struct LGNetClipboardOffer
{
  uint64_t                 offerID;
  uint64_t                 serial;
  uint64_t                 estimatedSize;
  uint32_t                 formatIndex;
  uint32_t                 formatCount;
  uint16_t                 mimeLength;
  LGNetClipboardOfferFlags flags;
  const uint8_t *          mime;
}
LGNetClipboardOffer;

typedef struct LGNetClipboardRequest
{
  uint64_t offerID;
  uint64_t requestID;
  uint32_t formatIndex;
  uint32_t maxChunkLength;
  uint64_t offset;
}
LGNetClipboardRequest;

typedef uint16_t LGNetClipboardChunkFlags;

enum
{
  LG_NET_CLIPBOARD_CHUNK_FIRST = 1U << 0,
  LG_NET_CLIPBOARD_CHUNK_FINAL = 1U << 1,
};

typedef struct LGNetClipboardChunk
{
  uint64_t                 requestID;
  uint64_t                 offerID;
  uint64_t                 offset;
  uint64_t                 totalLength;
  uint32_t                 dataLength;
  LGNetClipboardChunkFlags flags;
  const uint8_t *          data;
}
LGNetClipboardChunk;

typedef uint32_t LGNetClipboardStatusCode;

enum
{
  LG_NET_CLIPBOARD_STATUS_OK          = 1,
  LG_NET_CLIPBOARD_STATUS_NOT_FOUND   = 2,
  LG_NET_CLIPBOARD_STATUS_UNSUPPORTED = 3,
  LG_NET_CLIPBOARD_STATUS_CANCELLED   = 4,
  LG_NET_CLIPBOARD_STATUS_BUSY        = 5,
  LG_NET_CLIPBOARD_STATUS_ERROR       = 6,
};

typedef struct LGNetClipboardStatus
{
  uint64_t                 requestID;
  uint64_t                 offerID;
  LGNetClipboardStatusCode status;
  uint32_t                 detail;
  uint64_t                 processedLength;
}
LGNetClipboardStatus;

typedef uint32_t LGNetFileOfferFlags;

enum
{
  LG_NET_FILE_OFFER_COPY      = 1U << 0,
  LG_NET_FILE_OFFER_MOVE      = 1U << 1,
  LG_NET_FILE_OFFER_RECURSIVE = 1U << 2,
};

typedef struct LGNetFileOffer
{
  uint64_t            offerID;
  uint64_t            totalBytes;
  uint32_t            entryCount;
  LGNetFileOfferFlags flags;
  uint16_t            labelLength;
  const uint8_t *     label;
}
LGNetFileOffer;

typedef uint16_t LGNetFileEntryType;

enum
{
  LG_NET_FILE_ENTRY_REGULAR   = 1,
  LG_NET_FILE_ENTRY_DIRECTORY = 2,
  LG_NET_FILE_ENTRY_SYMLINK   = 3,
};

typedef uint16_t LGNetFileEntryFlags;

enum
{
  LG_NET_FILE_ENTRY_EXECUTABLE = 1U << 0,
  LG_NET_FILE_ENTRY_HIDDEN     = 1U << 1,
};

/* File path fields are strict UTF-8 without a trailing NUL. They are relative
 * to a receiver-selected transfer root and use '/' as their only separator.
 * Receivers must reject absolute paths, empty, '.' or '..' components, and
 * platform-native alternate separators before accessing the filesystem. The
 * same traversal rules apply to symbolic-link targets. */
typedef struct LGNetFileEntry
{
  uint64_t            offerID;
  uint64_t            entryID;
  uint64_t            parentID;
  uint64_t            size;
  uint64_t            modifiedNs;
  uint32_t            mode;
  LGNetFileEntryType  type;
  LGNetFileEntryFlags flags;
  uint16_t            pathLength;
  uint16_t            linkTargetLength;
  const uint8_t *     path;
  const uint8_t *     linkTarget;
}
LGNetFileEntry;

typedef uint32_t LGNetFileRequestFlags;

enum
{
  LG_NET_FILE_REQUEST_METADATA = 1U << 0,
  LG_NET_FILE_REQUEST_DATA     = 1U << 1,
};

typedef struct LGNetFileRequest
{
  uint64_t              requestID;
  uint64_t              offerID;
  uint64_t              entryID;
  uint64_t              offset;
  uint32_t              length;
  LGNetFileRequestFlags flags;
}
LGNetFileRequest;

typedef uint16_t LGNetFileChunkFlags;

enum
{
  LG_NET_FILE_CHUNK_FIRST = 1U << 0,
  LG_NET_FILE_CHUNK_FINAL = 1U << 1,
};

typedef struct LGNetFileChunk
{
  uint64_t            requestID;
  uint64_t            offerID;
  uint64_t            entryID;
  uint64_t            offset;
  uint64_t            totalLength;
  uint32_t            dataLength;
  LGNetFileChunkFlags flags;
  const uint8_t *     data;
}
LGNetFileChunk;

typedef uint32_t LGNetFileStatusCode;

enum
{
  LG_NET_FILE_STATUS_OK          = 1,
  LG_NET_FILE_STATUS_NOT_FOUND   = 2,
  LG_NET_FILE_STATUS_DENIED      = 3,
  LG_NET_FILE_STATUS_CANCELLED   = 4,
  LG_NET_FILE_STATUS_INVALID     = 5,
  LG_NET_FILE_STATUS_UNSUPPORTED = 6,
  LG_NET_FILE_STATUS_ERROR       = 7,
};

typedef struct LGNetFileStatus
{
  uint64_t            requestID;
  uint64_t            offerID;
  uint64_t            entryID;
  LGNetFileStatusCode status;
  uint32_t            detail;
  uint64_t            transferredLength;
}
LGNetFileStatus;

/* This header reserves framing only. It deliberately defines no USB
 * redirection operations or device semantics. */
typedef uint16_t LGNetUSBReservedFlags;

enum
{
  LG_NET_USB_RESERVED_RESPONSE = 1U << 0,
  LG_NET_USB_RESERVED_FINAL    = 1U << 1,
};

typedef struct LGNetUSBReserved
{
  uint16_t              version;
  uint16_t              headerSize;
  uint16_t              messageType;
  LGNetUSBReservedFlags flags;
  uint64_t              requestID;
  uint64_t              sequence;
  uint32_t              payloadLength;
  const uint8_t *       payload;
}
LGNetUSBReserved;

bool lgNetVideoStreamConfigValid(const LGNetVideoStreamConfig * config);
bool lgNetVideoStreamConfigEncode(void * data, size_t size,
  const LGNetVideoStreamConfig * config);
LGNetParseResult lgNetVideoStreamConfigDecode(
  LGNetVideoStreamConfig * config, const void * data, size_t size);

size_t lgNetVideoFrameSize(const LGNetVideoFrame * frame);
bool lgNetVideoFrameValid(const LGNetVideoFrame * frame);
bool lgNetVideoFrameEncode(
  void * data, size_t size, const LGNetVideoFrame * frame);
LGNetParseResult lgNetVideoFrameDecode(
  LGNetVideoFrame * frame, const void * data, size_t size);

size_t lgNetVideoFragmentSize(const LGNetVideoFragment * fragment);
bool lgNetVideoFragmentValid(const LGNetVideoFragment * fragment);
bool lgNetVideoFragmentEncode(
  void * data, size_t size, const LGNetVideoFragment * fragment);
LGNetParseResult lgNetVideoFragmentDecode(
  LGNetVideoFragment * fragment, const void * data, size_t size);

bool lgNetVideoFeedbackValid(const LGNetVideoFeedback * feedback);
bool lgNetVideoFeedbackEncode(
  void * data, size_t size, const LGNetVideoFeedback * feedback);
LGNetParseResult lgNetVideoFeedbackDecode(
  LGNetVideoFeedback * feedback, const void * data, size_t size);

bool lgNetCursorPositionValid(const LGNetCursorPosition * position);
bool lgNetCursorPositionEncode(
  void * data, size_t size, const LGNetCursorPosition * position);
LGNetParseResult lgNetCursorPositionDecode(
  LGNetCursorPosition * position, const void * data, size_t size);

size_t lgNetCursorShapeSize(const LGNetCursorShape * shape);
bool lgNetCursorShapeValid(const LGNetCursorShape * shape);
bool lgNetCursorShapeEncode(
  void * data, size_t size, const LGNetCursorShape * shape);
LGNetParseResult lgNetCursorShapeDecode(
  LGNetCursorShape * shape, const void * data, size_t size);

bool lgNetInputClaimValid(const LGNetInputClaim * claim);
bool lgNetInputClaimEncode(
  void * data, size_t size, const LGNetInputClaim * claim);
LGNetParseResult lgNetInputClaimDecode(
  LGNetInputClaim * claim, const void * data, size_t size);

bool lgNetInputStatusValid(const LGNetInputStatus * status);
bool lgNetInputStatusEncode(
  void * data, size_t size, const LGNetInputStatus * status);
LGNetParseResult lgNetInputStatusDecode(
  LGNetInputStatus * status, const void * data, size_t size);

bool lgNetInputRelativeValid(const LGNetInputRelative * relative);
bool lgNetInputRelativeEncode(
  void * data, size_t size, const LGNetInputRelative * relative);
LGNetParseResult lgNetInputRelativeDecode(
  LGNetInputRelative * relative, const void * data, size_t size);

bool lgNetInputAbsoluteValid(const LGNetInputAbsolute * absolute);
bool lgNetInputAbsoluteEncode(
  void * data, size_t size, const LGNetInputAbsolute * absolute);
LGNetParseResult lgNetInputAbsoluteDecode(
  LGNetInputAbsolute * absolute, const void * data, size_t size);

bool lgNetInputKeyboardValid(const LGNetInputKeyboard * keyboard);
bool lgNetInputKeyboardEncode(
  void * data, size_t size, const LGNetInputKeyboard * keyboard);
LGNetParseResult lgNetInputKeyboardDecode(
  LGNetInputKeyboard * keyboard, const void * data, size_t size);

bool lgNetInputLEDsValid(const LGNetInputLEDs * leds);
bool lgNetInputLEDsEncode(
  void * data, size_t size, const LGNetInputLEDs * leds);
LGNetParseResult lgNetInputLEDsDecode(
  LGNetInputLEDs * leds, const void * data, size_t size);

bool lgNetAudioFormatValid(const LGNetAudioFormat * format);
bool lgNetAudioFormatEncode(
  void * data, size_t size, const LGNetAudioFormat * format);
LGNetParseResult lgNetAudioFormatDecode(
  LGNetAudioFormat * format, const void * data, size_t size);

size_t lgNetAudioDataSize(const LGNetAudioData * audio);
bool lgNetAudioDataValid(const LGNetAudioData * audio);
bool lgNetAudioDataMatchesFormat(
  const LGNetAudioData * audio, const LGNetAudioFormat * format);
bool lgNetAudioDataEncode(
  void * data, size_t size, const LGNetAudioData * audio);
LGNetParseResult lgNetAudioDataDecode(
  LGNetAudioData * audio, const void * data, size_t size);

bool lgNetAudioStateValid(const LGNetAudioState * state);
bool lgNetAudioStateEncode(
  void * data, size_t size, const LGNetAudioState * state);
LGNetParseResult lgNetAudioStateDecode(
  LGNetAudioState * state, const void * data, size_t size);

bool lgNetAudioClockFeedbackValid(
  const LGNetAudioClockFeedback * feedback);
bool lgNetAudioClockFeedbackEncode(void * data, size_t size,
  const LGNetAudioClockFeedback * feedback);
LGNetParseResult lgNetAudioClockFeedbackDecode(
  LGNetAudioClockFeedback * feedback, const void * data, size_t size);

bool lgNetClipboardClaimValid(const LGNetClipboardClaim * claim);
bool lgNetClipboardClaimEncode(
  void * data, size_t size, const LGNetClipboardClaim * claim);
LGNetParseResult lgNetClipboardClaimDecode(
  LGNetClipboardClaim * claim, const void * data, size_t size);

size_t lgNetClipboardOfferSize(const LGNetClipboardOffer * offer);
bool lgNetClipboardOfferValid(const LGNetClipboardOffer * offer);
bool lgNetClipboardOfferEncode(
  void * data, size_t size, const LGNetClipboardOffer * offer);
LGNetParseResult lgNetClipboardOfferDecode(
  LGNetClipboardOffer * offer, const void * data, size_t size);

bool lgNetClipboardRequestValid(const LGNetClipboardRequest * request);
bool lgNetClipboardRequestEncode(
  void * data, size_t size, const LGNetClipboardRequest * request);
LGNetParseResult lgNetClipboardRequestDecode(
  LGNetClipboardRequest * request, const void * data, size_t size);

size_t lgNetClipboardChunkSize(const LGNetClipboardChunk * chunk);
bool lgNetClipboardChunkValid(const LGNetClipboardChunk * chunk);
bool lgNetClipboardChunkEncode(
  void * data, size_t size, const LGNetClipboardChunk * chunk);
LGNetParseResult lgNetClipboardChunkDecode(
  LGNetClipboardChunk * chunk, const void * data, size_t size);

bool lgNetClipboardStatusValid(const LGNetClipboardStatus * status);
bool lgNetClipboardStatusEncode(
  void * data, size_t size, const LGNetClipboardStatus * status);
LGNetParseResult lgNetClipboardStatusDecode(
  LGNetClipboardStatus * status, const void * data, size_t size);

size_t lgNetFileOfferSize(const LGNetFileOffer * offer);
bool lgNetFileOfferValid(const LGNetFileOffer * offer);
bool lgNetFileOfferEncode(
  void * data, size_t size, const LGNetFileOffer * offer);
LGNetParseResult lgNetFileOfferDecode(
  LGNetFileOffer * offer, const void * data, size_t size);

size_t lgNetFileEntrySize(const LGNetFileEntry * entry);
bool lgNetFileEntryValid(const LGNetFileEntry * entry);
bool lgNetFileEntryEncode(
  void * data, size_t size, const LGNetFileEntry * entry);
LGNetParseResult lgNetFileEntryDecode(
  LGNetFileEntry * entry, const void * data, size_t size);

bool lgNetFileRequestValid(const LGNetFileRequest * request);
bool lgNetFileRequestEncode(
  void * data, size_t size, const LGNetFileRequest * request);
LGNetParseResult lgNetFileRequestDecode(
  LGNetFileRequest * request, const void * data, size_t size);

size_t lgNetFileChunkSize(const LGNetFileChunk * chunk);
bool lgNetFileChunkValid(const LGNetFileChunk * chunk);
bool lgNetFileChunkEncode(
  void * data, size_t size, const LGNetFileChunk * chunk);
LGNetParseResult lgNetFileChunkDecode(
  LGNetFileChunk * chunk, const void * data, size_t size);

bool lgNetFileStatusValid(const LGNetFileStatus * status);
bool lgNetFileStatusEncode(
  void * data, size_t size, const LGNetFileStatus * status);
LGNetParseResult lgNetFileStatusDecode(
  LGNetFileStatus * status, const void * data, size_t size);

size_t lgNetUSBReservedSize(const LGNetUSBReserved * reserved);
bool lgNetUSBReservedValid(const LGNetUSBReserved * reserved);
bool lgNetUSBReservedEncode(
  void * data, size_t size, const LGNetUSBReserved * reserved);
LGNetParseResult lgNetUSBReservedDecode(
  LGNetUSBReserved * reserved, const void * data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
