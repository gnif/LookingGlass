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

#include "audio.h"

#include "common/event.h"
#include "common/locking.h"
#include "common/time.h"

#include <LGProtocol/NetworkServices.h>

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_AUDIO_TIMER_MS                   10U
#define NETWORK_AUDIO_SUBSCRIBE_RETRY_MS        500U
#define NETWORK_AUDIO_KEEPALIVE_MS              500U
#define NETWORK_AUDIO_RENEWAL_MS                1000U
#define NETWORK_AUDIO_RENEWAL_RETRY_MS          100U
#define NETWORK_AUDIO_RENEWAL_TIMEOUT_MS        3000U
#define NETWORK_AUDIO_TARGET_LATENCY_US         2000U
#define NETWORK_AUDIO_MAX_PACKET_FRAMES         2048U
#define NETWORK_AUDIO_MAX_PCM_BYTES             65536U
#define NETWORK_AUDIO_MAX_SAMPLE_RATE           384000U
#define NETWORK_AUDIO_MAX_CHANNELS              8U
#define NETWORK_AUDIO_CHANNEL_MASK              UINT32_C(0x0003FFFF)
#define NETWORK_AUDIO_PLAYBACK_STREAM_ID        1U
#define NETWORK_AUDIO_CAPTURE_STREAM_ID         2U
#define NETWORK_AUDIO_CLOCK_CACHE_SIZE          16U
#define NETWORK_AUDIO_DATAGRAM_PAYLOAD          1136U
#define NETWORK_AUDIO_DATAGRAM_PCM_BYTES        \
  (NETWORK_AUDIO_DATAGRAM_PAYLOAD - LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE)

#define NETWORK_AUDIO_SUBSCRIBE_FLAGS \
  (LG_NET_AUDIO_SUBSCRIBE_LOW_LATENCY       | \
   LG_NET_AUDIO_SUBSCRIBE_EXCLUSIVE_CAPTURE | \
   LG_NET_AUDIO_SUBSCRIBE_CLOCK_FEEDBACK    | \
   LG_NET_AUDIO_SUBSCRIBE_DATAGRAM_PCM      | \
   LG_NET_AUDIO_SUBSCRIBE_RELIABLE_PCM)

#define NETWORK_AUDIO_DIRECTIONS \
  (LG_NET_AUDIO_DIRECTIONS_PLAYBACK | LG_NET_AUDIO_DIRECTIONS_CAPTURE)

#define NETWORK_AUDIO_SEMANTIC_FLAGS \
  (LG_NET_ENVELOPE_RESPONSE     | LG_NET_ENVELOPE_FINAL | \
   LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_ERROR | \
   LG_NET_ENVELOPE_EARLY_DATA)

enum NetworkAudioPhase
{
  NETWORK_AUDIO_DISCONNECTED,
  NETWORK_AUDIO_SUBSCRIBING,
  NETWORK_AUDIO_GRANTED,
  NETWORK_AUDIO_WAIT_BARRIER,
  NETWORK_AUDIO_READY,
  NETWORK_AUDIO_RELEASING,
};

struct NetworkAudioClockEntry
{
  bool                 valid;
  LGNetAudioClockState state;
};

struct NetworkAudioStream
{
  uint32_t            streamID;
  LGNetAudioDirection direction;

  LGNetAudioFormat wireFormat;
  LG_AudioFormat   format;
  uint32_t         generation;
  bool             formatValid;
  bool             active;

  uint64_t stateSequence;
  bool     volumeValid;
  uint8_t  volumeChannels;
  uint16_t volume[LG_AUDIO_MAX_CHANNELS];
  bool     muteValid;
  bool     mute;
  bool     masterMute;
  uint32_t mutedChannels;

  bool              barrierPending;
  bool              barrierReady;
  uint64_t          barrierRequestID;
  LGNetAudioBarrier barrier;

  bool     packetKnown;
  uint64_t lastPacketID;
  bool     discontinuity;
  unsigned clockInsert;
  struct NetworkAudioClockEntry clocks[NETWORK_AUDIO_CLOCK_CACHE_SIZE];
};

struct NetworkAudio
{
  NetworkConnection * connection;
  LGTimer           * timer;
  LGEvent           * statusIdle;
  LGEvent           * eventIdle;
  LG_Lock             stateLock;
  LG_Lock             eventLock;

  bool                   destroying;
  bool                   connected;
  bool                   serviceAvailable;
  bool                   wanted;
  uint16_t               serviceVersion;
  enum NetworkAudioPhase phase;

  uint64_t subscriberID;
  uint64_t requestSerial;
  uint64_t subscribeRequestID;
  uint64_t grantRequestID;
  uint64_t subscriptionEpoch;
  uint64_t lastSubscriptionEpoch;
  uint64_t controlSequence;
  uint64_t capturePacketID;
  uint64_t captureFramePosition;
  uint64_t nextSubscribeNs;
  uint64_t nextKeepaliveNs;
  uint64_t nextRenewalNs;
  uint64_t renewalDeadlineNs;

  LGNetAudioRoles requestedRoles;
  LGNetAudioRoles grantedRoles;
  uint32_t        targetLatencyUs;
  uint32_t        maxPacketFrames;
  uint32_t        generationSerial;
  uint32_t        endpointGeneration;
  bool            statusAvailable;
  bool            statusNotifyPending;
  bool            statusDispatching;
  bool            eventDispatching;

  struct NetworkAudioStream playback;
  struct NetworkAudioStream capture;
  atomic_bool               feedbackAvailable;

  const LG_AudioEventOps * events;
  void                   * eventOpaque;
  bool                     attached;

  LG_AudioStatusFn statusCallback;
  void           * statusOpaque;
};

static _Thread_local NetworkAudio * l_networkAudioStatusCallback;
static const uint8_t                l_networkAudioSilence
  [NETWORK_AUDIO_MAX_PCM_BYTES];

static uint64_t networkAudioNext64(uint64_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint32_t networkAudioNext32(uint32_t value)
{
  ++value;
  return value ? value : 1U;
}

static bool networkAudioVersionSupported(uint16_t version)
{
  return version >= LG_NET_AUDIO_OWNERSHIP_INTRODUCED_SERVICE_VERSION &&
    version <= LG_NET_AUDIO_VERSION_CURRENT;
}

static uint64_t networkAudioDeadline(uint32_t milliseconds)
{
  return nanotime() + (uint64_t)milliseconds * UINT64_C(1000000);
}

static unsigned networkAudioBitCount(uint32_t value)
{
  unsigned count = 0;
  while (value)
  {
    count += value & 1U;
    value >>= 1;
  }
  return count;
}

static unsigned networkAudioSampleSize(LGNetAudioSampleFormat format)
{
  switch (format)
  {
    case LG_NET_AUDIO_SAMPLE_S16_LE:
      return 2U;

    case LG_NET_AUDIO_SAMPLE_S24_LE:
      return 3U;

    case LG_NET_AUDIO_SAMPLE_S32_LE:
    case LG_NET_AUDIO_SAMPLE_F32_LE:
      return 4U;
  }

  return 0;
}

static LG_AudioChannel networkAudioChannel(unsigned bit)
{
  static const LG_AudioChannel channels[] =
  {
    LG_AUDIO_CH_FRONT_LEFT,
    LG_AUDIO_CH_FRONT_RIGHT,
    LG_AUDIO_CH_FRONT_CENTER,
    LG_AUDIO_CH_LFE,
    LG_AUDIO_CH_REAR_LEFT,
    LG_AUDIO_CH_REAR_RIGHT,
    LG_AUDIO_CH_FRONT_LEFT_CENTER,
    LG_AUDIO_CH_FRONT_RIGHT_CENTER,
    LG_AUDIO_CH_REAR_CENTER,
    LG_AUDIO_CH_SIDE_LEFT,
    LG_AUDIO_CH_SIDE_RIGHT,
    LG_AUDIO_CH_TOP_CENTER,
    LG_AUDIO_CH_TOP_FRONT_LEFT,
    LG_AUDIO_CH_TOP_FRONT_CENTER,
    LG_AUDIO_CH_TOP_FRONT_RIGHT,
    LG_AUDIO_CH_TOP_REAR_LEFT,
    LG_AUDIO_CH_TOP_REAR_CENTER,
    LG_AUDIO_CH_TOP_REAR_RIGHT,
  };

  return bit < sizeof(channels) / sizeof(channels[0]) ?
    channels[bit] : LG_AUDIO_CH_UNKNOWN;
}

static bool networkAudioFormat(const LGNetAudioFormat * wire,
    uint32_t grantedFrames, LG_AudioFormat * format, size_t * frameSize)
{
  if (!wire || !format || !frameSize || !lgNetAudioFormatValid(wire) ||
      wire->sampleRate > NETWORK_AUDIO_MAX_SAMPLE_RATE ||
      wire->channels > NETWORK_AUDIO_MAX_CHANNELS ||
      !wire->channelMask ||
      (wire->channelMask & ~NETWORK_AUDIO_CHANNEL_MASK) ||
      networkAudioBitCount(wire->channelMask) != wire->channels ||
      wire->framesPerPacket > NETWORK_AUDIO_MAX_PACKET_FRAMES ||
      wire->maxPacketFrames > NETWORK_AUDIO_MAX_PACKET_FRAMES ||
      wire->maxPacketFrames > grantedFrames)
    return false;

  LG_AudioSampleFormat sampleFormat;
  const unsigned       sampleSize = networkAudioSampleSize(
      wire->sampleFormat);
  switch (wire->sampleFormat)
  {
    case LG_NET_AUDIO_SAMPLE_S16_LE:
      sampleFormat = LG_AUDIO_FMT_S16_LE;
      break;

    case LG_NET_AUDIO_SAMPLE_S24_LE:
      sampleFormat = LG_AUDIO_FMT_S24_LE;
      break;

    case LG_NET_AUDIO_SAMPLE_S32_LE:
      sampleFormat = LG_AUDIO_FMT_S32_LE;
      break;

    case LG_NET_AUDIO_SAMPLE_F32_LE:
      sampleFormat = LG_AUDIO_FMT_F32_LE;
      break;

    default:
      return false;
  }

  const size_t bytes = (size_t)sampleSize * wire->channels;
  if (!sampleSize || !bytes ||
      wire->maxPacketFrames > NETWORK_AUDIO_MAX_PCM_BYTES / bytes)
    return false;

  *format = (LG_AudioFormat)
  {
    .sampleFormat = sampleFormat,
    .sampleRate   = wire->sampleRate,
    .channelCount = (uint8_t)wire->channels,
  };
  unsigned channel = 0;
  for (unsigned bit = 0; bit < 18U; ++bit)
    if (wire->channelMask & (UINT32_C(1) << bit))
      format->channels[channel++] = networkAudioChannel(bit);

  *frameSize = bytes;
  return true;
}

static uint16_t networkAudioVolumeLevel(int32_t millibels)
{
  if (millibels <= LG_NET_AUDIO_VOLUME_MIN_MILLIBELS)
    return 0;
  if (millibels >= 0)
    return UINT16_MAX;

  const double linear = pow(10.0, (double)millibels / 2000.0);
  const double raw    = log((linear + 0.000172787) / 9.3234e-7) /
    log(1.000211902);
  if (!isfinite(raw) || raw <= 0.0)
    return 0;
  if (raw >= UINT16_MAX)
    return UINT16_MAX;
  return (uint16_t)(raw + 0.5);
}

static bool networkAudioRateQ32(double rate, bool allowZero,
    uint64_t * result)
{
  if (!result || !isfinite(rate) || rate < 0.0 ||
      (!allowZero && rate == 0.0) ||
      rate > NETWORK_AUDIO_MAX_SAMPLE_RATE)
    return false;

  const long double scaled = (long double)rate * 4294967296.0L;
  if (scaled > (long double)LG_NET_AUDIO_MAX_RATE_Q32)
    return false;

  *result = (uint64_t)(scaled + 0.5L);
  return allowZero || *result != 0;
}

static double networkAudioRate(uint64_t rateQ32)
{
  return (double)rateQ32 / 4294967296.0;
}

static LG_AudioStatus networkAudioStatusNL(const NetworkAudio * audio)
{
  return (LG_AudioStatus)
  {
    .available  = audio->statusAvailable,
    .generation = audio->endpointGeneration,
  };
}

static void networkAudioSetAvailableNL(NetworkAudio * audio,
    bool available, bool restart)
{
  if (!restart && audio->statusAvailable == available)
    return;

  audio->statusAvailable     = available;
  audio->endpointGeneration  =
    networkAudioNext32(audio->endpointGeneration);
  audio->statusNotifyPending = true;
}

static void networkAudioDispatchStatus(NetworkAudio * audio,
    LG_AudioStatusFn callback, void * opaque, LG_AudioStatus status)
{
  while (callback)
  {
    NetworkAudio * previous       = l_networkAudioStatusCallback;
    l_networkAudioStatusCallback  = audio;
    callback(opaque, &status);
    l_networkAudioStatusCallback  = previous;

    LG_LOCK(audio->stateLock);
    if (audio->statusNotifyPending && audio->statusCallback)
    {
      audio->statusNotifyPending = false;
      callback                   = audio->statusCallback;
      opaque                     = audio->statusOpaque;
      status                     = networkAudioStatusNL(audio);
      LG_UNLOCK(audio->stateLock);
      continue;
    }

    audio->statusDispatching = false;
    lgSignalEvent(audio->statusIdle);
    LG_UNLOCK(audio->stateLock);
    break;
  }
}

static void networkAudioNotifyStatus(NetworkAudio * audio)
{
  LG_AudioStatusFn callback;
  void           * opaque;
  LG_AudioStatus   status;

  LG_LOCK(audio->stateLock);
  if (!audio->statusNotifyPending || audio->statusDispatching)
  {
    LG_UNLOCK(audio->stateLock);
    return;
  }

  audio->statusNotifyPending = false;
  callback                   = audio->statusCallback;
  opaque                     = audio->statusOpaque;
  status                     = networkAudioStatusNL(audio);
  if (!callback)
  {
    LG_UNLOCK(audio->stateLock);
    return;
  }

  audio->statusDispatching = true;
  lgResetEvent(audio->statusIdle);
  LG_UNLOCK(audio->stateLock);
  networkAudioDispatchStatus(audio, callback, opaque, status);
}

static void networkAudioBeginEvents(NetworkAudio * audio)
{
  for (;;)
  {
    LG_LOCK(audio->eventLock);
    if (!audio->eventDispatching)
    {
      audio->eventDispatching = true;
      lgResetEvent(audio->eventIdle);
      return;
    }

    LG_UNLOCK(audio->eventLock);
    lgWaitEvent(audio->eventIdle, TIMEOUT_INFINITE);
  }
}

static void networkAudioAbortEventsNL(NetworkAudio * audio)
{
  audio->eventDispatching = false;
  lgSignalEvent(audio->eventIdle);
  LG_UNLOCK(audio->eventLock);
}

static void networkAudioCallEventsNL(NetworkAudio * audio)
{
  LG_UNLOCK(audio->eventLock);
}

static void networkAudioEndEvents(NetworkAudio * audio)
{
  LG_LOCK(audio->eventLock);
  audio->eventDispatching = false;
  lgSignalEvent(audio->eventIdle);
  LG_UNLOCK(audio->eventLock);
}

static void networkAudioClearClockNL(struct NetworkAudioStream * stream)
{
  stream->clockInsert = 0;
  memset(stream->clocks, 0, sizeof(stream->clocks));
}

static void networkAudioClearStreamNL(struct NetworkAudioStream * stream)
{
  const uint32_t            streamID  = stream->streamID;
  const LGNetAudioDirection direction = stream->direction;
  memset(stream, 0, sizeof(*stream));
  stream->streamID  = streamID;
  stream->direction = direction;
}

static bool networkAudioStreamRequiredNL(const NetworkAudio * audio,
    const struct NetworkAudioStream * stream)
{
  const LGNetAudioRoles role = stream->direction ==
      LG_NET_AUDIO_DIRECTION_PLAYBACK ?
    LG_NET_AUDIO_ROLE_PLAYBACK : LG_NET_AUDIO_ROLE_CAPTURE;
  return (audio->grantedRoles & role) != 0;
}

static void networkAudioUpdateReadyNL(NetworkAudio * audio)
{
  const bool granted       = audio->phase == NETWORK_AUDIO_GRANTED ||
    audio->phase == NETWORK_AUDIO_WAIT_BARRIER ||
    audio->phase == NETWORK_AUDIO_READY;
  const bool playbackReady =
    !networkAudioStreamRequiredNL(audio, &audio->playback) ||
    audio->playback.barrierReady;
  const bool captureReady  =
    !networkAudioStreamRequiredNL(audio, &audio->capture) ||
    audio->capture.barrierReady;

  if (granted)
    audio->phase = playbackReady && captureReady ?
      NETWORK_AUDIO_READY : NETWORK_AUDIO_WAIT_BARRIER;

  const bool feedback = audio->phase == NETWORK_AUDIO_READY &&
    audio->playback.barrierReady && audio->playback.active &&
    audio->playback.packetKnown &&
    (audio->grantedRoles & LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK);
  atomic_store_explicit(
      &audio->feedbackAvailable, feedback, memory_order_release);
}

static uint32_t networkAudioMapGenerationNL(NetworkAudio * audio,
    struct NetworkAudioStream * stream, uint64_t formatEpoch)
{
  if (stream->formatValid &&
      stream->wireFormat.formatEpoch == formatEpoch)
    return stream->generation;

  audio->generationSerial = networkAudioNext32(audio->generationSerial);
  return audio->generationSerial;
}

static void networkAudioInvalidateBarrierNL(NetworkAudio * audio,
    struct NetworkAudioStream * stream)
{
  stream->barrierPending   = false;
  stream->barrierReady     = false;
  stream->barrierRequestID = 0;
  memset(&stream->barrier, 0, sizeof(stream->barrier));
  networkAudioUpdateReadyNL(audio);
}

static bool networkAudioSubscriptionCurrentNL(const NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  return audio->connected && audio->serviceAvailable &&
    audio->subscriptionEpoch &&
    lgNetAudioEnvelopeMatchesSubscription(
      &packet->envelope, audio->subscriptionEpoch);
}

static void networkAudioStopStreams(NetworkAudio * audio)
{
  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  const LG_AudioEventOps * events             =
    audio->attached ? audio->events : NULL;
  void                   * opaque             = audio->eventOpaque;
  const uint32_t           playbackGeneration = audio->playback.active ?
    audio->playback.generation : 0;
  const uint32_t           captureGeneration  = audio->capture.active ?
    audio->capture.generation : 0;
  networkAudioClearStreamNL(&audio->playback);
  networkAudioClearStreamNL(&audio->capture);
  atomic_store_explicit(
      &audio->feedbackAvailable, false, memory_order_release);
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && playbackGeneration && events->playbackStop)
    events->playbackStop(opaque, playbackGeneration);
  if (events && captureGeneration && events->recordStop)
    events->recordStop(opaque, captureGeneration);
  networkAudioEndEvents(audio);
}

static bool networkAudioReliableFlags(const LGNetPacketView * packet,
    LGNetEnvelopeFlags semantic)
{
  return packet &&
    (packet->envelope.flags &
      (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM)) ==
        LG_NET_ENVELOPE_RELIABLE &&
    (packet->envelope.flags & NETWORK_AUDIO_SEMANTIC_FLAGS) == semantic;
}

static bool networkAudioDatagramFlags(const LGNetPacketView * packet)
{
  return packet &&
    (packet->envelope.flags &
      (LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM)) ==
        LG_NET_ENVELOPE_DATAGRAM &&
    (packet->envelope.flags & NETWORK_AUDIO_SEMANTIC_FLAGS) ==
      LG_NET_ENVELOPE_FINAL;
}

static LG_TransportStatus networkAudioSendReliableNL(NetworkAudio * audio,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  return networkConnectionSend(audio->connection, LG_NET_SERVICE_AUDIO,
      messageType, audio->serviceVersion, flags | LG_NET_ENVELOPE_FINAL,
      audio->subscriptionEpoch, requestID, payload, payloadLength);
}

static LG_TransportStatus networkAudioSendDatagramNL(NetworkAudio * audio,
    uint16_t messageType, uint64_t requestID,
    const void * payload, uint32_t payloadLength)
{
  return networkConnectionSendDatagram(audio->connection,
      LG_NET_SERVICE_AUDIO, messageType, audio->serviceVersion, 0,
      audio->subscriptionEpoch, requestID, payload, payloadLength, 0, false);
}

static bool networkAudioSendSubscribeNL(NetworkAudio * audio)
{
  if (!audio->connected || !audio->serviceAvailable || !audio->wanted ||
      audio->destroying)
    return false;

  const LGNetAudioSubscribe subscribe =
  {
    .subscriberID    = audio->subscriberID,
    .directions      = NETWORK_AUDIO_DIRECTIONS,
    .targetLatencyUs = NETWORK_AUDIO_TARGET_LATENCY_US,
    .maxPacketFrames = NETWORK_AUDIO_MAX_PACKET_FRAMES,
    .flags           = NETWORK_AUDIO_SUBSCRIBE_FLAGS,
  };
  uint8_t payload[LG_NET_AUDIO_SUBSCRIBE_WIRE_SIZE];
  if (!lgNetAudioSubscribeEncodeForVersion(payload, sizeof(payload),
        &subscribe, audio->serviceVersion))
    return false;

  if (!audio->subscribeRequestID)
  {
    audio->requestSerial      = networkAudioNext64(audio->requestSerial);
    audio->subscribeRequestID = audio->requestSerial;
  }

  const LG_TransportStatus status = networkConnectionSend(
      audio->connection, LG_NET_SERVICE_AUDIO,
      LG_NET_AUDIO_MESSAGE_SUBSCRIBE, audio->serviceVersion,
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL,
      0, audio->subscribeRequestID, payload, sizeof(payload));
  audio->phase           = NETWORK_AUDIO_SUBSCRIBING;
  audio->nextSubscribeNs = networkAudioDeadline(
      NETWORK_AUDIO_SUBSCRIBE_RETRY_MS);
  return status == LG_TRANSPORT_OK;
}

static bool networkAudioSendRenewalNL(NetworkAudio * audio)
{
  if (!audio->subscriptionEpoch || !audio->grantRequestID)
    return false;

  const LGNetAudioSubscribe subscribe =
  {
    .subscriberID    = audio->subscriberID,
    .directions      = NETWORK_AUDIO_DIRECTIONS,
    .targetLatencyUs = NETWORK_AUDIO_TARGET_LATENCY_US,
    .maxPacketFrames = NETWORK_AUDIO_MAX_PACKET_FRAMES,
    .flags           = NETWORK_AUDIO_SUBSCRIBE_FLAGS,
  };
  uint8_t payload[LG_NET_AUDIO_SUBSCRIBE_WIRE_SIZE];
  if (!lgNetAudioSubscribeEncodeForVersion(payload, sizeof(payload),
        &subscribe, audio->serviceVersion))
    return false;

  const LG_TransportStatus status = networkConnectionSend(
      audio->connection, LG_NET_SERVICE_AUDIO,
      LG_NET_AUDIO_MESSAGE_SUBSCRIBE, audio->serviceVersion,
      LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL,
      0, audio->grantRequestID, payload, sizeof(payload));
  audio->nextRenewalNs = networkAudioDeadline(
      status == LG_TRANSPORT_OK ? NETWORK_AUDIO_RENEWAL_MS :
        NETWORK_AUDIO_RENEWAL_RETRY_MS);
  return status == LG_TRANSPORT_OK;
}

static bool networkAudioSendBarrierAckNL(NetworkAudio * audio,
    struct NetworkAudioStream * stream)
{
  if (!stream->barrierPending || !audio->subscriptionEpoch)
    return true;

  uint8_t payload[LG_NET_AUDIO_BARRIER_WIRE_SIZE];
  if (!lgNetAudioBarrierEncode(payload, sizeof(payload), &stream->barrier))
    return false;

  const LG_TransportStatus status = networkAudioSendReliableNL(audio,
      LG_NET_AUDIO_MESSAGE_STATE_ACK, LG_NET_ENVELOPE_RESPONSE,
      stream->barrierRequestID, payload, sizeof(payload));
  if (status != LG_TRANSPORT_OK)
    return false;

  stream->barrierPending   = false;
  stream->barrierReady     = true;
  stream->barrierRequestID = 0;
  networkAudioUpdateReadyNL(audio);
  return true;
}

static bool networkAudioSendControlNL(NetworkAudio * audio,
    uint16_t messageType, LGNetAudioControlFlags flags)
{
  const bool playback =
    (audio->grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK) != 0;
  const struct NetworkAudioStream * stream = playback ?
    &audio->playback : &audio->capture;
  audio->controlSequence = networkAudioNext64(audio->controlSequence);
  const LGNetAudioControl control =
  {
    .streamID    = stream->streamID,
    .direction   = stream->direction,
    .flags       = flags,
    .sequence    = audio->controlSequence,
    .formatEpoch = stream->formatValid ?
      stream->wireFormat.formatEpoch : 0,
    .detail      = 0,
  };
  uint8_t payload[LG_NET_AUDIO_CONTROL_WIRE_SIZE];
  if (!lgNetAudioControlEncode(payload, sizeof(payload), &control))
    return false;

  return networkAudioSendReliableNL(audio, messageType, 0, 0,
      payload, sizeof(payload)) == LG_TRANSPORT_OK;
}

static void networkAudioClearSubscriptionNL(NetworkAudio * audio,
    bool disconnected)
{
  audio->phase                = NETWORK_AUDIO_DISCONNECTED;
  audio->subscribeRequestID   = 0;
  audio->grantRequestID       = 0;
  audio->subscriptionEpoch    = 0;
  audio->controlSequence      = 0;
  audio->capturePacketID      = 0;
  audio->captureFramePosition = 0;
  audio->nextSubscribeNs      = 0;
  audio->nextKeepaliveNs      = 0;
  audio->nextRenewalNs        = 0;
  audio->renewalDeadlineNs    = 0;
  audio->grantedRoles         = 0;
  audio->targetLatencyUs      = 0;
  audio->maxPacketFrames      = 0;
  if (disconnected)
    audio->lastSubscriptionEpoch = 0;
  networkAudioSetAvailableNL(audio, false, false);
  atomic_store_explicit(
      &audio->feedbackAvailable, false, memory_order_release);
}

void networkAudioProcess(NetworkAudio * audio)
{
  if (!audio)
    return;

  bool stopStreams = false;
  LG_LOCK(audio->stateLock);
  if (audio->destroying)
  {
    LG_UNLOCK(audio->stateLock);
    return;
  }

  const uint64_t now = nanotime();
  if (!audio->connected || !audio->serviceAvailable)
  {
    LG_UNLOCK(audio->stateLock);
    return;
  }

  if (!audio->wanted)
  {
    if (audio->phase == NETWORK_AUDIO_SUBSCRIBING)
      networkAudioClearSubscriptionNL(audio, false);
    else if (audio->subscriptionEpoch &&
        audio->phase != NETWORK_AUDIO_RELEASING)
      audio->phase = NETWORK_AUDIO_RELEASING;

    if (audio->phase == NETWORK_AUDIO_RELEASING &&
        networkAudioSendControlNL(audio, LG_NET_AUDIO_MESSAGE_RELEASE,
          LG_NET_AUDIO_CONTROL_GRACEFUL | LG_NET_AUDIO_CONTROL_FLUSH))
    {
      networkAudioClearSubscriptionNL(audio, false);
      stopStreams = true;
    }
    LG_UNLOCK(audio->stateLock);
    if (stopStreams)
      networkAudioStopStreams(audio);
    return;
  }

  if (audio->subscriptionEpoch && audio->renewalDeadlineNs &&
      now >= audio->renewalDeadlineNs)
  {
    networkAudioClearSubscriptionNL(audio, false);
    LG_UNLOCK(audio->stateLock);
    networkAudioStopStreams(audio);
    return;
  }

  if (audio->phase == NETWORK_AUDIO_DISCONNECTED ||
      (audio->phase == NETWORK_AUDIO_SUBSCRIBING &&
        now >= audio->nextSubscribeNs))
    networkAudioSendSubscribeNL(audio);

  if (audio->subscriptionEpoch &&
      (audio->phase == NETWORK_AUDIO_GRANTED ||
       audio->phase == NETWORK_AUDIO_WAIT_BARRIER ||
       audio->phase == NETWORK_AUDIO_READY))
  {
    networkAudioSendBarrierAckNL(audio, &audio->playback);
    networkAudioSendBarrierAckNL(audio, &audio->capture);
    if (now >= audio->nextRenewalNs)
      networkAudioSendRenewalNL(audio);
    if (now >= audio->nextKeepaliveNs)
    {
      if (networkAudioSendControlNL(audio,
            LG_NET_AUDIO_MESSAGE_KEEPALIVE, 0))
        audio->nextKeepaliveNs = networkAudioDeadline(
            NETWORK_AUDIO_KEEPALIVE_MS);
    }
  }
  LG_UNLOCK(audio->stateLock);
}

void networkAudioSendReady(NetworkAudio * audio, bool datagram)
{
  (void)datagram;
  networkAudioProcess(audio);
}

static bool networkAudioTimer(void * opaque)
{
  NetworkAudio * audio = opaque;
  LG_LOCK(audio->stateLock);
  const bool destroying = audio->destroying;
  LG_UNLOCK(audio->stateLock);
  if (destroying)
    return false;

  networkAudioProcess(audio);
  networkAudioNotifyStatus(audio);
  return true;
}

static void networkAudioSetStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  NetworkAudio * audio     = opaque;
  const bool     reentrant = l_networkAudioStatusCallback == audio;

  LG_LOCK(audio->stateLock);
  audio->statusCallback      = NULL;
  audio->statusOpaque        = NULL;
  audio->statusNotifyPending = false;

  bool dispatching = audio->statusDispatching;
  LG_UNLOCK(audio->stateLock);

  while (dispatching && !reentrant)
  {
    lgWaitEvent(audio->statusIdle, TIMEOUT_INFINITE);
    LG_LOCK(audio->stateLock);
    dispatching = audio->statusDispatching;
    LG_UNLOCK(audio->stateLock);
  }

  if (!callback)
  {
    LG_LOCK(audio->stateLock);
    audio->wanted = false;
    LG_UNLOCK(audio->stateLock);
    networkAudioProcess(audio);
    return;
  }

  LG_LOCK(audio->stateLock);
  audio->statusCallback      = callback;
  audio->statusOpaque        = callbackOpaque;
  audio->statusNotifyPending = false;
  audio->wanted              = true;

  const LG_AudioStatus status        = networkAudioStatusNL(audio);
  const bool           startDispatch = !audio->statusDispatching;
  if (startDispatch)
  {
    audio->statusDispatching = true;
    lgResetEvent(audio->statusIdle);
  }
  LG_UNLOCK(audio->stateLock);

  if (startDispatch)
    networkAudioDispatchStatus(audio, callback, callbackOpaque, status);
  else
  {
    NetworkAudio * previous       = l_networkAudioStatusCallback;
    l_networkAudioStatusCallback  = audio;
    callback(callbackOpaque, &status);
    l_networkAudioStatusCallback  = previous;
  }
  networkAudioProcess(audio);
}

static bool networkAudioAttach(void * opaque,
    const LG_AudioEventOps * events, void * eventOpaque)
{
  NetworkAudio * audio = opaque;
  if (!events)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  const bool admitted = audio->statusAvailable && !audio->attached &&
    audio->subscriptionEpoch &&
    audio->phase != NETWORK_AUDIO_RELEASING;
  if (admitted)
  {
    audio->events      = events;
    audio->eventOpaque = eventOpaque;
    audio->attached    = true;
  }
  const struct NetworkAudioStream playback = audio->playback;
  const struct NetworkAudioStream capture  = audio->capture;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (admitted && playback.active && events->playbackStart)
  {
    events->playbackStart(eventOpaque, playback.generation,
        &playback.format, NULL);
    if (playback.volumeValid && events->playbackVolume)
      events->playbackVolume(eventOpaque, playback.generation,
          playback.volumeChannels, playback.volume);
    if (playback.muteValid && events->playbackMute)
      events->playbackMute(eventOpaque, playback.generation,
          playback.mute);
  }
  if (admitted && capture.active && events->recordStart)
  {
    events->recordStart(eventOpaque, capture.generation,
        &capture.format);
    if (capture.volumeValid && events->recordVolume)
      events->recordVolume(eventOpaque, capture.generation,
          capture.volumeChannels, capture.volume);
    if (capture.muteValid && events->recordMute)
      events->recordMute(eventOpaque, capture.generation, capture.mute);
  }
  networkAudioEndEvents(audio);
  return admitted;
}

static void networkAudioDetach(void * opaque)
{
  NetworkAudio * audio = opaque;
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->stateLock);
  audio->events      = NULL;
  audio->eventOpaque = NULL;
  audio->attached    = false;
  LG_UNLOCK(audio->stateLock);

  while (audio->eventDispatching)
  {
    LG_UNLOCK(audio->eventLock);
    lgWaitEvent(audio->eventIdle, TIMEOUT_INFINITE);
    LG_LOCK(audio->eventLock);
  }
  LG_UNLOCK(audio->eventLock);
}

static bool networkAudioSendClockAndDataNL(NetworkAudio * audio,
    const uint8_t * clockPayload, const uint8_t * dataPayload,
    uint32_t dataPayloadLength)
{
  /* CLOCK_STATE and PCM are an ordered pair. Keep both on the same reliable
   * stream until a future protocol version provides atomic datagram framing. */
  if (!(audio->grantedRoles & LG_NET_AUDIO_ROLE_RELIABLE_PCM))
    return false;

  return networkAudioSendReliableNL(audio,
      LG_NET_AUDIO_MESSAGE_CLOCK_STATE, 0, 0, clockPayload,
      LG_NET_AUDIO_CLOCK_STATE_WIRE_SIZE) == LG_TRANSPORT_OK &&
    networkAudioSendReliableNL(audio,
      LG_NET_AUDIO_MESSAGE_CAPTURE_DATA, 0, 0, dataPayload,
      dataPayloadLength) == LG_TRANSPORT_OK;
}

static bool networkAudioRecordData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  NetworkAudio * audio = opaque;
  if (!generation || !data || !frames)
    return false;

  uint64_t rateQ32 = 0;
  if (sourceClock &&
      !networkAudioRateQ32(sourceClock->rate, true, &rateQ32))
    return false;

  LG_LOCK(audio->stateLock);
  struct NetworkAudioStream * stream = &audio->capture;
  if (audio->phase != NETWORK_AUDIO_READY ||
      !stream->active || !stream->formatValid ||
      !stream->barrierReady || stream->generation != generation ||
      !(audio->grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE) ||
      !(audio->grantedRoles & LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE))
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  const size_t frameSize = (size_t)networkAudioSampleSize(
      stream->wireFormat.sampleFormat) * stream->wireFormat.channels;
  if (!frameSize || frames > SIZE_MAX / frameSize)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }
  const uint64_t firstPosition = sourceClock ?
    sourceClock->position : audio->captureFramePosition;
  if (frames > UINT64_MAX - firstPosition)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  size_t maxFrames = stream->wireFormat.maxPacketFrames;
  if (maxFrames > audio->maxPacketFrames)
    maxFrames = audio->maxPacketFrames;
  if (maxFrames > NETWORK_AUDIO_MAX_PCM_BYTES / frameSize)
    maxFrames = NETWORK_AUDIO_MAX_PCM_BYTES / frameSize;
  if (audio->grantedRoles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM)
  {
    const size_t datagramFrames =
      NETWORK_AUDIO_DATAGRAM_PCM_BYTES / frameSize;
    if (datagramFrames && maxFrames > datagramFrames)
      maxFrames = datagramFrames;
  }
  if (!maxFrames)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  uint8_t * payload = malloc(LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE +
      maxFrames * frameSize);
  if (!payload)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  bool         result        = true;
  bool         discontinuity = stream->discontinuity ||
    (sourceClock && sourceClock->discontinuity);
  size_t       offset        = 0;
  const uint8_t * pcm        = data;
  while (offset < frames)
  {
    size_t chunkFrames = frames - offset;
    if (chunkFrames > maxFrames)
      chunkFrames = maxFrames;
    const size_t chunkBytes = chunkFrames * frameSize;

    audio->capturePacketID = networkAudioNext64(audio->capturePacketID);

    const uint64_t packetID  = audio->capturePacketID;
    uint64_t       position  = firstPosition + offset;
    int64_t        timeNs    = sourceClock ? sourceClock->time : 0;
    if (sourceClock && offset)
    {
      const long double elapsed  =
        (long double)offset * 1000000000.0L /
          stream->wireFormat.sampleRate;
      const long double adjusted = (long double)timeNs + elapsed;
      if (adjusted >= (long double)INT64_MIN &&
          adjusted <= (long double)INT64_MAX)
        timeNs = (int64_t)adjusted;
    }
    uint64_t timestampNs = timeNs > 0 ? (uint64_t)timeNs : nanotime();
    if (!timestampNs)
      timestampNs = 1;

    const LGNetAudioClockState clock =
    {
      .streamID          = stream->streamID,
      .direction         = stream->direction,
      .flags             = sourceClock ?
        LG_NET_AUDIO_CLOCK_VALID |
          (sourceClock->stable ? LG_NET_AUDIO_CLOCK_STABLE : 0) : 0,
      .formatEpoch       = stream->wireFormat.formatEpoch,
      .subscriptionEpoch = audio->subscriptionEpoch,
      .packetID          = packetID,
      .framePosition     = position,
      .timeNs            = timeNs,
      .rateQ32           = rateQ32,
      .targetRateQ32     = 0,
      .queuedFrames      = 0,
      .driftPpm          = 0,
    };
    const LGNetAudioData packet =
    {
      .streamID    = stream->streamID,
      .direction   = stream->direction,
      .flags       = discontinuity ?
        LG_NET_AUDIO_DATA_DISCONTINUITY : 0,
      .formatEpoch = stream->wireFormat.formatEpoch,
      .packetID    = packetID,
      .timestampNs = timestampNs,
      .startFrame  = position,
      .frameCount  = (uint32_t)chunkFrames,
      .dataLength  = (uint32_t)chunkBytes,
      .data        = pcm + offset * frameSize,
    };
    uint8_t      clockPayload[LG_NET_AUDIO_CLOCK_STATE_WIRE_SIZE];
    const size_t dataPayloadLength =
      LG_NET_AUDIO_DATA_HEADER_WIRE_SIZE + chunkBytes;
    if (!lgNetAudioClockStateEncode(clockPayload,
          sizeof(clockPayload), &clock) ||
        !lgNetAudioDataEncode(payload, dataPayloadLength, &packet) ||
        !networkAudioSendClockAndDataNL(audio, clockPayload, payload,
          (uint32_t)dataPayloadLength))
    {
      result = false;
      break;
    }

    discontinuity = false;
    offset        += chunkFrames;
  }

  audio->captureFramePosition = firstPosition + offset;
  stream->discontinuity       = !result;
  free(payload);
  LG_UNLOCK(audio->stateLock);
  return result;
}

static bool networkAudioClockFeedback(void * opaque, uint32_t generation,
    const LG_AudioClock * playbackClock, double targetRate)
{
  NetworkAudio * audio = opaque;
  uint64_t       rateQ32;
  uint64_t       targetRateQ32;
  if (!generation || !playbackClock ||
      !networkAudioRateQ32(playbackClock->rate, true, &rateQ32) ||
      !networkAudioRateQ32(targetRate, false, &targetRateQ32))
    return false;

  LG_LOCK(audio->stateLock);
  const struct NetworkAudioStream * stream = &audio->playback;
  if (audio->phase != NETWORK_AUDIO_READY ||
      !stream->active || !stream->formatValid ||
      !stream->barrierReady || !stream->packetKnown ||
      stream->generation != generation ||
      !(audio->grantedRoles & LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK))
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  const LGNetAudioClockState feedback =
  {
    .streamID          = stream->streamID,
    .direction         = stream->direction,
    .flags             = LG_NET_AUDIO_CLOCK_VALID |
      (playbackClock->stable ? LG_NET_AUDIO_CLOCK_STABLE : 0),
    .formatEpoch       = stream->wireFormat.formatEpoch,
    .subscriptionEpoch = audio->subscriptionEpoch,
    .packetID          = stream->lastPacketID,
    .framePosition     = playbackClock->position,
    .timeNs            = playbackClock->time,
    .rateQ32           = rateQ32,
    .targetRateQ32     = targetRateQ32,
    .queuedFrames      = 0,
    .driftPpm          = 0,
  };
  uint8_t    payload[LG_NET_AUDIO_CLOCK_STATE_WIRE_SIZE];
  const bool encoded = lgNetAudioClockStateEncode(
      payload, sizeof(payload), &feedback);
  bool       result  = false;
  if (encoded &&
      (audio->grantedRoles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM))
    result = networkAudioSendDatagramNL(audio,
        LG_NET_AUDIO_MESSAGE_CLOCK_STATE, 0,
        payload, sizeof(payload)) == LG_TRANSPORT_OK;
  if (encoded && !result &&
      (audio->grantedRoles & LG_NET_AUDIO_ROLE_RELIABLE_PCM))
    result = networkAudioSendReliableNL(audio,
        LG_NET_AUDIO_MESSAGE_CLOCK_STATE, 0, 0,
        payload, sizeof(payload)) == LG_TRANSPORT_OK;
  LG_UNLOCK(audio->stateLock);
  return result;
}

static bool networkAudioClockFeedbackAvailable(void * opaque)
{
  NetworkAudio * audio = opaque;
  return atomic_load_explicit(
      &audio->feedbackAvailable, memory_order_acquire);
}

static const LG_AudioOps NETWORK_AUDIO_OPS =
{
  .name                     = "Network",
  .playbackFlags            = 0,
  .playbackArrivalReserveUs = 500U,
  .setStatusListener        = networkAudioSetStatusListener,
  .attach                   = networkAudioAttach,
  .detach                   = networkAudioDetach,
  .recordData               = networkAudioRecordData,
  .clockFeedback            = networkAudioClockFeedback,
  .clockFeedbackAvailable   = networkAudioClockFeedbackAvailable,
};

const LG_AudioOps * networkAudioGetOps(void)
{
  return &NETWORK_AUDIO_OPS;
}

static struct NetworkAudioStream * networkAudioStreamNL(
    NetworkAudio * audio, uint32_t streamID,
    LGNetAudioDirection direction)
{
  if (streamID == NETWORK_AUDIO_PLAYBACK_STREAM_ID &&
      direction == LG_NET_AUDIO_DIRECTION_PLAYBACK)
    return &audio->playback;
  if (streamID == NETWORK_AUDIO_CAPTURE_STREAM_ID &&
      direction == LG_NET_AUDIO_DIRECTION_CAPTURE)
    return &audio->capture;
  return NULL;
}

static bool networkAudioHandleGrant(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  if (!networkAudioReliableFlags(packet,
        LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL))
    return false;

  LGNetAudioSubscriptionGrant grant;
  if (lgNetAudioSubscriptionGrantDecode(&grant,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      packet->envelope.componentEpoch != grant.subscriptionEpoch)
    return false;

  const bool datagramAvailable = networkConnectionServiceAvailable(
      audio->connection, LG_NET_SERVICE_AUDIO,
      LG_NET_CAPABILITY_DATAGRAM);
  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  const bool     awaitingGrant     =
    audio->phase == NETWORK_AUDIO_SUBSCRIBING;
  const uint64_t expectedRequestID = awaitingGrant ?
    audio->subscribeRequestID : audio->grantRequestID;
  if (!audio->connected || !audio->serviceAvailable ||
      grant.subscriberID != audio->subscriberID ||
      grant.requestedRoles != audio->requestedRoles)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }
  if (!expectedRequestID ||
      packet->envelope.requestID != expectedRequestID)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  const bool accepted = grant.status == LG_NET_AUDIO_GRANT_GRANTED ||
    grant.status == LG_NET_AUDIO_GRANT_PARTIAL;
  if (audio->subscriptionEpoch == grant.subscriptionEpoch &&
      audio->grantedRoles == grant.grantedRoles &&
      audio->phase != NETWORK_AUDIO_SUBSCRIBING)
  {
    if (!accepted || grant.targetLatencyUs != audio->targetLatencyUs ||
        grant.maxPacketFrames != audio->maxPacketFrames)
    {
      LG_UNLOCK(audio->stateLock);
      networkAudioAbortEventsNL(audio);
      return false;
    }
    audio->nextRenewalNs     = networkAudioDeadline(
        NETWORK_AUDIO_RENEWAL_MS);
    audio->renewalDeadlineNs = networkAudioDeadline(
        NETWORK_AUDIO_RENEWAL_TIMEOUT_MS);
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  if (grant.subscriptionEpoch == audio->lastSubscriptionEpoch)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  if (!accepted)
  {
    const LG_AudioEventOps * events             =
      audio->attached ? audio->events : NULL;
    void                   * opaque             = audio->eventOpaque;
    const uint32_t           playbackGeneration = audio->playback.active ?
      audio->playback.generation : 0;
    const uint32_t           captureGeneration  = audio->capture.active ?
      audio->capture.generation : 0;
    networkAudioClearSubscriptionNL(audio, false);
    networkAudioClearStreamNL(&audio->playback);
    networkAudioClearStreamNL(&audio->capture);
    audio->lastSubscriptionEpoch = grant.subscriptionEpoch;
    audio->phase                 = NETWORK_AUDIO_SUBSCRIBING;
    audio->nextSubscribeNs       = networkAudioDeadline(
        NETWORK_AUDIO_SUBSCRIBE_RETRY_MS);
    LG_UNLOCK(audio->stateLock);
    networkAudioCallEventsNL(audio);

    if (events && playbackGeneration && events->playbackStop)
      events->playbackStop(opaque, playbackGeneration);
    if (events && captureGeneration && events->recordStop)
      events->recordStop(opaque, captureGeneration);
    networkAudioEndEvents(audio);
    return true;
  }

  if (!grant.maxPacketFrames ||
      (grant.grantedRoles & ~audio->requestedRoles) ||
      grant.maxPacketFrames > NETWORK_AUDIO_MAX_PACKET_FRAMES ||
      (grant.grantedRoles & LG_NET_AUDIO_ROLE_DATAGRAM_PCM &&
        !datagramAvailable) ||
      (grant.grantedRoles & LG_NET_AUDIO_ROLE_CAPTURE &&
        !(grant.grantedRoles & LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE)) ||
      (grant.grantedRoles & LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK &&
        !(grant.grantedRoles & LG_NET_AUDIO_ROLE_PLAYBACK)))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  const LG_AudioEventOps * events             =
    audio->attached ? audio->events : NULL;
  void                   * opaque             = audio->eventOpaque;
  const uint32_t           playbackGeneration = audio->playback.active ?
    audio->playback.generation : 0;
  const uint32_t           captureGeneration  = audio->capture.active ?
    audio->capture.generation : 0;
  networkAudioClearStreamNL(&audio->playback);
  networkAudioClearStreamNL(&audio->capture);

  audio->subscribeRequestID    = 0;
  audio->grantRequestID        = packet->envelope.requestID;
  audio->subscriptionEpoch     = grant.subscriptionEpoch;
  audio->lastSubscriptionEpoch = grant.subscriptionEpoch;
  audio->grantedRoles          = grant.grantedRoles;
  audio->targetLatencyUs       = grant.targetLatencyUs;
  audio->maxPacketFrames       = grant.maxPacketFrames;
  audio->controlSequence       = 0;
  audio->capturePacketID       = 0;
  audio->captureFramePosition  = 0;
  audio->phase                 = NETWORK_AUDIO_GRANTED;
  audio->nextKeepaliveNs       = networkAudioDeadline(
      NETWORK_AUDIO_KEEPALIVE_MS);
  audio->nextRenewalNs         = networkAudioDeadline(
      NETWORK_AUDIO_RENEWAL_MS);
  audio->renewalDeadlineNs     = networkAudioDeadline(
      NETWORK_AUDIO_RENEWAL_TIMEOUT_MS);
  networkAudioSetAvailableNL(audio, true, true);
  networkAudioUpdateReadyNL(audio);
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && playbackGeneration && events->playbackStop)
    events->playbackStop(opaque, playbackGeneration);
  if (events && captureGeneration && events->recordStop)
    events->recordStop(opaque, captureGeneration);
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioHandleStart(NetworkAudio * audio,
    const LGNetPacketView * packet, struct NetworkAudioStream * stream)
{
  LGNetAudioFormat wire;
  if (lgNetAudioFormatDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.streamID != stream->streamID ||
      wire.direction != stream->direction)
    return false;

  LG_AudioFormat format;
  size_t         frameSize;
  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream) ||
      !networkAudioFormat(&wire, audio->maxPacketFrames,
        &format, &frameSize))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }
  (void)frameSize;

  if (stream->formatValid &&
      stream->wireFormat.formatEpoch == wire.formatEpoch)
  {
    const bool duplicate =
      memcmp(&stream->wireFormat, &wire, sizeof(wire)) == 0;
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return duplicate;
  }

  const uint32_t oldGeneration = stream->active ?
    stream->generation : 0;
  const uint32_t generation    = networkAudioMapGenerationNL(
      audio, stream, wire.formatEpoch);
  const bool     replace       =
    oldGeneration && generation != oldGeneration;
  stream->wireFormat     = wire;
  stream->format         = format;
  stream->generation     = generation;
  stream->formatValid    = true;
  stream->active         = true;
  stream->volumeValid    = false;
  stream->volumeChannels = 0;
  memset(stream->volume, 0, sizeof(stream->volume));
  stream->muteValid      = false;
  stream->mute           = false;
  stream->masterMute     = false;
  stream->mutedChannels  = 0;
  stream->packetKnown    = false;
  stream->lastPacketID   = 0;
  stream->discontinuity  = true;
  networkAudioClearClockNL(stream);
  networkAudioInvalidateBarrierNL(audio, stream);
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void                   * opaque = audio->eventOpaque;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && replace)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackStop)
      events->playbackStop(opaque, oldGeneration);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordStop)
      events->recordStop(opaque, oldGeneration);
  }
  if (events)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackStart)
      events->playbackStart(opaque, generation, &format, NULL);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordStart)
      events->recordStart(opaque, generation, &format);
  }
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioHandleStop(NetworkAudio * audio,
    const LGNetPacketView * packet, struct NetworkAudioStream * stream)
{
  LGNetAudioControl control;
  if (lgNetAudioControlDecode(&control,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      control.streamID != stream->streamID ||
      control.direction != stream->direction)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (control.sequence <= stream->stateSequence)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (stream->formatValid && control.formatEpoch &&
      control.formatEpoch != stream->wireFormat.formatEpoch)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  const uint32_t           generation = stream->active ?
    stream->generation : 0;
  const LG_AudioEventOps * events     =
    audio->attached ? audio->events : NULL;
  void                   * opaque     = audio->eventOpaque;
  stream->stateSequence = control.sequence;
  stream->active        = false;
  stream->formatValid   = false;
  stream->volumeValid   = false;
  stream->muteValid     = false;
  stream->discontinuity = true;
  networkAudioClearClockNL(stream);
  networkAudioInvalidateBarrierNL(audio, stream);
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && generation)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackStop)
      events->playbackStop(opaque, generation);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordStop)
      events->recordStop(opaque, generation);
  }
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioApplyVolumeNL(struct NetworkAudioStream * stream,
    const LGNetAudioVolume * volume)
{
  if (!stream->active || !stream->formatValid ||
      volume->channelCount > stream->wireFormat.channels)
    return false;

  if (volume->channelMask &&
      (volume->channelMask & ~stream->wireFormat.channelMask))
    return false;

  if (!stream->volumeValid)
    for (unsigned i = 0; i < stream->wireFormat.channels; ++i)
      stream->volume[i] = UINT16_MAX;

  if (!volume->channelMask)
  {
    for (unsigned i = 0; i < volume->channelCount; ++i)
      stream->volume[i] = networkAudioVolumeLevel(
          volume->volumeMillibels[i]);
  }
  else
  {
    unsigned valueIndex   = 0;
    unsigned channelIndex = 0;
    for (unsigned bit = 0; bit < 18U; ++bit)
    {
      const uint32_t mask = UINT32_C(1) << bit;
      if (!(stream->wireFormat.channelMask & mask))
        continue;
      if (volume->channelMask & mask)
        stream->volume[channelIndex] = networkAudioVolumeLevel(
            volume->volumeMillibels[valueIndex++]);
      ++channelIndex;
    }
  }

  stream->volumeChannels = (uint8_t)stream->wireFormat.channels;
  stream->volumeValid    = true;
  return true;
}

static bool networkAudioHandleVolume(NetworkAudio * audio,
    const LGNetPacketView * packet, struct NetworkAudioStream * stream)
{
  LGNetAudioVolume volume;
  if (lgNetAudioVolumeDecode(&volume,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      volume.streamID != stream->streamID ||
      volume.direction != stream->direction)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (volume.sequence <= stream->stateSequence)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (!networkAudioApplyVolumeNL(stream, &volume))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  stream->stateSequence = volume.sequence;
  networkAudioInvalidateBarrierNL(audio, stream);
  const uint32_t           generation = stream->generation;
  const uint8_t            channels   = stream->volumeChannels;
  uint16_t                 levels[LG_AUDIO_MAX_CHANNELS];
  memcpy(levels, stream->volume, sizeof(levels));
  const LG_AudioEventOps * events     =
    audio->attached ? audio->events : NULL;
  void                   * opaque     = audio->eventOpaque;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackVolume)
      events->playbackVolume(opaque, generation, channels, levels);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordVolume)
      events->recordVolume(opaque, generation, channels, levels);
  }
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioApplyMuteNL(struct NetworkAudioStream * stream,
    const LGNetAudioMute * mute)
{
  if (!stream->active || !stream->formatValid ||
      (mute->channelMask & ~stream->wireFormat.channelMask))
    return false;

  if (!mute->channelMask)
    stream->masterMute = mute->muted != 0;
  else if (mute->muted)
    stream->mutedChannels |= mute->channelMask;
  else
    stream->mutedChannels &= ~mute->channelMask;

  stream->mute      = stream->masterMute ||
    (stream->mutedChannels & stream->wireFormat.channelMask) ==
      stream->wireFormat.channelMask;
  stream->muteValid = true;
  return true;
}

static bool networkAudioHandleMute(NetworkAudio * audio,
    const LGNetPacketView * packet, struct NetworkAudioStream * stream)
{
  LGNetAudioMute mute;
  if (lgNetAudioMuteDecode(&mute,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      mute.streamID != stream->streamID ||
      mute.direction != stream->direction)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (mute.sequence <= stream->stateSequence)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (!networkAudioApplyMuteNL(stream, &mute))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  stream->stateSequence = mute.sequence;
  networkAudioInvalidateBarrierNL(audio, stream);
  const uint32_t           generation = stream->generation;
  const bool               muted      = stream->mute;
  const LG_AudioEventOps * events     =
    audio->attached ? audio->events : NULL;
  void                   * opaque     = audio->eventOpaque;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackMute)
      events->playbackMute(opaque, generation, muted);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordMute)
      events->recordMute(opaque, generation, muted);
  }
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioHandleStatus(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  LGNetAudioState state;
  if (lgNetAudioStateDecode(&state,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  struct NetworkAudioStream * stream = networkAudioStreamNL(
      audio, state.streamID, state.direction);
  if (!stream)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (state.stateSequence <= stream->stateSequence)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  const bool running = state.state == LG_NET_AUDIO_STATE_RUNNING ||
    state.state == LG_NET_AUDIO_STATE_PAUSED;
  if (running && (!stream->active || !stream->formatValid ||
      !(state.flags & LG_NET_AUDIO_STATE_FORMAT_ACTIVE) ||
      state.formatEpoch != stream->wireFormat.formatEpoch))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }
  if (!running && (state.flags & LG_NET_AUDIO_STATE_FORMAT_ACTIVE))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  const bool     wasActive     = stream->active;
  const uint32_t generation    = stream->generation;
  bool           volumeChanged = false;
  bool           muteChanged   = false;
  if (running)
  {
    const uint16_t level = networkAudioVolumeLevel(
        state.volumeMillibels);
    for (unsigned i = 0; i < stream->wireFormat.channels; ++i)
      stream->volume[i] = level;
    stream->volumeChannels = (uint8_t)stream->wireFormat.channels;
    stream->volumeValid    = true;
    volumeChanged          = true;

    stream->masterMute    =
      (state.flags & LG_NET_AUDIO_STATE_MUTED) != 0;
    stream->mutedChannels = 0;
    stream->mute          = stream->masterMute;
    stream->muteValid     = true;
    muteChanged           = true;
  }
  else
  {
    stream->active        = false;
    stream->formatValid   = false;
    stream->volumeValid   = false;
    stream->muteValid     = false;
    stream->discontinuity = true;
    networkAudioClearClockNL(stream);
  }
  stream->stateSequence = state.stateSequence;
  networkAudioInvalidateBarrierNL(audio, stream);

  const uint8_t channels = stream->volumeChannels;
  uint16_t      levels[LG_AUDIO_MAX_CHANNELS];
  memcpy(levels, stream->volume, sizeof(levels));
  const bool               mute   = stream->mute;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void                   * opaque = audio->eventOpaque;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && running)
  {
    if (volumeChanged &&
        stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackVolume)
      events->playbackVolume(opaque, generation, channels, levels);
    if (volumeChanged &&
        stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordVolume)
      events->recordVolume(opaque, generation, channels, levels);
    if (muteChanged &&
        stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackMute)
      events->playbackMute(opaque, generation, mute);
    if (muteChanged &&
        stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordMute)
      events->recordMute(opaque, generation, mute);
  }
  else if (events && wasActive)
  {
    if (stream->direction == LG_NET_AUDIO_DIRECTION_PLAYBACK &&
        events->playbackStop)
      events->playbackStop(opaque, generation);
    if (stream->direction == LG_NET_AUDIO_DIRECTION_CAPTURE &&
        events->recordStop)
      events->recordStop(opaque, generation);
  }
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioHandleBarrier(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  LGNetAudioBarrier barrier;
  if (lgNetAudioBarrierDecode(&barrier,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      packet->envelope.requestID != barrier.barrierID)
    return false;

  LG_LOCK(audio->stateLock);
  struct NetworkAudioStream * stream = networkAudioStreamNL(
      audio, barrier.streamID, barrier.direction);
  if (!stream)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }
  if (!networkAudioSubscriptionCurrentNL(audio, packet))
  {
    LG_UNLOCK(audio->stateLock);
    return true;
  }
  if (!networkAudioStreamRequiredNL(audio, stream) ||
      !stream->stateSequence ||
      barrier.stateSequence != stream->stateSequence)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }

  stream->barrier          = barrier;
  stream->barrierRequestID = packet->envelope.requestID;
  stream->barrierPending   = true;
  stream->barrierReady     = false;
  networkAudioUpdateReadyNL(audio);
  networkAudioSendBarrierAckNL(audio, stream);
  LG_UNLOCK(audio->stateLock);
  return true;
}

static struct NetworkAudioClockEntry * networkAudioFindClockNL(
    struct NetworkAudioStream * stream, uint64_t packetID)
{
  for (unsigned i = 0; i < NETWORK_AUDIO_CLOCK_CACHE_SIZE; ++i)
    if (stream->clocks[i].valid &&
        stream->clocks[i].state.packetID == packetID)
      return &stream->clocks[i];
  return NULL;
}

static bool networkAudioHandleClock(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  LGNetAudioClockState state;
  if (lgNetAudioClockStateDecode(&state,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return false;

  LG_LOCK(audio->stateLock);
  struct NetworkAudioStream * stream = networkAudioStreamNL(
      audio, state.streamID, state.direction);
  if (!stream || stream != &audio->playback)
  {
    LG_UNLOCK(audio->stateLock);
    return false;
  }
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream) ||
      state.subscriptionEpoch != audio->subscriptionEpoch)
  {
    LG_UNLOCK(audio->stateLock);
    return true;
  }
  if (!stream->active || !stream->formatValid ||
      state.formatEpoch != stream->wireFormat.formatEpoch ||
      (stream->packetKnown && state.packetID <= stream->lastPacketID))
  {
    LG_UNLOCK(audio->stateLock);
    return true;
  }

  struct NetworkAudioClockEntry * entry = networkAudioFindClockNL(
      stream, state.packetID);
  if (!entry)
  {
    entry = &stream->clocks[
      stream->clockInsert++ % NETWORK_AUDIO_CLOCK_CACHE_SIZE];
    if (entry->valid)
      stream->discontinuity = true;
  }
  entry->valid = true;
  entry->state = state;
  LG_UNLOCK(audio->stateLock);
  return true;
}

static bool networkAudioHandlePlaybackData(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  LGNetAudioData data;
  if (lgNetAudioDataDecode(&data,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      data.streamID != NETWORK_AUDIO_PLAYBACK_STREAM_ID ||
      data.direction != LG_NET_AUDIO_DIRECTION_PLAYBACK)
    return false;

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  struct NetworkAudioStream * stream = &audio->playback;
  if (!networkAudioSubscriptionCurrentNL(audio, packet) ||
      !networkAudioStreamRequiredNL(audio, stream))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (!stream->active || !stream->formatValid ||
      data.formatEpoch != stream->wireFormat.formatEpoch)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }
  if (!lgNetAudioDataMatchesFormat(&data, &stream->wireFormat) ||
      data.frameCount > audio->maxPacketFrames)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }
  if (stream->packetKnown && data.packetID <= stream->lastPacketID)
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  const bool sequenceGap = stream->packetKnown &&
    data.packetID != stream->lastPacketID + 1U;
  stream->packetKnown  = true;
  stream->lastPacketID = data.packetID;
  networkAudioUpdateReadyNL(audio);
  if (!stream->barrierReady || audio->phase != NETWORK_AUDIO_READY)
  {
    stream->discontinuity = true;
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return true;
  }

  struct NetworkAudioClockEntry * entry    = networkAudioFindClockNL(
      stream, data.packetID);
  LG_AudioClock                   clock;
  const LG_AudioClock           * clockPtr = NULL;
  if (entry && (entry->state.flags & LG_NET_AUDIO_CLOCK_VALID))
  {
    clock = (LG_AudioClock)
    {
      .position      = entry->state.framePosition,
      .time          = entry->state.timeNs,
      .rate          = networkAudioRate(entry->state.rateQ32),
      .stable        =
        (entry->state.flags & LG_NET_AUDIO_CLOCK_STABLE) != 0,
      .discontinuity = stream->discontinuity || sequenceGap ||
        (data.flags & LG_NET_AUDIO_DATA_DISCONTINUITY),
    };
    clockPtr              = &clock;
    stream->discontinuity = false;
  }
  else
    stream->discontinuity = true;
  if (entry)
    entry->valid = false;
  if (data.flags & LG_NET_AUDIO_DATA_END_OF_STREAM)
    stream->discontinuity = true;

  const size_t frameSize = (size_t)networkAudioSampleSize(
      stream->wireFormat.sampleFormat) * stream->wireFormat.channels;
  const size_t bytes     = frameSize * data.frameCount;
  const void * pcm       = data.flags & LG_NET_AUDIO_DATA_SILENT ?
    l_networkAudioSilence : data.data;
  if (!frameSize || bytes > sizeof(l_networkAudioSilence))
  {
    LG_UNLOCK(audio->stateLock);
    networkAudioAbortEventsNL(audio);
    return false;
  }

  const uint32_t           generation = stream->generation;
  const LG_AudioEventOps * events     =
    audio->attached ? audio->events : NULL;
  void                   * opaque     = audio->eventOpaque;
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);
  if (events && events->playbackData)
    events->playbackData(opaque, generation, pcm,
        data.frameCount, clockPtr);
  networkAudioEndEvents(audio);
  return true;
}

static bool networkAudioHandlePacket(NetworkAudio * audio,
    const LGNetPacketView * packet, bool datagram)
{
  if (!audio || !packet ||
      packet->envelope.service != LG_NET_SERVICE_AUDIO ||
      !networkAudioVersionSupported(packet->envelope.serviceVersion))
    return false;

  if (packet->envelope.messageType ==
      LG_NET_AUDIO_MESSAGE_SUBSCRIPTION_GRANT)
  {
    if (datagram)
      return false;
    return networkAudioHandleGrant(audio, packet);
  }

  LG_LOCK(audio->stateLock);
  const bool current = networkAudioSubscriptionCurrentNL(audio, packet);
  LG_UNLOCK(audio->stateLock);
  if (!current)
    return true;

  if (datagram)
  {
    if (!networkAudioDatagramFlags(packet) ||
        packet->envelope.requestID)
      return false;
  }
  else
  {
    LGNetEnvelopeFlags semantic = LG_NET_ENVELOPE_FINAL;
    if (packet->envelope.messageType ==
        LG_NET_AUDIO_MESSAGE_STATE_BARRIER)
      semantic |= LG_NET_ENVELOPE_ACK_REQUIRED;
    if (!networkAudioReliableFlags(packet, semantic))
      return false;
    if (packet->envelope.messageType !=
          LG_NET_AUDIO_MESSAGE_STATE_BARRIER &&
        packet->envelope.requestID)
      return false;
  }

  switch (packet->envelope.messageType)
  {
    case LG_NET_AUDIO_MESSAGE_STATUS:
      return !datagram && networkAudioHandleStatus(audio, packet);

    case LG_NET_AUDIO_MESSAGE_PLAYBACK_START:
      return !datagram && networkAudioHandleStart(
          audio, packet, &audio->playback);

    case LG_NET_AUDIO_MESSAGE_PLAYBACK_STOP:
      return !datagram && networkAudioHandleStop(
          audio, packet, &audio->playback);

    case LG_NET_AUDIO_MESSAGE_PLAYBACK_DATA:
      return networkAudioHandlePlaybackData(audio, packet);

    case LG_NET_AUDIO_MESSAGE_PLAYBACK_VOLUME:
      return !datagram && networkAudioHandleVolume(
          audio, packet, &audio->playback);

    case LG_NET_AUDIO_MESSAGE_PLAYBACK_MUTE:
      return !datagram && networkAudioHandleMute(
          audio, packet, &audio->playback);

    case LG_NET_AUDIO_MESSAGE_CAPTURE_START:
      return !datagram && networkAudioHandleStart(
          audio, packet, &audio->capture);

    case LG_NET_AUDIO_MESSAGE_CAPTURE_STOP:
      return !datagram && networkAudioHandleStop(
          audio, packet, &audio->capture);

    case LG_NET_AUDIO_MESSAGE_CAPTURE_VOLUME:
      return !datagram && networkAudioHandleVolume(
          audio, packet, &audio->capture);

    case LG_NET_AUDIO_MESSAGE_CAPTURE_MUTE:
      return !datagram && networkAudioHandleMute(
          audio, packet, &audio->capture);

    case LG_NET_AUDIO_MESSAGE_STATE_BARRIER:
      return !datagram && networkAudioHandleBarrier(audio, packet);

    case LG_NET_AUDIO_MESSAGE_CLOCK_STATE:
      return networkAudioHandleClock(audio, packet);

    default:
      return false;
  }
}

bool networkAudioPacket(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  return networkAudioHandlePacket(audio, packet, false);
}

bool networkAudioDatagram(NetworkAudio * audio,
    const LGNetPacketView * packet)
{
  return networkAudioHandlePacket(audio, packet, true);
}

bool networkAudioCreate(NetworkAudio ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkAudio * audio = calloc(1, sizeof(*audio));
  if (!audio)
    return false;

  LG_LOCK_INIT(audio->stateLock);
  LG_LOCK_INIT(audio->eventLock);
  atomic_init(&audio->feedbackAvailable, false);
  audio->connection            = connection;
  audio->phase                 = NETWORK_AUDIO_DISCONNECTED;
  audio->subscriberID          = nanotime() ^
    ((uint64_t)(uintptr_t)audio << 19);
  audio->requestedRoles        =
    LG_NET_AUDIO_ROLE_PLAYBACK          |
    LG_NET_AUDIO_ROLE_CAPTURE           |
    LG_NET_AUDIO_ROLE_CLOCK_FEEDBACK    |
    LG_NET_AUDIO_ROLE_DATAGRAM_PCM      |
    LG_NET_AUDIO_ROLE_RELIABLE_PCM      |
    LG_NET_AUDIO_ROLE_EXCLUSIVE_CAPTURE;
  audio->endpointGeneration    = 1;
  audio->generationSerial      = 1;
  audio->playback.streamID     = NETWORK_AUDIO_PLAYBACK_STREAM_ID;
  audio->playback.direction    = LG_NET_AUDIO_DIRECTION_PLAYBACK;
  audio->capture.streamID      = NETWORK_AUDIO_CAPTURE_STREAM_ID;
  audio->capture.direction     = LG_NET_AUDIO_DIRECTION_CAPTURE;
  if (!audio->subscriberID)
    audio->subscriberID = 1;

  audio->statusIdle = lgCreateEvent(false, 0);
  audio->eventIdle  = lgCreateEvent(false, 0);
  if (!audio->statusIdle || !audio->eventIdle ||
      !lgSignalEvent(audio->statusIdle) ||
      !lgSignalEvent(audio->eventIdle) ||
      !lgCreateTimer(NETWORK_AUDIO_TIMER_MS,
        networkAudioTimer, audio, &audio->timer))
  {
    if (audio->eventIdle)
      lgFreeEvent(audio->eventIdle);
    if (audio->statusIdle)
      lgFreeEvent(audio->statusIdle);
    LG_LOCK_FREE(audio->eventLock);
    LG_LOCK_FREE(audio->stateLock);
    free(audio);
    return false;
  }

  *result = audio;
  return true;
}

void networkAudioConnectionState(NetworkAudio * audio, bool connected)
{
  if (!audio)
    return;

  uint16_t   serviceVersion   = 0;
  const bool serviceAvailable = connected &&
    networkConnectionServiceAvailable(audio->connection,
      LG_NET_SERVICE_AUDIO, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(audio->connection,
      LG_NET_SERVICE_AUDIO, &serviceVersion) &&
    networkAudioVersionSupported(serviceVersion);

  networkAudioBeginEvents(audio);
  LG_LOCK(audio->stateLock);
  const LG_AudioEventOps * events             =
    audio->attached ? audio->events : NULL;
  void                   * opaque             = audio->eventOpaque;
  const uint32_t           playbackGeneration = audio->playback.active ?
    audio->playback.generation : 0;
  const uint32_t           captureGeneration  = audio->capture.active ?
    audio->capture.generation : 0;

  audio->connected        = connected;
  audio->serviceAvailable = serviceAvailable;
  audio->serviceVersion   = serviceAvailable ? serviceVersion : 0;
  networkAudioClearSubscriptionNL(audio, true);
  networkAudioClearStreamNL(&audio->playback);
  networkAudioClearStreamNL(&audio->capture);
  LG_UNLOCK(audio->stateLock);
  networkAudioCallEventsNL(audio);

  if (events && playbackGeneration && events->playbackStop)
    events->playbackStop(opaque, playbackGeneration);
  if (events && captureGeneration && events->recordStop)
    events->recordStop(opaque, captureGeneration);
  networkAudioEndEvents(audio);

  if (serviceAvailable)
    networkAudioProcess(audio);
}

void networkAudioDestroy(NetworkAudio ** result)
{
  if (!result || !*result)
    return;

  NetworkAudio * audio = *result;
  networkAudioSetStatusListener(audio, NULL, NULL);
  networkAudioDetach(audio);

  LG_LOCK(audio->stateLock);
  audio->destroying = true;
  LG_UNLOCK(audio->stateLock);
  lgTimerDestroy(audio->timer);

  networkAudioConnectionState(audio, false);
  lgFreeEvent(audio->eventIdle);
  lgFreeEvent(audio->statusIdle);
  LG_LOCK_FREE(audio->eventLock);
  LG_LOCK_FREE(audio->stateLock);
  free(audio);
  *result = NULL;
}
