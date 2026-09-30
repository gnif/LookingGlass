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

#include "common/network/services.h"

#include <limits.h>
#include <string.h>

static const LGNetVideoStreamFlags VIDEO_STREAM_FLAGS =
  LG_NET_VIDEO_STREAM_HDR             |
  LG_NET_VIDEO_STREAM_ALPHA           |
  LG_NET_VIDEO_STREAM_GPU_PLANES_ONLY |
  LG_NET_VIDEO_STREAM_DATAGRAMS;

static const LGNetVideoFrameFlags VIDEO_FRAME_FLAGS =
  LG_NET_VIDEO_FRAME_KEYFRAME      |
  LG_NET_VIDEO_FRAME_DISCONTINUITY |
  LG_NET_VIDEO_FRAME_END_OF_STREAM |
  LG_NET_VIDEO_FRAME_HAS_CHECKSUM;

static const LGNetVideoFragmentFlags VIDEO_FRAGMENT_FLAGS =
  LG_NET_VIDEO_FRAGMENT_BLOCK_START |
  LG_NET_VIDEO_FRAGMENT_BLOCK_END   |
  LG_NET_VIDEO_FRAGMENT_FRAME_END   |
  LG_NET_VIDEO_FRAGMENT_RECOVERY;

static const LGNetVideoFeedbackFlags VIDEO_FEEDBACK_FLAGS =
  LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME |
  LG_NET_VIDEO_FEEDBACK_CONGESTED        |
  LG_NET_VIDEO_FEEDBACK_DECODER_STALLED;

static const LGNetCursorPositionFlags CURSOR_POSITION_FLAGS =
  LG_NET_CURSOR_POSITION_VISIBLE |
  LG_NET_CURSOR_POSITION_RELATIVE;

static const LGNetCursorShapeFlags CURSOR_SHAPE_FLAGS =
  LG_NET_CURSOR_SHAPE_HIDDEN |
  LG_NET_CURSOR_SHAPE_ANIMATED;

static const LGNetInputClaimFlags INPUT_CLAIM_FLAGS =
  LG_NET_INPUT_CLAIM_KEYBOARD |
  LG_NET_INPUT_CLAIM_POINTER  |
  LG_NET_INPUT_CLAIM_EXCLUSIVE;

static const LGNetKeyboardFlags KEYBOARD_FLAGS =
  LG_NET_KEYBOARD_DOWN     |
  LG_NET_KEYBOARD_REPEAT   |
  LG_NET_KEYBOARD_EXTENDED |
  LG_NET_KEYBOARD_E1;

static const LGNetKeyboardLEDs KEYBOARD_LEDS =
  LG_NET_KEYBOARD_LED_NUM_LOCK    |
  LG_NET_KEYBOARD_LED_CAPS_LOCK   |
  LG_NET_KEYBOARD_LED_SCROLL_LOCK |
  LG_NET_KEYBOARD_LED_COMPOSE     |
  LG_NET_KEYBOARD_LED_KANA;

static const LGNetAudioFormatFlags AUDIO_FORMAT_FLAGS =
  LG_NET_AUDIO_FORMAT_INTERLEAVED;

static const LGNetAudioDataFlags AUDIO_DATA_FLAGS =
  LG_NET_AUDIO_DATA_DISCONTINUITY |
  LG_NET_AUDIO_DATA_SILENT        |
  LG_NET_AUDIO_DATA_END_OF_STREAM;

static const LGNetAudioStateFlags AUDIO_STATE_FLAGS =
  LG_NET_AUDIO_STATE_MUTED |
  LG_NET_AUDIO_STATE_FORMAT_ACTIVE;

static const LGNetClipboardClaimFlags CLIPBOARD_CLAIM_FLAGS =
  LG_NET_CLIPBOARD_CLAIM_READ |
  LG_NET_CLIPBOARD_CLAIM_WRITE;

static const LGNetClipboardOfferFlags CLIPBOARD_OFFER_FLAGS =
  LG_NET_CLIPBOARD_OFFER_LAZY |
  LG_NET_CLIPBOARD_OFFER_SENSITIVE;

static const LGNetClipboardChunkFlags CLIPBOARD_CHUNK_FLAGS =
  LG_NET_CLIPBOARD_CHUNK_FIRST |
  LG_NET_CLIPBOARD_CHUNK_FINAL;

static const LGNetFileOfferFlags FILE_OFFER_FLAGS =
  LG_NET_FILE_OFFER_COPY |
  LG_NET_FILE_OFFER_MOVE |
  LG_NET_FILE_OFFER_RECURSIVE;

static const LGNetFileEntryFlags FILE_ENTRY_FLAGS =
  LG_NET_FILE_ENTRY_EXECUTABLE |
  LG_NET_FILE_ENTRY_HIDDEN;

static const LGNetFileRequestFlags FILE_REQUEST_FLAGS =
  LG_NET_FILE_REQUEST_METADATA |
  LG_NET_FILE_REQUEST_DATA;

static const LGNetFileChunkFlags FILE_CHUNK_FLAGS =
  LG_NET_FILE_CHUNK_FIRST |
  LG_NET_FILE_CHUNK_FINAL;

static const LGNetUSBReservedFlags USB_RESERVED_FLAGS =
  LG_NET_USB_RESERVED_RESPONSE |
  LG_NET_USB_RESERVED_FINAL;

static bool variableSize(size_t headerSize, size_t payloadLength,
    size_t payloadLimit, size_t * wireSize)
{
  if (!wireSize || payloadLength > payloadLimit ||
      headerSize > SIZE_MAX - payloadLength ||
      headerSize + payloadLength > LG_NET_MAX_PAYLOAD_LENGTH)
    return false;

  *wireSize = headerSize + payloadLength;
  return true;
}

static LGNetParseResult fixedDecodeResult(
    const LGNetReader * reader, size_t expected, size_t actual)
{
  if (!reader || !lgNetReaderValid(reader))
    return LG_NET_PARSE_INVALID_VALUE;
  if (actual != expected || lgNetReaderConsumed(reader) != expected)
    return LG_NET_PARSE_INVALID_LENGTH;
  return LG_NET_PARSE_OK;
}

static LGNetParseResult variableDecodeSize(size_t headerSize,
    size_t payloadLength, size_t payloadLimit, size_t actual,
    size_t * expected)
{
  if (!variableSize(headerSize, payloadLength, payloadLimit, expected))
    return LG_NET_PARSE_INVALID_LENGTH;
  if (actual < *expected)
    return LG_NET_PARSE_TRUNCATED;
  if (actual != *expected)
    return LG_NET_PARSE_INVALID_LENGTH;
  return LG_NET_PARSE_OK;
}

static bool videoCodecKnown(LGNetVideoCodec codec)
{
  return codec == LG_NET_VIDEO_CODEC_PYROWAVE;
}

static bool videoPixelFormatKnown(LGNetVideoPixelFormat format)
{
  return format >= LG_NET_VIDEO_PIXEL_FORMAT_NV12 &&
    format <= LG_NET_VIDEO_PIXEL_FORMAT_YUV444P16F;
}

static bool videoPixelFormatValid(const LGNetVideoStreamConfig * config)
{
  switch (config->pixelFormat)
  {
    case LG_NET_VIDEO_PIXEL_FORMAT_NV12:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420 &&
        config->planeCount == 2 && config->bitDepth == 8;

    case LG_NET_VIDEO_PIXEL_FORMAT_P010:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420 &&
        config->planeCount == 2 && config->bitDepth == 10;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV420P8:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420 &&
        config->planeCount == 3 && config->bitDepth == 8;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV420P10:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_420 &&
        config->planeCount == 3 && config->bitDepth == 10;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV422P8:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_422 &&
        config->planeCount == 3 && config->bitDepth == 8;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV422P10:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_422 &&
        config->planeCount == 3 && config->bitDepth == 10;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV444P8:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_444 &&
        config->planeCount == 3 && config->bitDepth == 8;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV444P10:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_444 &&
        config->planeCount == 3 && config->bitDepth == 10;

    case LG_NET_VIDEO_PIXEL_FORMAT_YUV444P16F:
      return config->chromaSubsampling == LG_NET_VIDEO_CHROMA_444 &&
        config->planeCount == 3 && config->bitDepth == 16;
  }

  return false;
}

static bool videoChromaKnown(LGNetVideoChromaSubsampling chroma)
{
  return chroma >= LG_NET_VIDEO_CHROMA_420 &&
    chroma <= LG_NET_VIDEO_CHROMA_444;
}

static bool colorPrimariesKnown(LGNetColorPrimaries primaries)
{
  return primaries >= LG_NET_COLOR_PRIMARIES_BT601 &&
    primaries <= LG_NET_COLOR_PRIMARIES_BT2020;
}

static bool colorTransferKnown(LGNetColorTransfer transfer)
{
  return transfer >= LG_NET_COLOR_TRANSFER_LINEAR &&
    transfer <= LG_NET_COLOR_TRANSFER_HLG;
}

static bool colorMatrixKnown(LGNetColorMatrix matrix)
{
  return matrix >= LG_NET_COLOR_MATRIX_BT601 &&
    matrix <= LG_NET_COLOR_MATRIX_RGB;
}

static bool colorRangeKnown(LGNetColorRange range)
{
  return range == LG_NET_COLOR_RANGE_LIMITED ||
    range == LG_NET_COLOR_RANGE_FULL;
}

bool lgNetVideoStreamConfigValid(const LGNetVideoStreamConfig * config)
{
  return config && config->streamID && videoCodecKnown(config->codec) &&
    config->codecVersion && config->configEpoch && config->width &&
    config->width <= LG_NET_VIDEO_MAX_WIDTH && config->height &&
    config->height <= LG_NET_VIDEO_MAX_HEIGHT &&
    config->refreshNumerator && config->refreshDenominator &&
    videoPixelFormatKnown(config->pixelFormat) &&
    videoChromaKnown(config->chromaSubsampling) &&
    colorPrimariesKnown(config->colorPrimaries) &&
    colorTransferKnown(config->colorTransfer) &&
    colorMatrixKnown(config->colorMatrix) &&
    colorRangeKnown(config->colorRange) && config->planeCount &&
    config->planeCount <= LG_NET_VIDEO_MAX_PLANES && config->bitDepth >= 8 &&
    config->bitDepth <= 16 && !(config->flags & ~VIDEO_STREAM_FLAGS) &&
    (config->flags & LG_NET_VIDEO_STREAM_GPU_PLANES_ONLY) &&
    videoPixelFormatValid(config) &&
    config->maxFrameLength &&
    config->maxFrameLength <= LG_NET_VIDEO_MAX_FRAME_LENGTH &&
    config->maxFragmentLength &&
    config->maxFragmentLength <= LG_NET_VIDEO_MAX_FRAGMENT_LENGTH &&
    config->maxFragmentLength <= config->maxFrameLength &&
    config->maxFrameLatencyMs;
}

bool lgNetVideoStreamConfigEncode(void * data, size_t size,
    const LGNetVideoStreamConfig * config)
{
  if (!data || size < LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE ||
      !lgNetVideoStreamConfigValid(config))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, config->streamID)             &&
    lgNetWriterU16(&writer, config->codec)                &&
    lgNetWriterU16(&writer, config->codecVersion)         &&
    lgNetWriterU64(&writer, config->configEpoch)          &&
    lgNetWriterU32(&writer, config->width)                &&
    lgNetWriterU32(&writer, config->height)               &&
    lgNetWriterU32(&writer, config->refreshNumerator)     &&
    lgNetWriterU32(&writer, config->refreshDenominator)   &&
    lgNetWriterU16(&writer, config->pixelFormat)          &&
    lgNetWriterU8 (&writer, config->chromaSubsampling)    &&
    lgNetWriterU8 (&writer, config->colorPrimaries)       &&
    lgNetWriterU8 (&writer, config->colorTransfer)        &&
    lgNetWriterU8 (&writer, config->colorMatrix)          &&
    lgNetWriterU8 (&writer, config->colorRange)           &&
    lgNetWriterU8 (&writer, config->planeCount)           &&
    lgNetWriterU8 (&writer, config->bitDepth)             &&
    lgNetWriterU8 (&writer, 0)                            &&
    lgNetWriterU16(&writer, config->flags)                &&
    lgNetWriterU16(&writer, 0)                            &&
    lgNetWriterU32(&writer, config->maxFrameLength)       &&
    lgNetWriterU32(&writer, config->maxFragmentLength)    &&
    lgNetWriterU32(&writer, config->maxFrameLatencyMs)    &&
    lgNetWriterZero(&writer, 2)                           &&
    lgNetWriterSize(&writer) == LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE;
}

LGNetParseResult lgNetVideoStreamConfigDecode(
    LGNetVideoStreamConfig * config, const void * data, size_t size)
{
  if (!config || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetVideoStreamConfig decoded;
  LGNetReader            reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)             ||
      !lgNetReaderU16(&reader, &decoded.codec)                ||
      !lgNetReaderU16(&reader, &decoded.codecVersion)         ||
      !lgNetReaderU64(&reader, &decoded.configEpoch)          ||
      !lgNetReaderU32(&reader, &decoded.width)                ||
      !lgNetReaderU32(&reader, &decoded.height)               ||
      !lgNetReaderU32(&reader, &decoded.refreshNumerator)     ||
      !lgNetReaderU32(&reader, &decoded.refreshDenominator)   ||
      !lgNetReaderU16(&reader, &decoded.pixelFormat)          ||
      !lgNetReaderU8 (&reader, &decoded.chromaSubsampling)    ||
      !lgNetReaderU8 (&reader, &decoded.colorPrimaries)       ||
      !lgNetReaderU8 (&reader, &decoded.colorTransfer)        ||
      !lgNetReaderU8 (&reader, &decoded.colorMatrix)          ||
      !lgNetReaderU8 (&reader, &decoded.colorRange)           ||
      !lgNetReaderU8 (&reader, &decoded.planeCount)           ||
      !lgNetReaderU8 (&reader, &decoded.bitDepth)             ||
      !lgNetReaderZero(&reader, 1)                            ||
      !lgNetReaderU16(&reader, &decoded.flags)                ||
      !lgNetReaderZero(&reader, 2)                            ||
      !lgNetReaderU32(&reader, &decoded.maxFrameLength)       ||
      !lgNetReaderU32(&reader, &decoded.maxFragmentLength)    ||
      !lgNetReaderU32(&reader, &decoded.maxFrameLatencyMs)    ||
      !lgNetReaderZero(&reader, 2))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetVideoStreamConfigValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *config = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetVideoFrameSize(const LGNetVideoFrame * frame)
{
  size_t size;
  return frame && variableSize(LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE,
    frame->encodedLength, LG_NET_VIDEO_MAX_FRAME_LENGTH, &size) ? size : 0;
}

bool lgNetVideoFrameValid(const LGNetVideoFrame * frame)
{
  return frame && frame->streamID && videoCodecKnown(frame->codec) &&
    !(frame->flags & ~VIDEO_FRAME_FLAGS) && frame->configEpoch &&
    frame->frameID && frame->captureTimestampNs &&
    frame->presentationTimestampNs && frame->encodedLength && frame->data &&
    frame->blockCount && frame->fragmentCount && frame->deadlineMs &&
    ((frame->flags & LG_NET_VIDEO_FRAME_HAS_CHECKSUM) || !frame->checksum) &&
    lgNetVideoFrameSize(frame) != 0;
}

bool lgNetVideoFrameEncode(
    void * data, size_t size, const LGNetVideoFrame * frame)
{
  const size_t wireSize = lgNetVideoFrameSize(frame);
  if (!data || !wireSize || size < wireSize || !lgNetVideoFrameValid(frame))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, frame->streamID)                 &&
    lgNetWriterU16(&writer, frame->codec)                    &&
    lgNetWriterU16(&writer, frame->flags)                    &&
    lgNetWriterU64(&writer, frame->configEpoch)              &&
    lgNetWriterU64(&writer, frame->frameID)                  &&
    lgNetWriterU64(&writer, frame->captureTimestampNs)       &&
    lgNetWriterU64(&writer, frame->presentationTimestampNs)  &&
    lgNetWriterU32(&writer, frame->encodedLength)            &&
    lgNetWriterU16(&writer, frame->blockCount)               &&
    lgNetWriterU16(&writer, frame->fragmentCount)            &&
    lgNetWriterU32(&writer, frame->deadlineMs)               &&
    lgNetWriterU32(&writer, frame->checksum)                 &&
    lgNetWriterBytes(&writer, frame->data, frame->encodedLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetVideoFrameDecode(
    LGNetVideoFrame * frame, const void * data, size_t size)
{
  if (!frame || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetVideoFrame decoded;
  LGNetReader     reader;
  size_t          expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)                ||
      !lgNetReaderU16(&reader, &decoded.codec)                   ||
      !lgNetReaderU16(&reader, &decoded.flags)                   ||
      !lgNetReaderU64(&reader, &decoded.configEpoch)             ||
      !lgNetReaderU64(&reader, &decoded.frameID)                 ||
      !lgNetReaderU64(&reader, &decoded.captureTimestampNs)      ||
      !lgNetReaderU64(&reader, &decoded.presentationTimestampNs) ||
      !lgNetReaderU32(&reader, &decoded.encodedLength)           ||
      !lgNetReaderU16(&reader, &decoded.blockCount)              ||
      !lgNetReaderU16(&reader, &decoded.fragmentCount)           ||
      !lgNetReaderU32(&reader, &decoded.deadlineMs)              ||
      !lgNetReaderU32(&reader, &decoded.checksum))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE, decoded.encodedLength,
    LG_NET_VIDEO_MAX_FRAME_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.data, decoded.encodedLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetVideoFrameValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *frame = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetVideoFragmentSize(const LGNetVideoFragment * fragment)
{
  size_t size;
  return fragment && variableSize(LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE,
    fragment->payloadLength, LG_NET_VIDEO_MAX_FRAGMENT_LENGTH, &size) ?
    size : 0;
}

bool lgNetVideoFragmentValid(const LGNetVideoFragment * fragment)
{
  return fragment && fragment->streamID && fragment->configEpoch &&
    fragment->frameID && fragment->frameLength &&
    fragment->frameLength <= LG_NET_VIDEO_MAX_FRAME_LENGTH &&
    fragment->fragmentCount &&
    fragment->fragmentIndex < fragment->fragmentCount &&
    fragment->blockCount &&
    fragment->blockIndex < fragment->blockCount && fragment->payloadLength &&
    fragment->payload && !(fragment->flags & ~VIDEO_FRAGMENT_FLAGS) &&
    fragment->offset <= fragment->frameLength &&
    fragment->payloadLength <= fragment->frameLength - fragment->offset &&
    (!(fragment->flags & LG_NET_VIDEO_FRAGMENT_FRAME_END) ||
      fragment->offset + fragment->payloadLength == fragment->frameLength) &&
    lgNetVideoFragmentSize(fragment) != 0;
}

bool lgNetVideoFragmentEncode(
    void * data, size_t size, const LGNetVideoFragment * fragment)
{
  const size_t wireSize = lgNetVideoFragmentSize(fragment);
  if (!data || !wireSize || size < wireSize ||
      !lgNetVideoFragmentValid(fragment))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, fragment->streamID)       &&
    lgNetWriterU64(&writer, fragment->configEpoch)    &&
    lgNetWriterU64(&writer, fragment->frameID)        &&
    lgNetWriterU32(&writer, fragment->frameLength)    &&
    lgNetWriterU32(&writer, fragment->offset)         &&
    lgNetWriterU16(&writer, fragment->fragmentIndex)  &&
    lgNetWriterU16(&writer, fragment->fragmentCount)  &&
    lgNetWriterU16(&writer, fragment->blockIndex)     &&
    lgNetWriterU16(&writer, fragment->blockCount)     &&
    lgNetWriterU16(&writer, fragment->payloadLength)  &&
    lgNetWriterU16(&writer, fragment->flags)          &&
    lgNetWriterBytes(&writer, fragment->payload,
      fragment->payloadLength)                        &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetVideoFragmentDecode(
    LGNetVideoFragment * fragment, const void * data, size_t size)
{
  if (!fragment || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetVideoFragment decoded;
  LGNetReader        reader;
  size_t             expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)      ||
      !lgNetReaderU64(&reader, &decoded.configEpoch)   ||
      !lgNetReaderU64(&reader, &decoded.frameID)       ||
      !lgNetReaderU32(&reader, &decoded.frameLength)   ||
      !lgNetReaderU32(&reader, &decoded.offset)        ||
      !lgNetReaderU16(&reader, &decoded.fragmentIndex) ||
      !lgNetReaderU16(&reader, &decoded.fragmentCount) ||
      !lgNetReaderU16(&reader, &decoded.blockIndex)    ||
      !lgNetReaderU16(&reader, &decoded.blockCount)    ||
      !lgNetReaderU16(&reader, &decoded.payloadLength) ||
      !lgNetReaderU16(&reader, &decoded.flags))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE, decoded.payloadLength,
    LG_NET_VIDEO_MAX_FRAGMENT_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.payload, decoded.payloadLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetVideoFragmentValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *fragment = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetVideoFeedbackValid(const LGNetVideoFeedback * feedback)
{
  return feedback && feedback->streamID &&
    !(feedback->flags & ~VIDEO_FEEDBACK_FLAGS) &&
    feedback->lastCompleteFrameID <= feedback->highestSeenFrameID &&
    feedback->lastDecodedFrameID <= feedback->lastCompleteFrameID;
}

bool lgNetVideoFeedbackEncode(
    void * data, size_t size, const LGNetVideoFeedback * feedback)
{
  if (!data || size < LG_NET_VIDEO_FEEDBACK_WIRE_SIZE ||
      !lgNetVideoFeedbackValid(feedback))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, feedback->streamID)            &&
    lgNetWriterU32(&writer, feedback->flags)               &&
    lgNetWriterU64(&writer, feedback->lastCompleteFrameID) &&
    lgNetWriterU64(&writer, feedback->highestSeenFrameID)  &&
    lgNetWriterU64(&writer, feedback->lastDecodedFrameID)  &&
    lgNetWriterU32(&writer, feedback->lostFrames)          &&
    lgNetWriterU32(&writer, feedback->lostFragments)       &&
    lgNetWriterU32(&writer, feedback->receiveQueueUs)      &&
    lgNetWriterU32(&writer, feedback->decodeQueueUs)       &&
    lgNetWriterU32(&writer, feedback->roundTripUs)         &&
    lgNetWriterU32(&writer, 0)                             &&
    lgNetWriterSize(&writer) == LG_NET_VIDEO_FEEDBACK_WIRE_SIZE;
}

LGNetParseResult lgNetVideoFeedbackDecode(
    LGNetVideoFeedback * feedback, const void * data, size_t size)
{
  if (!feedback || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_VIDEO_FEEDBACK_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetVideoFeedback decoded;
  LGNetReader        reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)            ||
      !lgNetReaderU32(&reader, &decoded.flags)               ||
      !lgNetReaderU64(&reader, &decoded.lastCompleteFrameID) ||
      !lgNetReaderU64(&reader, &decoded.highestSeenFrameID)  ||
      !lgNetReaderU64(&reader, &decoded.lastDecodedFrameID)  ||
      !lgNetReaderU32(&reader, &decoded.lostFrames)          ||
      !lgNetReaderU32(&reader, &decoded.lostFragments)       ||
      !lgNetReaderU32(&reader, &decoded.receiveQueueUs)      ||
      !lgNetReaderU32(&reader, &decoded.decodeQueueUs)       ||
      !lgNetReaderU32(&reader, &decoded.roundTripUs)         ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_VIDEO_FEEDBACK_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetVideoFeedbackValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *feedback = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetCursorPositionValid(const LGNetCursorPosition * position)
{
  return position && position->updateID && position->timestampNs &&
    position->desktopWidth && position->desktopHeight &&
    position->desktopWidth <= LG_NET_VIDEO_MAX_WIDTH &&
    position->desktopHeight <= LG_NET_VIDEO_MAX_HEIGHT &&
    !(position->flags & ~CURSOR_POSITION_FLAGS);
}

bool lgNetCursorPositionEncode(
    void * data, size_t size, const LGNetCursorPosition * position)
{
  if (!data || size < LG_NET_CURSOR_POSITION_WIRE_SIZE ||
      !lgNetCursorPositionValid(position))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, position->updateID)      &&
    lgNetWriterU64(&writer, position->timestampNs)   &&
    lgNetWriterI32(&writer, position->x)             &&
    lgNetWriterI32(&writer, position->y)             &&
    lgNetWriterU32(&writer, position->desktopWidth)  &&
    lgNetWriterU32(&writer, position->desktopHeight) &&
    lgNetWriterU32(&writer, position->flags)         &&
    lgNetWriterU32(&writer, position->displayID)     &&
    lgNetWriterSize(&writer) == LG_NET_CURSOR_POSITION_WIRE_SIZE;
}

LGNetParseResult lgNetCursorPositionDecode(
    LGNetCursorPosition * position, const void * data, size_t size)
{
  if (!position || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CURSOR_POSITION_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetCursorPosition decoded;
  LGNetReader         reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.updateID)      ||
      !lgNetReaderU64(&reader, &decoded.timestampNs)   ||
      !lgNetReaderI32(&reader, &decoded.x)             ||
      !lgNetReaderI32(&reader, &decoded.y)             ||
      !lgNetReaderU32(&reader, &decoded.desktopWidth)  ||
      !lgNetReaderU32(&reader, &decoded.desktopHeight) ||
      !lgNetReaderU32(&reader, &decoded.flags)         ||
      !lgNetReaderU32(&reader, &decoded.displayID))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_CURSOR_POSITION_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetCursorPositionValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *position = decoded;
  return LG_NET_PARSE_OK;
}

static bool cursorFormatKnown(LGNetCursorFormat format)
{
  return format >= LG_NET_CURSOR_FORMAT_BGRA8_PREMULTIPLIED &&
    format <= LG_NET_CURSOR_FORMAT_MONOCHROME;
}

static bool cursorShapeLayoutValid(const LGNetCursorShape * shape)
{
  uint32_t minimumPitch;
  switch (shape->format)
  {
    case LG_NET_CURSOR_FORMAT_BGRA8_PREMULTIPLIED:
    case LG_NET_CURSOR_FORMAT_RGBA8_PREMULTIPLIED:
    case LG_NET_CURSOR_FORMAT_MASKED_COLOR:
      minimumPitch = shape->width * 4U;
      break;

    case LG_NET_CURSOR_FORMAT_MONOCHROME:
      minimumPitch = (shape->width + 7U) / 8U;
      break;

    default:
      return false;
  }

  return shape->pitch >= minimumPitch &&
    shape->height <= shape->dataLength / shape->pitch;
}

size_t lgNetCursorShapeSize(const LGNetCursorShape * shape)
{
  size_t size;
  return shape && variableSize(LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE,
    shape->dataLength, LG_NET_CURSOR_MAX_SHAPE_LENGTH, &size) ? size : 0;
}

bool lgNetCursorShapeValid(const LGNetCursorShape * shape)
{
  return shape && shape->shapeID && shape->width &&
    shape->width <= LG_NET_CURSOR_MAX_WIDTH && shape->height &&
    shape->height <= LG_NET_CURSOR_MAX_HEIGHT && shape->pitch &&
    shape->dataLength && shape->data && cursorFormatKnown(shape->format) &&
    !(shape->flags & ~CURSOR_SHAPE_FLAGS) && shape->hotspotX < shape->width &&
    shape->hotspotY < shape->height &&
    cursorShapeLayoutValid(shape) &&
    ((shape->flags & LG_NET_CURSOR_SHAPE_ANIMATED) ?
      shape->frameDurationMs != 0 : shape->frameDurationMs == 0) &&
    lgNetCursorShapeSize(shape) != 0;
}

bool lgNetCursorShapeEncode(
    void * data, size_t size, const LGNetCursorShape * shape)
{
  const size_t wireSize = lgNetCursorShapeSize(shape);
  if (!data || !wireSize || size < wireSize || !lgNetCursorShapeValid(shape))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, shape->shapeID)         &&
    lgNetWriterU32(&writer, shape->width)           &&
    lgNetWriterU32(&writer, shape->height)          &&
    lgNetWriterU32(&writer, shape->pitch)           &&
    lgNetWriterU32(&writer, shape->dataLength)      &&
    lgNetWriterU16(&writer, shape->format)          &&
    lgNetWriterU16(&writer, shape->flags)           &&
    lgNetWriterU32(&writer, shape->hotspotX)        &&
    lgNetWriterU32(&writer, shape->hotspotY)        &&
    lgNetWriterU32(&writer, shape->frameDurationMs) &&
    lgNetWriterBytes(&writer, shape->data, shape->dataLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetCursorShapeDecode(
    LGNetCursorShape * shape, const void * data, size_t size)
{
  if (!shape || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetCursorShape decoded;
  LGNetReader      reader;
  size_t           expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.shapeID)         ||
      !lgNetReaderU32(&reader, &decoded.width)           ||
      !lgNetReaderU32(&reader, &decoded.height)          ||
      !lgNetReaderU32(&reader, &decoded.pitch)           ||
      !lgNetReaderU32(&reader, &decoded.dataLength)      ||
      !lgNetReaderU16(&reader, &decoded.format)          ||
      !lgNetReaderU16(&reader, &decoded.flags)           ||
      !lgNetReaderU32(&reader, &decoded.hotspotX)        ||
      !lgNetReaderU32(&reader, &decoded.hotspotY)        ||
      !lgNetReaderU32(&reader, &decoded.frameDurationMs))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_CURSOR_SHAPE_HEADER_WIRE_SIZE, decoded.dataLength,
    LG_NET_CURSOR_MAX_SHAPE_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.data, decoded.dataLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetCursorShapeValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *shape = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetInputClaimValid(const LGNetInputClaim * claim)
{
  return claim && claim->claimantID && claim->claimEpoch && claim->leaseMs &&
    claim->flags && !(claim->flags & ~INPUT_CLAIM_FLAGS) &&
    (!(claim->flags & LG_NET_INPUT_CLAIM_EXCLUSIVE) ||
      (claim->flags & (LG_NET_INPUT_CLAIM_KEYBOARD |
        LG_NET_INPUT_CLAIM_POINTER)));
}

bool lgNetInputClaimEncode(
    void * data, size_t size, const LGNetInputClaim * claim)
{
  if (!data || size < LG_NET_INPUT_CLAIM_WIRE_SIZE ||
      !lgNetInputClaimValid(claim))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, claim->claimantID) &&
    lgNetWriterU64(&writer, claim->claimEpoch) &&
    lgNetWriterU32(&writer, claim->leaseMs)    &&
    lgNetWriterU32(&writer, claim->flags)      &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_CLAIM_WIRE_SIZE;
}

LGNetParseResult lgNetInputClaimDecode(
    LGNetInputClaim * claim, const void * data, size_t size)
{
  if (!claim || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_CLAIM_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputClaim decoded;
  LGNetReader     reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.claimantID) ||
      !lgNetReaderU64(&reader, &decoded.claimEpoch) ||
      !lgNetReaderU32(&reader, &decoded.leaseMs)    ||
      !lgNetReaderU32(&reader, &decoded.flags))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_CLAIM_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputClaimValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *claim = decoded;
  return LG_NET_PARSE_OK;
}

static bool inputStatusKnown(LGNetInputStatusCode status)
{
  return status >= LG_NET_INPUT_STATUS_ACCEPTED &&
    status <= LG_NET_INPUT_STATUS_ERROR;
}

bool lgNetInputStatusValid(const LGNetInputStatus * status)
{
  return status && status->claimEpoch && inputStatusKnown(status->status) &&
    !(status->flags & ~INPUT_CLAIM_FLAGS);
}

bool lgNetInputStatusEncode(
    void * data, size_t size, const LGNetInputStatus * status)
{
  if (!data || size < LG_NET_INPUT_STATUS_WIRE_SIZE ||
      !lgNetInputStatusValid(status))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, status->claimEpoch)       &&
    lgNetWriterU64(&writer, status->lastSequence)     &&
    lgNetWriterU32(&writer, status->status)           &&
    lgNetWriterU32(&writer, status->flags)            &&
    lgNetWriterU32(&writer, status->leaseRemainingMs) &&
    lgNetWriterU32(&writer, status->detail)           &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_STATUS_WIRE_SIZE;
}

LGNetParseResult lgNetInputStatusDecode(
    LGNetInputStatus * status, const void * data, size_t size)
{
  if (!status || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_STATUS_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputStatus decoded;
  LGNetReader      reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.claimEpoch)       ||
      !lgNetReaderU64(&reader, &decoded.lastSequence)     ||
      !lgNetReaderU32(&reader, &decoded.status)           ||
      !lgNetReaderU32(&reader, &decoded.flags)            ||
      !lgNetReaderU32(&reader, &decoded.leaseRemainingMs) ||
      !lgNetReaderU32(&reader, &decoded.detail))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_STATUS_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputStatusValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *status = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetInputRelativeValid(const LGNetInputRelative * relative)
{
  return relative && relative->sequence && relative->timestampNs;
}

bool lgNetInputRelativeEncode(
    void * data, size_t size, const LGNetInputRelative * relative)
{
  if (!data || size < LG_NET_INPUT_RELATIVE_WIRE_SIZE ||
      !lgNetInputRelativeValid(relative))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, relative->sequence)       &&
    lgNetWriterU64(&writer, relative->timestampNs)    &&
    lgNetWriterI32(&writer, relative->deltaX)         &&
    lgNetWriterI32(&writer, relative->deltaY)         &&
    lgNetWriterI32(&writer, relative->wheelX)         &&
    lgNetWriterI32(&writer, relative->wheelY)         &&
    lgNetWriterU32(&writer, relative->buttons)        &&
    lgNetWriterU32(&writer, relative->changedButtons) &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_RELATIVE_WIRE_SIZE;
}

LGNetParseResult lgNetInputRelativeDecode(
    LGNetInputRelative * relative, const void * data, size_t size)
{
  if (!relative || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_RELATIVE_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputRelative decoded;
  LGNetReader        reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.sequence)       ||
      !lgNetReaderU64(&reader, &decoded.timestampNs)    ||
      !lgNetReaderI32(&reader, &decoded.deltaX)         ||
      !lgNetReaderI32(&reader, &decoded.deltaY)         ||
      !lgNetReaderI32(&reader, &decoded.wheelX)         ||
      !lgNetReaderI32(&reader, &decoded.wheelY)         ||
      !lgNetReaderU32(&reader, &decoded.buttons)        ||
      !lgNetReaderU32(&reader, &decoded.changedButtons))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_RELATIVE_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputRelativeValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *relative = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetInputAbsoluteValid(const LGNetInputAbsolute * absolute)
{
  return absolute && absolute->sequence && absolute->timestampNs &&
    absolute->width &&
    absolute->width <= LG_NET_INPUT_ABSOLUTE_MAX_WIDTH &&
    absolute->height &&
    absolute->height <= LG_NET_INPUT_ABSOLUTE_MAX_HEIGHT &&
    absolute->x >= 0 && absolute->y >= 0 &&
    (uint32_t)absolute->x < absolute->width &&
    (uint32_t)absolute->y < absolute->height;
}

bool lgNetInputAbsoluteEncode(
    void * data, size_t size, const LGNetInputAbsolute * absolute)
{
  if (!data || size < LG_NET_INPUT_ABSOLUTE_WIRE_SIZE ||
      !lgNetInputAbsoluteValid(absolute))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, absolute->sequence)       &&
    lgNetWriterU64(&writer, absolute->timestampNs)    &&
    lgNetWriterI32(&writer, absolute->x)              &&
    lgNetWriterI32(&writer, absolute->y)              &&
    lgNetWriterU32(&writer, absolute->width)          &&
    lgNetWriterU32(&writer, absolute->height)         &&
    lgNetWriterI32(&writer, absolute->wheelX)         &&
    lgNetWriterI32(&writer, absolute->wheelY)         &&
    lgNetWriterU32(&writer, absolute->buttons)        &&
    lgNetWriterU32(&writer, absolute->changedButtons) &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_ABSOLUTE_WIRE_SIZE;
}

LGNetParseResult lgNetInputAbsoluteDecode(
    LGNetInputAbsolute * absolute, const void * data, size_t size)
{
  if (!absolute || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_ABSOLUTE_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputAbsolute decoded;
  LGNetReader        reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.sequence)       ||
      !lgNetReaderU64(&reader, &decoded.timestampNs)    ||
      !lgNetReaderI32(&reader, &decoded.x)              ||
      !lgNetReaderI32(&reader, &decoded.y)              ||
      !lgNetReaderU32(&reader, &decoded.width)          ||
      !lgNetReaderU32(&reader, &decoded.height)         ||
      !lgNetReaderI32(&reader, &decoded.wheelX)         ||
      !lgNetReaderI32(&reader, &decoded.wheelY)         ||
      !lgNetReaderU32(&reader, &decoded.buttons)        ||
      !lgNetReaderU32(&reader, &decoded.changedButtons))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_ABSOLUTE_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputAbsoluteValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *absolute = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetInputKeyboardValid(const LGNetInputKeyboard * keyboard)
{
  return keyboard && keyboard->sequence && keyboard->timestampNs &&
    (keyboard->usage || keyboard->scanCode) &&
    !(keyboard->flags & ~KEYBOARD_FLAGS) &&
    !(keyboard->modifiers & ~LG_NET_INPUT_KEYBOARD_MODIFIER_MASK);
}

bool lgNetInputKeyboardEncode(
    void * data, size_t size, const LGNetInputKeyboard * keyboard)
{
  if (!data || size < LG_NET_INPUT_KEYBOARD_WIRE_SIZE ||
      !lgNetInputKeyboardValid(keyboard))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, keyboard->sequence)    &&
    lgNetWriterU64(&writer, keyboard->timestampNs) &&
    lgNetWriterU16(&writer, keyboard->usagePage)   &&
    lgNetWriterU16(&writer, keyboard->usage)       &&
    lgNetWriterU16(&writer, keyboard->scanCode)    &&
    lgNetWriterU16(&writer, keyboard->flags)       &&
    lgNetWriterU32(&writer, keyboard->modifiers)   &&
    lgNetWriterU32(&writer, 0)                     &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_KEYBOARD_WIRE_SIZE;
}

LGNetParseResult lgNetInputKeyboardDecode(
    LGNetInputKeyboard * keyboard, const void * data, size_t size)
{
  if (!keyboard || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_KEYBOARD_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputKeyboard decoded;
  LGNetReader        reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.sequence)    ||
      !lgNetReaderU64(&reader, &decoded.timestampNs) ||
      !lgNetReaderU16(&reader, &decoded.usagePage)   ||
      !lgNetReaderU16(&reader, &decoded.usage)       ||
      !lgNetReaderU16(&reader, &decoded.scanCode)    ||
      !lgNetReaderU16(&reader, &decoded.flags)       ||
      !lgNetReaderU32(&reader, &decoded.modifiers)   ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_KEYBOARD_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputKeyboardValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *keyboard = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetInputLEDsValid(const LGNetInputLEDs * leds)
{
  return leds && leds->sequence && leds->validMask &&
    !(leds->state & ~KEYBOARD_LEDS) && !(leds->validMask & ~KEYBOARD_LEDS) &&
    !(leds->state & ~leds->validMask);
}

bool lgNetInputLEDsEncode(
    void * data, size_t size, const LGNetInputLEDs * leds)
{
  if (!data || size < LG_NET_INPUT_LEDS_WIRE_SIZE ||
      !lgNetInputLEDsValid(leds))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, leds->sequence)  &&
    lgNetWriterU32(&writer, leds->state)     &&
    lgNetWriterU32(&writer, leds->validMask) &&
    lgNetWriterSize(&writer) == LG_NET_INPUT_LEDS_WIRE_SIZE;
}

LGNetParseResult lgNetInputLEDsDecode(
    LGNetInputLEDs * leds, const void * data, size_t size)
{
  if (!leds || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_INPUT_LEDS_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetInputLEDs decoded;
  LGNetReader    reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.sequence)  ||
      !lgNetReaderU32(&reader, &decoded.state)     ||
      !lgNetReaderU32(&reader, &decoded.validMask))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_INPUT_LEDS_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetInputLEDsValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *leds = decoded;
  return LG_NET_PARSE_OK;
}

static bool audioDirectionKnown(LGNetAudioDirection direction)
{
  return direction == LG_NET_AUDIO_DIRECTION_PLAYBACK ||
    direction == LG_NET_AUDIO_DIRECTION_CAPTURE;
}

static bool audioSampleFormatKnown(LGNetAudioSampleFormat format)
{
  return format >= LG_NET_AUDIO_SAMPLE_S16_LE &&
    format <= LG_NET_AUDIO_SAMPLE_F32_LE;
}

static size_t audioSampleSize(LGNetAudioSampleFormat format)
{
  switch (format)
  {
    case LG_NET_AUDIO_SAMPLE_S16_LE:
      return 2;

    case LG_NET_AUDIO_SAMPLE_S24_LE:
      return 3;

    case LG_NET_AUDIO_SAMPLE_S32_LE:
    case LG_NET_AUDIO_SAMPLE_F32_LE:
      return 4;
  }

  return 0;
}

bool lgNetAudioFormatValid(const LGNetAudioFormat * format)
{
  return format && format->streamID &&
    audioDirectionKnown(format->direction) &&
    audioSampleFormatKnown(format->sampleFormat) && format->formatEpoch &&
    format->sampleRate && format->sampleRate <= LG_NET_AUDIO_MAX_SAMPLE_RATE &&
    format->channels && format->channels <= LG_NET_AUDIO_MAX_CHANNELS &&
    format->framesPerPacket &&
    format->framesPerPacket <= LG_NET_AUDIO_MAX_PACKET_FRAMES &&
    format->maxPacketFrames >= format->framesPerPacket &&
    format->maxPacketFrames <= LG_NET_AUDIO_MAX_PACKET_FRAMES &&
    !(format->flags & ~AUDIO_FORMAT_FLAGS);
}

bool lgNetAudioFormatEncode(
    void * data, size_t size, const LGNetAudioFormat * format)
{
  if (!data || size < LG_NET_AUDIO_FORMAT_WIRE_SIZE ||
      !lgNetAudioFormatValid(format))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, format->streamID)        &&
    lgNetWriterU16(&writer, format->direction)       &&
    lgNetWriterU16(&writer, format->sampleFormat)    &&
    lgNetWriterU64(&writer, format->formatEpoch)     &&
    lgNetWriterU32(&writer, format->sampleRate)      &&
    lgNetWriterU16(&writer, format->channels)        &&
    lgNetWriterU16(&writer, format->framesPerPacket) &&
    lgNetWriterU32(&writer, format->channelMask)     &&
    lgNetWriterU32(&writer, format->flags)           &&
    lgNetWriterU32(&writer, format->maxPacketFrames) &&
    lgNetWriterU32(&writer, 0)                       &&
    lgNetWriterSize(&writer) == LG_NET_AUDIO_FORMAT_WIRE_SIZE;
}

LGNetParseResult lgNetAudioFormatDecode(
    LGNetAudioFormat * format, const void * data, size_t size)
{
  if (!format || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_AUDIO_FORMAT_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetAudioFormat decoded;
  LGNetReader      reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)        ||
      !lgNetReaderU16(&reader, &decoded.direction)       ||
      !lgNetReaderU16(&reader, &decoded.sampleFormat)    ||
      !lgNetReaderU64(&reader, &decoded.formatEpoch)     ||
      !lgNetReaderU32(&reader, &decoded.sampleRate)      ||
      !lgNetReaderU16(&reader, &decoded.channels)        ||
      !lgNetReaderU16(&reader, &decoded.framesPerPacket) ||
      !lgNetReaderU32(&reader, &decoded.channelMask)     ||
      !lgNetReaderU32(&reader, &decoded.flags)           ||
      !lgNetReaderU32(&reader, &decoded.maxPacketFrames) ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_AUDIO_FORMAT_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetAudioFormatValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *format = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetAudioDataSize(const LGNetAudioData * audio)
{
  size_t size;
  return audio && variableSize(LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE,
    audio->dataLength, LG_NET_AUDIO_MAX_DATA_LENGTH, &size) ? size : 0;
}

bool lgNetAudioDataValid(const LGNetAudioData * audio)
{
  return audio && audio->streamID &&
    audioDirectionKnown(audio->direction) &&
    !(audio->flags & ~AUDIO_DATA_FLAGS) && audio->formatEpoch &&
    audio->packetID && audio->timestampNs && audio->frameCount &&
    audio->frameCount <= LG_NET_AUDIO_MAX_PACKET_FRAMES &&
    ((audio->flags & LG_NET_AUDIO_DATA_SILENT) ?
      !audio->dataLength && !audio->data : audio->dataLength && audio->data) &&
    lgNetAudioDataSize(audio) != 0;
}

bool lgNetAudioDataMatchesFormat(
    const LGNetAudioData * audio, const LGNetAudioFormat * format)
{
  if (!lgNetAudioDataValid(audio) || !lgNetAudioFormatValid(format) ||
      audio->streamID != format->streamID ||
      audio->direction != format->direction ||
      audio->formatEpoch != format->formatEpoch ||
      audio->frameCount > format->maxPacketFrames)
    return false;

  if (audio->flags & LG_NET_AUDIO_DATA_SILENT)
    return true;

  const size_t sampleSize = audioSampleSize(format->sampleFormat);
  if (!sampleSize || format->channels > SIZE_MAX / sampleSize)
    return false;

  const size_t frameSize = sampleSize * format->channels;
  return audio->frameCount <= SIZE_MAX / frameSize &&
    audio->dataLength == (size_t)audio->frameCount * frameSize;
}

bool lgNetAudioDataEncode(
    void * data, size_t size, const LGNetAudioData * audio)
{
  const size_t wireSize = lgNetAudioDataSize(audio);
  if (!data || !wireSize || size < wireSize || !lgNetAudioDataValid(audio))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, audio->streamID)       &&
    lgNetWriterU16(&writer, audio->direction)      &&
    lgNetWriterU16(&writer, audio->flags)          &&
    lgNetWriterU64(&writer, audio->formatEpoch)    &&
    lgNetWriterU64(&writer, audio->packetID)       &&
    lgNetWriterU64(&writer, audio->timestampNs)    &&
    lgNetWriterU64(&writer, audio->startFrame)     &&
    lgNetWriterU32(&writer, audio->frameCount)     &&
    lgNetWriterU32(&writer, audio->dataLength)     &&
    lgNetWriterBytes(&writer, audio->data, audio->dataLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetAudioDataDecode(
    LGNetAudioData * audio, const void * data, size_t size)
{
  if (!audio || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetAudioData decoded;
  LGNetReader    reader;
  size_t         expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)    ||
      !lgNetReaderU16(&reader, &decoded.direction)   ||
      !lgNetReaderU16(&reader, &decoded.flags)       ||
      !lgNetReaderU64(&reader, &decoded.formatEpoch) ||
      !lgNetReaderU64(&reader, &decoded.packetID)    ||
      !lgNetReaderU64(&reader, &decoded.timestampNs) ||
      !lgNetReaderU64(&reader, &decoded.startFrame)  ||
      !lgNetReaderU32(&reader, &decoded.frameCount)  ||
      !lgNetReaderU32(&reader, &decoded.dataLength))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE, decoded.dataLength,
    LG_NET_AUDIO_MAX_DATA_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.data, decoded.dataLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetAudioDataValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *audio = decoded;
  return LG_NET_PARSE_OK;
}

static bool audioStateKnown(LGNetAudioStateCode state)
{
  return state >= LG_NET_AUDIO_STATE_STOPPED &&
    state <= LG_NET_AUDIO_STATE_ERROR;
}

bool lgNetAudioStateValid(const LGNetAudioState * state)
{
  return state && state->streamID &&
    audioDirectionKnown(state->direction) && audioStateKnown(state->state) &&
    state->stateSequence && !(state->flags & ~AUDIO_STATE_FLAGS) &&
    (!(state->flags & LG_NET_AUDIO_STATE_FORMAT_ACTIVE) || state->formatEpoch);
}

bool lgNetAudioStateEncode(
    void * data, size_t size, const LGNetAudioState * state)
{
  if (!data || size < LG_NET_AUDIO_STATE_WIRE_SIZE ||
      !lgNetAudioStateValid(state))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, state->streamID)         &&
    lgNetWriterU16(&writer, state->direction)        &&
    lgNetWriterU16(&writer, state->state)            &&
    lgNetWriterU64(&writer, state->stateSequence)    &&
    lgNetWriterU64(&writer, state->formatEpoch)      &&
    lgNetWriterI32(&writer, state->volumeMillibels)  &&
    lgNetWriterU32(&writer, state->flags)            &&
    lgNetWriterSize(&writer) == LG_NET_AUDIO_STATE_WIRE_SIZE;
}

LGNetParseResult lgNetAudioStateDecode(
    LGNetAudioState * state, const void * data, size_t size)
{
  if (!state || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_AUDIO_STATE_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetAudioState decoded;
  LGNetReader     reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)        ||
      !lgNetReaderU16(&reader, &decoded.direction)       ||
      !lgNetReaderU16(&reader, &decoded.state)           ||
      !lgNetReaderU64(&reader, &decoded.stateSequence)   ||
      !lgNetReaderU64(&reader, &decoded.formatEpoch)     ||
      !lgNetReaderI32(&reader, &decoded.volumeMillibels) ||
      !lgNetReaderU32(&reader, &decoded.flags))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_AUDIO_STATE_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetAudioStateValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *state = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetAudioClockFeedbackValid(
    const LGNetAudioClockFeedback * feedback)
{
  return feedback && feedback->streamID &&
    audioDirectionKnown(feedback->direction) && feedback->formatEpoch &&
    feedback->packetID && feedback->timestampNs;
}

bool lgNetAudioClockFeedbackEncode(void * data, size_t size,
    const LGNetAudioClockFeedback * feedback)
{
  if (!data || size < LG_NET_AUDIO_CLOCK_FEEDBACK_WIRE_SIZE ||
      !lgNetAudioClockFeedbackValid(feedback))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU32(&writer, feedback->streamID)      &&
    lgNetWriterU16(&writer, feedback->direction)     &&
    lgNetWriterZero(&writer, 2)                      &&
    lgNetWriterU64(&writer, feedback->formatEpoch)   &&
    lgNetWriterU64(&writer, feedback->packetID)      &&
    lgNetWriterU64(&writer, feedback->framePosition) &&
    lgNetWriterU64(&writer, feedback->timestampNs)   &&
    lgNetWriterI32(&writer, feedback->queuedFrames)  &&
    lgNetWriterI32(&writer, feedback->driftPpm)      &&
    lgNetWriterSize(&writer) == LG_NET_AUDIO_CLOCK_FEEDBACK_WIRE_SIZE;
}

LGNetParseResult lgNetAudioClockFeedbackDecode(
    LGNetAudioClockFeedback * feedback, const void * data, size_t size)
{
  if (!feedback || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_AUDIO_CLOCK_FEEDBACK_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetAudioClockFeedback decoded;
  LGNetReader             reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU32(&reader, &decoded.streamID)      ||
      !lgNetReaderU16(&reader, &decoded.direction)     ||
      !lgNetReaderZero(&reader, 2)                     ||
      !lgNetReaderU64(&reader, &decoded.formatEpoch)   ||
      !lgNetReaderU64(&reader, &decoded.packetID)      ||
      !lgNetReaderU64(&reader, &decoded.framePosition) ||
      !lgNetReaderU64(&reader, &decoded.timestampNs)   ||
      !lgNetReaderI32(&reader, &decoded.queuedFrames)  ||
      !lgNetReaderI32(&reader, &decoded.driftPpm))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_AUDIO_CLOCK_FEEDBACK_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetAudioClockFeedbackValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *feedback = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetClipboardClaimValid(const LGNetClipboardClaim * claim)
{
  return claim && claim->ownerID && claim->claimEpoch && claim->leaseMs &&
    claim->flags && !(claim->flags & ~CLIPBOARD_CLAIM_FLAGS);
}

bool lgNetClipboardClaimEncode(
    void * data, size_t size, const LGNetClipboardClaim * claim)
{
  if (!data || size < LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE ||
      !lgNetClipboardClaimValid(claim))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, claim->ownerID)    &&
    lgNetWriterU64(&writer, claim->claimEpoch) &&
    lgNetWriterU32(&writer, claim->leaseMs)    &&
    lgNetWriterU32(&writer, claim->flags)      &&
    lgNetWriterSize(&writer) == LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE;
}

LGNetParseResult lgNetClipboardClaimDecode(
    LGNetClipboardClaim * claim, const void * data, size_t size)
{
  if (!claim || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetClipboardClaim decoded;
  LGNetReader         reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.ownerID)    ||
      !lgNetReaderU64(&reader, &decoded.claimEpoch) ||
      !lgNetReaderU32(&reader, &decoded.leaseMs)    ||
      !lgNetReaderU32(&reader, &decoded.flags))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetClipboardClaimValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *claim = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetClipboardOfferSize(const LGNetClipboardOffer * offer)
{
  size_t size;
  return offer && variableSize(LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE,
    offer->mimeLength, LG_NET_CLIPBOARD_MAX_MIME_LENGTH, &size) ? size : 0;
}

bool lgNetClipboardOfferValid(const LGNetClipboardOffer * offer)
{
  return offer && offer->offerID && offer->serial && offer->formatCount &&
    offer->formatCount <= LG_NET_CLIPBOARD_MAX_FORMATS &&
    offer->formatIndex < offer->formatCount && offer->mimeLength &&
    offer->mime && !(offer->flags & ~CLIPBOARD_OFFER_FLAGS) &&
    lgNetClipboardOfferSize(offer) != 0;
}

bool lgNetClipboardOfferEncode(
    void * data, size_t size, const LGNetClipboardOffer * offer)
{
  const size_t wireSize = lgNetClipboardOfferSize(offer);
  if (!data || !wireSize || size < wireSize ||
      !lgNetClipboardOfferValid(offer))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, offer->offerID)        &&
    lgNetWriterU64(&writer, offer->serial)         &&
    lgNetWriterU64(&writer, offer->estimatedSize)  &&
    lgNetWriterU32(&writer, offer->formatIndex)    &&
    lgNetWriterU32(&writer, offer->formatCount)    &&
    lgNetWriterU16(&writer, offer->mimeLength)     &&
    lgNetWriterU16(&writer, offer->flags)          &&
    lgNetWriterU32(&writer, 0)                     &&
    lgNetWriterBytes(&writer, offer->mime, offer->mimeLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetClipboardOfferDecode(
    LGNetClipboardOffer * offer, const void * data, size_t size)
{
  if (!offer || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetClipboardOffer decoded;
  LGNetReader         reader;
  size_t              expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.offerID)       ||
      !lgNetReaderU64(&reader, &decoded.serial)        ||
      !lgNetReaderU64(&reader, &decoded.estimatedSize) ||
      !lgNetReaderU32(&reader, &decoded.formatIndex)   ||
      !lgNetReaderU32(&reader, &decoded.formatCount)   ||
      !lgNetReaderU16(&reader, &decoded.mimeLength)    ||
      !lgNetReaderU16(&reader, &decoded.flags)         ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE, decoded.mimeLength,
    LG_NET_CLIPBOARD_MAX_MIME_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.mime, decoded.mimeLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetClipboardOfferValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *offer = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetClipboardRequestValid(const LGNetClipboardRequest * request)
{
  return request && request->offerID && request->requestID &&
    request->formatIndex < LG_NET_CLIPBOARD_MAX_FORMATS &&
    request->maxChunkLength &&
    request->maxChunkLength <= LG_NET_CLIPBOARD_MAX_CHUNK_LENGTH;
}

bool lgNetClipboardRequestEncode(
    void * data, size_t size, const LGNetClipboardRequest * request)
{
  if (!data || size < LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE ||
      !lgNetClipboardRequestValid(request))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, request->offerID)        &&
    lgNetWriterU64(&writer, request->requestID)      &&
    lgNetWriterU32(&writer, request->formatIndex)    &&
    lgNetWriterU32(&writer, request->maxChunkLength) &&
    lgNetWriterU64(&writer, request->offset)         &&
    lgNetWriterSize(&writer) == LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE;
}

LGNetParseResult lgNetClipboardRequestDecode(
    LGNetClipboardRequest * request, const void * data, size_t size)
{
  if (!request || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetClipboardRequest decoded;
  LGNetReader           reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.offerID)        ||
      !lgNetReaderU64(&reader, &decoded.requestID)      ||
      !lgNetReaderU32(&reader, &decoded.formatIndex)    ||
      !lgNetReaderU32(&reader, &decoded.maxChunkLength) ||
      !lgNetReaderU64(&reader, &decoded.offset))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetClipboardRequestValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *request = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetClipboardChunkSize(const LGNetClipboardChunk * chunk)
{
  size_t size;
  return chunk && variableSize(LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE,
    chunk->dataLength, LG_NET_CLIPBOARD_MAX_CHUNK_LENGTH, &size) ? size : 0;
}

bool lgNetClipboardChunkValid(const LGNetClipboardChunk * chunk)
{
  if (!chunk || !chunk->requestID || !chunk->offerID ||
      (chunk->flags & ~CLIPBOARD_CHUNK_FLAGS) ||
      lgNetClipboardChunkSize(chunk) == 0)
    return false;

  if (chunk->offset > chunk->totalLength ||
      chunk->dataLength > chunk->totalLength - chunk->offset)
    return false;

  const bool first = chunk->offset == 0;
  const bool final = chunk->dataLength == chunk->totalLength - chunk->offset;
  return (!!(chunk->flags & LG_NET_CLIPBOARD_CHUNK_FIRST) == first) &&
    (!!(chunk->flags & LG_NET_CLIPBOARD_CHUNK_FINAL) == final) &&
    (chunk->totalLength ? chunk->dataLength && chunk->data :
      !chunk->dataLength && !chunk->data);
}

bool lgNetClipboardChunkEncode(
    void * data, size_t size, const LGNetClipboardChunk * chunk)
{
  const size_t wireSize = lgNetClipboardChunkSize(chunk);
  if (!data || !wireSize || size < wireSize ||
      !lgNetClipboardChunkValid(chunk))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, chunk->requestID)   &&
    lgNetWriterU64(&writer, chunk->offerID)     &&
    lgNetWriterU64(&writer, chunk->offset)      &&
    lgNetWriterU64(&writer, chunk->totalLength) &&
    lgNetWriterU32(&writer, chunk->dataLength)  &&
    lgNetWriterU16(&writer, chunk->flags)       &&
    lgNetWriterZero(&writer, 2)                 &&
    lgNetWriterBytes(&writer, chunk->data, chunk->dataLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetClipboardChunkDecode(
    LGNetClipboardChunk * chunk, const void * data, size_t size)
{
  if (!chunk || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetClipboardChunk decoded;
  LGNetReader         reader;
  size_t              expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.requestID)   ||
      !lgNetReaderU64(&reader, &decoded.offerID)     ||
      !lgNetReaderU64(&reader, &decoded.offset)      ||
      !lgNetReaderU64(&reader, &decoded.totalLength) ||
      !lgNetReaderU32(&reader, &decoded.dataLength)  ||
      !lgNetReaderU16(&reader, &decoded.flags)       ||
      !lgNetReaderZero(&reader, 2))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, decoded.dataLength,
    LG_NET_CLIPBOARD_MAX_CHUNK_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.data, decoded.dataLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetClipboardChunkValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *chunk = decoded;
  return LG_NET_PARSE_OK;
}

static bool clipboardStatusKnown(LGNetClipboardStatusCode status)
{
  return status >= LG_NET_CLIPBOARD_STATUS_OK &&
    status <= LG_NET_CLIPBOARD_STATUS_ERROR;
}

bool lgNetClipboardStatusValid(const LGNetClipboardStatus * status)
{
  return status && status->requestID && status->offerID &&
    clipboardStatusKnown(status->status);
}

bool lgNetClipboardStatusEncode(
    void * data, size_t size, const LGNetClipboardStatus * status)
{
  if (!data || size < LG_NET_CLIPBOARD_STATUS_WIRE_SIZE ||
      !lgNetClipboardStatusValid(status))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, status->requestID)       &&
    lgNetWriterU64(&writer, status->offerID)         &&
    lgNetWriterU32(&writer, status->status)          &&
    lgNetWriterU32(&writer, status->detail)          &&
    lgNetWriterU64(&writer, status->processedLength) &&
    lgNetWriterSize(&writer) == LG_NET_CLIPBOARD_STATUS_WIRE_SIZE;
}

LGNetParseResult lgNetClipboardStatusDecode(
    LGNetClipboardStatus * status, const void * data, size_t size)
{
  if (!status || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_CLIPBOARD_STATUS_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetClipboardStatus decoded;
  LGNetReader          reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.requestID)       ||
      !lgNetReaderU64(&reader, &decoded.offerID)         ||
      !lgNetReaderU32(&reader, &decoded.status)          ||
      !lgNetReaderU32(&reader, &decoded.detail)          ||
      !lgNetReaderU64(&reader, &decoded.processedLength))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_CLIPBOARD_STATUS_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetClipboardStatusValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *status = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetFileOfferSize(const LGNetFileOffer * offer)
{
  size_t size;
  return offer && variableSize(LG_NET_FILE_OFFER_HEADER_WIRE_SIZE,
    offer->labelLength, LG_NET_FILE_MAX_LABEL_LENGTH, &size) ? size : 0;
}

bool lgNetFileOfferValid(const LGNetFileOffer * offer)
{
  const LGNetFileOfferFlags operation = offer ? offer->flags &
    (LG_NET_FILE_OFFER_COPY | LG_NET_FILE_OFFER_MOVE) : 0;
  return offer && offer->offerID && offer->entryCount &&
    offer->entryCount <= LG_NET_FILE_MAX_ENTRIES &&
    !(offer->flags & ~FILE_OFFER_FLAGS) &&
    (operation == LG_NET_FILE_OFFER_COPY ||
      operation == LG_NET_FILE_OFFER_MOVE) &&
    (!offer->labelLength || offer->label) && lgNetFileOfferSize(offer) != 0;
}

bool lgNetFileOfferEncode(
    void * data, size_t size, const LGNetFileOffer * offer)
{
  const size_t wireSize = lgNetFileOfferSize(offer);
  if (!data || !wireSize || size < wireSize || !lgNetFileOfferValid(offer))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, offer->offerID)       &&
    lgNetWriterU64(&writer, offer->totalBytes)    &&
    lgNetWriterU32(&writer, offer->entryCount)    &&
    lgNetWriterU32(&writer, offer->flags)         &&
    lgNetWriterU16(&writer, offer->labelLength)   &&
    lgNetWriterZero(&writer, 6)                   &&
    lgNetWriterBytes(&writer, offer->label, offer->labelLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetFileOfferDecode(
    LGNetFileOffer * offer, const void * data, size_t size)
{
  if (!offer || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_FILE_OFFER_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetFileOffer decoded;
  LGNetReader    reader;
  size_t         expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.offerID)     ||
      !lgNetReaderU64(&reader, &decoded.totalBytes)  ||
      !lgNetReaderU32(&reader, &decoded.entryCount)  ||
      !lgNetReaderU32(&reader, &decoded.flags)       ||
      !lgNetReaderU16(&reader, &decoded.labelLength) ||
      !lgNetReaderZero(&reader, 6))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_FILE_OFFER_HEADER_WIRE_SIZE, decoded.labelLength,
    LG_NET_FILE_MAX_LABEL_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.label, decoded.labelLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetFileOfferValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *offer = decoded;
  return LG_NET_PARSE_OK;
}

static bool fileEntryTypeKnown(LGNetFileEntryType type)
{
  return type >= LG_NET_FILE_ENTRY_REGULAR &&
    type <= LG_NET_FILE_ENTRY_SYMLINK;
}

size_t lgNetFileEntrySize(const LGNetFileEntry * entry)
{
  size_t variableLength;
  size_t size;
  if (!entry || entry->pathLength > LG_NET_FILE_MAX_PATH_LENGTH ||
      entry->linkTargetLength > LG_NET_FILE_MAX_LINK_TARGET_LENGTH)
    return 0;

  variableLength = (size_t)entry->pathLength + entry->linkTargetLength;
  return variableSize(LG_NET_FILE_ENTRY_HEADER_WIRE_SIZE, variableLength,
    LG_NET_FILE_MAX_PATH_LENGTH + LG_NET_FILE_MAX_LINK_TARGET_LENGTH,
    &size) ? size : 0;
}

bool lgNetFileEntryValid(const LGNetFileEntry * entry)
{
  return entry && entry->offerID && entry->entryID &&
    fileEntryTypeKnown(entry->type) && !(entry->flags & ~FILE_ENTRY_FLAGS) &&
    entry->pathLength && entry->pathLength <= LG_NET_FILE_MAX_PATH_LENGTH &&
    entry->path &&
    entry->linkTargetLength <= LG_NET_FILE_MAX_LINK_TARGET_LENGTH &&
    ((entry->type == LG_NET_FILE_ENTRY_SYMLINK) ?
      entry->linkTargetLength && entry->linkTarget :
      !entry->linkTargetLength && !entry->linkTarget) &&
    lgNetFileEntrySize(entry) != 0;
}

bool lgNetFileEntryEncode(
    void * data, size_t size, const LGNetFileEntry * entry)
{
  const size_t wireSize = lgNetFileEntrySize(entry);
  if (!data || !wireSize || size < wireSize || !lgNetFileEntryValid(entry))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, entry->offerID)          &&
    lgNetWriterU64(&writer, entry->entryID)          &&
    lgNetWriterU64(&writer, entry->parentID)         &&
    lgNetWriterU64(&writer, entry->size)             &&
    lgNetWriterU64(&writer, entry->modifiedNs)       &&
    lgNetWriterU32(&writer, entry->mode)             &&
    lgNetWriterU16(&writer, entry->type)             &&
    lgNetWriterU16(&writer, entry->flags)            &&
    lgNetWriterU16(&writer, entry->pathLength)       &&
    lgNetWriterU16(&writer, entry->linkTargetLength) &&
    lgNetWriterZero(&writer, 4)                      &&
    lgNetWriterBytes(&writer, entry->path, entry->pathLength) &&
    lgNetWriterBytes(&writer, entry->linkTarget,
      entry->linkTargetLength)                       &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetFileEntryDecode(
    LGNetFileEntry * entry, const void * data, size_t size)
{
  if (!entry || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_FILE_ENTRY_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetFileEntry decoded;
  LGNetReader    reader;
  size_t         expected;
  size_t         variableLength;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.offerID)          ||
      !lgNetReaderU64(&reader, &decoded.entryID)          ||
      !lgNetReaderU64(&reader, &decoded.parentID)         ||
      !lgNetReaderU64(&reader, &decoded.size)             ||
      !lgNetReaderU64(&reader, &decoded.modifiedNs)       ||
      !lgNetReaderU32(&reader, &decoded.mode)             ||
      !lgNetReaderU16(&reader, &decoded.type)             ||
      !lgNetReaderU16(&reader, &decoded.flags)            ||
      !lgNetReaderU16(&reader, &decoded.pathLength)       ||
      !lgNetReaderU16(&reader, &decoded.linkTargetLength) ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  if (decoded.pathLength > LG_NET_FILE_MAX_PATH_LENGTH ||
      decoded.linkTargetLength > LG_NET_FILE_MAX_LINK_TARGET_LENGTH)
    return LG_NET_PARSE_INVALID_LENGTH;
  variableLength = (size_t)decoded.pathLength + decoded.linkTargetLength;
  LGNetParseResult result = variableDecodeSize(
    LG_NET_FILE_ENTRY_HEADER_WIRE_SIZE, variableLength,
    LG_NET_FILE_MAX_PATH_LENGTH + LG_NET_FILE_MAX_LINK_TARGET_LENGTH,
    size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.path, decoded.pathLength) ||
      !lgNetReaderView(&reader, &decoded.linkTarget,
        decoded.linkTargetLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetFileEntryValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *entry = decoded;
  return LG_NET_PARSE_OK;
}

bool lgNetFileRequestValid(const LGNetFileRequest * request)
{
  return request && request->requestID && request->offerID &&
    request->entryID && request->length &&
    request->length <= LG_NET_FILE_MAX_CHUNK_LENGTH && request->flags &&
    !(request->flags & ~FILE_REQUEST_FLAGS) &&
    request->offset <= UINT64_MAX - request->length;
}

bool lgNetFileRequestEncode(
    void * data, size_t size, const LGNetFileRequest * request)
{
  if (!data || size < LG_NET_FILE_REQUEST_WIRE_SIZE ||
      !lgNetFileRequestValid(request))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, request->requestID) &&
    lgNetWriterU64(&writer, request->offerID)   &&
    lgNetWriterU64(&writer, request->entryID)   &&
    lgNetWriterU64(&writer, request->offset)    &&
    lgNetWriterU32(&writer, request->length)    &&
    lgNetWriterU32(&writer, request->flags)     &&
    lgNetWriterSize(&writer) == LG_NET_FILE_REQUEST_WIRE_SIZE;
}

LGNetParseResult lgNetFileRequestDecode(
    LGNetFileRequest * request, const void * data, size_t size)
{
  if (!request || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_FILE_REQUEST_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetFileRequest decoded;
  LGNetReader      reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.requestID) ||
      !lgNetReaderU64(&reader, &decoded.offerID)   ||
      !lgNetReaderU64(&reader, &decoded.entryID)   ||
      !lgNetReaderU64(&reader, &decoded.offset)    ||
      !lgNetReaderU32(&reader, &decoded.length)    ||
      !lgNetReaderU32(&reader, &decoded.flags))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_FILE_REQUEST_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetFileRequestValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *request = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetFileChunkSize(const LGNetFileChunk * chunk)
{
  size_t size;
  return chunk && variableSize(LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE,
    chunk->dataLength, LG_NET_FILE_MAX_CHUNK_LENGTH, &size) ? size : 0;
}

bool lgNetFileChunkValid(const LGNetFileChunk * chunk)
{
  if (!chunk || !chunk->requestID || !chunk->offerID || !chunk->entryID ||
      (chunk->flags & ~FILE_CHUNK_FLAGS) || lgNetFileChunkSize(chunk) == 0 ||
      chunk->offset > chunk->totalLength ||
      chunk->dataLength > chunk->totalLength - chunk->offset)
    return false;

  const bool first = chunk->offset == 0;
  const bool final = chunk->dataLength == chunk->totalLength - chunk->offset;
  return (!!(chunk->flags & LG_NET_FILE_CHUNK_FIRST) == first) &&
    (!!(chunk->flags & LG_NET_FILE_CHUNK_FINAL) == final) &&
    (chunk->totalLength ? chunk->dataLength && chunk->data :
      !chunk->dataLength && !chunk->data);
}

bool lgNetFileChunkEncode(
    void * data, size_t size, const LGNetFileChunk * chunk)
{
  const size_t wireSize = lgNetFileChunkSize(chunk);
  if (!data || !wireSize || size < wireSize || !lgNetFileChunkValid(chunk))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, chunk->requestID)   &&
    lgNetWriterU64(&writer, chunk->offerID)     &&
    lgNetWriterU64(&writer, chunk->entryID)     &&
    lgNetWriterU64(&writer, chunk->offset)      &&
    lgNetWriterU64(&writer, chunk->totalLength) &&
    lgNetWriterU32(&writer, chunk->dataLength)  &&
    lgNetWriterU16(&writer, chunk->flags)       &&
    lgNetWriterZero(&writer, 2)                 &&
    lgNetWriterBytes(&writer, chunk->data, chunk->dataLength) &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetFileChunkDecode(
    LGNetFileChunk * chunk, const void * data, size_t size)
{
  if (!chunk || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetFileChunk decoded;
  LGNetReader    reader;
  size_t         expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.requestID)   ||
      !lgNetReaderU64(&reader, &decoded.offerID)     ||
      !lgNetReaderU64(&reader, &decoded.entryID)     ||
      !lgNetReaderU64(&reader, &decoded.offset)      ||
      !lgNetReaderU64(&reader, &decoded.totalLength) ||
      !lgNetReaderU32(&reader, &decoded.dataLength)  ||
      !lgNetReaderU16(&reader, &decoded.flags)       ||
      !lgNetReaderZero(&reader, 2))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE, decoded.dataLength,
    LG_NET_FILE_MAX_CHUNK_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.data, decoded.dataLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetFileChunkValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *chunk = decoded;
  return LG_NET_PARSE_OK;
}

static bool fileStatusKnown(LGNetFileStatusCode status)
{
  return status >= LG_NET_FILE_STATUS_OK &&
    status <= LG_NET_FILE_STATUS_ERROR;
}

bool lgNetFileStatusValid(const LGNetFileStatus * status)
{
  return status && status->requestID && status->offerID && status->entryID &&
    fileStatusKnown(status->status);
}

bool lgNetFileStatusEncode(
    void * data, size_t size, const LGNetFileStatus * status)
{
  if (!data || size < LG_NET_FILE_STATUS_WIRE_SIZE ||
      !lgNetFileStatusValid(status))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU64(&writer, status->requestID)         &&
    lgNetWriterU64(&writer, status->offerID)           &&
    lgNetWriterU64(&writer, status->entryID)           &&
    lgNetWriterU32(&writer, status->status)            &&
    lgNetWriterU32(&writer, status->detail)            &&
    lgNetWriterU64(&writer, status->transferredLength) &&
    lgNetWriterSize(&writer) == LG_NET_FILE_STATUS_WIRE_SIZE;
}

LGNetParseResult lgNetFileStatusDecode(
    LGNetFileStatus * status, const void * data, size_t size)
{
  if (!status || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_FILE_STATUS_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetFileStatus decoded;
  LGNetReader     reader;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU64(&reader, &decoded.requestID)         ||
      !lgNetReaderU64(&reader, &decoded.offerID)           ||
      !lgNetReaderU64(&reader, &decoded.entryID)           ||
      !lgNetReaderU32(&reader, &decoded.status)            ||
      !lgNetReaderU32(&reader, &decoded.detail)            ||
      !lgNetReaderU64(&reader, &decoded.transferredLength))
    return LG_NET_PARSE_INVALID_VALUE;

  const LGNetParseResult result = fixedDecodeResult(
    &reader, LG_NET_FILE_STATUS_WIRE_SIZE, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetFileStatusValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *status = decoded;
  return LG_NET_PARSE_OK;
}

size_t lgNetUSBReservedSize(const LGNetUSBReserved * reserved)
{
  size_t size;
  return reserved && variableSize(LG_NET_USB_RESERVED_HEADER_WIRE_SIZE,
    reserved->payloadLength, LG_NET_USB_MAX_RESERVED_PAYLOAD_LENGTH,
    &size) ? size : 0;
}

bool lgNetUSBReservedValid(const LGNetUSBReserved * reserved)
{
  return reserved &&
    reserved->version == LG_NET_USB_RESERVED_INTRODUCED_SERVICE_VERSION &&
    reserved->headerSize == LG_NET_USB_RESERVED_HEADER_WIRE_SIZE &&
    reserved->messageType && !(reserved->flags & ~USB_RESERVED_FLAGS) &&
    (!reserved->payloadLength || reserved->payload) &&
    lgNetUSBReservedSize(reserved) != 0;
}

bool lgNetUSBReservedEncode(
    void * data, size_t size, const LGNetUSBReserved * reserved)
{
  const size_t wireSize = lgNetUSBReservedSize(reserved);
  if (!data || !wireSize || size < wireSize ||
      !lgNetUSBReservedValid(reserved))
    return false;

  LGNetWriter writer;
  lgNetWriterInit(&writer, data, size);
  return
    lgNetWriterU16(&writer, reserved->version)       &&
    lgNetWriterU16(&writer, reserved->headerSize)    &&
    lgNetWriterU16(&writer, reserved->messageType)   &&
    lgNetWriterU16(&writer, reserved->flags)         &&
    lgNetWriterU64(&writer, reserved->requestID)     &&
    lgNetWriterU64(&writer, reserved->sequence)      &&
    lgNetWriterU32(&writer, reserved->payloadLength) &&
    lgNetWriterZero(&writer, 4)                      &&
    lgNetWriterBytes(&writer, reserved->payload,
      reserved->payloadLength)                       &&
    lgNetWriterSize(&writer) == wireSize;
}

LGNetParseResult lgNetUSBReservedDecode(
    LGNetUSBReserved * reserved, const void * data, size_t size)
{
  if (!reserved || !data)
    return LG_NET_PARSE_INVALID_VALUE;
  if (size < LG_NET_USB_RESERVED_HEADER_WIRE_SIZE)
    return LG_NET_PARSE_TRUNCATED;

  LGNetUSBReserved decoded;
  LGNetReader      reader;
  size_t           expected;
  memset(&decoded, 0, sizeof(decoded));
  lgNetReaderInit(&reader, data, size);
  if (!lgNetReaderU16(&reader, &decoded.version)       ||
      !lgNetReaderU16(&reader, &decoded.headerSize)    ||
      !lgNetReaderU16(&reader, &decoded.messageType)   ||
      !lgNetReaderU16(&reader, &decoded.flags)         ||
      !lgNetReaderU64(&reader, &decoded.requestID)     ||
      !lgNetReaderU64(&reader, &decoded.sequence)      ||
      !lgNetReaderU32(&reader, &decoded.payloadLength) ||
      !lgNetReaderZero(&reader, 4))
    return LG_NET_PARSE_INVALID_VALUE;

  LGNetParseResult result = variableDecodeSize(
    LG_NET_USB_RESERVED_HEADER_WIRE_SIZE, decoded.payloadLength,
    LG_NET_USB_MAX_RESERVED_PAYLOAD_LENGTH, size, &expected);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetReaderView(&reader, &decoded.payload, decoded.payloadLength))
    return LG_NET_PARSE_INVALID_VALUE;
  result = fixedDecodeResult(&reader, expected, size);
  if (result != LG_NET_PARSE_OK)
    return result;
  if (!lgNetUSBReservedValid(&decoded))
    return LG_NET_PARSE_INVALID_VALUE;

  *reserved = decoded;
  return LG_NET_PARSE_OK;
}
