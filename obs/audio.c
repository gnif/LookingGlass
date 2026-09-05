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

#define _GNU_SOURCE

#include "audio.h"

#include <LGProtocol/KVMFR.h>

#include <obs/util/platform.h>
#include <obs/util/threading.h>

#include <samplerate.h>

#include <lgmp/spmc.h>
#include <lgmp/stream.h>

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define LG_OBS_AUDIO_QUEUE_SLOTS 16U
#define LG_OBS_AUDIO_RESAMPLE_FRAMES 4096U
#define LG_OBS_AUDIO_MAX_BATCH_FRAMES 512U
#define LG_OBS_AUDIO_MIN_SAMPLE_RATE 8000U
#define LG_OBS_AUDIO_BATCH_NS UINT64_C(2000000)
#define LG_OBS_AUDIO_TARGET_NS UINT64_C(4000000)
#define LG_OBS_AUDIO_MAX_QUEUE_NS UINT64_C(100000000)
#define LG_OBS_AUDIO_LATE_NS UINT64_C(20000000)
#define LG_OBS_AUDIO_RATE_WINDOW_NS UINT64_C(100000000)
#define LG_OBS_AUDIO_RATE_FILTER_NS UINT64_C(10000000000)
#define LG_OBS_AUDIO_GAIN_RAMP_NS UINT64_C(5000000)
#define LG_OBS_AUDIO_DIAGNOSTIC_NS UINT64_C(5000000000)
#define LG_OBS_AUDIO_CLAIM_RETRY_NS UINT64_C(200000000)
#define LG_OBS_AUDIO_OWNER_RETRY_NS UINT64_C(1200000000)
#define LG_OBS_AUDIO_ACTIVE_POLL_US 25U
#define LG_OBS_AUDIO_IDLE_POLL_MS 10U
#define LG_OBS_AUDIO_DRAIN_MAX KVMFR_AUDIO_STREAM_SLOT_COUNT

#define LG_OBS_AUDIO_MAX_RATE_CORRECTION 0.005
#define LG_OBS_AUDIO_MAX_RATE_SLEW       0.005
#define LG_OBS_AUDIO_PHASE_HZ            0.05
#define LG_OBS_AUDIO_PHASE_DEADBAND_SEC  0.00025

enum LGOBSAudioQueueState
{
  LG_OBS_AUDIO_QUEUE_FREE,
  LG_OBS_AUDIO_QUEUE_FILLING,
  LG_OBS_AUDIO_QUEUE_READY,
  LG_OBS_AUDIO_QUEUE_READING,
};

enum LGOBSAudioPacketResult
{
  LG_OBS_AUDIO_PACKET_OK,
  LG_OBS_AUDIO_PACKET_CANCELLED,
  LG_OBS_AUDIO_PACKET_DROPPED,
};

enum LGOBSAudioTransportPhase
{
  LG_OBS_AUDIO_TRANSPORT_IDLE,
  LG_OBS_AUDIO_TRANSPORT_CLAIMING,
  LG_OBS_AUDIO_TRANSPORT_BOUND,
  LG_OBS_AUDIO_TRANSPORT_RELEASING,
};

enum LGOBSAudioTransportWait
{
  LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK,
  LG_OBS_AUDIO_TRANSPORT_WAIT_ACTIVE,
  LG_OBS_AUDIO_TRANSPORT_WAIT_IDLE,
};

typedef struct
{
  enum speaker_layout speakers;
  uint8_t             channels;
  int8_t              source[8];
}
LGOBSAudioChannelMap;

typedef struct
{
  enum LGOBSAudioQueueState state;
  uint64_t                  queueSerial;
  uint64_t                  generation;
  KVMFRAudioFormat          format;
  uint16_t                  volume[KVMFR_AUDIO_STREAM_MAX_CHANNELS];
  bool                      volumeValid;
  bool                      mute;
  KVMFRAudioMessageFlags    flags;
  uint64_t                  rateQ32;
  uint64_t                  arrivalTime;
  uint32_t                  frames;
  uint8_t                   raw[KVMFR_AUDIO_PLAYBACK_SLOT_BYTES];
}
LGOBSAudioQueueSlot;

typedef struct
{
  bool             enabled;
  bool             sourceActive;
  bool             playbackActive;
  uint64_t         generation;
  KVMFRAudioFormat format;
  uint16_t         volume[KVMFR_AUDIO_STREAM_MAX_CHANNELS];
  bool             volumeValid;
  bool             mute;
}
LGOBSAudioControl;

struct LGOBSAudio
{
  obs_source_t * source;

  pthread_mutex_t lock;
  pthread_cond_t  transportCond;
  os_event_t    * wake;
  pthread_t       thread;
  bool            threadStarted;
  atomic_bool     stopping;
  atomic_bool     discontinuity;
  atomic_ullong   droppedPackets;

  LGOBSAudioControl   control;
  uint64_t            queueSerial;
  LGOBSAudioQueueSlot queue[LG_OBS_AUDIO_QUEUE_SLOTS];

  SRC_STATE * resampler[KVMFR_AUDIO_STREAM_MAX_CHANNELS];
  float framesIn[
    KVMFR_AUDIO_MAX_FRAMES_PER_PACKET *
    KVMFR_AUDIO_STREAM_MAX_CHANNELS];
  float framesOut[
    LG_OBS_AUDIO_RESAMPLE_FRAMES *
    KVMFR_AUDIO_STREAM_MAX_CHANNELS];
  float output[8][LG_OBS_AUDIO_MAX_BATCH_FRAMES];

  uint64_t             workerGeneration;
  KVMFRAudioFormat     workerFormat;
  LGOBSAudioChannelMap channelMap;
  SRC_STATE          * activeResampler;
  uint32_t             outputRate;
  uint32_t             outputBatchFrames;
  uint32_t             outputStagedFrames;

  float gain[8];
  float gainTarget[8];
  float gainStep[8];
  uint32_t gainFrames;

  bool     timelineValid;
  uint64_t timelineOrigin;
  uint64_t timelineFrames;
  double   phaseIntegral;
  double   lastRatio;

  uint64_t rateWindowTime;
  uint64_t rateWindowFrames;
  double   sourceRate;
  bool     sourceRateValid;

  uint64_t nextDiagnostic;
  uint64_t discontinuities;
  uint64_t resamplerErrors;

  PLGMPClient       client;
  uint32_t          clientID;
  bool              transportConnected;
  bool              transportDetached;
  bool              disconnectRequested;
  enum LGOBSAudioTransportPhase transportPhase;
  uint32_t          requestSerial;
  uint64_t          tokenState;
  unsigned          claimSends;
  uint64_t          nextClaim;
  KVMFRAudioClaim   claim;
  bool              responsePending;
  KVMFRAudioStreamResponse response;

  PLGMPClientSPMC   playbackStream;
  PLGMPClientStream hostControlStream;
  PLGMPClientStream clientControlStream;
  uint32_t          playbackReaderEpoch;
  uint32_t          bindingEpoch;
  uint32_t          roleEpoch;
  uint64_t          endpointGeneration;
  bool              hostControlActive;
  bool              clientControlActive;
  bool              controlSequenceKnown;
  uint64_t          expectedControlSequence;
  uint64_t          clientControlSequence;
  bool              playbackSequenceKnown;
  uint64_t          expectedPlaybackSequence;
  bool              playbackDiscontinuity;
  bool              playbackReadActive;
  bool              subscriptionPending;
  bool              stateAckPending;
  KVMFRAudioStateBarrier stateAck;
  uint64_t          stateAckGeneration;
  uint64_t          nextKeepalive;
  KVMFRAudioStatus  remoteStatus;
  bool              remoteStatusValid;
  bool              transportPlaybackActive;
  bool              transportPlaybackSupported;
  uint64_t          transportPlaybackGeneration;
  KVMFRAudioFormat  transportPlaybackFormat;
};

_Static_assert(sizeof(KVMFRCursor) == KVMFR_AUDIO_CURSOR_COMPAT_BYTES,
  "KVMFR cursor compatibility layout changed");
_Static_assert(
  LG_OBS_AUDIO_MAX_BATCH_FRAMES * UINT64_C(1000000000) >=
    KVMFR_AUDIO_MAX_SAMPLE_RATE * LG_OBS_AUDIO_BATCH_NS,
  "OBS audio staging cannot hold a two millisecond batch");
_Static_assert(sizeof(struct LGMPSPMCDescriptor) ==
    sizeof(KVMFRSPMCDescriptor),
  "LGMP and KVMFR SPMC descriptors differ");
_Static_assert(sizeof(struct LGMPStreamDescriptor) ==
    sizeof(KVMFRStreamDescriptor),
  "LGMP and KVMFR stream descriptors differ");

static bool playbackStart(LGOBSAudio * audio, uint64_t generation,
    const KVMFRAudioFormat * format, KVMFRAudioMessageFlags flags,
    uint64_t position, int64_t timeNs, uint64_t rateQ32);
static void playbackStop(LGOBSAudio * audio, uint64_t generation);
static bool playbackVolume(LGOBSAudio * audio, uint64_t generation,
    const KVMFRAudioVolume * volume);
static void playbackMute(LGOBSAudio * audio, uint64_t generation,
    bool mute);

static uint64_t nextNonzero(uint64_t value)
{
  ++value;
  return value ? value : 1;
}

static void addDroppedPackets(LGOBSAudio * audio, uint64_t count)
{
  if (!count)
    return;

  const unsigned long long increment = (unsigned long long)count;
  unsigned long long current = atomic_load_explicit(
    &audio->droppedPackets, memory_order_relaxed);
  for (;;)
  {
    const unsigned long long next = increment > ULLONG_MAX - current ?
      ULLONG_MAX : current + increment;
    if (atomic_compare_exchange_weak_explicit(&audio->droppedPackets,
          &current, next, memory_order_relaxed, memory_order_relaxed))
      return;
  }
}

static unsigned channelCount(uint32_t mask)
{
  unsigned result = 0;
  while (mask)
  {
    result += mask & 1U;
    mask >>= 1;
  }
  return result;
}

static unsigned bytesPerSample(KVMFRAudioSampleFormat format)
{
  switch (format)
  {
    case KVMFR_AUDIO_SAMPLE_S16_LE:
      return 2;
    case KVMFR_AUDIO_SAMPLE_S24_LE:
      return 3;
    case KVMFR_AUDIO_SAMPLE_S32_LE:
    case KVMFR_AUDIO_SAMPLE_F32_LE:
      return 4;
    default:
      return 0;
  }
}

static bool validFormat(const KVMFRAudioFormat * format)
{
  if (!format || format->version != KVMFR_AUDIO_FORMAT_VERSION ||
      format->size != sizeof(*format) ||
      format->sampleRate < LG_OBS_AUDIO_MIN_SAMPLE_RATE ||
      format->sampleRate > KVMFR_AUDIO_MAX_SAMPLE_RATE ||
      !format->channelMask ||
      (format->channelMask & ~KVMFR_AUDIO_CHANNEL_POSITION_MASK) ||
      !format->channelCount ||
      format->channelCount > KVMFR_AUDIO_STREAM_MAX_CHANNELS ||
      channelCount(format->channelMask) != format->channelCount ||
      format->reserved[0] || format->reserved[1])
    return false;

  const unsigned bytes = bytesPerSample(format->sampleFormat);
  if (!bytes || format->containerBits != bytes * 8U ||
      format->validBits != format->containerBits ||
      format->blockAlign != format->channelCount * bytes)
    return false;

  return true;
}

static bool validClock(KVMFRAudioMessageFlags flags, uint64_t position,
    int64_t timeNs, uint64_t rateQ32)
{
  const KVMFRAudioMessageFlags allowed =
    KVMFR_AUDIO_MESSAGE_DISCONTINUITY |
    KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
    KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
  if ((flags & ~allowed) ||
      ((flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) &&
        !(flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID)))
    return false;

  return (flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) ||
    (!position && !timeNs && !rateQ32);
}

static int sourceChannel(const KVMFRAudioFormat * format, uint32_t bit)
{
  if (!(format->channelMask & bit))
    return -1;

  int index = 0;
  for (uint32_t current = 1; current != bit; current <<= 1)
    if (format->channelMask & current)
      ++index;
  return index;
}

static bool makeChannelMap(const KVMFRAudioFormat * format,
    LGOBSAudioChannelMap * map)
{
  const uint32_t fl  = KVMFR_AUDIO_CHANNEL_FRONT_LEFT;
  const uint32_t fr  = KVMFR_AUDIO_CHANNEL_FRONT_RIGHT;
  const uint32_t fc  = KVMFR_AUDIO_CHANNEL_FRONT_CENTER;
  const uint32_t lfe = KVMFR_AUDIO_CHANNEL_LFE;
  const uint32_t bl  = KVMFR_AUDIO_CHANNEL_BACK_LEFT;
  const uint32_t br  = KVMFR_AUDIO_CHANNEL_BACK_RIGHT;
  const uint32_t bc  = KVMFR_AUDIO_CHANNEL_BACK_CENTER;
  const uint32_t sl  = KVMFR_AUDIO_CHANNEL_SIDE_LEFT;
  const uint32_t sr  = KVMFR_AUDIO_CHANNEL_SIDE_RIGHT;
  const uint32_t stereo   = fl | fr;
  const uint32_t point21  = stereo | lfe;
  const uint32_t quad     = stereo | bl | br;
  const uint32_t surround = stereo | fc | bc;
  const uint32_t point41  = surround | lfe;
  const uint32_t point51Back = stereo | fc | lfe | bl | br;

  memset(map, 0, sizeof(*map));
  for (unsigned i = 0; i < 8; ++i)
    map->source[i] = -1;

  const uint32_t mask = format->channelMask;
  if (mask == KVMFR_AUDIO_CHANNEL_MONO)
  {
    map->speakers = SPEAKERS_MONO;
    map->channels = 1;
    map->source[0] = sourceChannel(format, fc);
    return true;
  }
  if (mask == stereo)
  {
    map->speakers = SPEAKERS_STEREO;
    map->channels = 2;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    return true;
  }
  if (mask == point21)
  {
    map->speakers = SPEAKERS_2POINT1;
    map->channels = 3;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[2] = sourceChannel(format, lfe);
    return true;
  }
  if (mask == surround)
  {
    map->speakers = SPEAKERS_4POINT0;
    map->channels = 4;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[2] = sourceChannel(format, fc);
    map->source[3] = sourceChannel(format, bc);
    return true;
  }
  if (mask == point41)
  {
    map->speakers = SPEAKERS_4POINT1;
    map->channels = 5;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[2] = sourceChannel(format, fc);
    map->source[3] = sourceChannel(format, lfe);
    map->source[4] = sourceChannel(format, bc);
    return true;
  }
  if (mask == quad)
  {
    /* OBS has no quad layout. Use a 5.1 container with silent centre/LFE
     * planes so the back channels retain their physical positions. */
    map->speakers = SPEAKERS_5POINT1;
    map->channels = 6;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[4] = sourceChannel(format, bl);
    map->source[5] = sourceChannel(format, br);
    return true;
  }
  if (mask == KVMFR_AUDIO_CHANNEL_5POINT1 || mask == point51Back)
  {
    map->speakers = SPEAKERS_5POINT1;
    map->channels = 6;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[2] = sourceChannel(format, fc);
    map->source[3] = sourceChannel(format, lfe);
    map->source[4] = sourceChannel(format,
      mask == point51Back ? bl : sl);
    map->source[5] = sourceChannel(format,
      mask == point51Back ? br : sr);
    return true;
  }
  if (mask == KVMFR_AUDIO_CHANNEL_7POINT1)
  {
    map->speakers = SPEAKERS_7POINT1;
    map->channels = 8;
    map->source[0] = sourceChannel(format, fl);
    map->source[1] = sourceChannel(format, fr);
    map->source[2] = sourceChannel(format, fc);
    map->source[3] = sourceChannel(format, lfe);
    map->source[4] = sourceChannel(format, bl);
    map->source[5] = sourceChannel(format, br);
    map->source[6] = sourceChannel(format, sl);
    map->source[7] = sourceChannel(format, sr);
    return true;
  }

  return false;
}

static double clampDouble(double value, double minimum, double maximum)
{
  if (value < minimum)
    return minimum;
  if (value > maximum)
    return maximum;
  return value;
}

static bool controlActiveLocked(const LGOBSAudio * audio)
{
  return audio->control.enabled && audio->control.sourceActive &&
    audio->control.playbackActive;
}

static void flushQueueLocked(LGOBSAudio * audio)
{
  for (unsigned i = 0; i < LG_OBS_AUDIO_QUEUE_SLOTS; ++i)
    if (audio->queue[i].state == LG_OBS_AUDIO_QUEUE_READY)
      audio->queue[i].state = LG_OBS_AUDIO_QUEUE_FREE;
}

static bool streamStillActive(LGOBSAudio * audio, uint64_t generation)
{
  pthread_mutex_lock(&audio->lock);
  const bool active = controlActiveLocked(audio) &&
    audio->control.generation == generation;
  pthread_mutex_unlock(&audio->lock);
  return active;
}

static void resetTiming(LGOBSAudio * audio, bool discontinuity)
{
  if (audio->activeResampler)
    src_reset(audio->activeResampler);

  audio->outputStagedFrames = 0;
  audio->timelineValid      = false;
  audio->timelineOrigin     = 0;
  audio->timelineFrames     = 0;
  audio->phaseIntegral      = 0.0;
  audio->lastRatio          = 0.0;
  audio->rateWindowTime     = 0;
  audio->rateWindowFrames   = 0;
  audio->sourceRate         = 0.0;
  audio->sourceRateValid    = false;

  if (discontinuity)
    ++audio->discontinuities;
}

static void resetStream(LGOBSAudio * audio, bool discontinuity)
{
  resetTiming(audio, discontinuity);
  audio->workerGeneration    = 0;
  memset(&audio->workerFormat, 0, sizeof(audio->workerFormat));
  memset(&audio->channelMap, 0, sizeof(audio->channelMap));
  audio->activeResampler = NULL;
  audio->outputRate      = 0;
  audio->outputBatchFrames = 0;
  audio->gainFrames = 0;
  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
  {
    audio->gain[i]       = 1.0f;
    audio->gainTarget[i] = 1.0f;
    audio->gainStep[i]   = 0.0f;
  }
}

static float targetGain(const LGOBSAudioQueueSlot * slot, unsigned channel)
{
  if (slot->mute)
    return 0.0f;
  if (!slot->volumeValid)
    return 1.0f;
  return (float)slot->volume[channel] / (float)UINT16_MAX;
}

static void updateGain(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot, bool initialize)
{
  const unsigned channels = slot->format.channelCount;
  if (initialize)
  {
    for (unsigned channel = 0; channel < channels; ++channel)
      audio->gain[channel] = targetGain(slot, channel);
    audio->gainFrames = 0;
  }
  else
  {
    bool changed = false;
    for (unsigned channel = 0; channel < channels; ++channel)
      changed |= audio->gainTarget[channel] != targetGain(slot, channel);

    if (changed)
      audio->gainFrames = (uint32_t)(
        ((uint64_t)slot->format.sampleRate * LG_OBS_AUDIO_GAIN_RAMP_NS +
          UINT64_C(999999999)) / UINT64_C(1000000000));
  }

  for (unsigned channel = 0; channel < channels; ++channel)
  {
    audio->gainTarget[channel] = targetGain(slot, channel);
    audio->gainStep[channel] = audio->gainFrames ?
      (audio->gainTarget[channel] - audio->gain[channel]) /
        audio->gainFrames : 0.0f;
  }
}

static bool decodePacket(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot)
{
  const unsigned channels = slot->format.channelCount;
  const size_t samples = (size_t)slot->frames * channels;
  if (slot->flags & KVMFR_AUDIO_MESSAGE_SILENT)
    memset(audio->framesIn, 0, samples * sizeof(*audio->framesIn));
  else
  {
    const uint8_t * source =
      slot->raw + sizeof(KVMFRAudioMessage);
    for (size_t i = 0; i < samples; ++i)
    {
      float sample;
      switch (slot->format.sampleFormat)
      {
        case KVMFR_AUDIO_SAMPLE_S16_LE:
        {
          const uint16_t bits =
            (uint16_t)source[0] | (uint16_t)source[1] << 8;
          const int32_t value = bits < UINT16_C(0x8000) ?
            bits : (int32_t)bits - 65536;
          sample = (float)value / 32768.0f;
          source += 2;
          break;
        }

        case KVMFR_AUDIO_SAMPLE_S24_LE:
        {
          uint32_t bits = (uint32_t)source[0] |
            (uint32_t)source[1] << 8 | (uint32_t)source[2] << 16;
          const int32_t value = bits < UINT32_C(0x00800000) ?
            (int32_t)bits :
            (int32_t)((int64_t)bits - INT64_C(16777216));
          sample = (float)value / 8388608.0f;
          source += 3;
          break;
        }

        case KVMFR_AUDIO_SAMPLE_S32_LE:
        {
          const uint32_t bits = (uint32_t)source[0] |
            (uint32_t)source[1] << 8 | (uint32_t)source[2] << 16 |
            (uint32_t)source[3] << 24;
          const int64_t value = bits <= INT32_MAX ?
            bits : (int64_t)bits - INT64_C(4294967296);
          sample = (float)((double)value / 2147483648.0);
          source += 4;
          break;
        }

        case KVMFR_AUDIO_SAMPLE_F32_LE:
        {
          const uint32_t bits = (uint32_t)source[0] |
            (uint32_t)source[1] << 8 | (uint32_t)source[2] << 16 |
            (uint32_t)source[3] << 24;
          /* Bit-test before conversion because this target uses fast-math. */
          if ((bits & UINT32_C(0x7F800000)) == UINT32_C(0x7F800000))
            return false;
          memcpy(&sample, &bits, sizeof(sample));
          sample = fmaxf(-1.0f, fminf(1.0f, sample));
          source += 4;
          break;
        }

        default:
          return false;
      }
      audio->framesIn[i] = sample;
    }
  }

  for (uint32_t frame = 0; frame < slot->frames; ++frame)
  {
    if (audio->gainFrames)
      for (unsigned channel = 0; channel < channels; ++channel)
        audio->gain[channel] += audio->gainStep[channel];

    for (unsigned channel = 0; channel < channels; ++channel)
      audio->framesIn[(size_t)frame * channels + channel] *=
        audio->gain[channel];

    if (audio->gainFrames && !--audio->gainFrames)
      for (unsigned channel = 0; channel < channels; ++channel)
        audio->gain[channel] = audio->gainTarget[channel];
  }

  return true;
}

static void observeSourceRate(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot)
{
  const double nominal = slot->format.sampleRate;
  if ((slot->flags & (KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
        KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)) ==
      (KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
        KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) && slot->rateQ32)
  {
    const double rate = ldexp((double)slot->rateQ32, -32);
    if (rate >= nominal * 0.98 && rate <= nominal * 1.02)
    {
      audio->sourceRate      = rate;
      audio->sourceRateValid = true;
      audio->rateWindowTime  = slot->arrivalTime;
      audio->rateWindowFrames = 0;
      return;
    }
  }

  if (!audio->rateWindowTime)
  {
    audio->rateWindowTime   = slot->arrivalTime;
    audio->rateWindowFrames = 0;
    return;
  }

  audio->rateWindowFrames += slot->frames;
  const uint64_t elapsed = slot->arrivalTime - audio->rateWindowTime;
  if (elapsed < LG_OBS_AUDIO_RATE_WINDOW_NS)
    return;

  const double measured = audio->rateWindowFrames * 1.0e9 / elapsed;
  if (measured >= nominal * 0.98 && measured <= nominal * 1.02)
  {
    if (!audio->sourceRateValid)
      audio->sourceRate = measured;
    else
    {
      const double alpha = 1.0 - exp(
        -(double)elapsed / LG_OBS_AUDIO_RATE_FILTER_NS);
      audio->sourceRate += alpha * (measured - audio->sourceRate);
    }
    audio->sourceRateValid = true;
  }

  audio->rateWindowTime   = slot->arrivalTime;
  audio->rateWindowFrames = 0;
}

static uint64_t frameTime(uint64_t frames, uint32_t sampleRate)
{
  return frames / sampleRate * UINT64_C(1000000000) +
    frames % sampleRate * UINT64_C(1000000000) / sampleRate;
}

static double resampleRatio(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot)
{
  observeSourceRate(audio, slot);

  const double nominalRatio =
    (double)audio->outputRate / slot->format.sampleRate;
  const double sourceRate = audio->sourceRateValid ?
    audio->sourceRate : slot->format.sampleRate;
  const double baseRatio = audio->outputRate / sourceRate;
  double phaseError = 0.0;
  const uint64_t now = os_gettime_ns();

  if (audio->timelineValid)
  {
    const uint64_t tail = audio->timelineOrigin + frameTime(
      audio->timelineFrames + audio->outputStagedFrames,
      audio->outputRate);
    const int64_t lead = (int64_t)tail - (int64_t)now;
    if (lead > (int64_t)LG_OBS_AUDIO_MAX_QUEUE_NS ||
        lead < -(int64_t)LG_OBS_AUDIO_LATE_NS)
    {
      resetTiming(audio, true);
      audio->lastRatio = nominalRatio;
      return baseRatio;
    }

    phaseError = ((double)(int64_t)LG_OBS_AUDIO_TARGET_NS - lead) * 1.0e-9;
    if (fabs(phaseError) <= LG_OBS_AUDIO_PHASE_DEADBAND_SEC)
      phaseError = 0.0;
    else
      phaseError -= copysign(LG_OBS_AUDIO_PHASE_DEADBAND_SEC, phaseError);
  }

  const double period = (double)slot->frames / slot->format.sampleRate;
  const double omega = 2.0 * M_PI * LG_OBS_AUDIO_PHASE_HZ;
  const double candidateIntegral = audio->phaseIntegral + phaseError * period;
  const double correction = 2.0 * omega * phaseError +
    omega * omega * candidateIntegral;
  const double minimum = nominalRatio *
    (1.0 - LG_OBS_AUDIO_MAX_RATE_CORRECTION);
  const double maximum = nominalRatio *
    (1.0 + LG_OBS_AUDIO_MAX_RATE_CORRECTION);
  const double desired = baseRatio * (1.0 + correction);
  const double bounded = clampDouble(desired, minimum, maximum);
  if (desired == bounded ||
      (desired > bounded && phaseError < 0.0) ||
      (desired < bounded && phaseError > 0.0))
    audio->phaseIntegral = candidateIntegral;

  if (!audio->lastRatio)
    audio->lastRatio = nominalRatio;
  const double maxStep = nominalRatio * LG_OBS_AUDIO_MAX_RATE_SLEW * period;
  audio->lastRatio = clampDouble(
    bounded, audio->lastRatio - maxStep, audio->lastRatio + maxStep);
  return audio->lastRatio;
}

static bool submitOutput(LGOBSAudio * audio)
{
  if (!audio->outputStagedFrames)
    return true;
  if (!streamStillActive(audio, audio->workerGeneration))
    return false;

  if (!audio->timelineValid)
  {
    audio->timelineOrigin = os_gettime_ns();
    audio->timelineFrames = 0;
    audio->timelineValid  = true;
  }

  struct obs_source_audio output =
  {
    .frames           = audio->outputStagedFrames,
    .speakers         = audio->channelMap.speakers,
    .format           = AUDIO_FORMAT_FLOAT_PLANAR,
    .samples_per_sec  = audio->outputRate,
    .timestamp        = audio->timelineOrigin +
      frameTime(audio->timelineFrames, audio->outputRate),
  };
  for (unsigned channel = 0; channel < audio->channelMap.channels; ++channel)
    output.data[channel] = (const uint8_t *)audio->output[channel];

  obs_source_output_audio(audio->source, &output);
  audio->timelineFrames += audio->outputStagedFrames;
  audio->outputStagedFrames = 0;
  return true;
}

static bool appendOutput(LGOBSAudio * audio,
    const float * interleaved, uint32_t frames)
{
  const unsigned sourceChannels = audio->workerFormat.channelCount;
  uint32_t offset = 0;
  while (offset < frames)
  {
    const uint32_t count = audio->outputBatchFrames -
      audio->outputStagedFrames < frames - offset ?
        audio->outputBatchFrames - audio->outputStagedFrames :
        frames - offset;
    for (unsigned channel = 0;
         channel < audio->channelMap.channels; ++channel)
    {
      float * destination = audio->output[channel] +
        audio->outputStagedFrames;
      const int sourceChannel = audio->channelMap.source[channel];
      if (sourceChannel < 0)
        memset(destination, 0, count * sizeof(*destination));
      else
        for (uint32_t frame = 0; frame < count; ++frame)
          destination[frame] = interleaved[
            (size_t)(offset + frame) * sourceChannels + sourceChannel];
    }

    offset += count;
    audio->outputStagedFrames += count;
    if (audio->outputStagedFrames == audio->outputBatchFrames &&
        !submitOutput(audio))
      return false;
  }
  return true;
}

static bool configureWorker(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot)
{
  struct obs_audio_info info;
  if (!obs_get_audio_info(&info) ||
      info.samples_per_sec < LG_OBS_AUDIO_MIN_SAMPLE_RATE ||
      info.samples_per_sec > KVMFR_AUDIO_MAX_SAMPLE_RATE)
    return false;

  const bool newStream = audio->workerGeneration != slot->generation ||
    memcmp(&audio->workerFormat, &slot->format,
      sizeof(audio->workerFormat)) != 0;
  const bool rateChanged = audio->outputRate != info.samples_per_sec;
  if (newStream)
  {
    resetStream(audio, audio->workerGeneration != 0);
    audio->workerGeneration = slot->generation;
    audio->workerFormat     = slot->format;
    if (!makeChannelMap(&slot->format, &audio->channelMap))
      return false;
    audio->activeResampler =
      audio->resampler[slot->format.channelCount - 1];
    src_reset(audio->activeResampler);
  }
  else if (rateChanged)
    resetTiming(audio, true);

  audio->outputRate = info.samples_per_sec;
  audio->outputBatchFrames = (uint32_t)(
    ((uint64_t)audio->outputRate * LG_OBS_AUDIO_BATCH_NS +
      UINT64_C(999999999)) / UINT64_C(1000000000));
  if (!audio->outputBatchFrames ||
      audio->outputBatchFrames > LG_OBS_AUDIO_MAX_BATCH_FRAMES)
    return false;

  if (newStream)
    updateGain(audio, slot, true);
  return true;
}

static enum LGOBSAudioPacketResult processPacket(LGOBSAudio * audio,
    const LGOBSAudioQueueSlot * slot)
{
  if (!streamStillActive(audio, slot->generation))
    return LG_OBS_AUDIO_PACKET_CANCELLED;
  if (!configureWorker(audio, slot))
    return LG_OBS_AUDIO_PACKET_DROPPED;

  const uint64_t now = os_gettime_ns();
  if (now - slot->arrivalTime > LG_OBS_AUDIO_MAX_QUEUE_NS)
    return LG_OBS_AUDIO_PACKET_DROPPED;
  if (atomic_exchange_explicit(
        &audio->discontinuity, false, memory_order_acq_rel) ||
      (slot->flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY))
    resetTiming(audio, true);

  updateGain(audio, slot, false);
  if (!decodePacket(audio, slot))
    return LG_OBS_AUDIO_PACKET_DROPPED;

  const double ratio = resampleRatio(audio, slot);
  int consumed = 0;
  while (consumed < (int)slot->frames)
  {
    SRC_DATA data =
    {
      .data_in = audio->framesIn +
        (size_t)consumed * slot->format.channelCount,
      .data_out          = audio->framesOut,
      .input_frames      = slot->frames - consumed,
      .output_frames     = LG_OBS_AUDIO_RESAMPLE_FRAMES,
      .input_frames_used = 0,
      .output_frames_gen = 0,
      .end_of_input      = 0,
      .src_ratio         = ratio,
    };
    const int error = src_process(audio->activeResampler, &data);
    if (error || (!data.input_frames_used && !data.output_frames_gen))
    {
      ++audio->resamplerErrors;
      return LG_OBS_AUDIO_PACKET_DROPPED;
    }
    if (!appendOutput(audio, audio->framesOut, data.output_frames_gen))
      return LG_OBS_AUDIO_PACKET_CANCELLED;
    consumed += data.input_frames_used;
  }

  return LG_OBS_AUDIO_PACKET_OK;
}

static LGOBSAudioQueueSlot * takePacket(LGOBSAudio * audio)
{
  pthread_mutex_lock(&audio->lock);
  LGOBSAudioQueueSlot * result = NULL;
  for (unsigned i = 0; i < LG_OBS_AUDIO_QUEUE_SLOTS; ++i)
  {
    LGOBSAudioQueueSlot * slot = &audio->queue[i];
    if (slot->state != LG_OBS_AUDIO_QUEUE_READY ||
        (result && slot->queueSerial >= result->queueSerial))
      continue;
    result = slot;
  }
  if (result)
    result->state = LG_OBS_AUDIO_QUEUE_READING;
  const bool active = controlActiveLocked(audio);
  if (!active && result)
  {
    result->state = LG_OBS_AUDIO_QUEUE_FREE;
    result = NULL;
  }
  pthread_mutex_unlock(&audio->lock);
  return result;
}

static void releasePacket(LGOBSAudio * audio, LGOBSAudioQueueSlot * slot)
{
  pthread_mutex_lock(&audio->lock);
  slot->state = LG_OBS_AUDIO_QUEUE_FREE;
  pthread_mutex_unlock(&audio->lock);
}

static LGOBSAudioQueueSlot * reserveTransportPacket(LGOBSAudio * audio)
{
  pthread_mutex_lock(&audio->lock);
  LGOBSAudioQueueSlot * result = NULL;
  for (unsigned i = 0; i < LG_OBS_AUDIO_QUEUE_SLOTS; ++i)
    if (audio->queue[i].state == LG_OBS_AUDIO_QUEUE_FREE)
    {
      result = &audio->queue[i];
      result->state = LG_OBS_AUDIO_QUEUE_FILLING;
      break;
    }
  pthread_mutex_unlock(&audio->lock);
  return result;
}

static void reportDiagnostics(LGOBSAudio * audio, uint64_t now)
{
  if (!audio->nextDiagnostic)
    audio->nextDiagnostic = now + LG_OBS_AUDIO_DIAGNOSTIC_NS;
  if (now < audio->nextDiagnostic)
    return;

  const uint64_t dropped = atomic_exchange_explicit(
    &audio->droppedPackets, 0, memory_order_acq_rel);
  if (dropped || audio->resamplerErrors)
    blog(LOG_WARNING, "Looking Glass audio: dropped %llu packet(s), "
        "%llu discontinuity event(s), %llu resampler error(s)",
        (unsigned long long)dropped,
        (unsigned long long)audio->discontinuities,
        (unsigned long long)audio->resamplerErrors);

  audio->discontinuities = 0;
  audio->resamplerErrors = 0;
  audio->nextDiagnostic = now + LG_OBS_AUDIO_DIAGNOSTIC_NS;
}

enum LGOBSAudioWriteResult
{
  LG_OBS_AUDIO_WRITE_OK,
  LG_OBS_AUDIO_WRITE_FULL,
  LG_OBS_AUDIO_WRITE_LOST,
};

static KVMFRAudioSubscriptionFlags desiredSubscriptionsLocked(
    const LGOBSAudio * audio)
{
  return audio->control.enabled && audio->control.sourceActive ?
    KVMFR_AUDIO_SUBSCRIBE_PLAYBACK : 0;
}

static void newClaimLocked(LGOBSAudio * audio, uint64_t delay)
{
  if (!++audio->requestSerial)
    ++audio->requestSerial;

  uint64_t token = audio->tokenState;
  token ^= token << 13;
  token ^= token >> 7;
  token ^= token << 17;
  if (!token)
    token = ((uint64_t)audio->requestSerial << 32) | audio->clientID;
  if (!token)
    token = 1;
  audio->tokenState = token;

  audio->claim = (KVMFRAudioClaim)
  {
    .messageType        = KVMFR_MESSAGE_AUDIO_CLAIM,
    .version            = KVMFR_AUDIO_CONTROL_VERSION,
    .size               = sizeof(KVMFRAudioClaim),
    .requestGeneration  = audio->requestSerial,
    .flags              = KVMFR_AUDIO_CLAIM_BIND,
    .claimToken         = token,
    .protocolMin        = KVMFR_AUDIO_PROTOCOL_MIN,
    .protocolMax        = KVMFR_AUDIO_PROTOCOL_MAX,
    .clientCapabilities = KVMFR_AUDIO_CAP_PLAYBACK,
    .subscriptions      = desiredSubscriptionsLocked(audio),
  };
  audio->claimSends      = 0;
  audio->nextClaim       = os_gettime_ns() + delay;
  audio->responsePending = false;
  audio->transportPhase  = LG_OBS_AUDIO_TRANSPORT_CLAIMING;
}

static struct LGMPStreamDescriptor streamDescriptor(
    const KVMFRStreamDescriptor * wire)
{
  return (struct LGMPStreamDescriptor)
  {
    .magic      = wire->magic,
    .version    = wire->version,
    .size       = wire->size,
    .offset     = wire->offset,
    .regionSize = wire->regionSize,
    .direction  = wire->direction,
    .policy     = wire->policy,
    .slotCount  = wire->slotCount,
    .slotSize   = wire->slotSize,
  };
}

static struct LGMPSPMCDescriptor spmcDescriptor(
    const KVMFRSPMCDescriptor * wire)
{
  return (struct LGMPSPMCDescriptor)
  {
    .magic      = wire->magic,
    .version    = wire->version,
    .size       = wire->size,
    .offset     = wire->offset,
    .regionSize = wire->regionSize,
    .slotCount  = wire->slotCount,
    .slotSize   = wire->slotSize,
    .maxReaders = wire->maxReaders,
    .reserved   = wire->reserved,
  };
}

static bool validStreamDescriptor(const KVMFRStreamDescriptor * descriptor,
    enum LGMPStreamDirection direction, uint32_t slotSize)
{
  return descriptor->magic == LGMP_STREAM_DESCRIPTOR_MAGIC &&
    descriptor->version == LGMP_STREAM_DESCRIPTOR_VERSION &&
    descriptor->size == sizeof(*descriptor) && descriptor->offset &&
    descriptor->regionSize &&
    descriptor->direction == (uint32_t)direction &&
    descriptor->policy == LGMP_STREAM_RELIABLE_FIFO &&
    descriptor->slotCount == KVMFR_AUDIO_STREAM_SLOT_COUNT &&
    descriptor->slotSize == slotSize;
}

static bool validPlaybackDescriptor(
    const KVMFRSPMCDescriptor * descriptor)
{
  return descriptor->magic == LGMP_SPMC_DESCRIPTOR_MAGIC &&
    descriptor->version == LGMP_SPMC_DESCRIPTOR_VERSION &&
    descriptor->size == sizeof(*descriptor) && descriptor->offset &&
    descriptor->regionSize &&
    descriptor->slotCount == KVMFR_AUDIO_STREAM_SLOT_COUNT &&
    descriptor->slotSize == KVMFR_AUDIO_PLAYBACK_SLOT_BYTES &&
    descriptor->maxReaders == KVMFR_AUDIO_MAX_PLAYBACK_READERS &&
    !descriptor->reserved;
}

static bool validResponseLocked(const LGOBSAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  if (response->magic != KVMFR_AUDIO_RESPONSE_MAGIC ||
      response->version != KVMFR_AUDIO_CONTROL_VERSION ||
      response->size != sizeof(*response) ||
      response->requestGeneration != audio->claim.requestGeneration ||
      response->boundClientID != audio->clientID ||
      response->claimToken != audio->claim.claimToken ||
      response->result < KVMFR_AUDIO_RESPONSE_BOUND ||
      response->result > KVMFR_AUDIO_RESPONSE_ERROR)
    return false;
  for (unsigned i = 0; i < sizeof(response->cursorCompatibility); ++i)
    if (response->cursorCompatibility[i])
      return false;

  if (response->result != KVMFR_AUDIO_RESPONSE_BOUND)
  {
    const KVMFRSPMCDescriptor emptySPMC = { 0 };
    const KVMFRStreamDescriptor emptyStream = { 0 };
    return !response->protocolVersion &&
      !response->grantedCapabilities && !response->bindingEpoch &&
      !response->roleEpoch && !response->playbackReaderID &&
      !memcmp(&response->playback, &emptySPMC, sizeof(emptySPMC)) &&
      !memcmp(&response->hostControl, &emptyStream, sizeof(emptyStream)) &&
      !memcmp(&response->clientControl, &emptyStream,
        sizeof(emptyStream)) &&
      !memcmp(&response->capture, &emptyStream, sizeof(emptyStream));
  }
  return response->protocolVersion == KVMFR_AUDIO_PROTOCOL_MAX &&
    response->endpointGeneration && response->bindingEpoch &&
    response->roleEpoch &&
    response->playbackReaderID < KVMFR_AUDIO_MAX_PLAYBACK_READERS &&
    response->grantedCapabilities == KVMFR_AUDIO_CAP_PLAYBACK &&
    validPlaybackDescriptor(&response->playback) &&
    validStreamDescriptor(&response->hostControl,
      LGMP_STREAM_HOST_TO_CLIENT, KVMFR_AUDIO_HOST_CONTROL_SLOT_BYTES) &&
    validStreamDescriptor(&response->clientControl,
      LGMP_STREAM_CLIENT_TO_HOST, KVMFR_AUDIO_CLIENT_CONTROL_SLOT_BYTES) &&
    validStreamDescriptor(&response->capture,
      LGMP_STREAM_CLIENT_TO_HOST, KVMFR_AUDIO_CAPTURE_SLOT_BYTES);
}

static void detachTransportStreams(LGOBSAudio * audio)
{
  if (audio->playbackStream)
    lgmpClientSPMCDetach(&audio->playbackStream);
  if (audio->hostControlStream)
    lgmpClientStreamDetach(&audio->hostControlStream);
  if (audio->clientControlStream)
    lgmpClientStreamDetach(&audio->clientControlStream);
  audio->playbackReaderEpoch = 0;
  audio->hostControlActive   = false;
  audio->clientControlActive = false;
}

static void resetTransportPlayback(LGOBSAudio * audio)
{
  const uint64_t generation = audio->transportPlaybackGeneration;
  const bool supported = audio->transportPlaybackSupported;
  audio->transportPlaybackActive     = false;
  audio->transportPlaybackSupported  = false;
  audio->transportPlaybackGeneration = 0;
  memset(&audio->transportPlaybackFormat, 0,
    sizeof(audio->transportPlaybackFormat));
  audio->playbackReadActive     = false;
  audio->playbackSequenceKnown  = false;
  audio->playbackDiscontinuity  = false;
  if (generation && supported)
    playbackStop(audio, generation);
  else
  {
    pthread_mutex_lock(&audio->lock);
    audio->control.playbackActive = false;
    audio->control.generation = 0;
    memset(&audio->control.format, 0, sizeof(audio->control.format));
    flushQueueLocked(audio);
    atomic_store_explicit(
      &audio->discontinuity, true, memory_order_release);
    pthread_mutex_unlock(&audio->lock);
    os_event_signal(audio->wake);
  }
}

static enum LGOBSAudioWriteResult writeTransportMessage(
    LGOBSAudio * audio, KVMFRAudioMessageType type,
    uint64_t generation, const void * payload, uint32_t length)
{
  if (!audio->clientControlStream || !audio->clientControlActive)
    return LG_OBS_AUDIO_WRITE_LOST;

  LGMPStreamBuffer buffer = { 0 };
  LGMP_STATUS status = lgmpClientStreamWriteAcquire(
    audio->clientControlStream, &buffer);
  if (status == LGMP_ERR_STREAM_FULL)
    return LG_OBS_AUDIO_WRITE_FULL;
  if (status != LGMP_OK)
    return LG_OBS_AUDIO_WRITE_LOST;

  const uint32_t bytes = sizeof(KVMFRAudioMessage) + length;
  if (!buffer.data || buffer.capacity < bytes)
  {
    (void)lgmpClientStreamWriteCancel(
      audio->clientControlStream, &buffer);
    return LG_OBS_AUDIO_WRITE_LOST;
  }

  const KVMFRAudioMessage message =
  {
    .version          = KVMFR_AUDIO_MESSAGE_VERSION,
    .size             = sizeof(KVMFRAudioMessage),
    .type             = type,
    .streamGeneration = generation,
    .sequence         = audio->clientControlSequence,
    .length           = length,
  };
  memcpy(buffer.data, &message, sizeof(message));
  if (length)
    memcpy((uint8_t *)buffer.data + sizeof(message), payload, length);
  status = lgmpClientStreamWriteCommit(
    audio->clientControlStream, &buffer, bytes);
  if (status != LGMP_OK)
    return LG_OBS_AUDIO_WRITE_LOST;
  ++audio->clientControlSequence;
  return LG_OBS_AUDIO_WRITE_OK;
}

static bool attachTransportStreams(LGOBSAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  const struct LGMPSPMCDescriptor playback =
    spmcDescriptor(&response->playback);
  LGMP_STATUS status = lgmpClientSPMCAttach(audio->client, &playback,
    response->playbackReaderID, &audio->playbackStream);
  if (status == LGMP_OK)
    status = lgmpClientSPMCActivate(
      audio->playbackStream, &audio->playbackReaderEpoch);
  uint32_t clientID = 0;
  uint32_t epoch = 0;
  if (status == LGMP_OK)
    status = lgmpClientSPMCGetBinding(
      audio->playbackStream, &clientID, &epoch);
  if (status != LGMP_OK || clientID != audio->clientID ||
      !epoch || epoch != audio->playbackReaderEpoch)
    goto fail;

  const struct LGMPStreamDescriptor host =
    streamDescriptor(&response->hostControl);
  status = lgmpClientStreamAttach(
    audio->client, &host, &audio->hostControlStream);
  if (status == LGMP_OK)
    status = lgmpClientStreamActivate(audio->hostControlStream, &epoch);
  if (status == LGMP_OK)
    status = lgmpClientStreamGetBinding(
      audio->hostControlStream, &clientID, &epoch);
  if (status != LGMP_OK || clientID != audio->clientID || !epoch)
    goto fail;
  audio->hostControlActive = true;

  const struct LGMPStreamDescriptor client =
    streamDescriptor(&response->clientControl);
  status = lgmpClientStreamAttach(
    audio->client, &client, &audio->clientControlStream);
  if (status == LGMP_OK)
    status = lgmpClientStreamActivate(audio->clientControlStream, &epoch);
  if (status == LGMP_OK)
    status = lgmpClientStreamGetBinding(
      audio->clientControlStream, &clientID, &epoch);
  if (status != LGMP_OK || clientID != audio->clientID || !epoch)
    goto fail;
  audio->clientControlActive = true;

  audio->bindingEpoch      = response->bindingEpoch;
  audio->roleEpoch         = response->roleEpoch;
  audio->endpointGeneration = response->endpointGeneration;
  audio->controlSequenceKnown = false;
  audio->expectedControlSequence = 0;
  audio->clientControlSequence = 0;
  audio->playbackSequenceKnown = false;
  audio->expectedPlaybackSequence = 0;
  audio->playbackDiscontinuity = false;
  audio->playbackReadActive = false;
  audio->subscriptionPending = true;
  audio->stateAckPending = false;
  audio->remoteStatusValid = false;
  audio->nextKeepalive = os_gettime_ns();
  memset(&audio->stateAck, 0, sizeof(audio->stateAck));
  resetTransportPlayback(audio);
  return true;

fail:
  blog(LOG_WARNING, "Looking Glass audio: failed to attach streams: %s",
    lgmpStatusString(status));
  detachTransportStreams(audio);
  return false;
}

static bool parseTransportRecord(const void * data, uint32_t size,
    uint32_t capacity, KVMFRAudioMessage * message,
    const uint8_t ** payload)
{
  if (!data || size < sizeof(*message) || size > capacity)
    return false;
  memcpy(message, data, sizeof(*message));
  if (message->version != KVMFR_AUDIO_MESSAGE_VERSION ||
      message->size != sizeof(*message) || message->reserved0 ||
      message->length > KVMFR_AUDIO_PCM_BYTES ||
      size != sizeof(*message) + message->length)
    return false;
  *payload = (const uint8_t *)data + sizeof(*message);
  return true;
}

static bool emptyTransportMessage(const KVMFRAudioMessage * message,
    KVMFRAudioMessageFlags allowedFlags)
{
  return !(message->flags & ~allowedFlags) && !message->position &&
    !message->timeNs && !message->rateQ32 && !message->frames &&
    !message->length;
}

static bool emptyFormat(const KVMFRAudioFormat * format)
{
  const KVMFRAudioFormat empty = { 0 };
  return !memcmp(format, &empty, sizeof(empty));
}

static bool validTransportStatus(LGOBSAudio * audio,
    const KVMFRAudioStatus * status)
{
  const KVMFRAudioCapabilities allCapabilities =
    KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE |
    KVMFR_AUDIO_CAP_CLOCK_FEEDBACK;
  const KVMFRAudioCapabilities allowedGrants =
    KVMFR_AUDIO_CAP_PLAYBACK;
  const KVMFRAudioStatusFlags allFlags =
    KVMFR_AUDIO_STATUS_AVAILABLE | KVMFR_AUDIO_STATUS_BOUND |
    KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE |
    KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE |
    KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE |
    KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE;
  if (status->version != KVMFR_AUDIO_STATUS_VERSION ||
      status->size != sizeof(*status) || (status->flags & ~allFlags) ||
      status->protocolVersion != KVMFR_AUDIO_PROTOCOL_MAX ||
      status->requestGeneration != audio->claim.requestGeneration ||
      (status->capabilities & ~allCapabilities) ||
      (status->grantedCapabilities & ~allowedGrants) ||
      (status->grantedCapabilities & ~status->capabilities) ||
      status->bindingEpoch != audio->bindingEpoch || !status->roleEpoch ||
      status->boundClientID != audio->clientID ||
      !(status->flags & KVMFR_AUDIO_STATUS_BOUND) ||
      (!(status->capabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
        (status->feedbackAuthorityClientID ||
          status->feedbackAuthorityEpoch)) ||
      (!(status->capabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
        status->captureOwnerClientID) ||
      (!!status->feedbackAuthorityClientID !=
        !!status->feedbackAuthorityEpoch) ||
      status->feedbackAuthorityClientID == audio->clientID ||
      status->captureOwnerClientID == audio->clientID ||
      status->reserved0 || status->reserved1 ||
      !status->endpointGeneration ||
      status->playbackState > KVMFR_AUDIO_STREAM_RUNNING ||
      status->captureState > KVMFR_AUDIO_STREAM_RUNNING ||
      (audio->roleEpoch &&
        (int32_t)(status->roleEpoch - audio->roleEpoch) < 0))
    return false;

  const bool playbackActive =
    status->flags & KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE;
  const bool available = status->flags & KVMFR_AUDIO_STATUS_AVAILABLE;
  const bool playbackAvailable =
    status->flags & KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE;
  const bool captureAvailable =
    status->flags & KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE;
  if (playbackAvailable !=
        ((status->capabilities & KVMFR_AUDIO_CAP_PLAYBACK) != 0) ||
      captureAvailable !=
        ((status->capabilities & KVMFR_AUDIO_CAP_CAPTURE) != 0) ||
      (playbackActive &&
        (!available || !playbackAvailable ||
          !(status->grantedCapabilities & KVMFR_AUDIO_CAP_PLAYBACK))) ||
      (status->flags & KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE))
    return false;
  if (playbackActive &&
      (!status->playbackGeneration ||
       status->playbackState == KVMFR_AUDIO_STREAM_STOPPED ||
       !validFormat(&status->playbackFormat)))
    return false;
  if ((!playbackActive && !emptyFormat(&status->playbackFormat) &&
        !validFormat(&status->playbackFormat)) ||
      (!emptyFormat(&status->captureFormat) &&
        !validFormat(&status->captureFormat)))
    return false;
  return true;
}

static bool processTransportStatus(LGOBSAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload)
{
  if (message->streamGeneration || message->flags || message->position ||
      message->timeNs || message->rateQ32 || message->frames ||
      message->length != sizeof(KVMFRAudioStatus))
    return false;
  KVMFRAudioStatus status;
  memcpy(&status, payload, sizeof(status));
  if (!validTransportStatus(audio, &status))
    return false;
  if (status.roleEpoch != audio->roleEpoch)
  {
    pthread_mutex_lock(&audio->lock);
    audio->roleEpoch = status.roleEpoch;
    audio->subscriptionPending = true;
    pthread_mutex_unlock(&audio->lock);
  }
  audio->playbackReadActive = false;
  audio->stateAckPending = false;
  audio->endpointGeneration = status.endpointGeneration;
  audio->remoteStatus = status;
  audio->remoteStatusValid = true;
  return true;
}

static bool processTransportBarrier(LGOBSAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload)
{
  if (message->flags || message->position || message->timeNs ||
      message->rateQ32 || message->frames ||
      message->length != sizeof(KVMFRAudioStateBarrier) ||
      !audio->remoteStatusValid)
    return false;

  KVMFRAudioStateBarrier barrier;
  memcpy(&barrier, payload, sizeof(barrier));
  const bool statusActive =
    audio->remoteStatus.flags & KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE;
  const bool stateInstalled = statusActive ?
    audio->transportPlaybackActive &&
      audio->transportPlaybackGeneration ==
        audio->remoteStatus.playbackGeneration &&
      !memcmp(&audio->transportPlaybackFormat,
        &audio->remoteStatus.playbackFormat,
        sizeof(audio->transportPlaybackFormat)) :
    !audio->transportPlaybackActive;
  if (!barrier.bindingEpoch || !barrier.roleEpoch || !barrier.stateSerial ||
      barrier.bindingEpoch != audio->bindingEpoch ||
      barrier.roleEpoch != audio->roleEpoch || !stateInstalled ||
      ((statusActive && message->streamGeneration !=
          audio->remoteStatus.playbackGeneration) ||
       (!statusActive && message->streamGeneration)))
    return false;

  const bool sameBarrier =
    barrier.bindingEpoch == audio->stateAck.bindingEpoch &&
    barrier.roleEpoch == audio->stateAck.roleEpoch &&
    barrier.stateSerial == audio->stateAck.stateSerial &&
    message->streamGeneration == audio->stateAckGeneration;
  if (sameBarrier)
  {
    audio->stateAckPending = true;
    return true;
  }
  if (audio->stateAck.stateSerial &&
      barrier.stateSerial <= audio->stateAck.stateSerial)
    return false;

  uint64_t skipped = 0;
  const LGMP_STATUS status =
    lgmpClientSPMCSync(audio->playbackStream, &skipped);
  if (status != LGMP_OK)
    return false;
  audio->stateAck = barrier;
  audio->stateAckGeneration = message->streamGeneration;
  audio->stateAckPending = true;
  audio->playbackReadActive = false;
  audio->playbackSequenceKnown = false;
  audio->expectedPlaybackSequence = 0;
  audio->playbackDiscontinuity = skipped != 0 ||
    audio->transportPlaybackActive;
  return true;
}

static bool processTransportControl(LGOBSAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload)
{
  switch (message->type)
  {
    case KVMFR_AUDIO_MESSAGE_STATUS:
      return processTransportStatus(audio, message, payload);

    case KVMFR_AUDIO_MESSAGE_STATE_BARRIER:
      return processTransportBarrier(audio, message, payload);

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_START:
    {
      if (!message->streamGeneration || message->frames ||
          message->length != sizeof(KVMFRAudioFormat) ||
          !audio->remoteStatusValid ||
          !(audio->remoteStatus.flags &
            KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE) ||
          audio->remoteStatus.playbackGeneration !=
            message->streamGeneration ||
          !validClock(message->flags, message->position,
            message->timeNs, message->rateQ32))
        return false;
      KVMFRAudioFormat format;
      memcpy(&format, payload, sizeof(format));
      if (!validFormat(&format) ||
          memcmp(&format, &audio->remoteStatus.playbackFormat,
            sizeof(format)))
        return false;
      if (audio->transportPlaybackActive &&
          audio->transportPlaybackSupported)
        playbackStop(
          audio, audio->transportPlaybackGeneration);
      audio->transportPlaybackActive = true;
      audio->transportPlaybackGeneration = message->streamGeneration;
      audio->transportPlaybackFormat = format;
      audio->transportPlaybackSupported = playbackStart(audio,
        message->streamGeneration, &format, message->flags,
        message->position, message->timeNs, message->rateQ32);
      audio->playbackDiscontinuity = true;
      return true;
    }

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP:
      if (!message->streamGeneration || !emptyTransportMessage(message, 0))
        return false;
      if (audio->transportPlaybackActive &&
          message->streamGeneration == audio->transportPlaybackGeneration)
        resetTransportPlayback(audio);
      return true;

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME:
    {
      if (!message->streamGeneration || message->flags ||
          message->position || message->timeNs || message->rateQ32 ||
          message->frames || message->length != sizeof(KVMFRAudioVolume))
        return false;
      KVMFRAudioVolume volume;
      memcpy(&volume, payload, sizeof(volume));
      if (volume.version != KVMFR_AUDIO_VOLUME_VERSION ||
          volume.size != sizeof(volume) || volume.reserved ||
          !volume.channelCount ||
          volume.channelCount > KVMFR_AUDIO_STREAM_MAX_CHANNELS)
        return false;
      for (unsigned i = volume.channelCount;
           i < KVMFR_AUDIO_MAX_CHANNELS; ++i)
        if (volume.values[i])
          return false;
      if (!audio->transportPlaybackActive ||
          message->streamGeneration !=
            audio->transportPlaybackGeneration)
        return true;
      if (volume.channelCount !=
            audio->transportPlaybackFormat.channelCount)
        return false;
      return !audio->transportPlaybackSupported ||
        playbackVolume(
          audio, message->streamGeneration, &volume);
    }

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_MUTE:
      if (!message->streamGeneration || !emptyTransportMessage(
            message, KVMFR_AUDIO_MESSAGE_MUTED))
        return false;
      if (audio->transportPlaybackActive &&
          audio->transportPlaybackSupported &&
          message->streamGeneration == audio->transportPlaybackGeneration)
        playbackMute(audio, message->streamGeneration,
          (message->flags & KVMFR_AUDIO_MESSAGE_MUTED) != 0);
      return true;

    default:
      return false;
  }
}

static bool drainTransportControl(LGOBSAudio * audio)
{
  for (unsigned drained = 0; drained < LG_OBS_AUDIO_DRAIN_MAX; ++drained)
  {
    LGMPStreamBuffer buffer = { 0 };
    LGMP_STATUS status = lgmpClientStreamReadPeek(
      audio->hostControlStream, &buffer);
    if (status == LGMP_ERR_STREAM_EMPTY)
      return true;
    if (status != LGMP_OK)
      return false;

    KVMFRAudioMessage message;
    const uint8_t * payload;
    bool valid = parseTransportRecord(buffer.data, buffer.size,
      buffer.capacity, &message, &payload);
    if (valid)
    {
      const bool sequenceGap = audio->controlSequenceKnown ?
        message.sequence != audio->expectedControlSequence :
        message.sequence != 0;
      audio->controlSequenceKnown = true;
      audio->expectedControlSequence = message.sequence + 1;
      valid = !sequenceGap &&
        processTransportControl(audio, &message, payload);
    }

    status = lgmpClientStreamReadRelease(
      audio->hostControlStream, &buffer);
    if (status != LGMP_OK || !valid)
      return false;
  }
  return true;
}

static bool processTransportPlayback(LGOBSAudio * audio,
    LGOBSAudioQueueSlot * slot, const KVMFRAudioMessage * message,
    bool discontinuity)
{
  const KVMFRAudioMessageFlags allowed =
    KVMFR_AUDIO_MESSAGE_DISCONTINUITY |
    KVMFR_AUDIO_MESSAGE_SILENT |
    KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
    KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
  if (message->type != KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA ||
      !message->streamGeneration || !message->frames ||
      message->frames > KVMFR_AUDIO_MAX_FRAMES_PER_PACKET ||
      (message->flags & ~allowed) ||
      !validClock(message->flags & ~KVMFR_AUDIO_MESSAGE_SILENT,
        message->position, message->timeNs, message->rateQ32) ||
      !audio->transportPlaybackActive ||
      message->streamGeneration != audio->transportPlaybackGeneration)
    return false;

  const bool silent = message->flags & KVMFR_AUDIO_MESSAGE_SILENT;
  const uint64_t bytes = (uint64_t)message->frames *
    audio->transportPlaybackFormat.blockAlign;
  if (bytes > KVMFR_AUDIO_PCM_BYTES ||
      (silent ? message->length != 0 : message->length != bytes))
    return false;
  if (!audio->transportPlaybackSupported)
  {
    releasePacket(audio, slot);
    return true;
  }

  pthread_mutex_lock(&audio->lock);
  if (!controlActiveLocked(audio) ||
      audio->control.generation != message->streamGeneration)
  {
    slot->state = LG_OBS_AUDIO_QUEUE_FREE;
    pthread_mutex_unlock(&audio->lock);
    audio->playbackDiscontinuity = true;
    return true;
  }

  slot->queueSerial = nextNonzero(audio->queueSerial);
  audio->queueSerial = slot->queueSerial;
  slot->generation  = message->streamGeneration;
  slot->format      = audio->control.format;
  memcpy(slot->volume, audio->control.volume, sizeof(slot->volume));
  slot->volumeValid = audio->control.volumeValid;
  slot->mute         = audio->control.mute;
  slot->flags        = message->flags;
  slot->rateQ32      = message->rateQ32;
  slot->arrivalTime  = os_gettime_ns();
  slot->frames       = message->frames;
  slot->state        = LG_OBS_AUDIO_QUEUE_READY;
  if (discontinuity || audio->playbackDiscontinuity)
    atomic_store_explicit(
      &audio->discontinuity, true, memory_order_release);
  pthread_mutex_unlock(&audio->lock);
  audio->playbackDiscontinuity = false;
  return true;
}

static bool drainTransportPlayback(LGOBSAudio * audio)
{
  if (!audio->playbackReadActive)
    return true;
  for (unsigned drained = 0; drained < LG_OBS_AUDIO_DRAIN_MAX; ++drained)
  {
    LGOBSAudioQueueSlot * slot = reserveTransportPacket(audio);
    if (!slot)
      return true;
    struct LGMPSPMCRecord record = { 0 };
    LGMP_STATUS status = lgmpClientSPMCRead(audio->playbackStream,
      slot->raw, sizeof(slot->raw), &record);
    if (status == LGMP_ERR_STREAM_EMPTY)
    {
      releasePacket(audio, slot);
      return true;
    }
    if (status == LGMP_ERR_STREAM_BUSY)
    {
      releasePacket(audio, slot);
      audio->playbackDiscontinuity = true;
      return true;
    }
    if (status != LGMP_OK)
    {
      releasePacket(audio, slot);
      return false;
    }
    addDroppedPackets(audio, record.skipped);

    KVMFRAudioMessage message;
    const uint8_t * payload;
    bool valid = !record.reserved && record.size &&
      parseTransportRecord(slot->raw, record.size,
        sizeof(slot->raw), &message, &payload) &&
      message.sequence == record.sequence;
    bool discontinuity = record.skipped != 0;
    if (valid)
    {
      if (audio->playbackSequenceKnown &&
          record.sequence != audio->expectedPlaybackSequence)
        discontinuity = true;
      audio->playbackSequenceKnown = true;
      audio->expectedPlaybackSequence = record.sequence + 1;
      valid = processTransportPlayback(
        audio, slot, &message, discontinuity);
    }
    if (!valid)
    {
      releasePacket(audio, slot);
      return false;
    }
  }
  return true;
}

static void detachAndRetry(LGOBSAudio * audio, uint64_t delay)
{
  resetTransportPlayback(audio);
  detachTransportStreams(audio);
  pthread_mutex_lock(&audio->lock);
  audio->bindingEpoch       = 0;
  audio->roleEpoch          = 0;
  audio->endpointGeneration = 0;
  audio->remoteStatusValid  = false;
  audio->stateAckPending    = false;
  if (audio->transportConnected && !audio->disconnectRequested)
    newClaimLocked(audio, delay);
  else
    audio->transportPhase = LG_OBS_AUDIO_TRANSPORT_IDLE;
  pthread_mutex_unlock(&audio->lock);
}

static void finishTransportDisconnect(LGOBSAudio * audio)
{
  resetTransportPlayback(audio);
  detachTransportStreams(audio);
  pthread_mutex_lock(&audio->lock);
  audio->transportConnected = false;
  audio->disconnectRequested = false;
  audio->transportDetached = true;
  audio->transportPhase = LG_OBS_AUDIO_TRANSPORT_IDLE;
  audio->client = NULL;
  audio->clientID = 0;
  audio->responsePending = false;
  pthread_cond_broadcast(&audio->transportCond);
  pthread_mutex_unlock(&audio->lock);
}

static bool processTransportResponse(LGOBSAudio * audio)
{
  KVMFRAudioStreamResponse response;
  pthread_mutex_lock(&audio->lock);
  if (!audio->responsePending)
  {
    pthread_mutex_unlock(&audio->lock);
    return true;
  }
  response = audio->response;
  audio->responsePending = false;
  const bool valid = validResponseLocked(audio, &response);
  pthread_mutex_unlock(&audio->lock);
  if (!valid)
  {
    blog(LOG_WARNING, "Looking Glass audio: malformed stream response");
    pthread_mutex_lock(&audio->lock);
    audio->claimSends = 0;
    audio->nextClaim = os_gettime_ns() + LG_OBS_AUDIO_CLAIM_RETRY_NS;
    pthread_mutex_unlock(&audio->lock);
    return true;
  }
  if (response.result != KVMFR_AUDIO_RESPONSE_BOUND)
  {
    pthread_mutex_lock(&audio->lock);
    newClaimLocked(audio, LG_OBS_AUDIO_OWNER_RETRY_NS);
    pthread_mutex_unlock(&audio->lock);
    return true;
  }
  if (!attachTransportStreams(audio, &response))
  {
    pthread_mutex_lock(&audio->lock);
    audio->claimSends = 0;
    audio->nextClaim = os_gettime_ns() + LG_OBS_AUDIO_CLAIM_RETRY_NS;
    pthread_mutex_unlock(&audio->lock);
    return true;
  }

  pthread_mutex_lock(&audio->lock);
  audio->transportPhase = LG_OBS_AUDIO_TRANSPORT_BOUND;
  pthread_mutex_unlock(&audio->lock);
  blog(LOG_INFO, "Looking Glass audio: playback stream connected");
  return true;
}

static enum LGOBSAudioTransportWait maintainTransport(LGOBSAudio * audio)
{
  pthread_mutex_lock(&audio->lock);
  const bool connected = audio->transportConnected;
  const bool disconnect = audio->disconnectRequested;
  enum LGOBSAudioTransportPhase phase = audio->transportPhase;
  PLGMPClient client = audio->client;
  pthread_mutex_unlock(&audio->lock);
  if (!connected)
    return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;
  if (!client || !lgmpClientSessionValid(client))
  {
    finishTransportDisconnect(audio);
    return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;
  }

  processTransportResponse(audio);
  pthread_mutex_lock(&audio->lock);
  phase = audio->transportPhase;
  pthread_mutex_unlock(&audio->lock);
  if (disconnect)
  {
    if (phase == LG_OBS_AUDIO_TRANSPORT_BOUND)
      (void)writeTransportMessage(
        audio, KVMFR_AUDIO_MESSAGE_RELEASE, 0, NULL, 0);
    finishTransportDisconnect(audio);
    return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;
  }
  if (phase != LG_OBS_AUDIO_TRANSPORT_BOUND)
    return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;

  if (!drainTransportControl(audio))
  {
    blog(LOG_WARNING, "Looking Glass audio: control stream lost");
    (void)writeTransportMessage(
      audio, KVMFR_AUDIO_MESSAGE_RELEASE, 0, NULL, 0);
    detachAndRetry(audio, LG_OBS_AUDIO_OWNER_RETRY_NS);
    return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;
  }

  pthread_mutex_lock(&audio->lock);
  const KVMFRAudioSubscriptionFlags desired =
    desiredSubscriptionsLocked(audio);
  const bool subscriptionPending = audio->subscriptionPending;
  pthread_mutex_unlock(&audio->lock);
  if (!desired)
    audio->playbackReadActive = false;
  if (subscriptionPending)
  {
    const KVMFRAudioSubscription subscription =
    {
      .bindingEpoch  = audio->bindingEpoch,
      .roleEpoch     = audio->roleEpoch,
      .subscriptions = desired,
    };
    const enum LGOBSAudioWriteResult result = writeTransportMessage(
      audio, KVMFR_AUDIO_MESSAGE_PLAYBACK_SUBSCRIBE,
      0, &subscription, sizeof(subscription));
    if (result == LG_OBS_AUDIO_WRITE_LOST)
      goto lost;
    if (result == LG_OBS_AUDIO_WRITE_OK)
    {
      pthread_mutex_lock(&audio->lock);
      if (desired == desiredSubscriptionsLocked(audio))
        audio->subscriptionPending = false;
      pthread_mutex_unlock(&audio->lock);
    }
  }

  if (audio->stateAckPending)
  {
    const enum LGOBSAudioWriteResult result = writeTransportMessage(
      audio, KVMFR_AUDIO_MESSAGE_STATE_ACK,
      audio->stateAckGeneration, &audio->stateAck,
      sizeof(audio->stateAck));
    if (result == LG_OBS_AUDIO_WRITE_LOST)
      goto lost;
    if (result == LG_OBS_AUDIO_WRITE_OK)
    {
      audio->stateAckPending = false;
      pthread_mutex_lock(&audio->lock);
      audio->playbackReadActive =
        desiredSubscriptionsLocked(audio) != 0;
      pthread_mutex_unlock(&audio->lock);
    }
  }

  const uint64_t now = os_gettime_ns();
  if (now >= audio->nextKeepalive)
  {
    const enum LGOBSAudioWriteResult result = writeTransportMessage(
      audio, KVMFR_AUDIO_MESSAGE_KEEPALIVE, 0, NULL, 0);
    if (result == LG_OBS_AUDIO_WRITE_LOST)
      goto lost;
    if (result == LG_OBS_AUDIO_WRITE_OK)
      audio->nextKeepalive = now +
        (uint64_t)KVMFR_AUDIO_KEEPALIVE_MS * UINT64_C(1000000);
  }

  if (!drainTransportPlayback(audio))
    goto lost;

  pthread_mutex_lock(&audio->lock);
  const bool pollActive = desiredSubscriptionsLocked(audio) != 0 ||
    audio->subscriptionPending;
  pthread_mutex_unlock(&audio->lock);
  return pollActive || audio->stateAckPending ||
      audio->playbackReadActive ?
    LG_OBS_AUDIO_TRANSPORT_WAIT_ACTIVE :
    LG_OBS_AUDIO_TRANSPORT_WAIT_IDLE;

lost:
  blog(LOG_WARNING, "Looking Glass audio: playback stream lost");
  (void)writeTransportMessage(
    audio, KVMFR_AUDIO_MESSAGE_RELEASE, 0, NULL, 0);
  detachAndRetry(audio, LG_OBS_AUDIO_OWNER_RETRY_NS);
  return LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK;
}

static void * audioThread(void * opaque)
{
  LGOBSAudio * audio = opaque;
  while (!atomic_load_explicit(&audio->stopping, memory_order_acquire))
  {
    const enum LGOBSAudioTransportWait transportWait =
      maintainTransport(audio);
    if (atomic_load_explicit(&audio->stopping, memory_order_acquire))
      break;

    for (;;)
    {
      LGOBSAudioQueueSlot * slot = takePacket(audio);
      if (!slot)
        break;
      if (processPacket(audio, slot) == LG_OBS_AUDIO_PACKET_DROPPED)
      {
        addDroppedPackets(audio, 1);
        atomic_store_explicit(
          &audio->discontinuity, true, memory_order_release);
      }
      releasePacket(audio, slot);
    }

    pthread_mutex_lock(&audio->lock);
    const bool active = controlActiveLocked(audio);
    pthread_mutex_unlock(&audio->lock);
    if (!active)
      resetStream(audio, false);
    reportDiagnostics(audio, os_gettime_ns());
    switch (transportWait)
    {
      case LG_OBS_AUDIO_TRANSPORT_WAIT_ACTIVE:
        usleep(LG_OBS_AUDIO_ACTIVE_POLL_US);
        break;

      case LG_OBS_AUDIO_TRANSPORT_WAIT_IDLE:
        (void)os_event_timedwait(audio->wake,
          LG_OBS_AUDIO_IDLE_POLL_MS);
        break;

      case LG_OBS_AUDIO_TRANSPORT_WAIT_BLOCK:
        (void)os_event_wait(audio->wake);
        break;
    }
  }

  resetStream(audio, false);
  return NULL;
}

bool lgObsAudioCreate(LGOBSAudio ** result, obs_source_t * source)
{
  if (!result || !source)
    return false;
  *result = NULL;

  LGOBSAudio * audio = bzalloc(sizeof(*audio));
  audio->source = source;
  atomic_init(&audio->stopping, false);
  atomic_init(&audio->discontinuity, false);
  atomic_init(&audio->droppedPackets, 0);
  if (pthread_mutex_init(&audio->lock, NULL))
  {
    bfree(audio);
    return false;
  }
  if (pthread_cond_init(&audio->transportCond, NULL))
  {
    pthread_mutex_destroy(&audio->lock);
    bfree(audio);
    return false;
  }
  if (os_event_init(&audio->wake, OS_EVENT_TYPE_AUTO))
  {
    pthread_cond_destroy(&audio->transportCond);
    pthread_mutex_destroy(&audio->lock);
    bfree(audio);
    return false;
  }

  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
  {
    int error;
    audio->resampler[i] = src_new(SRC_SINC_FASTEST, i + 1, &error);
    if (audio->resampler[i])
      continue;

    blog(LOG_ERROR, "Looking Glass audio: failed to create resampler: %s",
      src_strerror(error));
    for (unsigned j = 0; j < i; ++j)
      src_delete(audio->resampler[j]);
    os_event_destroy(audio->wake);
    pthread_cond_destroy(&audio->transportCond);
    pthread_mutex_destroy(&audio->lock);
    bfree(audio);
    return false;
  }

  audio->control.enabled      = true;
  audio->control.sourceActive = obs_source_active(source);
  audio->transportDetached    = true;
  if (getrandom(&audio->tokenState, sizeof(audio->tokenState),
        GRND_NONBLOCK) != (ssize_t)sizeof(audio->tokenState))
    audio->tokenState = os_gettime_ns() ^ (uintptr_t)audio;
  if (!audio->tokenState)
    audio->tokenState = 1;
  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
    audio->control.volume[i] = UINT16_MAX;
  resetStream(audio, false);

  if (pthread_create(&audio->thread, NULL, audioThread, audio))
  {
    for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
      src_delete(audio->resampler[i]);
    os_event_destroy(audio->wake);
    pthread_cond_destroy(&audio->transportCond);
    pthread_mutex_destroy(&audio->lock);
    bfree(audio);
    return false;
  }
  audio->threadStarted = true;
  pthread_setname_np(audio->thread, "LGAudioThread");
  *result = audio;
  return true;
}

bool lgObsAudioConnect(LGOBSAudio * audio, PLGMPClient client,
    uint32_t clientID)
{
  if (!audio || !client || !clientID)
    return false;
  pthread_mutex_lock(&audio->lock);
  if (audio->transportConnected || !audio->transportDetached)
  {
    pthread_mutex_unlock(&audio->lock);
    return false;
  }
  audio->client = client;
  audio->clientID = clientID;
  audio->transportConnected = true;
  audio->transportDetached = false;
  audio->disconnectRequested = false;
  newClaimLocked(audio, 0);
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
  return true;
}

void lgObsAudioPumpPointer(LGOBSAudio * audio,
    PLGMPClientQueue pointerQueue)
{
  if (!audio || !pointerQueue)
    return;

  KVMFRAudioClaim claim;
  bool send = false;
  const uint64_t now = os_gettime_ns();
  pthread_mutex_lock(&audio->lock);
  if (audio->transportConnected && !audio->disconnectRequested &&
      audio->transportPhase == LG_OBS_AUDIO_TRANSPORT_CLAIMING &&
      now >= audio->nextClaim)
  {
    if (audio->claimSends >= 3)
    {
      audio->claimSends = 0;
      audio->nextClaim = now + LG_OBS_AUDIO_OWNER_RETRY_NS;
    }
    else
    {
      claim = audio->claim;
      send = true;
    }
  }
  pthread_mutex_unlock(&audio->lock);
  if (!send)
    return;

  uint32_t serial;
  const LGMP_STATUS status = lgmpClientTrySendData(
    pointerQueue, &claim, sizeof(claim), &serial);
  pthread_mutex_lock(&audio->lock);
  if (audio->transportConnected &&
      audio->claim.requestGeneration == claim.requestGeneration &&
      audio->claim.claimToken == claim.claimToken)
  {
    if (status == LGMP_OK)
    {
      ++audio->claimSends;
      audio->nextClaim = now + LG_OBS_AUDIO_CLAIM_RETRY_NS;
    }
    else if (status == LGMP_ERR_QUEUE_BUSY ||
        status == LGMP_ERR_QUEUE_FULL)
      audio->nextClaim = now + UINT64_C(1000000);
    else
      audio->nextClaim = now + LG_OBS_AUDIO_CLAIM_RETRY_NS;
  }
  pthread_mutex_unlock(&audio->lock);
}

void lgObsAudioHandleResponse(LGOBSAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  if (!audio || !response)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->transportConnected && !audio->disconnectRequested &&
      audio->transportPhase == LG_OBS_AUDIO_TRANSPORT_CLAIMING &&
      response->requestGeneration == audio->claim.requestGeneration &&
      response->boundClientID == audio->clientID &&
      response->claimToken == audio->claim.claimToken &&
      !audio->responsePending)
  {
    audio->response = *response;
    audio->responsePending = true;
  }
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
}

void lgObsAudioDestroy(LGOBSAudio ** audioPtr)
{
  if (!audioPtr || !*audioPtr)
    return;

  LGOBSAudio * audio = *audioPtr;
  lgObsAudioDisconnect(audio);
  atomic_store_explicit(&audio->stopping, true, memory_order_release);
  os_event_signal(audio->wake);
  if (audio->threadStarted)
    pthread_join(audio->thread, NULL);
  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
    src_delete(audio->resampler[i]);
  os_event_destroy(audio->wake);
  pthread_cond_destroy(&audio->transportCond);
  pthread_mutex_destroy(&audio->lock);
  bfree(audio);
  *audioPtr = NULL;
}

void lgObsAudioSetEnabled(LGOBSAudio * audio, bool enabled)
{
  if (!audio)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->control.enabled != enabled)
  {
    audio->control.enabled = enabled;
    if (!enabled)
      flushQueueLocked(audio);
    atomic_store_explicit(
      &audio->discontinuity, true, memory_order_release);
    if (audio->transportPhase == LG_OBS_AUDIO_TRANSPORT_BOUND)
      audio->subscriptionPending = true;
  }
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
}

void lgObsAudioSetActive(LGOBSAudio * audio, bool active)
{
  if (!audio)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->control.sourceActive != active)
  {
    audio->control.sourceActive = active;
    if (!active)
      flushQueueLocked(audio);
    atomic_store_explicit(
      &audio->discontinuity, true, memory_order_release);
    if (audio->transportPhase == LG_OBS_AUDIO_TRANSPORT_BOUND)
      audio->subscriptionPending = true;
  }
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
}

void lgObsAudioDisconnect(LGOBSAudio * audio)
{
  if (!audio)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->transportConnected && !audio->transportDetached)
  {
    audio->disconnectRequested = true;
    pthread_mutex_unlock(&audio->lock);
    os_event_signal(audio->wake);
    pthread_mutex_lock(&audio->lock);
    while (!audio->transportDetached &&
        !atomic_load_explicit(&audio->stopping, memory_order_acquire))
      pthread_cond_wait(&audio->transportCond, &audio->lock);
  }
  audio->control.playbackActive = false;
  audio->control.generation     = 0;
  memset(&audio->control.format, 0, sizeof(audio->control.format));
  flushQueueLocked(audio);
  atomic_store_explicit(
    &audio->discontinuity, true, memory_order_release);
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
}

static bool playbackStart(LGOBSAudio * audio, uint64_t generation,
    const KVMFRAudioFormat * format, KVMFRAudioMessageFlags flags,
    uint64_t position, int64_t timeNs, uint64_t rateQ32)
{
  LGOBSAudioChannelMap map;
  if (!audio || !generation || !validFormat(format) ||
      !makeChannelMap(format, &map) ||
      !validClock(flags, position, timeNs, rateQ32))
    return false;

  pthread_mutex_lock(&audio->lock);
  flushQueueLocked(audio);
  audio->control.playbackActive = true;
  audio->control.generation     = generation;
  audio->control.format         = *format;
  audio->control.volumeValid    = false;
  audio->control.mute           = false;
  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_MAX_CHANNELS; ++i)
    audio->control.volume[i] = UINT16_MAX;
  atomic_store_explicit(
    &audio->discontinuity, true, memory_order_release);
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
  return true;
}

static void playbackStop(LGOBSAudio * audio, uint64_t generation)
{
  if (!audio)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->control.playbackActive &&
      audio->control.generation == generation)
  {
    audio->control.playbackActive = false;
    flushQueueLocked(audio);
    atomic_store_explicit(
      &audio->discontinuity, true, memory_order_release);
  }
  pthread_mutex_unlock(&audio->lock);
  os_event_signal(audio->wake);
}

static bool playbackVolume(LGOBSAudio * audio, uint64_t generation,
    const KVMFRAudioVolume * volume)
{
  if (!audio || !volume || volume->version != KVMFR_AUDIO_VOLUME_VERSION ||
      volume->size != sizeof(*volume) || volume->reserved)
    return false;
  for (unsigned i = volume->channelCount;
       i < KVMFR_AUDIO_MAX_CHANNELS; ++i)
    if (volume->values[i])
      return false;

  pthread_mutex_lock(&audio->lock);
  if (!audio->control.playbackActive ||
      audio->control.generation != generation ||
      volume->channelCount != audio->control.format.channelCount)
  {
    pthread_mutex_unlock(&audio->lock);
    return false;
  }
  memcpy(audio->control.volume, volume->values,
    sizeof(audio->control.volume));
  audio->control.volumeValid = true;
  pthread_mutex_unlock(&audio->lock);
  return true;
}

static void playbackMute(LGOBSAudio * audio, uint64_t generation,
    bool mute)
{
  if (!audio)
    return;
  pthread_mutex_lock(&audio->lock);
  if (audio->control.playbackActive &&
      audio->control.generation == generation)
  {
    audio->control.mute = mute;
  }
  pthread_mutex_unlock(&audio->lock);
}
