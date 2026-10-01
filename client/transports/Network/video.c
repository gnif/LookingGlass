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

#include "video.h"

#include "video/decoder.h"
#include "video/pyrowave.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <LGProtocol/NetworkCRC32C.h>
#include <LGProtocol/NetworkServices.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_VIDEO_TIMER_MS                 10U
#define NETWORK_VIDEO_SUBSCRIBE_RETRY_MS       500U
#define NETWORK_VIDEO_FEEDBACK_INTERVAL_MS     16U
#define NETWORK_VIDEO_KEYFRAME_RETRY_MS        100U
#define NETWORK_VIDEO_MAX_WIDTH                8192U
#define NETWORK_VIDEO_MAX_HEIGHT               8192U
#define NETWORK_VIDEO_MAX_FRAME_LENGTH         \
  LG_NET_VIDEO_MAX_FRAME_LENGTH
#define NETWORK_VIDEO_MAX_FRAGMENTS            LG_NET_VIDEO_MAX_FRAGMENTS
#define NETWORK_VIDEO_MAX_BLOCKS               LG_NET_VIDEO_MAX_BLOCKS
#define NETWORK_VIDEO_MAX_FRAME_LATENCY_MS     50U
#define NETWORK_VIDEO_ASSEMBLY_STALL_MS        2000U
#define NETWORK_VIDEO_MAX_FRAME_RATE           UINT32_MAX
#define NETWORK_VIDEO_SCHEDULE_CACHE           8U
#define NETWORK_VIDEO_MAX_LEASES               8U
#define NETWORK_VIDEO_PYROWAVE_HEADER_SIZE     8U
#define NETWORK_VIDEO_PYROWAVE_COUNT_MASK      UINT32_C(0x00FFFFFF)

#define NETWORK_VIDEO_FORBIDDEN_FLAGS \
  (LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_ERROR | \
   LG_NET_ENVELOPE_EARLY_DATA)

struct NetworkVideoBlockCoverage
{
  uint32_t base;
  uint32_t length;
  uint32_t received;
  bool     known;
  bool     start;
  bool     end;
};

struct NetworkVideoAssembly
{
  bool     active;
  bool     sparseAllowed;
  uint64_t generation;
  uint64_t configEpoch;
  uint64_t frameID;
  uint64_t firstReceiveNs;
  uint64_t deadlineNs;
  uint32_t frameLength;
  uint32_t fragmentCount;
  uint32_t blockCount;
  uint32_t receivedBytes;

  uint8_t  * data;
  uint64_t * coverage;
  size_t     coverageWords;
  uint64_t * fragmentSeen;
  uint32_t * fragmentOffsets;
  uint32_t * fragmentLengths;
  uint32_t * fragmentBlocks;

  struct NetworkVideoBlockCoverage * blocks;

  bool               scheduleValid;
  LGNetVideoSchedule schedule;
};

struct NetworkVideoScheduleEntry
{
  bool               valid;
  LGNetVideoSchedule schedule;
};

struct NetworkVideoOutput
{
  struct NetworkVideoOutput * next;

  NetworkVideoDecodedFrame decoded;
  uint64_t                 frameID;
  uint64_t                 handle;
  uint64_t                 epoch;
  uint64_t                 timestamp;
  uint64_t                 captureTimestampNs;
  uint64_t                 presentationTimestampNs;
  uint64_t                 receiveTimeNs;
  bool                     releaseRequested;
};

struct NetworkVideoEncoded
{
  LGNetVideoStreamConfig config;
  const uint8_t        * data;
  uint8_t              * ownedData;
  uint32_t               length;
  uint32_t               blockCount;
  uint64_t               generation;
  uint64_t               frameID;
  uint64_t               captureTimestampNs;
  uint64_t               presentationTimestampNs;
  uint64_t               receiveTimeNs;
  LGNetVideoFrameFlags   flags;
  bool                   complete;
  bool                   scheduleValid;
  LGNetVideoSchedule     schedule;
};

struct NetworkVideo
{
  NetworkConnection   * connection;
  NetworkVideoDecoder * decoder;
  LGTimer             * timer;
  LGEvent             * frameEvent;
  LGEvent             * decodeEvent;
  LGThread            * decodeThread;
  LG_Lock               stateLock;
  LG_Lock               callbackLock;

  bool     destroying;
  bool     connected;
  bool     serviceAvailable;
  bool     datagramsAvailable;
  bool     rendererAttached;
  bool     wireConfigured;
  bool     decoderReady;
  bool     subscribePending;
  bool     waitCancelled;
  bool     decodeStopping;
  bool     decodePending;
  uint16_t serviceVersion;

  uint64_t subscriberID;
  uint64_t requestSerial;
  uint64_t subscribeRequestID;
  uint64_t nextSubscribeNs;
  uint64_t nextFeedbackNs;
  uint64_t nextKeyframeNs;

  LGNetVideoStreamConfig config;
  uint32_t               localConfigGeneration;
  uint64_t               decodeGeneration;
  uint64_t               highestSeenFrameID;
  uint64_t               lastCompleteFrameID;
  uint64_t               lastDecodedFrameID;
  uint64_t               statusSequence;
  uint32_t               lostFrames;
  uint32_t               lostFragments;

  LGNetVideoFeedbackFlags feedbackFlags;
  bool                    feedbackPending;
  bool                    keyframePending;
  LGNetVideoControlReason keyframeReason;
  uint64_t                keyframeFrameID;

  struct NetworkVideoAssembly      assembly;
  struct NetworkVideoEncoded       pendingDecode;
  struct NetworkVideoScheduleEntry schedules[NETWORK_VIDEO_SCHEDULE_CACHE];
  struct NetworkVideoOutput      * output;
  struct NetworkVideoOutput      * leases;
  uint64_t                         leaseSerial;
  uint32_t                         leaseCount;

  LG_VideoComponentStatus  status;
  NetworkVideoStatusFn     statusCallback;
  void                   * statusOpaque;
};

enum NetworkVideoCoverageState
{
  NETWORK_VIDEO_COVERAGE_EMPTY,
  NETWORK_VIDEO_COVERAGE_FULL,
  NETWORK_VIDEO_COVERAGE_PARTIAL,
};

static uint64_t networkVideoNext64(uint64_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint32_t networkVideoSaturatingIncrement(uint32_t value)
{
  return value == UINT32_MAX ? value : value + 1U;
}

/* Frame latency remains the datagram presentation deadline. Reliable frame
 * assembly instead expires after this period without receiving new data. */
static uint64_t networkVideoAssemblyDeadline(uint64_t now)
{
  return now + (uint64_t)NETWORK_VIDEO_ASSEMBLY_STALL_MS *
    UINT64_C(1000000);
}

static uint32_t networkVideoReadU32(const uint8_t * data)
{
  return (uint32_t)data[0]        |
    (uint32_t)data[1] << 8        |
    (uint32_t)data[2] << 16       |
    (uint32_t)data[3] << 24;
}

static void networkVideoWriteU32(uint8_t * data, uint32_t value)
{
  data[0] = (uint8_t)value;
  data[1] = (uint8_t)(value >> 8);
  data[2] = (uint8_t)(value >> 16);
  data[3] = (uint8_t)(value >> 24);
}

static bool networkVideoVersionSupported(uint16_t version)
{
  return version >= LG_NET_VIDEO_BLOCK_FRAGMENTATION_INTRODUCED_SERVICE_VERSION &&
    version <= LG_NET_VIDEO_VERSION_CURRENT;
}

static bool networkVideoConfigEqual(
    const LGNetVideoStreamConfig * left,
    const LGNetVideoStreamConfig * right)
{
  return left->streamID == right->streamID &&
    left->codec == right->codec &&
    left->codecVersion == right->codecVersion &&
    left->configEpoch == right->configEpoch &&
    left->width == right->width &&
    left->height == right->height &&
    left->refreshNumerator == right->refreshNumerator &&
    left->refreshDenominator == right->refreshDenominator &&
    left->pixelFormat == right->pixelFormat &&
    left->chromaSubsampling == right->chromaSubsampling &&
    left->colorPrimaries == right->colorPrimaries &&
    left->colorTransfer == right->colorTransfer &&
    left->colorMatrix == right->colorMatrix &&
    left->colorRange == right->colorRange &&
    left->planeCount == right->planeCount &&
    left->bitDepth == right->bitDepth &&
    left->flags == right->flags &&
    left->maxFrameLength == right->maxFrameLength &&
    left->maxFragmentLength == right->maxFragmentLength &&
    left->maxFrameLatencyMs == right->maxFrameLatencyMs;
}

static void networkVideoFreeOutput(struct NetworkVideoOutput ** output)
{
  if (!output || !*output)
    return;
  networkVideoDecoderRelease(&(*output)->decoded);
  free(*output);
  *output = NULL;
}

static void networkVideoClearEncoded(struct NetworkVideoEncoded * encoded)
{
  if (!encoded)
    return;
  free(encoded->ownedData);
  memset(encoded, 0, sizeof(*encoded));
}

static void networkVideoInvalidateDecodeNL(NetworkVideo * video,
    struct NetworkVideoEncoded * pending)
{
  video->decodeGeneration = networkVideoNext64(video->decodeGeneration);
  *pending                = video->pendingDecode;
  video->pendingDecode    = (struct NetworkVideoEncoded){0};
  video->decodePending    = false;
}

static struct NetworkVideoOutput * networkVideoFindLeaseNL(
    NetworkVideo * video, uint64_t handle)
{
  for (struct NetworkVideoOutput * output = video->leases;
       output; output = output->next)
    if (output->handle == handle)
      return output;
  return NULL;
}

static struct NetworkVideoOutput * networkVideoTakeLeaseNL(
    NetworkVideo * video, uint64_t handle)
{
  struct NetworkVideoOutput ** link = &video->leases;
  while (*link && (*link)->handle != handle)
    link = &(*link)->next;
  if (!*link)
    return NULL;

  struct NetworkVideoOutput * output = *link;
  *link        = output->next;
  output->next = NULL;
  if (video->leaseCount)
    --video->leaseCount;
  return output;
}

static struct NetworkVideoOutput * networkVideoTakeReleasedLeasesNL(
    NetworkVideo * video)
{
  struct NetworkVideoOutput  * released = NULL;
  struct NetworkVideoOutput ** link     = &video->leases;
  while (*link)
  {
    struct NetworkVideoOutput * output = *link;
    if (!output->releaseRequested)
    {
      link = &output->next;
      continue;
    }

    *link        = output->next;
    output->next = released;
    released     = output;
    if (video->leaseCount)
      --video->leaseCount;
  }
  return released;
}

static struct NetworkVideoOutput * networkVideoTakeLeasesNL(
    NetworkVideo * video)
{
  struct NetworkVideoOutput * leases = video->leases;
  video->leases     = NULL;
  video->leaseCount = 0;
  return leases;
}

static void networkVideoFreeOutputList(struct NetworkVideoOutput * output)
{
  while (output)
  {
    struct NetworkVideoOutput * next = output->next;
    output->next = NULL;
    networkVideoFreeOutput(&output);
    output       = next;
  }
}

static void networkVideoReleaseLease(void * opaque, uint64_t handle)
{
  NetworkVideo * video = opaque;
  if (!video || !handle)
    return;

  LG_LOCK(video->stateLock);
  struct NetworkVideoOutput * output =
    networkVideoFindLeaseNL(video, handle);
  if (output)
    output->releaseRequested = true;
  LG_UNLOCK(video->stateLock);
  lgSignalEvent(video->frameEvent);
}

static void networkVideoClearAssembly(struct NetworkVideoAssembly * assembly)
{
  if (!assembly)
    return;
  free(assembly->blocks);
  free(assembly->fragmentLengths);
  free(assembly->fragmentOffsets);
  free(assembly->fragmentBlocks);
  free(assembly->fragmentSeen);
  free(assembly->coverage);
  free(assembly->data);
  memset(assembly, 0, sizeof(*assembly));
}

static struct NetworkVideoAssembly networkVideoTakeAssemblyNL(
    NetworkVideo * video)
{
  struct NetworkVideoAssembly assembly = video->assembly;
  video->assembly = (struct NetworkVideoAssembly){0};
  return assembly;
}

static void networkVideoClearAssemblies(
    struct NetworkVideoAssembly * assemblies, size_t count)
{
  for (size_t index = 0; index < count; ++index)
    networkVideoClearAssembly(&assemblies[index]);
}

static bool networkVideoSetStatusNL(NetworkVideo * video, bool available,
    LG_TransportStatus reason)
{
  if (video->status.available == available && video->status.reason == reason)
    return false;
  if (video->status.available != available)
  {
    video->status.epoch = networkVideoNext64(video->status.epoch);
    if (video->status.epoch == 0)
      video->status.epoch = 1;
  }
  video->status.available = available;
  video->status.reason    = available ? LG_TRANSPORT_OK : reason;
  return true;
}

static bool networkVideoUpdateStatusNL(NetworkVideo * video)
{
  const bool         available = video->connected && video->serviceAvailable &&
    video->rendererAttached && video->wireConfigured && video->decoderReady;
  LG_TransportStatus reason    = LG_TRANSPORT_UNAVAILABLE;
  if (!video->connected)
    reason = LG_TRANSPORT_DISCONNECTED;
  else if (!video->serviceAvailable)
    reason = LG_TRANSPORT_INVALID_VERSION;
  return networkVideoSetStatusNL(video, available, reason);
}

static void networkVideoNotifyStatus(NetworkVideo * video)
{
  LG_VideoComponentStatus  status;
  NetworkVideoStatusFn     callback;
  void                   * opaque;

  LG_LOCK(video->callbackLock);
  LG_LOCK(video->stateLock);
  callback = video->statusCallback;
  opaque   = video->statusOpaque;
  status   = video->status;
  LG_UNLOCK(video->stateLock);
  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(video->callbackLock);
}

static void networkVideoQueueFeedbackNL(NetworkVideo * video,
    LGNetVideoFeedbackFlags flags)
{
  video->feedbackFlags   |= flags;
  video->feedbackPending = true;
}

static void networkVideoRequestKeyframeNL(NetworkVideo * video,
    LGNetVideoControlReason reason, uint64_t frameID)
{
  video->keyframePending = true;
  video->keyframeReason  = reason;
  video->keyframeFrameID = frameID;
  networkVideoQueueFeedbackNL(video,
      LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME);
}

static bool networkVideoPacketFlags(
    const LGNetPacketView * packet, bool datagram)
{
  if (!packet || !(packet->envelope.flags & LG_NET_ENVELOPE_FINAL) ||
      (packet->envelope.flags & NETWORK_VIDEO_FORBIDDEN_FLAGS))
    return false;
  const LGNetEnvelopeFlags transport = packet->envelope.flags &
    (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM);
  if (datagram)
    return transport == LG_NET_ENVELOPE_DATAGRAM;

  /* Bulk stream envelopes deliberately carry neither delivery flag. The
   * connection has already authenticated and route-validated the bound
   * stream before invoking this endpoint. */
  return transport == LG_NET_ENVELOPE_RELIABLE || !transport;
}

static bool networkVideoResponseNL(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  const bool response =
    (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) != 0;
  if (!response)
    return packet->envelope.requestID == 0;
  if (!packet->envelope.requestID || !video->requestSerial ||
      packet->envelope.requestID > video->requestSerial)
    return false;
  if (packet->envelope.requestID == video->subscribeRequestID)
    video->subscribeRequestID = 0;
  return true;
}

static bool networkVideoUnsolicited(const LGNetPacketView * packet)
{
  return !(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) &&
    !packet->envelope.requestID;
}

static LG_TransportStatus networkVideoSendReliableNL(NetworkVideo * video,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t componentEpoch,
    uint64_t requestID, const void * payload, uint32_t payloadLength)
{
  return networkConnectionSend(video->connection, LG_NET_SERVICE_VIDEO,
      messageType, video->serviceVersion, flags | LG_NET_ENVELOPE_FINAL,
      componentEpoch, requestID, payload, payloadLength);
}

static LG_TransportStatus networkVideoSendDatagramNL(NetworkVideo * video,
    uint16_t messageType, uint64_t componentEpoch,
    const void * payload, uint32_t payloadLength)
{
  return networkConnectionSendDatagram(video->connection,
      LG_NET_SERVICE_VIDEO, messageType, video->serviceVersion, 0,
      componentEpoch, 0, payload, payloadLength, true);
}

static void networkVideoSendSubscribeNL(NetworkVideo * video, uint64_t now)
{
  if (!video->connected || !video->serviceAvailable ||
      video->wireConfigured || video->destroying ||
      (!video->subscribePending && now < video->nextSubscribeNs))
    return;

  /* Codec blocks are too small to use one QUIC datagram each efficiently.
   * Prefer bulk delivery until the video protocol can aggregate multiple
   * blocks into each datagram. */
  const LGNetVideoSubscribe subscribe =
  {
    .subscriptionID    = video->subscriberID,
    .preferredCodec    = LG_NET_VIDEO_CODEC_PYROWAVE,
    .flags             = LG_NET_VIDEO_SUBSCRIBE_LOW_LATENCY,
    .maxWidth          = NETWORK_VIDEO_MAX_WIDTH,
    .maxHeight         = NETWORK_VIDEO_MAX_HEIGHT,
    .maxFrameLength    = NETWORK_VIDEO_MAX_FRAME_LENGTH,
    .maxFrameLatencyMs = NETWORK_VIDEO_MAX_FRAME_LATENCY_MS,
    .maxFrameRate      = NETWORK_VIDEO_MAX_FRAME_RATE,
  };
  uint8_t payload[LG_NET_VIDEO_SUBSCRIBE_WIRE_SIZE];
  if (!lgNetVideoSubscribeEncode(payload, sizeof(payload), &subscribe))
    return;

  if (!video->subscribeRequestID)
  {
    video->requestSerial      = networkVideoNext64(video->requestSerial);
    video->subscribeRequestID = video->requestSerial;
  }
  (void)networkVideoSendReliableNL(video,
      LG_NET_VIDEO_MESSAGE_SUBSCRIBE, LG_NET_ENVELOPE_ACK_REQUIRED,
      0, video->subscribeRequestID, payload, sizeof(payload));
  video->subscribePending = false;
  video->nextSubscribeNs  = now +
    (uint64_t)NETWORK_VIDEO_SUBSCRIBE_RETRY_MS * UINT64_C(1000000);
}

static void networkVideoSendFeedbackNL(NetworkVideo * video, uint64_t now)
{
  if (!video->feedbackPending || !video->wireConfigured ||
      now < video->nextFeedbackNs)
    return;

  const LGNetVideoFeedback feedback =
  {
    .streamID            = video->config.streamID,
    .flags               = video->feedbackFlags,
    .lastCompleteFrameID = video->lastCompleteFrameID,
    .highestSeenFrameID  = video->highestSeenFrameID,
    .lastDecodedFrameID  = video->lastDecodedFrameID,
    .lostFrames          = video->lostFrames,
    .lostFragments       = video->lostFragments,
    .receiveQueueUs      = 0,
    .decodeQueueUs       = 0,
    .roundTripUs         = 0,
  };
  uint8_t payload[LG_NET_VIDEO_FEEDBACK_WIRE_SIZE];
  if (!lgNetVideoFeedbackEncode(payload, sizeof(payload), &feedback))
    return;

  const LG_TransportStatus status = video->datagramsAvailable ?
    networkVideoSendDatagramNL(video, LG_NET_VIDEO_MESSAGE_FEEDBACK,
      video->config.configEpoch, payload, sizeof(payload)) :
    networkVideoSendReliableNL(video, LG_NET_VIDEO_MESSAGE_FEEDBACK, 0,
      video->config.configEpoch, 0, payload, sizeof(payload));
  if (status == LG_TRANSPORT_OK)
  {
    video->feedbackFlags   = 0;
    video->feedbackPending = false;
  }
  video->nextFeedbackNs = now +
    (uint64_t)NETWORK_VIDEO_FEEDBACK_INTERVAL_MS * UINT64_C(1000000);
}

static void networkVideoSendKeyframeNL(NetworkVideo * video, uint64_t now)
{
  if (!video->keyframePending || !video->wireConfigured ||
      now < video->nextKeyframeNs)
    return;

  const LGNetVideoControl control =
  {
    .streamID    = video->config.streamID,
    .reason      = video->keyframeReason,
    .configEpoch = video->config.configEpoch,
    .frameID     = video->keyframeFrameID,
  };
  uint8_t payload[LG_NET_VIDEO_CONTROL_WIRE_SIZE];
  if (!lgNetVideoControlEncode(payload, sizeof(payload), &control))
    return;

  const LG_TransportStatus status = video->datagramsAvailable ?
    networkVideoSendDatagramNL(video,
      LG_NET_VIDEO_MESSAGE_KEYFRAME_REQUEST,
      video->config.configEpoch, payload, sizeof(payload)) :
    networkVideoSendReliableNL(video,
      LG_NET_VIDEO_MESSAGE_KEYFRAME_REQUEST, 0,
      video->config.configEpoch, 0, payload, sizeof(payload));
  if (status == LG_TRANSPORT_OK)
    video->keyframePending = false;

  video->nextKeyframeNs = now +
    (uint64_t)NETWORK_VIDEO_KEYFRAME_RETRY_MS * UINT64_C(1000000);
}

static uint64_t networkVideoCoverageMask(
    uint64_t word, uint64_t begin, uint64_t end)
{
  const unsigned low   = begin > word * 64U ?
    (unsigned)(begin - word * 64U) : 0U;
  const unsigned high  = end < (word + 1U) * 64U ?
    (unsigned)(end - word * 64U) : 64U;
  const uint64_t lower = low ? (UINT64_C(1) << low) - 1U : 0U;
  const uint64_t upper = high == 64U ? UINT64_MAX :
    (UINT64_C(1) << high) - 1U;
  return upper & ~lower;
}

static enum NetworkVideoCoverageState networkVideoCoverage(
    const struct NetworkVideoAssembly * assembly,
    uint32_t offset, uint32_t length)
{
  const uint64_t begin = offset;
  const uint64_t end   = begin + length;
  const uint64_t first = begin / 64U;
  const uint64_t last  = (end - 1U) / 64U;

  bool any = false;
  bool all = true;
  for (uint64_t word = first; word <= last; ++word)
  {
    const uint64_t mask = networkVideoCoverageMask(word, begin, end);
    const uint64_t set  = assembly->coverage[word] & mask;

    any |= set != 0;
    all &= set == mask;
  }
  if (all)
    return NETWORK_VIDEO_COVERAGE_FULL;
  return any ? NETWORK_VIDEO_COVERAGE_PARTIAL : NETWORK_VIDEO_COVERAGE_EMPTY;
}

static void networkVideoSetCoverage(struct NetworkVideoAssembly * assembly,
    uint32_t offset, uint32_t length)
{
  const uint64_t begin = offset;
  const uint64_t end   = begin + length;
  const uint64_t first = begin / 64U;
  const uint64_t last  = (end - 1U) / 64U;
  for (uint64_t word = first; word <= last; ++word)
    assembly->coverage[word] |= networkVideoCoverageMask(word, begin, end);
}

static bool networkVideoFragmentSeen(
    const struct NetworkVideoAssembly * assembly, uint32_t index)
{
  return (assembly->fragmentSeen[index >> 6] >> (index & 63U)) & 1U;
}

static void networkVideoSetFragmentSeen(
    struct NetworkVideoAssembly * assembly, uint32_t index)
{
  assembly->fragmentSeen[index >> 6] |= UINT64_C(1) << (index & 63U);
}

static bool networkVideoFindScheduleNL(NetworkVideo * video,
    uint64_t configEpoch, uint64_t frameID, LGNetVideoSchedule * schedule)
{
  struct NetworkVideoScheduleEntry * entry =
    &video->schedules[frameID % NETWORK_VIDEO_SCHEDULE_CACHE];
  if (!entry->valid || entry->schedule.configEpoch != configEpoch ||
      entry->schedule.frameID != frameID)
    return false;
  *schedule    = entry->schedule;
  entry->valid = false;
  return true;
}

static bool networkVideoAllocateAssemblyNL(NetworkVideo * video,
    const LGNetVideoFragment * fragment, uint64_t now,
    struct NetworkVideoAssembly * failed)
{
  *failed = (struct NetworkVideoAssembly){0};
  if (fragment->frameLength > NETWORK_VIDEO_MAX_FRAME_LENGTH ||
      fragment->frameLength > video->config.maxFrameLength ||
      fragment->fragmentCount > NETWORK_VIDEO_MAX_FRAGMENTS ||
      fragment->blockCount > NETWORK_VIDEO_MAX_BLOCKS)
    return false;

  const bool                  sparseAllowed =
    (video->config.flags & LG_NET_VIDEO_STREAM_DATAGRAMS) != 0;
  struct NetworkVideoAssembly assembly      =
  {
    .active         = true,
    .sparseAllowed  = sparseAllowed,
    .generation     = video->decodeGeneration,
    .configEpoch    = fragment->configEpoch,
    .frameID        = fragment->frameID,
    .firstReceiveNs = now,
    .deadlineNs     = sparseAllowed ?
      now + (uint64_t)video->config.maxFrameLatencyMs * UINT64_C(1000000) :
      networkVideoAssemblyDeadline(now),
    .frameLength    = fragment->frameLength,
    .fragmentCount  = fragment->fragmentCount,
    .blockCount     = fragment->blockCount,
    .coverageWords  = ((size_t)fragment->frameLength + 63U) / 64U,
  };
  const size_t fragmentWords =
    ((size_t)fragment->fragmentCount + 63U) / 64U;
  assembly.data            = malloc(fragment->frameLength);
  assembly.coverage        = calloc(assembly.coverageWords,
      sizeof(*assembly.coverage));
  assembly.fragmentSeen    = calloc(fragmentWords,
      sizeof(*assembly.fragmentSeen));
  assembly.fragmentOffsets = malloc(
      (size_t)fragment->fragmentCount * sizeof(*assembly.fragmentOffsets));
  assembly.fragmentLengths = malloc(
      (size_t)fragment->fragmentCount * sizeof(*assembly.fragmentLengths));
  assembly.fragmentBlocks  = malloc(
      (size_t)fragment->fragmentCount * sizeof(*assembly.fragmentBlocks));
  assembly.blocks          = calloc(fragment->blockCount,
      sizeof(*assembly.blocks));
  if (!assembly.data || !assembly.coverage || !assembly.fragmentSeen ||
      !assembly.fragmentOffsets || !assembly.fragmentLengths ||
      !assembly.fragmentBlocks || !assembly.blocks)
  {
    *failed = assembly;
    return false;
  }

  assembly.scheduleValid = networkVideoFindScheduleNL(video,
      fragment->configEpoch, fragment->frameID, &assembly.schedule);
  video->assembly = assembly;
  return true;
}

static bool networkVideoBlockComplete(
    const struct NetworkVideoBlockCoverage * block)
{
  return block->known && block->start && block->end &&
    block->received == block->length;
}

static bool networkVideoAssemblyComplete(
    const struct NetworkVideoAssembly * assembly)
{
  if (assembly->receivedBytes != assembly->frameLength)
    return false;
  for (uint32_t index = 0; index < assembly->blockCount; ++index)
    if (!networkVideoBlockComplete(&assembly->blocks[index]))
      return false;
  return true;
}

static bool networkVideoCompactAssemblyNL(NetworkVideo * video,
    const struct NetworkVideoAssembly * assembly, uint64_t now,
    struct NetworkVideoEncoded * encoded)
{
  /* Transport block zero contains the sequence header and first codec block.
   * Complete later blocks remain independently decodable; absent codec block
   * indices are zero-filled by the decoder. */
  if (!assembly->active || !assembly->sparseAllowed ||
      networkVideoAssemblyComplete(assembly) || !assembly->blockCount ||
      assembly->blockCount > NETWORK_VIDEO_PYROWAVE_COUNT_MASK)
    return false;

  const struct NetworkVideoBlockCoverage * first = &assembly->blocks[0];
  if (!networkVideoBlockComplete(first) || first->base != 0 ||
      first->length <= NETWORK_VIDEO_PYROWAVE_HEADER_SIZE)
    return false;

  uint32_t compactLength = 0;
  uint32_t compactBlocks = 0;
  for (uint32_t index = 0; index < assembly->blockCount; ++index)
  {
    const struct NetworkVideoBlockCoverage * block = &assembly->blocks[index];
    if (!networkVideoBlockComplete(block))
      continue;
    if (block->base > assembly->frameLength ||
        block->length > assembly->frameLength - block->base ||
        block->length > assembly->frameLength - compactLength)
      return false;
    compactLength += block->length;
    ++compactBlocks;
  }

  uint8_t * data = malloc(compactLength);
  if (!data)
    return false;

  uint32_t offset = 0;
  for (uint32_t index = 0; index < assembly->blockCount; ++index)
  {
    const struct NetworkVideoBlockCoverage * block = &assembly->blocks[index];
    if (!networkVideoBlockComplete(block))
      continue;
    memcpy(data + offset, assembly->data + block->base, block->length);
    offset += block->length;
  }
  if (offset != compactLength)
  {
    free(data);
    return false;
  }

  const uint32_t firstWord  = networkVideoReadU32(data);
  if (!(firstWord & UINT32_C(0x80000000)))
  {
    free(data);
    return false;
  }
  const uint32_t secondWord = networkVideoReadU32(
      data + sizeof(uint32_t));
  if ((secondWord & NETWORK_VIDEO_PYROWAVE_COUNT_MASK) !=
      assembly->blockCount)
  {
    free(data);
    return false;
  }
  networkVideoWriteU32(data + sizeof(uint32_t),
      (secondWord & ~NETWORK_VIDEO_PYROWAVE_COUNT_MASK) |
        compactBlocks);

  *encoded = (struct NetworkVideoEncoded)
  {
    .config                  = video->config,
    .data                    = data,
    .ownedData               = data,
    .length                  = compactLength,
    .blockCount              = compactBlocks,
    .generation              = assembly->generation,
    .frameID                 = assembly->frameID,
    .captureTimestampNs      = assembly->scheduleValid ?
      assembly->schedule.captureTimestampNs : now,
    .presentationTimestampNs = assembly->scheduleValid ?
      assembly->schedule.presentationTimestampNs : now,
    .receiveTimeNs           = now - assembly->firstReceiveNs,
    .flags                   = LG_NET_VIDEO_FRAME_KEYFRAME,
    .complete                = false,
    .scheduleValid           = assembly->scheduleValid,
    .schedule                = assembly->schedule,
  };
  return true;
}

static bool networkVideoAddFragmentNL(NetworkVideo * video,
    const LGNetVideoFragment * fragment, uint64_t now,
    struct NetworkVideoEncoded encoded[2], size_t * encodedCount,
    struct NetworkVideoAssembly retired[2], size_t * retiredCount)
{
  struct NetworkVideoAssembly * assembly = &video->assembly;
  if (!assembly->active || fragment->frameID > assembly->frameID)
  {
    if (assembly->active)
    {
      video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);
      if (*encodedCount < 2U &&
          networkVideoCompactAssemblyNL(video, assembly, now,
            &encoded[*encodedCount]))
        ++*encodedCount;
      retired[(*retiredCount)++] = networkVideoTakeAssemblyNL(video);
    }
    if (!networkVideoAllocateAssemblyNL(video, fragment, now,
          &retired[*retiredCount]))
    {
      ++*retiredCount;
      return false;
    }
    assembly = &video->assembly;
  }
  else if (fragment->frameID < assembly->frameID)
    return true;

  if (assembly->configEpoch != fragment->configEpoch ||
      assembly->frameLength != fragment->frameLength ||
      assembly->fragmentCount != fragment->fragmentCount ||
      assembly->blockCount != fragment->blockCount)
    return false;

  if (networkVideoFragmentSeen(assembly, fragment->fragmentIndex))
  {
    const uint32_t offset =
      assembly->fragmentOffsets[fragment->fragmentIndex];
    const uint32_t length =
      assembly->fragmentLengths[fragment->fragmentIndex];
    return offset == fragment->frameOffset &&
      length == fragment->payloadLength &&
      assembly->fragmentBlocks[fragment->fragmentIndex] ==
        fragment->blockIndex &&
      !memcmp(assembly->data + offset,
        fragment->payload, fragment->payloadLength);
  }

  struct NetworkVideoBlockCoverage * block     =
    &assembly->blocks[fragment->blockIndex];
  const uint32_t                     blockBase =
    fragment->frameOffset - fragment->blockOffset;
  if (!block->known)
  {
    block->base   = blockBase;
    block->length = fragment->blockLength;
    block->known  = true;
  }
  else if (block->base != blockBase ||
      block->length != fragment->blockLength)
    return false;

  const enum NetworkVideoCoverageState coverage = networkVideoCoverage(
      assembly, fragment->frameOffset, fragment->payloadLength);
  if (coverage == NETWORK_VIDEO_COVERAGE_PARTIAL)
    return false;
  if (coverage == NETWORK_VIDEO_COVERAGE_FULL)
  {
    if (memcmp(assembly->data + fragment->frameOffset,
          fragment->payload, fragment->payloadLength))
      return false;
    networkVideoSetFragmentSeen(assembly, fragment->fragmentIndex);
    assembly->fragmentOffsets[fragment->fragmentIndex] =
      fragment->frameOffset;
    assembly->fragmentLengths[fragment->fragmentIndex] =
      fragment->payloadLength;
    assembly->fragmentBlocks[fragment->fragmentIndex]  =
      fragment->blockIndex;
    return true;
  }

  memcpy(assembly->data + fragment->frameOffset,
      fragment->payload, fragment->payloadLength);
  networkVideoSetCoverage(assembly,
      fragment->frameOffset, fragment->payloadLength);
  networkVideoSetFragmentSeen(assembly, fragment->fragmentIndex);
  assembly->fragmentOffsets[fragment->fragmentIndex] = fragment->frameOffset;
  assembly->fragmentLengths[fragment->fragmentIndex] = fragment->payloadLength;
  assembly->fragmentBlocks[fragment->fragmentIndex]  = fragment->blockIndex;

  assembly->receivedBytes += fragment->payloadLength;
  block->received         += fragment->payloadLength;
  block->start            |=
    (fragment->flags & LG_NET_VIDEO_FRAGMENT_BLOCK_START) != 0;
  block->end              |=
    (fragment->flags & LG_NET_VIDEO_FRAGMENT_BLOCK_END) != 0;
  if (!assembly->sparseAllowed)
    assembly->deadlineNs = networkVideoAssemblyDeadline(now);

  if (!networkVideoAssemblyComplete(assembly))
    return true;

  if (*encodedCount >= 2U)
    return false;
  encoded[*encodedCount] = (struct NetworkVideoEncoded)
  {
    .config                  = video->config,
    .data                    = assembly->data,
    .ownedData               = assembly->data,
    .length                  = assembly->frameLength,
    .blockCount              = assembly->blockCount,
    .generation              = assembly->generation,
    .frameID                 = assembly->frameID,
    .captureTimestampNs      = assembly->scheduleValid ?
      assembly->schedule.captureTimestampNs : now,
    .presentationTimestampNs = assembly->scheduleValid ?
      assembly->schedule.presentationTimestampNs : now,
    .receiveTimeNs           = now - assembly->firstReceiveNs,
    .flags                   = LG_NET_VIDEO_FRAME_KEYFRAME,
    .complete                = true,
    .scheduleValid           = assembly->scheduleValid,
    .schedule                = assembly->schedule,
  };
  ++*encodedCount;
  assembly->data             = NULL;
  retired[(*retiredCount)++] = networkVideoTakeAssemblyNL(video);
  return true;
}

static void networkVideoDecodeFailure(NetworkVideo * video,
    const struct NetworkVideoEncoded * encoded,
    NetworkVideoDecodeResult result)
{
  bool notify = false;
  LG_LOCK(video->stateLock);
  if (video->decodeGeneration == encoded->generation &&
      video->wireConfigured &&
      video->config.configEpoch == encoded->config.configEpoch)
  {
    video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);

    const LGNetVideoFeedbackFlags flags = result ==
      NETWORK_VIDEO_DECODE_ERROR ?
        LG_NET_VIDEO_FEEDBACK_DECODER_STALLED : 0;
    networkVideoQueueFeedbackNL(video,
        flags | LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME);
    networkVideoRequestKeyframeNL(video,
        result == NETWORK_VIDEO_DECODE_CORRUPT ?
          LG_NET_VIDEO_CONTROL_CORRUPT : LG_NET_VIDEO_CONTROL_STALLED,
        encoded->frameID);
    if (result == NETWORK_VIDEO_DECODE_ERROR ||
        result == NETWORK_VIDEO_DECODE_UNSUPPORTED)
    {
      video->decoderReady = false;
      notify              = networkVideoUpdateStatusNL(video);
    }
  }
  LG_UNLOCK(video->stateLock);
  if (notify)
    networkVideoNotifyStatus(video);
}

static void networkVideoDecode(NetworkVideo * video,
    struct NetworkVideoEncoded * encoded)
{
  LG_LOCK(video->stateLock);
  const bool current = !video->destroying && !video->decodeStopping &&
    video->connected && video->wireConfigured && video->rendererAttached &&
    video->decoderReady &&
    video->decodeGeneration == encoded->generation &&
    video->config.configEpoch == encoded->config.configEpoch;
  LG_UNLOCK(video->stateLock);
  if (!current)
  {
    networkVideoClearEncoded(encoded);
    return;
  }

  NetworkVideoDecodedFrame       decoded;
  const NetworkVideoDecodeResult result = networkVideoDecoderDecode(
      video->decoder, &encoded->config, encoded->data,
      encoded->length, encoded->blockCount, &decoded);
  free(encoded->ownedData);
  if (result != NETWORK_VIDEO_DECODE_OK)
  {
    networkVideoDecodeFailure(video, encoded, result);
    return;
  }

  struct NetworkVideoOutput * output   = calloc(1, sizeof(*output));
  struct NetworkVideoOutput * replaced = NULL;
  if (!output)
  {
    networkVideoDecoderRelease(&decoded);
    networkVideoDecodeFailure(video, encoded, NETWORK_VIDEO_DECODE_ERROR);
    return;
  }
  output->decoded                 = decoded;
  output->frameID                 = encoded->frameID;
  output->timestamp               = encoded->presentationTimestampNs;
  output->captureTimestampNs      = encoded->captureTimestampNs;
  output->presentationTimestampNs = encoded->presentationTimestampNs;
  output->receiveTimeNs           = encoded->receiveTimeNs;
  LG_LOCK(video->stateLock);
  if (video->decodeGeneration != encoded->generation ||
      !video->wireConfigured ||
      video->config.configEpoch != encoded->config.configEpoch ||
      !video->rendererAttached || !video->decoderReady ||
      output->frameID <= video->lastDecodedFrameID)
  {
    LG_UNLOCK(video->stateLock);
    networkVideoFreeOutput(&output);
    return;
  }

  if (video->output)
  {
    video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);
    replaced          = video->output;
  }
  if (encoded->complete)
    video->lastCompleteFrameID   = output->frameID;
  video->lastDecodedFrameID      = output->frameID;
  output->decoded.format.version = video->localConfigGeneration;
  output->epoch                  = video->status.epoch;
  video->output                  = output;
  networkVideoQueueFeedbackNL(video, 0);
  LG_UNLOCK(video->stateLock);
  networkVideoFreeOutput(&replaced);
  lgSignalEvent(video->frameEvent);
}

static void networkVideoQueueDecode(NetworkVideo * video,
    struct NetworkVideoEncoded * encoded)
{
  if (!video || !encoded || !encoded->ownedData ||
      encoded->data != encoded->ownedData)
  {
    networkVideoClearEncoded(encoded);
    return;
  }

  struct NetworkVideoEncoded replaced = {0};
  bool                       queued   = false;
  LG_LOCK(video->stateLock);
  if (!video->destroying && !video->decodeStopping && video->connected &&
      video->wireConfigured && video->rendererAttached &&
      video->decoderReady &&
      video->decodeGeneration == encoded->generation &&
      video->config.configEpoch == encoded->config.configEpoch &&
      encoded->frameID > video->lastDecodedFrameID &&
      (!video->decodePending ||
        encoded->frameID > video->pendingDecode.frameID))
  {
    if (video->decodePending)
    {
      video->lostFrames = networkVideoSaturatingIncrement(
          video->lostFrames);
      networkVideoQueueFeedbackNL(video, LG_NET_VIDEO_FEEDBACK_CONGESTED);
      replaced = video->pendingDecode;
    }
    video->pendingDecode = *encoded;
    memset(encoded, 0, sizeof(*encoded));
    video->decodePending = true;
    queued               = true;
  }
  LG_UNLOCK(video->stateLock);

  networkVideoClearEncoded(&replaced);
  networkVideoClearEncoded(encoded);
  if (queued)
    lgSignalEvent(video->decodeEvent);
}

static int networkVideoDecodeThread(void * opaque)
{
  NetworkVideo * video = opaque;
  for (;;)
  {
    lgWaitEvent(video->decodeEvent, TIMEOUT_INFINITE);
    for (;;)
    {
      struct NetworkVideoEncoded encoded = {0};
      LG_LOCK(video->stateLock);
      if (video->decodeStopping)
      {
        encoded              = video->pendingDecode;
        video->pendingDecode = (struct NetworkVideoEncoded){0};
        video->decodePending = false;
        LG_UNLOCK(video->stateLock);
        networkVideoClearEncoded(&encoded);
        return 0;
      }
      if (video->decodePending)
      {
        encoded              = video->pendingDecode;
        video->pendingDecode = (struct NetworkVideoEncoded){0};
        video->decodePending = false;
      }
      LG_UNLOCK(video->stateLock);

      if (!encoded.ownedData)
        break;
      networkVideoDecode(video, &encoded);
    }
  }
}

static bool networkVideoHandleConfig(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  LGNetVideoStreamConfig config;
  if (lgNetVideoStreamConfigDecode(&config,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      (packet->envelope.componentEpoch &&
        packet->envelope.componentEpoch != config.configEpoch) ||
      config.width > NETWORK_VIDEO_MAX_WIDTH ||
      config.height > NETWORK_VIDEO_MAX_HEIGHT ||
      config.maxFrameLength > NETWORK_VIDEO_MAX_FRAME_LENGTH ||
      config.maxFragmentLength > LG_NET_VIDEO_MAX_FRAGMENT_LENGTH ||
      config.maxFrameLatencyMs > NETWORK_VIDEO_MAX_FRAME_LATENCY_MS ||
      !networkPyroWaveProfileValid(&config))
    return false;

  struct NetworkVideoAssembly  retiredAssembly = {0};
  struct NetworkVideoEncoded   pendingDecode   = {0};
  struct NetworkVideoOutput  * replaced        = NULL;
  bool                         attached;
  bool                         decoderReady;
  uint64_t                     decodeGeneration;
  LG_LOCK(video->stateLock);
  if (!video->connected || !video->serviceAvailable)
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  if (!networkVideoResponseNL(video, packet))
  {
    LG_UNLOCK(video->stateLock);
    return false;
  }
  if ((config.flags & LG_NET_VIDEO_STREAM_DATAGRAMS) &&
      !video->datagramsAvailable)
  {
    LG_UNLOCK(video->stateLock);
    return false;
  }
  if (video->wireConfigured &&
      video->config.streamID == config.streamID &&
      video->config.configEpoch == config.configEpoch &&
      !networkVideoConfigEqual(&video->config, &config))
  {
    LG_UNLOCK(video->stateLock);
    return false;
  }
  const bool changed = !video->wireConfigured ||
    video->config.streamID != config.streamID ||
    video->config.configEpoch != config.configEpoch;
  if (changed)
  {
    networkVideoInvalidateDecodeNL(video, &pendingDecode);
    retiredAssembly = networkVideoTakeAssemblyNL(video);
    replaced        = video->output;
    video->output   = NULL;
    memset(video->schedules, 0, sizeof(video->schedules));
    video->highestSeenFrameID  = 0;
    video->lastCompleteFrameID = 0;
    video->lastDecodedFrameID  = 0;
    video->statusSequence      = 0;
    video->lostFrames          = 0;
    video->lostFragments       = 0;
    video->decoderReady        = false;
    video->localConfigGeneration++;
    if (!video->localConfigGeneration)
      video->localConfigGeneration = 1;
  }
  video->config             = config;
  video->wireConfigured     = true;
  video->subscribePending   = false;
  video->subscribeRequestID = 0;
  attached                  = video->rendererAttached;
  decoderReady              = video->decoderReady;
  decodeGeneration          = video->decodeGeneration;
  LG_UNLOCK(video->stateLock);

  networkVideoClearAssembly(&retiredAssembly);
  networkVideoClearEncoded(&pendingDecode);
  networkVideoFreeOutput(&replaced);

  const NetworkVideoDecodeResult result = decoderReady ?
    NETWORK_VIDEO_DECODE_OK : attached ?
      networkVideoDecoderConfigure(video->decoder, &config) :
      NETWORK_VIDEO_DECODE_UNSUPPORTED;
  bool                           notify;
  LG_LOCK(video->stateLock);
  if (video->decodeGeneration == decodeGeneration &&
      video->wireConfigured && video->rendererAttached &&
      networkVideoConfigEqual(&video->config, &config))
  {
    video->decoderReady = result == NETWORK_VIDEO_DECODE_OK;
    notify              = false;
    if (changed && video->status.available &&
        result == NETWORK_VIDEO_DECODE_OK)
    {
      video->status.epoch = networkVideoNext64(video->status.epoch);
      notify              = true;
    }
    notify |= networkVideoUpdateStatusNL(video);
    if (result != NETWORK_VIDEO_DECODE_OK && attached)
    {
      networkVideoQueueFeedbackNL(video,
          LG_NET_VIDEO_FEEDBACK_DECODER_STALLED |
          LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME);
      networkVideoRequestKeyframeNL(video,
          LG_NET_VIDEO_CONTROL_STALLED, 0);
    }
  }
  else
    notify = false;
  LG_UNLOCK(video->stateLock);
  if (notify)
    networkVideoNotifyStatus(video);
  return true;
}

static bool networkVideoHandleFrame(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  LGNetVideoFrame frame;
  if (lgNetVideoFrameDecode(&frame,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  if (frame.flags & LG_NET_VIDEO_FRAME_HAS_CHECKSUM)
  {
    uint32_t checksum;
    if (!lgNetCRC32C(frame.data, frame.encodedLength, &checksum) ||
        checksum != frame.checksum)
    {
      LG_LOCK(video->stateLock);
      video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);
      networkVideoRequestKeyframeNL(video,
          LG_NET_VIDEO_CONTROL_CORRUPT, frame.frameID);
      LG_UNLOCK(video->stateLock);
      return true;
    }
  }

  const uint64_t               receiveStart    = nanotime();
  struct NetworkVideoAssembly  retiredAssembly = {0};
  struct NetworkVideoEncoded   recovered       = {0};
  struct NetworkVideoEncoded   encoded;
  LG_LOCK(video->stateLock);
  if (!video->wireConfigured || frame.streamID != video->config.streamID ||
      frame.configEpoch != video->config.configEpoch ||
      frame.codec != video->config.codec ||
      frame.encodedLength > video->config.maxFrameLength ||
      frame.deadlineMs > video->config.maxFrameLatencyMs ||
      (packet->envelope.componentEpoch &&
        packet->envelope.componentEpoch != frame.configEpoch))
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  if (frame.frameID <= video->lastDecodedFrameID ||
      frame.frameID < video->highestSeenFrameID ||
      (frame.frameID == video->highestSeenFrameID &&
        (!video->assembly.active ||
          video->assembly.frameID != frame.frameID)))
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  if (video->assembly.active &&
      video->assembly.frameID <= frame.frameID)
  {
    video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);
    if (video->assembly.frameID < frame.frameID)
      (void)networkVideoCompactAssemblyNL(video, &video->assembly,
          receiveStart, &recovered);
    retiredAssembly   = networkVideoTakeAssemblyNL(video);
  }
  if (frame.frameID > video->highestSeenFrameID)
    video->highestSeenFrameID = frame.frameID;
  encoded = (struct NetworkVideoEncoded)
  {
    .config                  = video->config,
    .data                    = frame.data,
    .length                  = frame.encodedLength,
    .blockCount              = frame.blockCount,
    .generation              = video->decodeGeneration,
    .frameID                 = frame.frameID,
    .captureTimestampNs      = frame.captureTimestampNs,
    .presentationTimestampNs = frame.presentationTimestampNs,
    .receiveTimeNs           = nanotime() - receiveStart,
    .flags                   = frame.flags,
    .complete                = true,
  };
  encoded.scheduleValid = networkVideoFindScheduleNL(video,
      frame.configEpoch, frame.frameID, &encoded.schedule);
  LG_UNLOCK(video->stateLock);

  networkVideoClearAssembly(&retiredAssembly);
  encoded.ownedData = malloc(encoded.length);
  if (!encoded.ownedData)
  {
    if (recovered.ownedData)
      networkVideoQueueDecode(video, &recovered);
    networkVideoDecodeFailure(video, &encoded, NETWORK_VIDEO_DECODE_ERROR);
    return true;
  }
  memcpy(encoded.ownedData, encoded.data, encoded.length);
  encoded.data          = encoded.ownedData;
  encoded.receiveTimeNs = nanotime() - receiveStart;
  if (recovered.ownedData)
    networkVideoQueueDecode(video, &recovered);
  networkVideoQueueDecode(video, &encoded);
  return true;
}

static bool networkVideoHandleFragment(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  LGNetVideoFragment fragment;
  if (lgNetVideoFragmentDecode(&fragment,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  if (fragment.flags & LG_NET_VIDEO_FRAGMENT_CHECKSUM)
  {
    uint32_t checksum;
    if (!lgNetCRC32C(fragment.payload,
          fragment.payloadLength, &checksum) || checksum != fragment.checksum)
    {
      LG_LOCK(video->stateLock);
      video->lostFragments = networkVideoSaturatingIncrement(
          video->lostFragments);
      networkVideoRequestKeyframeNL(video,
          LG_NET_VIDEO_CONTROL_CORRUPT, fragment.frameID);
      LG_UNLOCK(video->stateLock);
      return true;
    }
  }

  struct NetworkVideoAssembly  retired[2]   = {0};
  struct NetworkVideoEncoded   encoded[2]   = {0};
  size_t                       retiredCount = 0;
  size_t                       encodedCount = 0;
  bool                         valid;
  const uint64_t               now          = nanotime();
  LG_LOCK(video->stateLock);
  if (!video->wireConfigured ||
      fragment.streamID != video->config.streamID ||
      fragment.configEpoch != video->config.configEpoch ||
      (packet->envelope.componentEpoch &&
        packet->envelope.componentEpoch != fragment.configEpoch) ||
      fragment.frameID <= video->lastDecodedFrameID ||
      fragment.frameID < video->highestSeenFrameID ||
      (fragment.frameID == video->highestSeenFrameID &&
        (!video->assembly.active ||
          video->assembly.frameID != fragment.frameID)))
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  if (fragment.frameID > video->highestSeenFrameID)
    video->highestSeenFrameID = fragment.frameID;
  valid = networkVideoAddFragmentNL(video, &fragment, now,
      encoded, &encodedCount, retired, &retiredCount);
  if (!valid)
  {
    video->lostFragments = networkVideoSaturatingIncrement(
        video->lostFragments);
    if (video->assembly.active)
      retired[retiredCount++] = networkVideoTakeAssemblyNL(video);
    networkVideoRequestKeyframeNL(video,
        LG_NET_VIDEO_CONTROL_CORRUPT, fragment.frameID);
  }
  LG_UNLOCK(video->stateLock);
  networkVideoClearAssemblies(retired, retiredCount);
  for (size_t index = 0; index < encodedCount; ++index)
    networkVideoQueueDecode(video, &encoded[index]);
  return true;
}

static bool networkVideoHandleSchedule(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  LGNetVideoSchedule schedule;
  if (lgNetVideoScheduleDecode(&schedule,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  LG_LOCK(video->stateLock);
  if (!video->wireConfigured ||
      schedule.streamID != video->config.streamID ||
      schedule.configEpoch != video->config.configEpoch ||
      schedule.frameID <= video->lastDecodedFrameID ||
      (packet->envelope.componentEpoch &&
        packet->envelope.componentEpoch != schedule.configEpoch))
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  if (video->assembly.active &&
      video->assembly.frameID == schedule.frameID)
  {
    video->assembly.schedule      = schedule;
    video->assembly.scheduleValid = true;
  }
  else
  {
    struct NetworkVideoScheduleEntry * entry =
      &video->schedules[schedule.frameID % NETWORK_VIDEO_SCHEDULE_CACHE];
    entry->valid    = true;
    entry->schedule = schedule;
  }
  if (schedule.flags & LG_NET_VIDEO_SCHEDULE_REQUEST_KEYFRAME)
    networkVideoRequestKeyframeNL(video,
        LG_NET_VIDEO_CONTROL_RECOVERY, schedule.frameID);
  LG_UNLOCK(video->stateLock);
  return true;
}

static bool networkVideoHandleStatus(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  LGNetVideoStatus status;
  if (lgNetVideoStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  struct NetworkVideoAssembly  retiredAssembly = {0};
  struct NetworkVideoEncoded   pendingDecode   = {0};
  struct NetworkVideoOutput  * replaced        = NULL;
  bool                         notify          = false;
  bool                         reset           = false;
  const bool                   response        =
    (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) != 0;
  LG_LOCK(video->stateLock);
  if (!networkVideoResponseNL(video, packet))
  {
    LG_UNLOCK(video->stateLock);
    return false;
  }
  const bool initialError = !video->wireConfigured && response &&
    status.state == LG_NET_VIDEO_STATE_ERROR;
  if ((!video->wireConfigured && !initialError) ||
      (video->wireConfigured &&
        (status.streamID != video->config.streamID ||
         status.configEpoch != video->config.configEpoch)) ||
      (packet->envelope.componentEpoch &&
        packet->envelope.componentEpoch != status.configEpoch) ||
      status.statusSequence <= video->statusSequence)
  {
    LG_UNLOCK(video->stateLock);
    return true;
  }
  video->statusSequence = status.statusSequence;
  if (status.flags & LG_NET_VIDEO_STATUS_KEYFRAME_PENDING)
    networkVideoQueueFeedbackNL(video, 0);
  if (status.state == LG_NET_VIDEO_STATE_STALLED)
  {
    networkVideoQueueFeedbackNL(video,
        LG_NET_VIDEO_FEEDBACK_DECODER_STALLED |
        LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME);
    networkVideoRequestKeyframeNL(video,
        LG_NET_VIDEO_CONTROL_STALLED, status.lastFrameID);
  }
  if (status.state == LG_NET_VIDEO_STATE_STOPPED ||
      status.state == LG_NET_VIDEO_STATE_ERROR)
  {
    video->wireConfigured     = false;
    video->decoderReady       = false;
    video->subscribePending   = false;
    video->subscribeRequestID = 0;
    video->nextSubscribeNs    = nanotime() +
      (uint64_t)NETWORK_VIDEO_SUBSCRIBE_RETRY_MS * UINT64_C(1000000);
    networkVideoInvalidateDecodeNL(video, &pendingDecode);
    retiredAssembly = networkVideoTakeAssemblyNL(video);
    replaced        = video->output;
    video->output   = NULL;
    memset(video->schedules, 0, sizeof(video->schedules));
    notify = networkVideoSetStatusNL(video, false,
        status.state == LG_NET_VIDEO_STATE_ERROR ?
          LG_TRANSPORT_ERROR : LG_TRANSPORT_UNAVAILABLE);
    reset  = true;
    lgSignalEvent(video->frameEvent);
  }
  LG_UNLOCK(video->stateLock);
  networkVideoClearAssembly(&retiredAssembly);
  networkVideoClearEncoded(&pendingDecode);
  networkVideoFreeOutput(&replaced);
  if (reset)
    networkVideoDecoderReset(video->decoder);
  if (notify)
    networkVideoNotifyStatus(video);
  if (reset)
    networkVideoProcess(video);
  return true;
}

static bool networkVideoHandlePacket(NetworkVideo * video,
    const LGNetPacketView * packet, bool datagram)
{
  if (!video || !packet ||
      packet->envelope.service != LG_NET_SERVICE_VIDEO ||
      !networkVideoVersionSupported(packet->envelope.serviceVersion) ||
      !networkVideoPacketFlags(packet, datagram))
    return false;

  const bool bulk = !datagram &&
    !(packet->envelope.flags & LG_NET_ENVELOPE_RELIABLE);

  switch (packet->envelope.messageType)
  {
    case LG_NET_VIDEO_MESSAGE_STREAM_CONFIG:
      return !datagram && !bulk && networkVideoHandleConfig(video, packet);

    case LG_NET_VIDEO_MESSAGE_FRAME:
      return !datagram && networkVideoUnsolicited(packet) &&
        networkVideoHandleFrame(video, packet);

    case LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT:
      return networkVideoUnsolicited(packet) &&
        networkVideoHandleFragment(video, packet);

    case LG_NET_VIDEO_MESSAGE_SCHEDULE:
      return !datagram && !bulk && networkVideoUnsolicited(packet) &&
        networkVideoHandleSchedule(video, packet);

    case LG_NET_VIDEO_MESSAGE_STATUS:
      return !datagram && !bulk && networkVideoHandleStatus(video, packet);

    default:
      return false;
  }
}

bool networkVideoPacket(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  return networkVideoHandlePacket(video, packet, false);
}

bool networkVideoDatagram(NetworkVideo * video,
    const LGNetPacketView * packet)
{
  return networkVideoHandlePacket(video, packet, true);
}

void networkVideoProcess(NetworkVideo * video)
{
  if (!video)
    return;

  const uint64_t               now             = nanotime();
  struct NetworkVideoAssembly  retiredAssembly = {0};
  struct NetworkVideoEncoded   recovered       = {0};
  LG_LOCK(video->stateLock);
  if (video->destroying)
  {
    LG_UNLOCK(video->stateLock);
    return;
  }
  if (video->assembly.active && now >= video->assembly.deadlineNs)
  {
    video->lostFrames = networkVideoSaturatingIncrement(video->lostFrames);
    (void)networkVideoCompactAssemblyNL(video, &video->assembly,
        now, &recovered);
    retiredAssembly   = networkVideoTakeAssemblyNL(video);
    networkVideoRequestKeyframeNL(video,
        LG_NET_VIDEO_CONTROL_STALLED, video->highestSeenFrameID);
  }
  networkVideoSendSubscribeNL(video, now);
  networkVideoSendKeyframeNL(video, now);
  networkVideoSendFeedbackNL(video, now);
  LG_UNLOCK(video->stateLock);
  networkVideoClearAssembly(&retiredAssembly);
  if (recovered.ownedData)
    networkVideoQueueDecode(video, &recovered);
}

static bool networkVideoTimer(void * opaque)
{
  networkVideoProcess(opaque);
  return true;
}

void networkVideoSendReady(NetworkVideo * video, bool datagram)
{
  if (!video)
    return;
  LG_LOCK(video->stateLock);
  if (datagram)
  {
    video->nextFeedbackNs = 0;
    video->nextKeyframeNs = 0;
  }
  else
  {
    video->subscribePending = true;
    video->nextFeedbackNs   = 0;
    video->nextKeyframeNs   = 0;
  }
  LG_UNLOCK(video->stateLock);
  networkVideoProcess(video);
}

bool networkVideoCreate(NetworkVideo ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;
  *result = NULL;

  NetworkVideo * video = calloc(1, sizeof(*video));
  if (!video)
    return false;
  LG_LOCK_INIT(video->stateLock);
  LG_LOCK_INIT(video->callbackLock);
  video->connection    = connection;
  video->subscriberID  = nanotime() ^ ((uint64_t)(uintptr_t)video << 17);
  video->status.epoch  = 1;
  video->status.reason = LG_TRANSPORT_DISCONNECTED;
  if (!video->subscriberID)
    video->subscriberID = 1;

  video->frameEvent  = lgCreateEvent(true, 0);
  video->decodeEvent = lgCreateEvent(true, 0);
  if (!video->frameEvent || !video->decodeEvent ||
      !networkVideoDecoderCreate(&video->decoder) ||
      !lgCreateThread("networkDecode", networkVideoDecodeThread,
        video, &video->decodeThread))
  {
    networkVideoDecoderDestroy(&video->decoder);
    if (video->decodeEvent)
      lgFreeEvent(video->decodeEvent);
    if (video->frameEvent)
      lgFreeEvent(video->frameEvent);
    LG_LOCK_FREE(video->callbackLock);
    LG_LOCK_FREE(video->stateLock);
    free(video);
    return false;
  }

  if (!lgCreateTimer(NETWORK_VIDEO_TIMER_MS,
      networkVideoTimer, video, &video->timer))
  {
    LGThread * decodeThread;
    LG_LOCK(video->stateLock);
    video->decodeStopping = true;
    LG_UNLOCK(video->stateLock);
    lgSignalEvent(video->decodeEvent);
    decodeThread        = video->decodeThread;
    video->decodeThread = NULL;
    if (!lgJoinThread(decodeThread, NULL))
    {
      DEBUG_ERROR("Failed to stop the network video decode worker; "
          "preserving state");
      return false;
    }
    networkVideoDecoderDestroy(&video->decoder);
    lgFreeEvent(video->decodeEvent);
    lgFreeEvent(video->frameEvent);
    LG_LOCK_FREE(video->callbackLock);
    LG_LOCK_FREE(video->stateLock);
    free(video);
    return false;
  }

  *result = video;
  return true;
}

void networkVideoConnectionState(NetworkVideo * video, bool connected)
{
  if (!video)
    return;

  uint16_t                     serviceVersion     = 0;
  const bool                   serviceAvailable   = connected &&
    networkConnectionServiceAvailable(video->connection,
      LG_NET_SERVICE_VIDEO, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(video->connection,
      LG_NET_SERVICE_VIDEO, &serviceVersion) &&
    networkVideoVersionSupported(serviceVersion);
  const bool                   datagramsAvailable = serviceAvailable &&
    networkConnectionServiceAvailable(video->connection,
      LG_NET_SERVICE_VIDEO, LG_NET_CAPABILITY_DATAGRAM);
  struct NetworkVideoAssembly  retiredAssembly    = {0};
  struct NetworkVideoEncoded   pendingDecode      = {0};
  struct NetworkVideoOutput  * replaced           = NULL;
  bool                         notify;

  LG_LOCK(video->stateLock);
  video->connected           = connected;
  video->serviceAvailable    = serviceAvailable;
  video->datagramsAvailable  = datagramsAvailable;
  video->serviceVersion      = serviceAvailable ? serviceVersion : 0;
  video->wireConfigured      = false;
  video->decoderReady        = false;
  video->subscribePending    = serviceAvailable;
  video->subscribeRequestID  = 0;
  video->nextSubscribeNs     = 0;
  video->nextFeedbackNs      = 0;
  video->nextKeyframeNs      = 0;
  video->feedbackFlags       = 0;
  video->feedbackPending     = false;
  video->keyframePending     = false;
  video->statusSequence      = 0;
  video->lostFrames          = 0;
  video->lostFragments       = 0;
  video->waitCancelled       = true;
  networkVideoInvalidateDecodeNL(video, &pendingDecode);
  memset(&video->config, 0, sizeof(video->config));
  memset(video->schedules, 0, sizeof(video->schedules));
  retiredAssembly = networkVideoTakeAssemblyNL(video);
  replaced        = video->output;
  video->output   = NULL;
  notify          = networkVideoUpdateStatusNL(video);
  LG_UNLOCK(video->stateLock);
  networkVideoClearAssembly(&retiredAssembly);
  networkVideoClearEncoded(&pendingDecode);
  networkVideoFreeOutput(&replaced);
  networkVideoDecoderReset(video->decoder);
  lgSignalEvent(video->frameEvent);
  if (notify)
    networkVideoNotifyStatus(video);
  if (serviceAvailable)
    networkVideoProcess(video);
}

void networkVideoDestroy(NetworkVideo ** result)
{
  if (!result || !*result)
    return;
  NetworkVideo               * video         = *result;
  struct NetworkVideoEncoded   pendingDecode = {0};
  LGThread                   * decodeThread;
  struct NetworkVideoOutput  * leases;
  *result = NULL;
  networkVideoSetStatusListener(video, NULL, NULL);
  LG_LOCK(video->stateLock);
  video->destroying     = true;
  video->decodeStopping = true;
  networkVideoInvalidateDecodeNL(video, &pendingDecode);
  LG_UNLOCK(video->stateLock);
  networkVideoClearEncoded(&pendingDecode);
  lgTimerDestroy(video->timer);
  lgSignalEvent(video->decodeEvent);
  decodeThread        = video->decodeThread;
  video->decodeThread = NULL;
  if (!lgJoinThread(decodeThread, NULL))
  {
    DEBUG_ERROR("Failed to stop the network video decode worker; "
        "preserving state");
    return;
  }
  networkVideoConnectionState(video, false);
  networkVideoClearAssembly(&video->assembly);
  networkVideoFreeOutput(&video->output);
  LG_LOCK(video->stateLock);
  leases = networkVideoTakeLeasesNL(video);
  LG_UNLOCK(video->stateLock);
  networkVideoFreeOutputList(leases);
  networkVideoDecoderDestroy(&video->decoder);
  lgFreeEvent(video->decodeEvent);
  lgFreeEvent(video->frameEvent);
  LG_LOCK_FREE(video->callbackLock);
  LG_LOCK_FREE(video->stateLock);
  free(video);
}

void networkVideoSetStatusListener(NetworkVideo * video,
    NetworkVideoStatusFn callback, void * opaque)
{
  if (!video)
    return;

  LG_VideoComponentStatus status;
  LG_LOCK(video->callbackLock);
  LG_LOCK(video->stateLock);
  video->statusCallback = callback;
  video->statusOpaque   = opaque;
  status                = video->status;
  LG_UNLOCK(video->stateLock);
  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(video->callbackLock);
}

LG_VideoComponentStatus networkVideoGetStatus(NetworkVideo * video)
{
  LG_VideoComponentStatus status =
  {
    .available = false,
    .epoch     = 1,
    .reason    = LG_TRANSPORT_DISCONNECTED,
  };
  if (!video)
    return status;
  LG_LOCK(video->stateLock);
  status = video->status;
  LG_UNLOCK(video->stateLock);
  return status;
}

bool networkVideoAttachRenderer(NetworkVideo * video,
    const LG_RendererInterop * interop)
{
  if (!video || !networkVideoDecoderAttach(video->decoder, interop))
    return false;

  LGNetVideoStreamConfig config;
  bool                   configured;
  uint64_t               decodeGeneration;
  LG_LOCK(video->stateLock);
  video->rendererAttached = true;
  configured              = video->wireConfigured;
  config                  = video->config;
  decodeGeneration        = video->decodeGeneration;
  LG_UNLOCK(video->stateLock);
  const NetworkVideoDecodeResult result = configured ?
    networkVideoDecoderConfigure(video->decoder, &config) :
    NETWORK_VIDEO_DECODE_UNSUPPORTED;

  LG_LOCK(video->stateLock);
  if (video->decodeGeneration == decodeGeneration &&
      video->rendererAttached && video->wireConfigured && configured &&
      video->config.configEpoch == config.configEpoch)
    video->decoderReady = result == NETWORK_VIDEO_DECODE_OK;

  const bool notify = networkVideoUpdateStatusNL(video);
  LG_UNLOCK(video->stateLock);
  if (notify)
    networkVideoNotifyStatus(video);
  return true;
}

void networkVideoDetachRenderer(NetworkVideo * video)
{
  if (!video)
    return;
  struct NetworkVideoEncoded   pendingDecode = {0};
  struct NetworkVideoOutput  * replaced;
  struct NetworkVideoOutput  * leases;
  bool                         notify;
  LG_LOCK(video->stateLock);
  video->rendererAttached = false;
  video->decoderReady     = false;
  video->waitCancelled    = true;
  networkVideoInvalidateDecodeNL(video, &pendingDecode);
  replaced      = video->output;
  video->output = NULL;
  leases        = networkVideoTakeLeasesNL(video);
  notify        = networkVideoUpdateStatusNL(video);
  LG_UNLOCK(video->stateLock);
  networkVideoClearEncoded(&pendingDecode);
  networkVideoFreeOutput(&replaced);
  networkVideoFreeOutputList(leases);
  networkVideoDecoderDetach(video->decoder);
  lgSignalEvent(video->frameEvent);
  if (notify)
    networkVideoNotifyStatus(video);
}

LG_TransportStatus networkVideoNextFrame(NetworkVideo * video, bool useDMA,
    LG_TransportFrame * frame)
{
  (void)useDMA;
  if (!video || !frame)
    return LG_TRANSPORT_ERROR;
  memset(frame, 0, sizeof(*frame));

  for (;;)
  {
    struct NetworkVideoOutput * released;
    LG_TransportStatus          status = LG_TRANSPORT_OK;
    bool                        ready  = false;
    LG_LOCK(video->stateLock);
    released = networkVideoTakeReleasedLeasesNL(video);
    if (video->waitCancelled)
    {
      video->waitCancelled = false;
      status               = LG_TRANSPORT_UNAVAILABLE;
      ready                = true;
    }
    else if (!video->connected)
    {
      status = LG_TRANSPORT_DISCONNECTED;
      ready  = true;
    }
    else if (!video->status.available)
    {
      status = video->status.reason;
      ready  = true;
    }
    else
    {
      struct NetworkVideoOutput * output = video->output;
      if (output && video->leaseCount < NETWORK_VIDEO_MAX_LEASES)
      {
        video->output      = NULL;
        video->leaseSerial = networkVideoNext64(video->leaseSerial);
        output->handle     = video->leaseSerial;
        output->next       = video->leases;
        video->leases      = output;
        ++video->leaseCount;
        frame->serial                 = output->frameID;
        frame->epoch                  = output->epoch;
        frame->timestamp              = output->timestamp;
        frame->format                 = &output->decoded.format;
        frame->externalFrame          =
          lgExternalFrameRetain(output->decoded.externalFrame);
        frame->releaseFn              = networkVideoReleaseLease;
        frame->releaseOpaque          = video;
        frame->releaseHandle          = output->handle;
        ready                         = true;
      }
    }
    LG_UNLOCK(video->stateLock);
    networkVideoFreeOutputList(released);
    if (ready)
      return status;
    if (!lgWaitEvent(video->frameEvent, 100))
      networkVideoProcess(video);
  }
}

void networkVideoGetFrameTiming(NetworkVideo * video,
    const LG_TransportFrame * frame, LG_TransportFrameTiming * timing)
{
  (void)video;
  if (!timing)
    return;
  memset(timing, 0, sizeof(*timing));
  if (!video || !frame || !frame->releaseHandle)
    return;

  LG_LOCK(video->stateLock);
  const struct NetworkVideoOutput * output =
    networkVideoFindLeaseNL(video, frame->releaseHandle);
  if (!output)
  {
    LG_UNLOCK(video->stateLock);
    return;
  }
  *timing                         = output->decoded.timing;
  timing->providerValid           = true;
  timing->receiveTime             = output->receiveTimeNs;
  timing->prepareTime             = output->decoded.decodeTimeNs;
  LG_UNLOCK(video->stateLock);
}

void networkVideoReleaseFrame(NetworkVideo * video,
    LG_TransportFrame * frame)
{
  if (!video || !frame)
    return;

  lgExternalFrameRelease(frame->externalFrame);
  frame->externalFrame = NULL;

  LG_LOCK(video->stateLock);
  struct NetworkVideoOutput * output =
    networkVideoTakeLeaseNL(video, frame->releaseHandle);
  LG_UNLOCK(video->stateLock);
  networkVideoFreeOutput(&output);
  memset(frame, 0, sizeof(*frame));
}

void networkVideoCancelFrameWait(NetworkVideo * video)
{
  if (!video)
    return;
  LG_LOCK(video->stateLock);
  video->waitCancelled = true;
  LG_UNLOCK(video->stateLock);
  lgSignalEvent(video->frameEvent);
}

void networkVideoStopFrame(NetworkVideo * video)
{
  if (!video)
    return;
  struct NetworkVideoEncoded   pendingDecode = {0};
  struct NetworkVideoOutput  * replaced;
  struct NetworkVideoOutput  * leases;
  LG_LOCK(video->stateLock);
  networkVideoInvalidateDecodeNL(video, &pendingDecode);
  replaced      = video->output;
  video->output = NULL;
  leases        = networkVideoTakeLeasesNL(video);
  LG_UNLOCK(video->stateLock);
  networkVideoClearEncoded(&pendingDecode);
  networkVideoFreeOutput(&replaced);
  networkVideoFreeOutputList(leases);
  lgSignalEvent(video->frameEvent);
}
