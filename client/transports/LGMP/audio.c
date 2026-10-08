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

#include <LGProtocol/KVMFR.h>
#include "common/debug.h"
#include "common/locking.h"
#include "common/random.h"
#include "common/thread.h"
#include "common/time.h"

#include <lgmp/stream.h>
#include <lgmp/spmc.h>

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define AUDIO_IDLE_POLL_MS        20U
#define AUDIO_ACTIVE_POLL_US      25U
#define AUDIO_CLAIM_RETRY_US  200000U
#define AUDIO_OWNER_RETRY_US 1200000U
#define AUDIO_DRAIN_MAX            32U
#define AUDIO_CAPTURE_WRITE_TIMEOUT_US 5000U
#define AUDIO_CAPTURE_WRITE_MAX_WAIT_US 250U

#define AUDIO_CAPABILITIES \
  (KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE | \
   KVMFR_AUDIO_CAP_CLOCK_FEEDBACK)

#define AUDIO_SUBSCRIPTIONS \
  (KVMFR_AUDIO_SUBSCRIBE_PLAYBACK | KVMFR_AUDIO_SUBSCRIBE_CAPTURE)

#define AUDIO_STATUS_FLAGS \
  (KVMFR_AUDIO_STATUS_AVAILABLE | KVMFR_AUDIO_STATUS_BOUND | \
   KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE | \
   KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE | \
   KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE | \
   KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE)

#define AUDIO_CLOCK_FLAGS \
  (KVMFR_AUDIO_MESSAGE_CLOCK_VALID | \
   KVMFR_AUDIO_MESSAGE_CLOCK_STABLE | \
   KVMFR_AUDIO_MESSAGE_DISCONTINUITY)

#define AUDIO_DATA_FLAGS \
  (AUDIO_CLOCK_FLAGS | KVMFR_AUDIO_MESSAGE_SILENT)

enum AudioPhase
{
  AUDIO_PHASE_IDLE,
  AUDIO_PHASE_CLAIM_PENDING,
  AUDIO_PHASE_CLAIM_SENT,
  AUDIO_PHASE_BOUND,
  AUDIO_PHASE_RELEASING,
  AUDIO_PHASE_COOLDOWN,
};

struct AudioGeneration
{
  uint64_t wire;
  uint32_t local;
};

struct AudioStreamState
{
  struct AudioGeneration generation;
  KVMFRAudioFormat       wireFormat;
  LG_AudioFormat         format;
  bool                   active;
  bool                   formatValid;
  bool                   volumeValid;
  bool                   muteValid;
  bool                   mute;
  bool                   clockValid;
  LG_AudioClock          clock;
  uint8_t                volumeChannels;
  uint16_t               volume[KVMFR_AUDIO_MAX_CHANNELS];
};

struct AudioStatusCallbackFrame
{
  const LGMPAudio                  * audio;
  struct AudioStatusCallbackFrame * previous;
};

struct AudioCallbackWaiter
{
  struct AudioCallbackWaiter * next;
  LGEvent                    * event;
  unsigned                     depth;
};

struct AudioCallbackWaitQueue
{
  LG_Lock                      lock;
  atomic_uint                  count;
  struct AudioCallbackWaiter * waiters;
};

struct LGMPAudio
{
  PLGMPClient       client;
  LGEvent         * pointerWake;
  LGEvent         * event;
  LGThread        * thread;
  LG_Lock           lock;
  LG_Lock           eventLock;
  atomic_flag       controlWriteGate;
  atomic_flag       captureWriteGate;
  atomic_bool       stop;
  atomic_bool       dataPlaneLost;
  atomic_bool       captureDiscontinuity;
  atomic_bool       clockFeedbackAvailable;
  atomic_uint       statusInFlight;
  struct AudioCallbackWaitQueue statusWait;

  bool              connected;
  bool              discover;
  bool              attached;
  bool              statusAvailable;
  bool              statusDirty;
  bool              responsePending;
  bool              claimPosting;
  bool              controlSequenceKnown;
  bool              playbackSequenceKnown;
  bool              playbackDiscontinuity;
  bool              playbackReadActive;
  bool              stateAcknowledged;
  bool              stateAckPending;
  bool              subscriptionPending;
  bool              releaseRequested;
  bool              releaseSent;
  bool              hostControlActive;
  bool              clientControlActive;
  bool              captureWriteActive;
  enum AudioPhase   phase;
  uint32_t          clientID;
  uint32_t          requestSerial;
  uint32_t          requestGeneration;
  unsigned          claimSends;
  uint64_t          claimToken;
  uint64_t          nextClaimAt;
  uint64_t          releaseDeadline;
  uint64_t          nextKeepalive;
  uint64_t          nextAttachAt;
  uint64_t          expectedControlSequence;
  uint64_t          expectedPlaybackSequence;
  uint64_t          stateAckGeneration;
  uint32_t          bindingEpoch;
  uint32_t          roleEpoch;
  uint32_t          playbackReaderEpoch;
  uint32_t          generationSerial;
  KVMFRAudioStateBarrier stateAck;
  KVMFRAudioSubscription subscription;
  struct AudioGeneration endpointGeneration;
  KVMFRAudioClaim   claim;
  KVMFRAudioStreamResponse response;
  KVMFRAudioStatus  remoteStatus;
  struct AudioStreamState playback;
  struct AudioStreamState capture;

  PLGMPClientSPMC   playbackStream;
  PLGMPClientStream hostControlStream;
  PLGMPClientStream clientControlStream;
  PLGMPClientStream captureStream;
  LGMPStreamPollState streamPoll;
  uint8_t playbackRecord[KVMFR_AUDIO_PLAYBACK_SLOT_BYTES];

  /* Protected by the corresponding write gate. Shared admission fields are
   * changed only while both gates are held. */
  uint64_t          clientControlSequence;
  uint64_t          captureSequence;
  bool              controlWriteBound;
  bool              captureWriteBound;
  bool              publicDataAllowed;
  KVMFRAudioCapabilities writeGrantedCapabilities;
  uint32_t          writeBindingEpoch;
  uint32_t          writeRoleEpoch;
  uint64_t          writeCaptureGeneration;
  uint32_t          writeCaptureLocalGeneration;
  KVMFRAudioFormat  writeCaptureFormat;
  uint64_t          writePlaybackGeneration;
  uint32_t          writePlaybackLocalGeneration;

  const LG_AudioEventOps * events;
  void                   * eventOpaque;
  LG_AudioStatusFn         statusCallback;
  void                   * statusOpaque;
};

static _Thread_local struct AudioStatusCallbackFrame * l_statusFrames;

_Static_assert(sizeof(KVMFRCursor) == KVMFR_AUDIO_CURSOR_COMPAT_BYTES,
    "KVMFR cursor compatibility layout changed");
_Static_assert(sizeof(struct LGMPStreamDescriptor) ==
    sizeof(KVMFRStreamDescriptor),
    "LGMP and KVMFR stream descriptor sizes differ");
_Static_assert(sizeof(struct LGMPSPMCDescriptor) ==
    sizeof(KVMFRSPMCDescriptor),
    "LGMP and KVMFR SPMC descriptor sizes differ");
_Static_assert(offsetof(struct LGMPStreamDescriptor, offset) ==
    offsetof(KVMFRStreamDescriptor, offset),
    "LGMP and KVMFR stream descriptor offsets differ");
_Static_assert(offsetof(struct LGMPStreamDescriptor, slotSize) ==
    offsetof(KVMFRStreamDescriptor, slotSize),
    "LGMP and KVMFR stream descriptor geometry differs");
_Static_assert(offsetof(struct LGMPSPMCDescriptor, offset) ==
    offsetof(KVMFRSPMCDescriptor, offset),
    "LGMP and KVMFR SPMC descriptor offsets differ");
_Static_assert(offsetof(struct LGMPSPMCDescriptor, maxReaders) ==
    offsetof(KVMFRSPMCDescriptor, maxReaders),
    "LGMP and KVMFR SPMC descriptor geometry differs");
_Static_assert(KVMFR_AUDIO_MAX_CHANNELS <= LG_AUDIO_MAX_CHANNELS,
    "client audio cannot represent every KVMFR speaker position");

static uint32_t nextNonzero(uint32_t * value)
{
  if (++*value == 0)
    ++*value;
  return *value;
}

static bool tryControlWriteGate(LGMPAudio * audio)
{
  return !atomic_flag_test_and_set_explicit(
      &audio->controlWriteGate, memory_order_acquire);
}

static void lockControlWriteGate(LGMPAudio * audio)
{
  while (!tryControlWriteGate(audio))
  {
  }
}

static void unlockControlWriteGate(LGMPAudio * audio)
{
  atomic_flag_clear_explicit(
      &audio->controlWriteGate, memory_order_release);
}

static void lockCaptureWriteGate(LGMPAudio * audio)
{
  while (atomic_flag_test_and_set_explicit(
        &audio->captureWriteGate, memory_order_acquire))
  {
  }
}

static void unlockCaptureWriteGate(LGMPAudio * audio)
{
  atomic_flag_clear_explicit(
      &audio->captureWriteGate, memory_order_release);
}

/* Lifecycle updates acquire the capture gate before the control gate. This
 * keeps capture backpressure from suppressing clock feedback. */
static void lockWriteGates(LGMPAudio * audio)
{
  lockCaptureWriteGate(audio);
  lockControlWriteGate(audio);
}

static void unlockWriteGates(LGMPAudio * audio)
{
  unlockControlWriteGate(audio);
  unlockCaptureWriteGate(audio);
}

/* Both write gates must be held. */
static void closePublicDataLocked(LGMPAudio * audio)
{
  audio->publicDataAllowed = false;
  audio->writeGrantedCapabilities = 0;
  audio->writeBindingEpoch = 0;
  audio->writeRoleEpoch = 0;
  audio->writeCaptureGeneration      = 0;
  audio->writeCaptureLocalGeneration = 0;
  audio->writePlaybackGeneration      = 0;
  audio->writePlaybackLocalGeneration = 0;
  memset(&audio->writeCaptureFormat, 0,
      sizeof(audio->writeCaptureFormat));
}

static void signalWorker(LGMPAudio * audio)
{
  lgSignalEvent(audio->event);
}

static unsigned statusCallbackDepth(const LGMPAudio * audio)
{
  unsigned depth = 0;
  for (const struct AudioStatusCallbackFrame * frame = l_statusFrames;
      frame; frame = frame->previous)
    if (frame->audio == audio)
      ++depth;
  return depth;
}

static void signalCallbackWaiters(
    struct AudioCallbackWaitQueue * queue, unsigned remaining)
{
  if (!atomic_load_explicit(&queue->count, memory_order_acquire))
    return;

  LG_LOCK(queue->lock);
  struct AudioCallbackWaiter ** link = &queue->waiters;
  while (*link)
  {
    struct AudioCallbackWaiter * waiter = *link;
    if (remaining > waiter->depth)
    {
      link = &waiter->next;
      continue;
    }

    *link = waiter->next;
    atomic_fetch_sub_explicit(
        &queue->count, 1, memory_order_release);
    lgSignalEvent(waiter->event);
  }
  LG_UNLOCK(queue->lock);
}

/* audio->lock must protect callback admission. */
static void beginStatusCallback(LGMPAudio * audio,
    struct AudioStatusCallbackFrame * frame)
{
  atomic_fetch_add_explicit(
      &audio->statusInFlight, 1, memory_order_relaxed);
  frame->audio    = audio;
  frame->previous = l_statusFrames;
  l_statusFrames  = frame;
}

static void endStatusCallback(LGMPAudio * audio,
    const struct AudioStatusCallbackFrame * frame)
{
  l_statusFrames = frame->previous;
  const unsigned previous = atomic_fetch_sub_explicit(
      &audio->statusInFlight, 1, memory_order_release);
  DEBUG_ASSERT(previous > 0);
  signalCallbackWaiters(&audio->statusWait, previous - 1);
}

static void waitStatusCallbacks(LGMPAudio * audio)
{
  const unsigned depth = statusCallbackDepth(audio);
  if (atomic_load_explicit(
        &audio->statusInFlight, memory_order_acquire) <= depth)
    return;

  struct AudioCallbackWaiter waiter =
  {
    .event = lgCreateEvent(true, 0),
    .depth = depth,
  };
  if (!waiter.event)
    DEBUG_FATAL("Failed to create LGMP audio callback wait event");

  bool queued = false;
  LG_LOCK(audio->statusWait.lock);
  atomic_fetch_add_explicit(
      &audio->statusWait.count, 1, memory_order_release);
  if (atomic_load_explicit(
        &audio->statusInFlight, memory_order_acquire) > depth)
  {
    waiter.next                = audio->statusWait.waiters;
    audio->statusWait.waiters  = &waiter;
    queued = true;
  }
  else
    atomic_fetch_sub_explicit(
        &audio->statusWait.count, 1, memory_order_release);
  LG_UNLOCK(audio->statusWait.lock);

  if (queued)
  {
    if (!lgWaitEvent(waiter.event, TIMEOUT_INFINITE))
      DEBUG_FATAL("Failed to wait for LGMP audio status callbacks");
    LG_LOCK(audio->statusWait.lock);
    LG_UNLOCK(audio->statusWait.lock);
  }
  lgFreeEvent(waiter.event);
}

static unsigned channelCount(uint32_t mask)
{
  unsigned count = 0;
  while (mask)
  {
    count += mask & 1U;
    mask >>= 1;
  }
  return count;
}

static LG_AudioChannel channelFromBit(unsigned bit)
{
  static const LG_AudioChannel channels[KVMFR_AUDIO_MAX_CHANNELS] =
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
  return bit < KVMFR_AUDIO_MAX_CHANNELS ?
    channels[bit] : LG_AUDIO_CH_UNKNOWN;
}

static bool validFormat(const KVMFRAudioFormat * wire,
    LG_AudioFormat * result)
{
  if (!wire || wire->version != KVMFR_AUDIO_FORMAT_VERSION ||
      wire->size != sizeof(*wire) || !wire->sampleRate ||
      wire->sampleRate > KVMFR_AUDIO_MAX_SAMPLE_RATE ||
      !wire->channelMask ||
      (wire->channelMask & ~KVMFR_AUDIO_CHANNEL_POSITION_MASK) ||
      !wire->channelCount ||
      wire->channelCount > KVMFR_AUDIO_STREAM_MAX_CHANNELS ||
      channelCount(wire->channelMask) != wire->channelCount ||
      wire->reserved[0] || wire->reserved[1])
    return false;

  LG_AudioSampleFormat format;
  unsigned bytesPerSample;
  switch (wire->sampleFormat)
  {
    case KVMFR_AUDIO_SAMPLE_S16_LE:
      format = LG_AUDIO_FMT_S16_LE;
      bytesPerSample = 2;
      if (wire->validBits != 16 || wire->containerBits != 16)
        return false;
      break;

    case KVMFR_AUDIO_SAMPLE_S24_LE:
      format = LG_AUDIO_FMT_S24_LE;
      bytesPerSample = 3;
      if (wire->validBits != 24 || wire->containerBits != 24)
        return false;
      break;

    case KVMFR_AUDIO_SAMPLE_S32_LE:
      format = LG_AUDIO_FMT_S32_LE;
      bytesPerSample = 4;
      if (wire->validBits != 32 || wire->containerBits != 32)
        return false;
      break;

    case KVMFR_AUDIO_SAMPLE_F32_LE:
      format = LG_AUDIO_FMT_F32_LE;
      bytesPerSample = 4;
      if (wire->validBits != 32 || wire->containerBits != 32)
        return false;
      break;

    default:
      return false;
  }

  if (wire->blockAlign != wire->channelCount * bytesPerSample)
    return false;

  *result = (LG_AudioFormat)
  {
    .sampleFormat = format,
    .sampleRate   = wire->sampleRate,
    .channelCount = (uint8_t)wire->channelCount,
  };
  unsigned channel = 0;
  for (unsigned bit = 0; bit < KVMFR_AUDIO_MAX_CHANNELS; ++bit)
    if (wire->channelMask & (UINT32_C(1) << bit))
      result->channels[channel++] = channelFromBit(bit);
  return true;
}

static bool sameFormat(const KVMFRAudioFormat * left,
    const KVMFRAudioFormat * right)
{
  return !memcmp(left, right, sizeof(*left));
}

static bool emptyFormat(const KVMFRAudioFormat * format)
{
  const KVMFRAudioFormat empty = { 0 };
  return !memcmp(format, &empty, sizeof(empty));
}

static bool validClockFlags(uint32_t flags)
{
  return !(flags & ~AUDIO_CLOCK_FLAGS) &&
    (!(flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) ||
      (flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID));
}

static LG_AudioClock clockFromMessage(const KVMFRAudioMessage * message,
    bool discontinuity)
{
  return (LG_AudioClock)
  {
    .position      = message->position,
    .time          = message->timeNs,
    .rate          = (double)message->rateQ32 / 4294967296.0,
    .stable        =
      (message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_STABLE) != 0,
    .discontinuity = discontinuity ||
      (message->flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY) != 0,
  };
}

static uint32_t mapGeneration(LGMPAudio * audio,
    struct AudioGeneration * generation, uint64_t wire)
{
  if (!wire)
    return 0;
  if (generation->wire != wire || !generation->local)
  {
    generation->wire  = wire;
    generation->local = nextNonzero(&audio->generationSerial);
  }
  return generation->local;
}

static struct LGMPStreamDescriptor descriptorFromWire(
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

static struct LGMPSPMCDescriptor spmcDescriptorFromWire(
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

static bool validDescriptor(const KVMFRStreamDescriptor * descriptor,
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

static bool validSPMCDescriptor(const KVMFRSPMCDescriptor * descriptor)
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

static void markStatusNL(LGMPAudio * audio, bool available,
    uint64_t endpointGeneration)
{
  const uint32_t oldGeneration = audio->endpointGeneration.local;
  if (endpointGeneration)
    mapGeneration(audio, &audio->endpointGeneration, endpointGeneration);
  if (audio->statusAvailable != available ||
      oldGeneration != audio->endpointGeneration.local)
    audio->statusDirty = true;
  audio->statusAvailable = available;
  atomic_store_explicit(&audio->clockFeedbackAvailable,
      available && (audio->remoteStatus.grantedCapabilities &
        KVMFR_AUDIO_CAP_CLOCK_FEEDBACK), memory_order_release);
}

static void publishStatus(LGMPAudio * audio)
{
  LG_LOCK(audio->lock);
  if (!audio->statusDirty)
  {
    LG_UNLOCK(audio->lock);
    return;
  }
  audio->statusDirty = false;
  const LG_AudioStatusFn callback = audio->statusCallback;
  void * opaque = audio->statusOpaque;
  const LG_AudioStatus status =
  {
    .available  = audio->connected && audio->statusAvailable,
    .generation = audio->endpointGeneration.local,
  };
  struct AudioStatusCallbackFrame frame;
  if (callback)
    beginStatusCallback(audio, &frame);
  LG_UNLOCK(audio->lock);
  if (callback)
  {
    callback(opaque, &status);
    endStatusCallback(audio, &frame);
  }
}

static void updateWriteStreamState(LGMPAudio * audio)
{
  lockWriteGates(audio);
  LG_LOCK(audio->lock);
  const bool admitted = audio->controlWriteBound &&
    audio->phase == AUDIO_PHASE_BOUND && !audio->releaseRequested;
  const bool playbackAdmitted = admitted && audio->stateAcknowledged;
  const KVMFRAudioCapabilities granted =
    admitted && audio->statusAvailable ?
      audio->remoteStatus.grantedCapabilities : 0;
  const bool captureGranted =
    (granted & KVMFR_AUDIO_CAP_CAPTURE) != 0 &&
    (audio->subscription.subscriptions &
      KVMFR_AUDIO_SUBSCRIBE_CAPTURE) != 0;
  const uint64_t captureGeneration = admitted && captureGranted &&
    audio->captureWriteBound && audio->capture.active ?
    audio->capture.generation.wire : 0;
  const uint32_t captureLocalGeneration =
    admitted && captureGranted && audio->captureWriteBound &&
    audio->capture.active ?
    audio->capture.generation.local : 0;
  const bool captureChanged =
    audio->writeCaptureGeneration != captureGeneration ||
    audio->writeCaptureLocalGeneration != captureLocalGeneration;
  audio->writeCaptureGeneration      = captureGeneration;
  audio->writeCaptureLocalGeneration = captureLocalGeneration;
  audio->writeCaptureFormat = audio->capture.wireFormat;
  audio->writePlaybackGeneration = playbackAdmitted &&
    (granted & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
    (audio->subscription.subscriptions &
      KVMFR_AUDIO_SUBSCRIBE_PLAYBACK) &&
    audio->playback.active ?
    audio->playback.generation.wire : 0;
  audio->writePlaybackLocalGeneration =
    playbackAdmitted &&
    (granted & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
    (audio->subscription.subscriptions &
      KVMFR_AUDIO_SUBSCRIBE_PLAYBACK) &&
    audio->playback.active ?
    audio->playback.generation.local : 0;
  audio->writeGrantedCapabilities = granted;
  audio->writeBindingEpoch = admitted ? audio->bindingEpoch : 0;
  audio->writeRoleEpoch = admitted ? audio->roleEpoch : 0;
  audio->publicDataAllowed = admitted &&
    (audio->writeCaptureGeneration || audio->writePlaybackGeneration);
  LG_UNLOCK(audio->lock);
  if (captureChanged)
    atomic_store_explicit(
        &audio->captureDiscontinuity, false, memory_order_release);
  unlockWriteGates(audio);
}

static void stopActiveStreams(LGMPAudio * audio)
{
  const LG_AudioEventOps * events;
  void * opaque;
  uint32_t playbackGeneration;
  uint32_t captureGeneration;

  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  events             = audio->attached ? audio->events : NULL;
  opaque             = audio->eventOpaque;
  playbackGeneration = audio->playback.active ?
    audio->playback.generation.local : 0;
  captureGeneration  = audio->capture.active ?
    audio->capture.generation.local : 0;
  memset(&audio->playback, 0, sizeof(audio->playback));
  memset(&audio->capture, 0, sizeof(audio->capture));
  audio->playbackDiscontinuity = false;
  LG_UNLOCK(audio->lock);
  updateWriteStreamState(audio);

  if (events && playbackGeneration && events->playbackStop)
    events->playbackStop(opaque, playbackGeneration);
  if (events && captureGeneration && events->recordStop)
    events->recordStop(opaque, captureGeneration);
  LG_UNLOCK(audio->eventLock);
}

static void detachStreams(LGMPAudio * audio)
{
  lockWriteGates(audio);
  audio->publicDataAllowed = false;
  audio->controlWriteBound = false;
  audio->captureWriteBound = false;
  audio->writeGrantedCapabilities = 0;
  audio->writeBindingEpoch = 0;
  audio->writeRoleEpoch = 0;
  audio->writeCaptureGeneration      = 0;
  audio->writeCaptureLocalGeneration = 0;
  audio->writePlaybackGeneration      = 0;
  audio->writePlaybackLocalGeneration = 0;
  memset(&audio->writeCaptureFormat, 0,
      sizeof(audio->writeCaptureFormat));

  lgmpClientStreamDetach(&audio->captureStream);
  lgmpClientStreamDetach(&audio->clientControlStream);
  lgmpClientStreamDetach(&audio->hostControlStream);
  lgmpClientSPMCDetach(&audio->playbackStream);
  audio->hostControlActive   = false;
  audio->clientControlActive = false;
  audio->captureWriteActive  = false;
  audio->playbackReadActive  = false;
  audio->stateAcknowledged   = false;
  audio->playbackReaderEpoch = 0;
  audio->clientControlSequence = 0;
  audio->captureSequence = 0;
  atomic_store_explicit(
      &audio->captureDiscontinuity, false, memory_order_release);
  unlockWriteGates(audio);
}

static void scheduleRetryNL(LGMPAudio * audio, uint64_t delay)
{
  audio->phase             = AUDIO_PHASE_IDLE;
  audio->requestGeneration = 0;
  audio->claimToken        = 0;
  audio->claimSends        = 0;
  audio->claimPosting      = false;
  audio->releaseRequested  = false;
  audio->releaseSent       = false;
  memset(&audio->claim, 0, sizeof(audio->claim));
  audio->nextClaimAt = microtime() + delay;
}

static void finishBinding(LGMPAudio * audio, uint64_t delay)
{
  stopActiveStreams(audio);
  detachStreams(audio);
  atomic_store_explicit(
      &audio->dataPlaneLost, false, memory_order_release);

  LG_LOCK(audio->lock);
  memset(&audio->remoteStatus, 0, sizeof(audio->remoteStatus));
  audio->controlSequenceKnown = false;
  audio->playbackSequenceKnown = false;
  audio->expectedControlSequence = 0;
  audio->expectedPlaybackSequence = 0;
  audio->stateAckPending = false;
  audio->stateAcknowledged = false;
  audio->subscriptionPending = false;
  audio->bindingEpoch = 0;
  audio->roleEpoch = 0;
  audio->stateAckGeneration = 0;
  memset(&audio->stateAck, 0, sizeof(audio->stateAck));
  memset(&audio->subscription, 0, sizeof(audio->subscription));
  audio->releaseRequested = false;
  audio->releaseSent      = false;
  audio->responsePending  = false;
  markStatusNL(audio, false, 0);
  audio->endpointGeneration.wire = 0;
  if (audio->connected && audio->discover)
    scheduleRetryNL(audio, delay);
  else
  {
    audio->phase             = AUDIO_PHASE_IDLE;
    audio->requestGeneration = 0;
    audio->claimToken        = 0;
    audio->claimSends        = 0;
    audio->claimPosting      = false;
  }
  LG_UNLOCK(audio->lock);
  if (audio->pointerWake)
    lgSignalEvent(audio->pointerWake);
}

static bool attachStreams(LGMPAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  if (!validSPMCDescriptor(&response->playback) ||
      !validDescriptor(&response->hostControl,
        LGMP_STREAM_HOST_TO_CLIENT,
        KVMFR_AUDIO_HOST_CONTROL_SLOT_BYTES) ||
      !validDescriptor(&response->clientControl,
        LGMP_STREAM_CLIENT_TO_HOST,
        KVMFR_AUDIO_CLIENT_CONTROL_SLOT_BYTES) ||
      !validDescriptor(&response->capture,
        LGMP_STREAM_CLIENT_TO_HOST, KVMFR_AUDIO_CAPTURE_SLOT_BYTES))
  {
    DEBUG_WARN("Invalid LGMP audio stream descriptors");
    return false;
  }

  LGMP_STATUS playbackStatus = LGMP_OK;
  if (!audio->playbackStream)
  {
    const struct LGMPSPMCDescriptor playback =
      spmcDescriptorFromWire(&response->playback);
    playbackStatus = lgmpClientSPMCAttach(audio->client, &playback,
        response->playbackReaderID, &audio->playbackStream);
  }
  uint32_t playbackEpoch = 0;
  if (playbackStatus == LGMP_OK)
    playbackStatus = lgmpClientSPMCActivate(
        audio->playbackStream, &playbackEpoch);
  bool playbackOwner = playbackStatus == LGMP_OK;
  if (playbackOwner)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    playbackOwner = lgmpClientSPMCGetBinding(audio->playbackStream,
        &clientID, &epoch) == LGMP_OK && clientID == audio->clientID &&
      epoch && epoch == playbackEpoch;
  }
  if (playbackStatus != LGMP_OK)
    DEBUG_WARN("Failed to attach/activate playback audio stream: %s",
        lgmpStatusString(playbackStatus));
  audio->playbackReaderEpoch = playbackOwner ? playbackEpoch : 0;
  audio->playbackReadActive  = false;

  LGMP_STATUS hostStatus = LGMP_OK;
  if (!audio->hostControlStream)
  {
    const struct LGMPStreamDescriptor descriptor =
      descriptorFromWire(&response->hostControl);
    hostStatus = lgmpClientStreamAttach(audio->client,
        &descriptor, &audio->hostControlStream);
  }
  if (hostStatus == LGMP_OK)
  {
    uint32_t epoch = 0;
    hostStatus = lgmpClientStreamActivate(
        audio->hostControlStream, &epoch);
    audio->hostControlActive = hostStatus == LGMP_OK;
  }
  bool hostOwner = audio->hostControlActive;
  if (hostOwner)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    hostOwner = lgmpClientStreamGetBinding(
        audio->hostControlStream, &clientID, &epoch) == LGMP_OK &&
      clientID == audio->clientID && epoch;
  }
  if (hostStatus != LGMP_OK)
    DEBUG_WARN("Failed to attach/activate host audio control stream: %s",
        lgmpStatusString(hostStatus));
  audio->hostControlActive = hostOwner;

  lockWriteGates(audio);
  LGMP_STATUS clientStatus = LGMP_OK;
  if (!audio->clientControlStream)
  {
    const struct LGMPStreamDescriptor descriptor =
      descriptorFromWire(&response->clientControl);
    clientStatus = lgmpClientStreamAttach(audio->client,
        &descriptor, &audio->clientControlStream);
  }
  if (clientStatus == LGMP_OK)
  {
    uint32_t epoch = 0;
    clientStatus = lgmpClientStreamActivate(
        audio->clientControlStream, &epoch);
    audio->clientControlActive = clientStatus == LGMP_OK;
  }
  bool clientOwner = audio->clientControlActive;
  if (clientOwner)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    clientOwner = lgmpClientStreamGetBinding(
        audio->clientControlStream, &clientID, &epoch) == LGMP_OK &&
      clientID == audio->clientID && epoch;
  }
  audio->clientControlActive = clientOwner;
  const bool wasControlBound = audio->controlWriteBound;
  audio->controlWriteBound = clientOwner;
  if (audio->controlWriteBound && !wasControlBound)
    audio->clientControlSequence = 0;

  LGMP_STATUS captureStatus = LGMP_OK;
  if (!audio->captureStream)
  {
    const struct LGMPStreamDescriptor descriptor =
      descriptorFromWire(&response->capture);
    captureStatus = lgmpClientStreamAttach(audio->client,
        &descriptor, &audio->captureStream);
  }
  const bool captureGranted =
    (response->grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) != 0;
  if (captureStatus == LGMP_OK && captureGranted)
  {
    uint32_t epoch = 0;
    captureStatus = lgmpClientStreamActivate(
        audio->captureStream, &epoch);
    audio->captureWriteActive = captureStatus == LGMP_OK;
  }
  else
    audio->captureWriteActive = false;
  bool captureOwner = captureStatus == LGMP_OK &&
    (!captureGranted || audio->captureWriteActive);
  if (captureGranted && captureOwner)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    captureOwner = lgmpClientStreamGetBinding(audio->captureStream,
        &clientID, &epoch) == LGMP_OK && clientID == audio->clientID && epoch;
  }
  audio->captureWriteActive = captureGranted && captureOwner;
  const bool wasCaptureBound = audio->captureWriteBound;
  audio->captureWriteBound = audio->captureWriteActive;
  if (audio->captureWriteBound && !wasCaptureBound)
    audio->captureSequence = 0;
  unlockWriteGates(audio);

  if (clientStatus != LGMP_OK)
    DEBUG_WARN("Failed to attach/activate client audio control stream: %s",
        lgmpStatusString(clientStatus));
  if (captureStatus != LGMP_OK)
    DEBUG_WARN("Failed to attach/activate capture audio stream: %s",
        lgmpStatusString(captureStatus));
  lgmpStreamPollActivity(&audio->streamPoll);
  return playbackOwner && hostOwner && clientOwner && captureOwner;
}

static bool refreshCaptureBinding(LGMPAudio * audio, bool granted)
{
  lockCaptureWriteGate(audio);
  if (!granted)
  {
    audio->captureWriteActive = false;
    audio->captureWriteBound  = false;
    unlockCaptureWriteGate(audio);
    return true;
  }

  LGMP_STATUS status = audio->captureStream ?
    LGMP_OK : LGMP_ERR_STREAM_UNBOUND;
  if (status == LGMP_OK)
  {
    uint32_t epoch = 0;
    status = lgmpClientStreamActivate(audio->captureStream, &epoch);
  }
  bool owner = status == LGMP_OK;
  if (owner)
  {
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    status = lgmpClientStreamGetBinding(audio->captureStream,
        &clientID, &epoch);
    owner = status == LGMP_OK && clientID == audio->clientID && epoch;
    if (status == LGMP_OK && !owner)
      status = LGMP_ERR_STREAM_STALE;
  }
  const bool wasBound = audio->captureWriteBound;
  audio->captureWriteActive = owner;
  audio->captureWriteBound  = owner;
  if (owner && !wasBound)
    audio->captureSequence = 0;
  unlockCaptureWriteGate(audio);

  if (!owner)
    DEBUG_WARN("Failed to activate granted capture audio stream: %s",
        lgmpStatusString(status));
  return owner;
}

static bool parseRecord(const void * data, uint32_t size, uint32_t capacity,
    KVMFRAudioMessage * message, const uint8_t ** payload)
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

static bool parseMessage(const LGMPStreamBuffer * buffer,
    KVMFRAudioMessage * message, const uint8_t ** payload)
{
  return parseRecord(buffer->data, buffer->size, buffer->capacity,
      message, payload);
}

static void requestRelease(LGMPAudio * audio, const char * reason)
{
  bool log = false;
  lockWriteGates(audio);
  LG_LOCK(audio->lock);
  closePublicDataLocked(audio);
  if (!audio->releaseRequested)
  {
    audio->releaseRequested = true;
    audio->releaseDeadline =
      microtime() + AUDIO_OWNER_RETRY_US;
    markStatusNL(audio, false, 0);
    log = true;
  }
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  audio->stateAckPending = false;
  audio->subscriptionPending = false;
  if (audio->phase == AUDIO_PHASE_BOUND)
    audio->phase = AUDIO_PHASE_RELEASING;
  LG_UNLOCK(audio->lock);
  unlockWriteGates(audio);
  if (log)
    DEBUG_INFO("LGMP audio binding releasing: %s", reason);
  signalWorker(audio);
}

static bool validEmptyMessage(const KVMFRAudioMessage * message,
    uint32_t allowedFlags)
{
  return !(message->flags & ~allowedFlags) &&
    !message->position && !message->timeNs && !message->rateQ32 &&
    !message->frames && !message->length;
}

static bool validVolumePayload(const KVMFRAudioVolume * volume)
{
  if (volume->version != KVMFR_AUDIO_VOLUME_VERSION ||
      volume->size != sizeof(*volume) || volume->reserved ||
      !volume->channelCount ||
      volume->channelCount > KVMFR_AUDIO_STREAM_MAX_CHANNELS)
    return false;
  for (unsigned i = volume->channelCount;
      i < KVMFR_AUDIO_MAX_CHANNELS; ++i)
    if (volume->values[i])
      return false;
  return true;
}

static bool validStatus(const LGMPAudio * audio,
    const KVMFRAudioStatus * status)
{
  if (status->version != KVMFR_AUDIO_STATUS_VERSION ||
      status->size != sizeof(*status) ||
      (status->flags & ~AUDIO_STATUS_FLAGS) ||
      status->protocolVersion != KVMFR_AUDIO_PROTOCOL_MAX ||
      status->requestGeneration != audio->requestGeneration ||
      (status->capabilities & ~AUDIO_CAPABILITIES) ||
      (status->grantedCapabilities & ~status->capabilities) ||
      (status->grantedCapabilities & ~audio->claim.clientCapabilities) ||
      status->bindingEpoch != audio->bindingEpoch ||
      !status->roleEpoch ||
      status->boundClientID != audio->clientID ||
      !(status->flags & KVMFR_AUDIO_STATUS_BOUND) ||
      status->reserved0 || status->reserved1 ||
      !status->endpointGeneration ||
      status->playbackState > KVMFR_AUDIO_STREAM_RUNNING ||
      status->captureState > KVMFR_AUDIO_STREAM_RUNNING)
    return false;

  if (audio->roleEpoch &&
      (int32_t)(status->roleEpoch - audio->roleEpoch) < 0)
    return false;

  const bool playbackGranted =
    (status->grantedCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) != 0;
  const bool captureGranted =
    (status->grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) != 0;
  const bool feedbackGranted =
    (status->grantedCapabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) != 0;
  if ((feedbackGranted && !playbackGranted) ||
      (!(status->capabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
        (status->feedbackAuthorityClientID ||
          status->feedbackAuthorityEpoch)) ||
      (!(status->capabilities & KVMFR_AUDIO_CAP_CAPTURE) &&
        status->captureOwnerClientID) ||
      (!!status->feedbackAuthorityClientID !=
        !!status->feedbackAuthorityEpoch) ||
      (feedbackGranted !=
        (status->feedbackAuthorityClientID == audio->clientID)) ||
      (status->feedbackAuthorityClientID == audio->clientID &&
        status->feedbackAuthorityEpoch != status->roleEpoch) ||
      (captureGranted !=
        (status->captureOwnerClientID == audio->clientID)))
    return false;

  const bool playbackAvailable =
    (status->flags & KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE) != 0;
  const bool captureAvailable =
    (status->flags & KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE) != 0;
  if (playbackAvailable !=
        ((status->capabilities & KVMFR_AUDIO_CAP_PLAYBACK) != 0) ||
      captureAvailable !=
        ((status->capabilities & KVMFR_AUDIO_CAP_CAPTURE) != 0))
    return false;

  const bool playbackActive =
    (status->flags & KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE) != 0;
  const bool captureActive =
    (status->flags & KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE) != 0;
  const bool available =
    (status->flags & KVMFR_AUDIO_STATUS_AVAILABLE) != 0;
  LG_AudioFormat ignored;
  if (playbackActive &&
      (!available || !playbackAvailable || !playbackGranted ||
       !status->playbackGeneration ||
       status->playbackState == KVMFR_AUDIO_STREAM_STOPPED ||
       !validFormat(&status->playbackFormat, &ignored)))
    return false;
  if (captureActive &&
      (!available || !captureAvailable || !captureGranted ||
       !status->captureGeneration ||
       status->captureState == KVMFR_AUDIO_STREAM_STOPPED ||
       !validFormat(&status->captureFormat, &ignored)))
    return false;
  if ((!emptyFormat(&status->playbackFormat) &&
        !validFormat(&status->playbackFormat, &ignored)) ||
      (!emptyFormat(&status->captureFormat) &&
        !validFormat(&status->captureFormat, &ignored)))
    return false;
  return true;
}

static bool statusRequiresBarrier(const KVMFRAudioStatus * current,
    const KVMFRAudioStatus * next)
{
  if (current->version != KVMFR_AUDIO_STATUS_VERSION ||
      current->flags != next->flags ||
      current->protocolVersion != next->protocolVersion ||
      current->requestGeneration != next->requestGeneration ||
      current->capabilities != next->capabilities ||
      current->grantedCapabilities != next->grantedCapabilities ||
      current->bindingEpoch != next->bindingEpoch ||
      current->roleEpoch != next->roleEpoch ||
      current->endpointGeneration != next->endpointGeneration ||
      current->playbackGeneration != next->playbackGeneration ||
      current->captureGeneration != next->captureGeneration ||
      current->playbackState != next->playbackState ||
      current->captureState != next->captureState ||
      !sameFormat(&current->playbackFormat, &next->playbackFormat) ||
      !sameFormat(&current->captureFormat, &next->captureFormat))
    return true;

  if ((next->grantedCapabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) &&
      (current->feedbackAuthorityClientID !=
          next->feedbackAuthorityClientID ||
        current->feedbackAuthorityEpoch != next->feedbackAuthorityEpoch))
    return true;

  return false;
}

static bool currentPlaybackNL(const LGMPAudio * audio,
    uint64_t wireGeneration)
{
  return audio->playback.active && wireGeneration &&
    audio->playback.generation.wire == wireGeneration;
}

static bool currentCaptureNL(const LGMPAudio * audio,
    uint64_t wireGeneration)
{
  return audio->capture.active && wireGeneration &&
    audio->capture.generation.wire == wireGeneration;
}

static bool startMatchesStatus(LGMPAudio * audio,
    KVMFRAudioMessageType type, uint64_t generation,
    const KVMFRAudioFormat * format)
{
  LG_LOCK(audio->lock);
  const bool playback = type == KVMFR_AUDIO_MESSAGE_PLAYBACK_START;
  const KVMFRAudioCapabilities capability = playback ?
    KVMFR_AUDIO_CAP_PLAYBACK : KVMFR_AUDIO_CAP_CAPTURE;
  const KVMFRAudioStatusFlags activeFlag = playback ?
    KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE :
    KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE;
  const uint64_t statusGeneration = playback ?
    audio->remoteStatus.playbackGeneration :
    audio->remoteStatus.captureGeneration;
  const KVMFRAudioFormat * statusFormat = playback ?
    &audio->remoteStatus.playbackFormat :
    &audio->remoteStatus.captureFormat;
  const bool result =
    audio->remoteStatus.version == KVMFR_AUDIO_STATUS_VERSION &&
    audio->remoteStatus.bindingEpoch == audio->bindingEpoch &&
    audio->remoteStatus.roleEpoch == audio->roleEpoch &&
    (audio->remoteStatus.grantedCapabilities & capability) &&
    (audio->remoteStatus.flags & activeFlag) &&
    generation == statusGeneration && sameFormat(format, statusFormat);
  LG_UNLOCK(audio->lock);
  return result;
}

static void dispatchPlaybackStop(LGMPAudio * audio,
    uint64_t wireGeneration)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentPlaybackNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->playback.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  memset(&audio->playback, 0, sizeof(audio->playback));
  audio->playbackDiscontinuity = false;
  LG_UNLOCK(audio->lock);
  updateWriteStreamState(audio);
  if (events && events->playbackStop)
    events->playbackStop(opaque, generation);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchCaptureStop(LGMPAudio * audio,
    uint64_t wireGeneration)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentCaptureNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->capture.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  memset(&audio->capture, 0, sizeof(audio->capture));
  LG_UNLOCK(audio->lock);
  updateWriteStreamState(audio);
  if (events && events->recordStop)
    events->recordStop(opaque, generation);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchPlaybackStart(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const KVMFRAudioFormat * wire,
    const LG_AudioFormat * format, bool sequenceGap)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  const uint32_t oldGeneration = audio->playback.active ?
    audio->playback.generation.local : 0;
  const uint32_t generation = mapGeneration(audio,
      &audio->playback.generation, message->streamGeneration);
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->playback.wireFormat  = *wire;
  audio->playback.format      = *format;
  audio->playback.active      = true;
  audio->playback.formatValid = true;
  audio->playback.volumeValid = false;
  audio->playback.muteValid   = false;
  audio->playback.clockValid =
    (message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) != 0;
  audio->playback.clock = clockFromMessage(message, sequenceGap);
  audio->playbackDiscontinuity = sequenceGap ||
    (message->flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY);
  LG_UNLOCK(audio->lock);
  updateWriteStreamState(audio);

  if (events && oldGeneration && oldGeneration != generation &&
      events->playbackStop)
    events->playbackStop(opaque, oldGeneration);
  if (events && events->playbackStart)
  {
    const bool clockValid =
      (message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) != 0;
    const LG_AudioClock clock =
      clockFromMessage(message, sequenceGap);
    events->playbackStart(opaque, generation, format,
        clockValid ? &clock : NULL);
  }
  LG_UNLOCK(audio->eventLock);
}

static void dispatchCaptureStart(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const KVMFRAudioFormat * wire,
    const LG_AudioFormat * format)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  const uint32_t oldGeneration = audio->capture.active ?
    audio->capture.generation.local : 0;
  const uint32_t generation = mapGeneration(audio,
      &audio->capture.generation, message->streamGeneration);
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->capture.wireFormat  = *wire;
  audio->capture.format      = *format;
  audio->capture.active      = true;
  audio->capture.formatValid = true;
  audio->capture.volumeValid = false;
  audio->capture.muteValid   = false;
  LG_UNLOCK(audio->lock);
  updateWriteStreamState(audio);

  if (events && oldGeneration && oldGeneration != generation &&
      events->recordStop)
    events->recordStop(opaque, oldGeneration);
  if (events && events->recordStart)
    events->recordStart(opaque, generation, format);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchPlaybackVolume(LGMPAudio * audio,
    uint64_t wireGeneration, const KVMFRAudioVolume * volume)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentPlaybackNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->playback.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->playback.volumeValid    = true;
  audio->playback.volumeChannels = (uint8_t)volume->channelCount;
  memcpy(audio->playback.volume, volume->values,
      sizeof(audio->playback.volume));
  LG_UNLOCK(audio->lock);
  if (events && events->playbackVolume)
    events->playbackVolume(opaque, generation,
        (uint8_t)volume->channelCount, volume->values);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchCaptureVolume(LGMPAudio * audio,
    uint64_t wireGeneration, const KVMFRAudioVolume * volume)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentCaptureNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->capture.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->capture.volumeValid    = true;
  audio->capture.volumeChannels = (uint8_t)volume->channelCount;
  memcpy(audio->capture.volume, volume->values,
      sizeof(audio->capture.volume));
  LG_UNLOCK(audio->lock);
  if (events && events->recordVolume)
    events->recordVolume(opaque, generation,
        (uint8_t)volume->channelCount, volume->values);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchPlaybackMute(LGMPAudio * audio,
    uint64_t wireGeneration, bool mute)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentPlaybackNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->playback.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->playback.muteValid = true;
  audio->playback.mute      = mute;
  LG_UNLOCK(audio->lock);
  if (events && events->playbackMute)
    events->playbackMute(opaque, generation, mute);
  LG_UNLOCK(audio->eventLock);
}

static void dispatchCaptureMute(LGMPAudio * audio,
    uint64_t wireGeneration, bool mute)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentCaptureNL(audio, wireGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }
  const uint32_t generation = audio->capture.generation.local;
  const LG_AudioEventOps * events =
    audio->attached ? audio->events : NULL;
  void * opaque = audio->eventOpaque;
  audio->capture.muteValid = true;
  audio->capture.mute      = mute;
  LG_UNLOCK(audio->lock);
  if (events && events->recordMute)
    events->recordMute(opaque, generation, mute);
  LG_UNLOCK(audio->eventLock);
}

static const uint8_t l_silence[KVMFR_AUDIO_PCM_BYTES];

static void dispatchPlaybackData(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload,
    bool sequenceGap)
{
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  if (!currentPlaybackNL(audio, message->streamGeneration))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    return;
  }

  const bool silent =
    (message->flags & KVMFR_AUDIO_MESSAGE_SILENT) != 0;
  const size_t bytes =
    (size_t)message->frames * audio->playback.wireFormat.blockAlign;
  if (bytes > KVMFR_AUDIO_PCM_BYTES ||
      (silent ? message->length != 0 : message->length != bytes))
  {
    LG_UNLOCK(audio->lock);
    LG_UNLOCK(audio->eventLock);
    requestRelease(audio, "invalid playback audio length");
    return;
  }

  const uint32_t           generation    =
    audio->playback.generation.local;
  const LG_AudioEventOps * events        =
    audio->attached ? audio->events : NULL;
  void                   * opaque        = audio->eventOpaque;
  const bool               discontinuity = sequenceGap ||
    audio->playbackDiscontinuity ||
    (message->flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY) != 0;
  const bool               clockValid    =
    (message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) != 0;
  if (!events || !events->playbackData)
    audio->playbackDiscontinuity = true;
  else
    audio->playbackDiscontinuity = false;
  LG_UNLOCK(audio->lock);

  if (events && events->playbackData)
  {
    const LG_AudioClock clock =
      clockFromMessage(message, discontinuity);
    events->playbackData(opaque, generation,
        silent ? l_silence : payload, message->frames,
        clockValid ? &clock : NULL, discontinuity);
  }
  LG_UNLOCK(audio->eventLock);
}

static bool processStatus(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload)
{
  if (message->streamGeneration || message->flags ||
      message->position || message->timeNs || message->rateQ32 ||
      message->frames || message->length != sizeof(KVMFRAudioStatus))
    return false;

  KVMFRAudioStatus status;
  memcpy(&status, payload, sizeof(status));
  lockWriteGates(audio);
  LG_LOCK(audio->lock);
  const bool valid = validStatus(audio, &status);
  if (valid)
  {
    const bool stateUpdate =
      statusRequiresBarrier(&audio->remoteStatus, &status);
    audio->remoteStatus = status;
    audio->roleEpoch = status.roleEpoch;
    if (audio->subscriptionPending)
    {
      audio->subscription.bindingEpoch = status.bindingEpoch;
      audio->subscription.roleEpoch = status.roleEpoch;
    }
    if (stateUpdate)
    {
      audio->playbackReadActive = false;
      audio->stateAcknowledged = false;
      audio->stateAckPending = false;
      audio->writePlaybackGeneration = 0;
      audio->writePlaybackLocalGeneration = 0;
      if (!audio->writeCaptureGeneration)
        audio->publicDataAllowed = false;
    }
    const bool available =
      (status.flags & KVMFR_AUDIO_STATUS_AVAILABLE) != 0;
    markStatusNL(audio, available, status.endpointGeneration);
    audio->writeGrantedCapabilities =
      available ? status.grantedCapabilities : 0;
    audio->writeBindingEpoch = available ? status.bindingEpoch : 0;
    audio->writeRoleEpoch = available ? status.roleEpoch : 0;
    if (!(status.grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE))
    {
      audio->captureWriteActive = false;
      audio->captureWriteBound = false;
      audio->writeCaptureGeneration = 0;
      audio->writeCaptureLocalGeneration = 0;
    }
    if (!(status.grantedCapabilities & KVMFR_AUDIO_CAP_CLOCK_FEEDBACK))
    {
      audio->writePlaybackGeneration = 0;
      audio->writePlaybackLocalGeneration = 0;
    }
  }
  LG_UNLOCK(audio->lock);
  unlockWriteGates(audio);
  if (!valid)
    return false;

  const bool captureGranted =
    (status.grantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) != 0;
  if (!refreshCaptureBinding(audio, captureGranted))
    return false;
  updateWriteStreamState(audio);
  return true;
}

static bool processStateBarrier(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload)
{
  if (message->flags || message->position || message->timeNs ||
      message->rateQ32 || message->frames ||
      message->length != sizeof(KVMFRAudioStateBarrier))
    return false;

  KVMFRAudioStateBarrier barrier;
  memcpy(&barrier, payload, sizeof(barrier));
  if (!barrier.bindingEpoch || !barrier.roleEpoch || !barrier.stateSerial)
    return false;

  lockControlWriteGate(audio);
  LG_LOCK(audio->lock);
  const bool statusPlaybackActive =
    (audio->remoteStatus.flags &
      KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE) != 0;
  const bool statusCaptureActive =
    (audio->remoteStatus.flags &
      KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE) != 0;
  const bool stateInstalled =
    (statusPlaybackActive ?
      currentPlaybackNL(audio,
        audio->remoteStatus.playbackGeneration) &&
      sameFormat(&audio->playback.wireFormat,
        &audio->remoteStatus.playbackFormat) :
      !audio->playback.active) &&
    (statusCaptureActive ?
      currentCaptureNL(audio, audio->remoteStatus.captureGeneration) &&
      sameFormat(&audio->capture.wireFormat,
        &audio->remoteStatus.captureFormat) :
      !audio->capture.active);
  const bool sameBarrier =
    barrier.bindingEpoch == audio->stateAck.bindingEpoch &&
    barrier.roleEpoch == audio->stateAck.roleEpoch &&
    barrier.stateSerial == audio->stateAck.stateSerial &&
    message->streamGeneration == audio->stateAckGeneration;
  const bool current = audio->phase == AUDIO_PHASE_BOUND &&
    audio->remoteStatus.version == KVMFR_AUDIO_STATUS_VERSION &&
    barrier.bindingEpoch == audio->bindingEpoch &&
    barrier.roleEpoch == audio->roleEpoch &&
    stateInstalled &&
    ((statusPlaybackActive && message->streamGeneration ==
        audio->remoteStatus.playbackGeneration) ||
      (!statusPlaybackActive && !message->streamGeneration)) &&
    audio->playbackStream && audio->playbackReaderEpoch &&
    (sameBarrier || !audio->stateAck.stateSerial ||
      barrier.stateSerial > audio->stateAck.stateSerial);
  if (current && sameBarrier)
    audio->stateAckPending = true;
  else if (current)
  {
    audio->playbackReadActive = false;
    audio->stateAcknowledged = false;
  }
  LG_UNLOCK(audio->lock);
  if (!current)
  {
    unlockControlWriteGate(audio);
    return false;
  }
  if (sameBarrier)
  {
    unlockControlWriteGate(audio);
    return true;
  }

  uint64_t skipped = 0;
  const LGMP_STATUS status =
    lgmpClientSPMCSync(audio->playbackStream, &skipped);
  if (status != LGMP_OK)
  {
    DEBUG_WARN("Failed to synchronize playback audio stream: %s",
        lgmpStatusString(status));
    unlockControlWriteGate(audio);
    return false;
  }

  LG_LOCK(audio->lock);
  const bool stillCurrent = audio->phase == AUDIO_PHASE_BOUND &&
    barrier.bindingEpoch == audio->bindingEpoch &&
    barrier.roleEpoch == audio->roleEpoch;
  if (stillCurrent)
  {
    audio->stateAck = barrier;
    audio->stateAckGeneration = message->streamGeneration;
    audio->stateAckPending = true;
    audio->playbackSequenceKnown = false;
    audio->expectedPlaybackSequence = 0;
    audio->playbackDiscontinuity = skipped != 0 || audio->playback.active;
  }
  LG_UNLOCK(audio->lock);
  unlockControlWriteGate(audio);
  return stillCurrent;
}

static bool processInboundMessage(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload,
    bool sequenceGap)
{
  switch (message->type)
  {
    case KVMFR_AUDIO_MESSAGE_STATUS:
      return processStatus(audio, message, payload);

    case KVMFR_AUDIO_MESSAGE_STATE_BARRIER:
      return processStateBarrier(audio, message, payload);

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_START:
    case KVMFR_AUDIO_MESSAGE_CAPTURE_START:
    {
      if (!message->streamGeneration || message->frames ||
          message->length != sizeof(KVMFRAudioFormat) ||
          !validClockFlags(message->flags) ||
          (!(message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) &&
            (message->position || message->timeNs || message->rateQ32)))
        return false;
      KVMFRAudioFormat wire;
      LG_AudioFormat format;
      memcpy(&wire, payload, sizeof(wire));
      if (!validFormat(&wire, &format))
        return false;
      if (!startMatchesStatus(
            audio, message->type, message->streamGeneration, &wire))
        return false;
      if (message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_START)
        dispatchPlaybackStart(
            audio, message, &wire, &format, sequenceGap);
      else
        dispatchCaptureStart(audio, message, &wire, &format);
      return true;
    }

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP:
    case KVMFR_AUDIO_MESSAGE_CAPTURE_STOP:
      if (!message->streamGeneration ||
          !validEmptyMessage(message, 0))
        return false;
      if (message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP)
        dispatchPlaybackStop(audio, message->streamGeneration);
      else
        dispatchCaptureStop(audio, message->streamGeneration);
      return true;

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME:
    case KVMFR_AUDIO_MESSAGE_CAPTURE_VOLUME:
    {
      if (!message->streamGeneration || message->flags ||
          message->position || message->timeNs || message->rateQ32 ||
          message->frames ||
          message->length != sizeof(KVMFRAudioVolume))
        return false;
      KVMFRAudioVolume volume;
      memcpy(&volume, payload, sizeof(volume));
      if (!validVolumePayload(&volume))
        return false;
      LG_LOCK(audio->lock);
      const bool current =
        message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME ?
          currentPlaybackNL(audio, message->streamGeneration) :
          currentCaptureNL(audio, message->streamGeneration);
      const uint8_t channels =
        message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME ?
          audio->playback.format.channelCount :
          audio->capture.format.channelCount;
      LG_UNLOCK(audio->lock);
      if (!current)
        return true;
      if (volume.channelCount != channels)
        return false;
      if (message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_VOLUME)
        dispatchPlaybackVolume(
            audio, message->streamGeneration, &volume);
      else
        dispatchCaptureVolume(
            audio, message->streamGeneration, &volume);
      return true;
    }

    case KVMFR_AUDIO_MESSAGE_PLAYBACK_MUTE:
    case KVMFR_AUDIO_MESSAGE_CAPTURE_MUTE:
      if (!message->streamGeneration ||
          !validEmptyMessage(message, KVMFR_AUDIO_MESSAGE_MUTED))
        return false;
      if (message->type == KVMFR_AUDIO_MESSAGE_PLAYBACK_MUTE)
        dispatchPlaybackMute(audio, message->streamGeneration,
            (message->flags & KVMFR_AUDIO_MESSAGE_MUTED) != 0);
      else
        dispatchCaptureMute(audio, message->streamGeneration,
            (message->flags & KVMFR_AUDIO_MESSAGE_MUTED) != 0);
      return true;

    default:
      return false;
  }
}

static bool processPlaybackMessage(LGMPAudio * audio,
    const KVMFRAudioMessage * message, const uint8_t * payload,
    bool discontinuity)
{
  if (message->type != KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA ||
      !message->streamGeneration || !message->frames ||
      message->frames > KVMFR_AUDIO_MAX_FRAMES_PER_PACKET ||
      (message->flags & ~AUDIO_DATA_FLAGS) ||
      !validClockFlags(message->flags & AUDIO_CLOCK_FLAGS) ||
      (!(message->flags & KVMFR_AUDIO_MESSAGE_CLOCK_VALID) &&
        (message->position || message->timeNs || message->rateQ32)))
    return false;
  dispatchPlaybackData(audio, message, payload, discontinuity);
  return true;
}

enum WriteResult
{
  WRITE_OK,
  WRITE_FULL,
  WRITE_LOST,
};

static enum WriteResult writeMessageLocked(LGMPAudio * audio,
    PLGMPClientStream stream, uint64_t * sequence,
    KVMFRAudioMessageType type, uint64_t streamGeneration,
    KVMFRAudioMessageFlags flags, uint64_t position, int64_t timeNs,
    uint64_t rateQ32, uint32_t frames, const void * payload,
    uint32_t length)
{
  if (!stream || !sequence)
    return WRITE_LOST;

  LGMPStreamBuffer buffer = { 0 };
  LGMP_STATUS status = lgmpClientStreamWriteAcquire(
      stream, &buffer);
  if (status == LGMP_ERR_STREAM_FULL)
    return WRITE_FULL;
  if (status != LGMP_OK)
  {
    atomic_store_explicit(
        &audio->dataPlaneLost, true, memory_order_release);
    return WRITE_LOST;
  }

  const uint32_t bytes = (uint32_t)sizeof(KVMFRAudioMessage) + length;
  if (!buffer.data || buffer.capacity < bytes)
  {
    (void)lgmpClientStreamWriteCancel(
        stream, &buffer);
    atomic_store_explicit(
        &audio->dataPlaneLost, true, memory_order_release);
    return WRITE_LOST;
  }

  const KVMFRAudioMessage message =
  {
    .version          = KVMFR_AUDIO_MESSAGE_VERSION,
    .size             = sizeof(KVMFRAudioMessage),
    .type             = type,
    .streamGeneration = streamGeneration,
    .flags            = flags,
    .sequence         = *sequence,
    .position         = position,
    .timeNs           = timeNs,
    .rateQ32          = rateQ32,
    .frames           = frames,
    .length           = length,
  };
  memcpy(buffer.data, &message, sizeof(message));
  if (length)
    memcpy((uint8_t *)buffer.data + sizeof(message), payload, length);

  status = lgmpClientStreamWriteCommit(
      stream, &buffer, bytes);
  if (status == LGMP_OK)
  {
    ++*sequence;
    return WRITE_OK;
  }
  atomic_store_explicit(
      &audio->dataPlaneLost, true, memory_order_release);
  return WRITE_LOST;
}

static bool validResponse(const LGMPAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  if (response->magic != KVMFR_AUDIO_RESPONSE_MAGIC ||
      response->version != KVMFR_AUDIO_CONTROL_VERSION ||
      response->size != sizeof(*response) ||
      response->requestGeneration != audio->requestGeneration ||
      response->boundClientID != audio->clientID ||
      response->claimToken != audio->claimToken ||
      response->result < KVMFR_AUDIO_RESPONSE_BOUND ||
      response->result > KVMFR_AUDIO_RESPONSE_ERROR)
    return false;
  for (unsigned i = 0; i < sizeof(response->cursorCompatibility); ++i)
    if (response->cursorCompatibility[i])
      return false;

  if (response->result == KVMFR_AUDIO_RESPONSE_BOUND)
    return response->protocolVersion == KVMFR_AUDIO_PROTOCOL_MAX &&
      response->endpointGeneration &&
      response->bindingEpoch && response->roleEpoch &&
      response->playbackReaderID < KVMFR_AUDIO_MAX_PLAYBACK_READERS &&
      !(response->grantedCapabilities & ~AUDIO_CAPABILITIES) &&
      !(response->grantedCapabilities &
        ~audio->claim.clientCapabilities) &&
      (response->grantedCapabilities & KVMFR_AUDIO_CAP_PLAYBACK) &&
      (!(response->grantedCapabilities &
          KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) ||
        (response->grantedCapabilities & KVMFR_AUDIO_CAP_PLAYBACK)) &&
      validSPMCDescriptor(&response->playback) &&
      validDescriptor(&response->hostControl,
        LGMP_STREAM_HOST_TO_CLIENT,
        KVMFR_AUDIO_HOST_CONTROL_SLOT_BYTES) &&
      validDescriptor(&response->clientControl,
        LGMP_STREAM_CLIENT_TO_HOST,
        KVMFR_AUDIO_CLIENT_CONTROL_SLOT_BYTES) &&
      validDescriptor(&response->capture,
        LGMP_STREAM_CLIENT_TO_HOST, KVMFR_AUDIO_CAPTURE_SLOT_BYTES);

  const KVMFRStreamDescriptor empty = { 0 };
  const KVMFRSPMCDescriptor emptySPMC = { 0 };
  return !response->protocolVersion &&
    !response->grantedCapabilities && !response->bindingEpoch &&
    !response->roleEpoch && !response->playbackReaderID &&
    !memcmp(&response->playback, &emptySPMC, sizeof(emptySPMC)) &&
    !memcmp(&response->hostControl, &empty, sizeof(empty)) &&
    !memcmp(&response->clientControl, &empty, sizeof(empty)) &&
    !memcmp(&response->capture, &empty, sizeof(empty));
}

static void processResponse(LGMPAudio * audio)
{
  KVMFRAudioStreamResponse response;
  bool pending = false;
  LG_LOCK(audio->lock);
  if (audio->responsePending)
  {
    response = audio->response;
    audio->responsePending = false;
    pending = true;
  }
  LG_UNLOCK(audio->lock);
  if (!pending)
    return;

  LG_LOCK(audio->lock);
  const bool current = audio->connected &&
    (audio->phase == AUDIO_PHASE_CLAIM_SENT ||
     audio->phase == AUDIO_PHASE_CLAIM_PENDING ||
     audio->phase == AUDIO_PHASE_COOLDOWN) &&
    response.requestGeneration == audio->requestGeneration &&
    response.claimToken == audio->claimToken;
  const bool valid = current && validResponse(audio, &response);
  LG_UNLOCK(audio->lock);
  if (!current)
    return;
  if (!valid)
  {
    DEBUG_WARN("Ignoring malformed matching LGMP audio response");
    LG_LOCK(audio->lock);
    audio->phase = AUDIO_PHASE_COOLDOWN;
    audio->releaseDeadline =
      microtime() + AUDIO_OWNER_RETRY_US;
    LG_UNLOCK(audio->lock);
    return;
  }

  if (response.result != KVMFR_AUDIO_RESPONSE_BOUND)
  {
    if (response.result == KVMFR_AUDIO_RESPONSE_INCOMPATIBLE)
      DEBUG_WARN("The host LGMP audio protocol is incompatible");
    LG_LOCK(audio->lock);
    scheduleRetryNL(audio, response.result ==
        KVMFR_AUDIO_RESPONSE_INCOMPATIBLE ?
          AUDIO_OWNER_RETRY_US : AUDIO_CLAIM_RETRY_US);
    LG_UNLOCK(audio->lock);
    if (audio->pointerWake)
      lgSignalEvent(audio->pointerWake);
    return;
  }

  if (!attachStreams(audio, &response))
  {
    LG_LOCK(audio->lock);
    audio->phase = AUDIO_PHASE_RELEASING;
    audio->releaseRequested = true;
    audio->releaseSent = false;
    audio->releaseDeadline =
      microtime() + AUDIO_OWNER_RETRY_US;
    audio->nextAttachAt = microtime() + AUDIO_CLAIM_RETRY_US;
    LG_UNLOCK(audio->lock);
    return;
  }

  LG_LOCK(audio->lock);
  if (!audio->connected ||
      response.requestGeneration != audio->requestGeneration ||
      response.claimToken != audio->claimToken)
  {
    audio->releaseRequested = true;
    audio->phase = AUDIO_PHASE_RELEASING;
  }
  else
  {
    mapGeneration(audio, &audio->endpointGeneration,
        response.endpointGeneration);
    audio->bindingEpoch = response.bindingEpoch;
    audio->roleEpoch = response.roleEpoch;
    audio->subscription = (KVMFRAudioSubscription)
    {
      .bindingEpoch  = response.bindingEpoch,
      .roleEpoch     = response.roleEpoch,
      .subscriptions = audio->claim.subscriptions,
    };
    audio->subscriptionPending = true;
    audio->phase = audio->releaseRequested || !audio->discover ?
      AUDIO_PHASE_RELEASING : AUDIO_PHASE_BOUND;
  }
  audio->controlSequenceKnown = false;
  audio->playbackSequenceKnown = false;
  audio->expectedControlSequence = 0;
  audio->expectedPlaybackSequence = 0;
  audio->stateAckPending = false;
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  memset(&audio->stateAck, 0, sizeof(audio->stateAck));
  audio->stateAckGeneration = 0;
  audio->releaseSent       = false;
  audio->nextKeepalive     = microtime();
  LG_UNLOCK(audio->lock);
  DEBUG_INFO("LGMP audio streams attached for request %u",
      response.requestGeneration);
}

static unsigned drainControl(LGMPAudio * audio)
{
  unsigned drained = 0;
  while (audio->hostControlStream && audio->hostControlActive &&
      drained < AUDIO_DRAIN_MAX)
  {
    LGMPStreamBuffer buffer = { 0 };
    LGMP_STATUS status = lgmpClientStreamReadPeek(
        audio->hostControlStream, &buffer);
    if (status == LGMP_ERR_STREAM_EMPTY)
      break;
    if (status == LGMP_ERR_STREAM_UNBOUND ||
        status == LGMP_ERR_STREAM_STALE)
    {
      atomic_store_explicit(
          &audio->dataPlaneLost, true, memory_order_release);
      break;
    }
    if (status != LGMP_OK)
    {
      DEBUG_WARN("Failed to read LGMP audio stream: %s",
          lgmpStatusString(status));
      atomic_store_explicit(
          &audio->dataPlaneLost, true, memory_order_release);
      break;
    }

    ++drained;
    LG_LOCK(audio->lock);
    const bool releasing = audio->phase == AUDIO_PHASE_RELEASING;
    LG_UNLOCK(audio->lock);
    bool valid = true;
    if (!releasing)
    {
      KVMFRAudioMessage message;
      const uint8_t * payload;
      valid = parseMessage(&buffer, &message, &payload);
      if (valid)
      {
        LG_LOCK(audio->lock);
        const bool sequenceGap = audio->controlSequenceKnown ?
          message.sequence != audio->expectedControlSequence :
          message.sequence != 0;
        audio->controlSequenceKnown = true;
        audio->expectedControlSequence = message.sequence + 1;
        LG_UNLOCK(audio->lock);
        valid = !sequenceGap && processInboundMessage(
            audio, &message, payload, false);
      }
    }

    status = lgmpClientStreamReadRelease(
        audio->hostControlStream, &buffer);
    if (status != LGMP_OK)
    {
      if (status != LGMP_ERR_STREAM_UNBOUND &&
          status != LGMP_ERR_STREAM_STALE)
        DEBUG_WARN("Failed to release LGMP audio record: %s",
            lgmpStatusString(status));
      atomic_store_explicit(
          &audio->dataPlaneLost, true, memory_order_release);
      break;
    }
    if (!valid)
    {
      requestRelease(audio, "malformed host-to-client record");
      break;
    }
  }
  return drained;
}

static unsigned drainPlayback(LGMPAudio * audio)
{
  unsigned drained = 0;
  while (audio->playbackStream && audio->playbackReadActive &&
      drained < AUDIO_DRAIN_MAX)
  {
    struct LGMPSPMCRecord record = { 0 };
    const LGMP_STATUS status = lgmpClientSPMCRead(audio->playbackStream,
        audio->playbackRecord,
        (uint32_t)sizeof(audio->playbackRecord), &record);
    if (status == LGMP_ERR_STREAM_EMPTY)
      break;
    if (status == LGMP_ERR_STREAM_BUSY)
    {
      LG_LOCK(audio->lock);
      audio->playbackDiscontinuity = true;
      LG_UNLOCK(audio->lock);
      break;
    }
    if (status == LGMP_ERR_STREAM_UNBOUND ||
        status == LGMP_ERR_STREAM_STALE)
    {
      atomic_store_explicit(
          &audio->dataPlaneLost, true, memory_order_release);
      break;
    }
    if (status != LGMP_OK)
    {
      DEBUG_WARN("Failed to read LGMP playback audio stream: %s",
          lgmpStatusString(status));
      atomic_store_explicit(
          &audio->dataPlaneLost, true, memory_order_release);
      break;
    }

    ++drained;
    KVMFRAudioMessage message;
    const uint8_t * payload;
    bool valid = !record.reserved && record.size &&
      parseRecord(audio->playbackRecord, record.size,
        (uint32_t)sizeof(audio->playbackRecord), &message, &payload) &&
      message.sequence == record.sequence;
    bool discontinuity = record.skipped != 0;
    if (valid)
    {
      LG_LOCK(audio->lock);
      if (audio->playbackSequenceKnown &&
          record.sequence != audio->expectedPlaybackSequence)
        discontinuity = true;
      audio->playbackSequenceKnown = true;
      audio->expectedPlaybackSequence = record.sequence + 1;
      LG_UNLOCK(audio->lock);
      valid = processPlaybackMessage(
          audio, &message, payload, discontinuity);
    }
    if (!valid)
    {
      requestRelease(audio, "malformed playback SPMC record");
      break;
    }
  }
  return drained;
}

static bool bindingGone(LGMPAudio * audio)
{
  bool any = false;
  const PLGMPClientStream streams[2] =
  {
    audio->hostControlStream,
    audio->clientControlStream,
  };
  for (unsigned i = 0; i < 2; ++i)
  {
    if (!streams[i])
      continue;
    any = true;
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    if (i == 1)
      lockControlWriteGate(audio);
    const LGMP_STATUS status = lgmpClientStreamGetBinding(
        streams[i], &clientID, &epoch);
    if (i == 1)
      unlockControlWriteGate(audio);
    if (status == LGMP_OK && clientID == audio->clientID && epoch)
      return false;
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
        status != LGMP_ERR_STREAM_STALE)
    {
      DEBUG_WARN("Failed to query LGMP audio binding: %s",
          lgmpStatusString(status));
      return false;
    }
  }

  if (audio->playbackStream)
  {
    any = true;
    uint32_t clientID = 0;
    uint32_t epoch = 0;
    const LGMP_STATUS status = lgmpClientSPMCGetBinding(
        audio->playbackStream, &clientID, &epoch);
    if (status == LGMP_OK && clientID == audio->clientID && epoch)
      return false;
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND &&
        status != LGMP_ERR_STREAM_STALE)
    {
      DEBUG_WARN("Failed to query LGMP playback binding: %s",
          lgmpStatusString(status));
      return false;
    }
  }
  return any;
}

static bool bindingPhaseActiveNL(const LGMPAudio * audio)
{
  return audio->phase == AUDIO_PHASE_BOUND ||
    audio->phase == AUDIO_PHASE_RELEASING;
}

static void maintainBinding(LGMPAudio * audio)
{
  const uint64_t now = microtime();
  LG_LOCK(audio->lock);
  const enum AudioPhase phase = audio->phase;
  const bool releaseRequested = audio->releaseRequested;
  const bool releaseSent      = audio->releaseSent;
  const bool subscriptionPending = audio->subscriptionPending;
  const bool stateAckPending  = audio->stateAckPending;
  const uint64_t deadline     = audio->releaseDeadline;
  const uint64_t nextAttachAt = audio->nextAttachAt;
  const bool keepalive = phase == AUDIO_PHASE_BOUND &&
    !releaseRequested && now >= audio->nextKeepalive;
  LG_UNLOCK(audio->lock);

  if ((phase == AUDIO_PHASE_CLAIM_PENDING ||
       phase == AUDIO_PHASE_CLAIM_SENT ||
       phase == AUDIO_PHASE_COOLDOWN) &&
      releaseRequested && now >= deadline)
  {
    LG_LOCK(audio->lock);
    if ((audio->phase == AUDIO_PHASE_CLAIM_PENDING ||
         audio->phase == AUDIO_PHASE_CLAIM_SENT ||
         audio->phase == AUDIO_PHASE_COOLDOWN) &&
        audio->releaseRequested &&
        !audio->claimPosting)
    {
      audio->responsePending = false;
      scheduleRetryNL(audio, AUDIO_CLAIM_RETRY_US);
    }
    LG_UNLOCK(audio->lock);
    return;
  }

  if (phase == AUDIO_PHASE_COOLDOWN && now >= deadline)
  {
    bool wakePointer = false;
    LG_LOCK(audio->lock);
    if (audio->phase == AUDIO_PHASE_COOLDOWN &&
        now >= audio->releaseDeadline &&
        !audio->releaseRequested && audio->discover)
    {
      audio->phase = audio->claimSends ?
        AUDIO_PHASE_CLAIM_SENT : AUDIO_PHASE_CLAIM_PENDING;
      audio->claimSends  = 0;
      audio->nextClaimAt = 0;
      wakePointer = true;
    }
    else if (audio->phase == AUDIO_PHASE_COOLDOWN &&
        !audio->claimSends && !audio->claimPosting)
      scheduleRetryNL(audio, AUDIO_CLAIM_RETRY_US);
    LG_UNLOCK(audio->lock);
    if (wakePointer && audio->pointerWake)
      lgSignalEvent(audio->pointerWake);
    return;
  }

  if (phase != AUDIO_PHASE_BOUND &&
      phase != AUDIO_PHASE_RELEASING)
    return;

  if (phase == AUDIO_PHASE_RELEASING &&
      (!audio->hostControlActive || !audio->clientControlActive ||
       !audio->playbackStream) &&
      now >= nextAttachAt)
  {
    LG_LOCK(audio->lock);
    const KVMFRAudioStreamResponse response = audio->response;
    audio->nextAttachAt = now + AUDIO_CLAIM_RETRY_US;
    LG_UNLOCK(audio->lock);
    attachStreams(audio, &response);
  }

  if (phase == AUDIO_PHASE_RELEASING && !releaseSent &&
      audio->clientControlActive)
  {
    lockControlWriteGate(audio);
    const enum WriteResult result = writeMessageLocked(audio,
        audio->clientControlStream, &audio->clientControlSequence,
        KVMFR_AUDIO_MESSAGE_RELEASE, 0, 0, 0, 0, 0, 0, NULL, 0);
    unlockControlWriteGate(audio);
    if (result == WRITE_OK)
    {
      LG_LOCK(audio->lock);
      if (audio->phase == AUDIO_PHASE_RELEASING)
        audio->releaseSent = true;
      LG_UNLOCK(audio->lock);
    }
  }
  else if (phase == AUDIO_PHASE_BOUND && !releaseRequested &&
      subscriptionPending && audio->clientControlActive)
  {
    LG_LOCK(audio->lock);
    const KVMFRAudioSubscription subscription = audio->subscription;
    LG_UNLOCK(audio->lock);
    lockControlWriteGate(audio);
    const enum WriteResult result = writeMessageLocked(audio,
        audio->clientControlStream, &audio->clientControlSequence,
        KVMFR_AUDIO_MESSAGE_PLAYBACK_SUBSCRIBE, 0, 0, 0, 0, 0, 0,
        &subscription, sizeof(subscription));
    unlockControlWriteGate(audio);
    if (result == WRITE_OK)
    {
      LG_LOCK(audio->lock);
      if (!memcmp(&audio->subscription, &subscription,
            sizeof(subscription)))
        audio->subscriptionPending = false;
      LG_UNLOCK(audio->lock);
    }
  }
  else if (phase == AUDIO_PHASE_BOUND && !releaseRequested &&
      stateAckPending && audio->clientControlActive)
  {
    LG_LOCK(audio->lock);
    const KVMFRAudioStateBarrier ack = audio->stateAck;
    const uint64_t generation = audio->stateAckGeneration;
    LG_UNLOCK(audio->lock);
    lockControlWriteGate(audio);
    const enum WriteResult result = writeMessageLocked(audio,
        audio->clientControlStream, &audio->clientControlSequence,
        KVMFR_AUDIO_MESSAGE_STATE_ACK, generation, 0, 0, 0, 0, 0,
        &ack, sizeof(ack));
    unlockControlWriteGate(audio);
    if (result == WRITE_OK)
    {
      bool acknowledged = false;
      LG_LOCK(audio->lock);
      if (!memcmp(&audio->stateAck, &ack, sizeof(ack)) &&
          audio->stateAckGeneration == generation)
      {
        audio->stateAckPending = false;
        acknowledged = audio->phase == AUDIO_PHASE_BOUND &&
          !audio->releaseRequested &&
          ack.bindingEpoch == audio->bindingEpoch &&
          ack.roleEpoch == audio->roleEpoch;
        audio->stateAcknowledged = acknowledged;
        audio->playbackReadActive = acknowledged &&
          audio->statusAvailable &&
          (audio->remoteStatus.grantedCapabilities &
            KVMFR_AUDIO_CAP_PLAYBACK) &&
          (audio->subscription.subscriptions &
            KVMFR_AUDIO_SUBSCRIBE_PLAYBACK);
      }
      LG_UNLOCK(audio->lock);
      if (acknowledged)
        updateWriteStreamState(audio);
    }
  }
  else if (keepalive)
  {
    lockControlWriteGate(audio);
    const enum WriteResult result = writeMessageLocked(audio,
        audio->clientControlStream, &audio->clientControlSequence,
        KVMFR_AUDIO_MESSAGE_KEEPALIVE, 0, 0, 0, 0, 0, 0, NULL, 0);
    unlockControlWriteGate(audio);
    if (result == WRITE_OK)
    {
      LG_LOCK(audio->lock);
      if (audio->phase == AUDIO_PHASE_BOUND)
        audio->nextKeepalive =
          now + (uint64_t)KVMFR_AUDIO_KEEPALIVE_MS * 1000U;
      LG_UNLOCK(audio->lock);
    }
  }

  if (phase == AUDIO_PHASE_RELEASING && bindingGone(audio))
    finishBinding(audio, AUDIO_CLAIM_RETRY_US);
}

static void stopWorkerBinding(LGMPAudio * audio)
{
  const uint64_t now = microtime();
  LG_LOCK(audio->lock);
  const bool active = bindingPhaseActiveNL(audio);
  audio->phase = active ? AUDIO_PHASE_RELEASING : audio->phase;
  if (active && !audio->releaseDeadline)
    audio->releaseDeadline = now + AUDIO_OWNER_RETRY_US;
  const uint64_t releaseDeadline = audio->releaseDeadline;
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  audio->stateAckPending = false;
  audio->subscriptionPending = false;
  LG_UNLOCK(audio->lock);
  if (!active)
  {
    stopActiveStreams(audio);
    detachStreams(audio);
    return;
  }

  for (;;)
  {
    drainControl(audio);
    drainPlayback(audio);
    maintainBinding(audio);

    LG_LOCK(audio->lock);
    const bool bindingActive = bindingPhaseActiveNL(audio);
    LG_UNLOCK(audio->lock);
    if (!bindingActive || !lgmpClientSessionValid(audio->client))
      break;
    if (microtime() >= releaseDeadline)
    {
      DEBUG_WARN("Timed out waiting for LGMP audio binding release");
      break;
    }
    lgWaitEvent(audio->event, 1);
  }
  stopActiveStreams(audio);
  detachStreams(audio);
}

static int audioThread(void * opaque)
{
  LGMPAudio * audio = opaque;
  const struct LGMPStreamPollConfig pollConfig =
  {
    .spinCount = 64,
    .minWaitUs = 25,
    .maxWaitUs = AUDIO_ACTIVE_POLL_US,
  };
  if (lgmpStreamPollInit(&audio->streamPoll, pollConfig) != LGMP_OK)
    return -1;

  while (!atomic_load_explicit(&audio->stop, memory_order_acquire))
  {
    processResponse(audio);
    const unsigned controlDrained = drainControl(audio);
    const unsigned playbackDrained = drainPlayback(audio);
    const unsigned drained = controlDrained + playbackDrained;
    if (drained)
      lgmpStreamPollActivity(&audio->streamPoll);
    maintainBinding(audio);
    if (atomic_exchange_explicit(
          &audio->dataPlaneLost, false, memory_order_acq_rel))
    {
      requestRelease(audio, "audio stream operation failed");
      if (bindingGone(audio))
        finishBinding(audio, AUDIO_CLAIM_RETRY_US);
    }
    publishStatus(audio);

    if (atomic_load_explicit(&audio->stop, memory_order_acquire))
      break;
    LG_LOCK(audio->lock);
    const bool active = bindingPhaseActiveNL(audio);
    LG_UNLOCK(audio->lock);
    if (active)
    {
      if (controlDrained == AUDIO_DRAIN_MAX ||
          playbackDrained == AUDIO_DRAIN_MAX)
      {
        lgmpStreamPollActivity(&audio->streamPoll);
        continue;
      }
      const uint32_t waitUs = lgmpStreamPollIdle(&audio->streamPoll);
      if (!waitUs)
        continue;
      if (lgWaitEventNS(audio->event, waitUs * 1000U))
        lgmpStreamPollActivity(&audio->streamPoll);
    }
    else
      lgWaitEvent(audio->event, AUDIO_IDLE_POLL_MS);
  }

  stopWorkerBinding(audio);
  publishStatus(audio);
  return 0;
}

bool lgmpAudio_create(PLGMPClient client, LGEvent * pointerWake,
    LGMPAudio ** result)
{
  if (!client || !pointerWake || !result)
    return false;
  LGMPAudio * audio = calloc(1, sizeof(*audio));
  if (!audio)
    return false;
  audio->client       = client;
  audio->pointerWake  = pointerWake;
  audio->event        = lgCreateEvent(true, 0);
  if (!audio->event)
  {
    free(audio);
    return false;
  }
  audio->generationSerial = 1;
  if (!lgRandomBytes(&audio->requestSerial,
        sizeof(audio->requestSerial)))
  {
    lgFreeEvent(audio->event);
    free(audio);
    return false;
  }
  if (!audio->requestSerial)
    audio->requestSerial = 1;
  LG_LOCK_INIT(audio->lock);
  LG_LOCK_INIT(audio->eventLock);
  LG_LOCK_INIT(audio->statusWait.lock);
  atomic_flag_clear(&audio->controlWriteGate);
  atomic_flag_clear(&audio->captureWriteGate);
  atomic_init(&audio->stop, false);
  atomic_init(&audio->dataPlaneLost, false);
  atomic_init(&audio->captureDiscontinuity, false);
  atomic_init(&audio->clockFeedbackAvailable, false);
  atomic_init(&audio->statusInFlight, 0);
  atomic_init(&audio->statusWait.count, 0);
  *result = audio;
  return true;
}

void lgmpAudio_destroy(LGMPAudio ** audio)
{
  if (!audio || !*audio)
    return;
  lgmpAudio_disconnect(*audio);
  lgFreeEvent((*audio)->event);
  LG_LOCK_FREE((*audio)->statusWait.lock);
  LG_LOCK_FREE((*audio)->eventLock);
  LG_LOCK_FREE((*audio)->lock);
  free(*audio);
  *audio = NULL;
}

bool lgmpAudio_connect(LGMPAudio * audio, uint32_t clientID)
{
  if (!audio || !clientID)
    return false;
  LG_LOCK(audio->lock);
  if (audio->connected)
  {
    const bool same = audio->clientID == clientID;
    LG_UNLOCK(audio->lock);
    return same;
  }
  if (audio->thread)
  {
    LG_UNLOCK(audio->lock);
    return false;
  }
  lgResetEvent(audio->event);
  audio->connected          = true;
  audio->clientID           = clientID;
  audio->phase              = AUDIO_PHASE_IDLE;
  audio->statusAvailable    = false;
  audio->statusDirty        = true;
  audio->nextClaimAt        = 0;
  audio->releaseRequested   = false;
  audio->releaseSent        = false;
  audio->bindingEpoch       = 0;
  audio->roleEpoch          = 0;
  audio->playbackReaderEpoch = 0;
  audio->stateAckPending    = false;
  audio->subscriptionPending = false;
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  memset(&audio->endpointGeneration, 0,
      sizeof(audio->endpointGeneration));
  atomic_store_explicit(&audio->stop, false, memory_order_release);
  atomic_store_explicit(
      &audio->dataPlaneLost, false, memory_order_release);

  LGThread * thread;
  if (lgCreateThread("lgmpAudio", audioThread, audio, &thread))
  {
    audio->thread = thread;
    LG_UNLOCK(audio->lock);
    return true;
  }

  audio->connected = false;
  LG_UNLOCK(audio->lock);
  return false;
}

void lgmpAudio_disconnect(LGMPAudio * audio)
{
  if (!audio)
    return;

  LG_LOCK(audio->eventLock);
  lockWriteGates(audio);
  LG_LOCK(audio->lock);
  closePublicDataLocked(audio);
  audio->events      = NULL;
  audio->eventOpaque = NULL;
  audio->attached    = false;
  audio->discover    = false;
  audio->releaseRequested = true;
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  audio->stateAckPending = false;
  audio->subscriptionPending = false;
  audio->releaseDeadline =
    microtime() + AUDIO_OWNER_RETRY_US;
  if (audio->phase == AUDIO_PHASE_BOUND)
    audio->phase = AUDIO_PHASE_RELEASING;
  markStatusNL(audio, false, 0);
  audio->connected = false;
  LGThread * thread = audio->thread;
  LGEvent * event   = audio->event;
  atomic_store_explicit(&audio->stop, true, memory_order_release);
  LG_UNLOCK(audio->lock);
  unlockWriteGates(audio);
  LG_UNLOCK(audio->eventLock);

  if (event)
    lgSignalEvent(event);
  if (thread)
    lgJoinThread(thread, NULL);

  LG_LOCK(audio->lock);
  audio->thread             = NULL;
  audio->clientID           = 0;
  audio->phase              = AUDIO_PHASE_IDLE;
  audio->responsePending    = false;
  audio->claimPosting       = false;
  audio->requestGeneration  = 0;
  audio->claimToken         = 0;
  audio->claimSends         = 0;
  audio->releaseRequested   = false;
  audio->releaseSent        = false;
  audio->bindingEpoch       = 0;
  audio->roleEpoch          = 0;
  audio->playbackReaderEpoch = 0;
  audio->stateAckPending    = false;
  audio->subscriptionPending = false;
  audio->playbackReadActive = false;
  audio->stateAcknowledged = false;
  audio->statusCallback     = NULL;
  audio->statusOpaque       = NULL;
  memset(&audio->claim, 0, sizeof(audio->claim));
  memset(&audio->response, 0, sizeof(audio->response));
  memset(&audio->remoteStatus, 0, sizeof(audio->remoteStatus));
  memset(&audio->stateAck, 0, sizeof(audio->stateAck));
  memset(&audio->subscription, 0, sizeof(audio->subscription));
  LG_UNLOCK(audio->lock);
  waitStatusCallbacks(audio);
}

static bool newClaimNL(LGMPAudio * audio)
{
  uint64_t token = 0;
  if (!lgRandomBytes(&token, sizeof(token)))
  {
    token = microtime() ^ ((uint64_t)audio->clientID << 32) ^
      audio->requestSerial;
  }
  if (!token)
    token = 1;

  audio->requestGeneration = nextNonzero(&audio->requestSerial);
  audio->claimToken         = token;
  audio->claim = (KVMFRAudioClaim)
  {
    .messageType        = KVMFR_MESSAGE_AUDIO_CLAIM,
    .version            = KVMFR_AUDIO_CONTROL_VERSION,
    .size               = sizeof(KVMFRAudioClaim),
    .requestGeneration  = audio->requestGeneration,
    .flags              = KVMFR_AUDIO_CLAIM_BIND,
    .claimToken         = audio->claimToken,
    .protocolMin        = KVMFR_AUDIO_PROTOCOL_MIN,
    .protocolMax        = KVMFR_AUDIO_PROTOCOL_MAX,
    .clientCapabilities = AUDIO_CAPABILITIES,
    .subscriptions      = AUDIO_SUBSCRIPTIONS,
  };
  audio->claimSends  = 0;
  audio->claimPosting = false;
  audio->phase       = AUDIO_PHASE_CLAIM_PENDING;
  audio->nextClaimAt = 0;
  return true;
}

void lgmpAudio_pumpClaim(LGMPAudio * audio,
    PLGMPClientQueue pointerQueue)
{
  if (!audio || !pointerQueue)
    return;

  KVMFRAudioClaim claim;
  bool send = false;
  const uint64_t now = microtime();
  LG_LOCK(audio->lock);
  if (audio->connected && audio->discover &&
      audio->phase == AUDIO_PHASE_IDLE &&
      now >= audio->nextClaimAt)
    newClaimNL(audio);

  if (audio->connected && !audio->releaseRequested &&
      (audio->phase == AUDIO_PHASE_CLAIM_PENDING ||
       audio->phase == AUDIO_PHASE_CLAIM_SENT) &&
      now >= audio->nextClaimAt)
  {
    if (audio->claimSends >= 3)
    {
      audio->phase = AUDIO_PHASE_COOLDOWN;
      audio->releaseDeadline = now + AUDIO_OWNER_RETRY_US;
    }
    else
    {
      claim = audio->claim;
      send  = true;
      audio->claimPosting = true;
    }
  }
  LG_UNLOCK(audio->lock);
  if (!send)
    return;

  uint32_t serial = 0;
  const LGMP_STATUS status = lgmpClientSendData(pointerQueue,
      &claim, sizeof(claim), &serial);
  LG_LOCK(audio->lock);
  if (claim.requestGeneration == audio->requestGeneration &&
      claim.claimToken == audio->claimToken)
  {
    audio->claimPosting = false;
    if (audio->connected)
    {
      if (status == LGMP_OK)
      {
        audio->phase = AUDIO_PHASE_CLAIM_SENT;
        ++audio->claimSends;
        audio->nextClaimAt = now + AUDIO_CLAIM_RETRY_US;
      }
      else if (status == LGMP_ERR_QUEUE_FULL ||
          status == LGMP_ERR_QUEUE_BUSY)
        audio->nextClaimAt = now + 1000U;
      else
      {
        audio->nextClaimAt = now + AUDIO_CLAIM_RETRY_US;
        DEBUG_WARN("Failed to send LGMP audio claim: %s",
            lgmpStatusString(status));
      }
    }
  }
  LG_UNLOCK(audio->lock);
}

void lgmpAudio_handleResponse(LGMPAudio * audio,
    const KVMFRAudioStreamResponse * response)
{
  if (!audio || !response)
    return;
  LG_LOCK(audio->lock);
  if (audio->connected &&
      response->requestGeneration == audio->requestGeneration &&
      response->claimToken == audio->claimToken &&
      response->boundClientID == audio->clientID &&
      (audio->phase == AUDIO_PHASE_CLAIM_PENDING ||
       audio->phase == AUDIO_PHASE_CLAIM_SENT ||
       audio->phase == AUDIO_PHASE_COOLDOWN) &&
      !audio->responsePending)
  {
    audio->response        = *response;
    audio->responsePending = true;
  }
  LG_UNLOCK(audio->lock);
  signalWorker(audio);
}

static void setStatusListener(void * opaque,
    LG_AudioStatusFn callback, void * callbackOpaque)
{
  LGMPAudio * audio = opaque;
  if (!callback)
    lockWriteGates(audio);
  LG_LOCK(audio->lock);
  if (!callback)
    closePublicDataLocked(audio);
  audio->statusCallback = callback;
  audio->statusOpaque   = callbackOpaque;
  audio->discover       = callback && audio->connected;
  if (audio->discover && audio->phase == AUDIO_PHASE_IDLE)
  {
    audio->releaseRequested = false;
    audio->nextClaimAt = 0;
  }
  if (!callback)
  {
    const bool ownsOrMayOwn =
      audio->phase != AUDIO_PHASE_IDLE;
    audio->releaseRequested = ownsOrMayOwn;
    audio->playbackReadActive = false;
    audio->stateAcknowledged = false;
    audio->stateAckPending = false;
    audio->subscriptionPending = false;
    if (ownsOrMayOwn)
    {
      audio->releaseDeadline =
        microtime() + AUDIO_OWNER_RETRY_US;
      if (audio->phase == AUDIO_PHASE_BOUND)
        audio->phase = AUDIO_PHASE_RELEASING;
    }
    markStatusNL(audio, false, 0);
  }
  const LG_AudioStatus status =
  {
    .available  = audio->connected && audio->statusAvailable,
    .generation = audio->endpointGeneration.local,
  };
  struct AudioStatusCallbackFrame frame;
  if (callback)
    beginStatusCallback(audio, &frame);
  audio->statusDirty = false;
  LG_UNLOCK(audio->lock);
  if (!callback)
    unlockWriteGates(audio);
  if (callback)
  {
    callback(callbackOpaque, &status);
    endStatusCallback(audio, &frame);
  }
  else
    waitStatusCallbacks(audio);
  if (callback && audio->pointerWake)
    lgSignalEvent(audio->pointerWake);
  signalWorker(audio);
}

static bool attach(void * opaque, const LG_AudioEventOps * events,
    void * eventOpaque)
{
  LGMPAudio * audio = opaque;
  if (!events)
    return false;

  struct AudioStreamState playback;
  struct AudioStreamState capture;
  bool admitted;
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  admitted = audio->connected && audio->statusAvailable &&
    audio->phase == AUDIO_PHASE_BOUND && !audio->releaseRequested &&
    !audio->attached;
  if (admitted)
  {
    audio->events      = events;
    audio->eventOpaque = eventOpaque;
    audio->attached    = true;
  }
  playback = audio->playback;
  capture  = audio->capture;
  LG_UNLOCK(audio->lock);

  if (admitted && playback.active && events->playbackStart)
  {
    events->playbackStart(eventOpaque, playback.generation.local,
        &playback.format,
        playback.clockValid ? &playback.clock : NULL);
    if (playback.volumeValid && events->playbackVolume)
      events->playbackVolume(eventOpaque, playback.generation.local,
          playback.volumeChannels, playback.volume);
    if (playback.muteValid && events->playbackMute)
      events->playbackMute(eventOpaque, playback.generation.local,
          playback.mute);
  }
  if (admitted && capture.active && events->recordStart)
  {
    events->recordStart(eventOpaque, capture.generation.local,
        &capture.format);
    if (capture.volumeValid && events->recordVolume)
      events->recordVolume(eventOpaque, capture.generation.local,
          capture.volumeChannels, capture.volume);
    if (capture.muteValid && events->recordMute)
      events->recordMute(eventOpaque, capture.generation.local,
          capture.mute);
  }
  LG_UNLOCK(audio->eventLock);
  if (admitted)
    signalWorker(audio);
  return admitted;
}

static void detach(void * opaque)
{
  LGMPAudio * audio = opaque;
  LG_LOCK(audio->eventLock);
  LG_LOCK(audio->lock);
  audio->attached    = false;
  audio->events      = NULL;
  audio->eventOpaque = NULL;
  LG_UNLOCK(audio->lock);
  LG_UNLOCK(audio->eventLock);
}

static bool rateToQ32(double rate, bool allowZero, uint64_t * result)
{
  if (!isfinite(rate) || rate < 0.0 || (!allowZero && rate == 0.0) ||
      rate > (double)KVMFR_AUDIO_MAX_SAMPLE_RATE * 2.0)
    return false;
  const long double scaled = (long double)rate * 4294967296.0L;
  if (scaled > (long double)UINT64_MAX)
    return false;
  *result = (uint64_t)(scaled + 0.5L);
  return allowZero || *result != 0;
}

static bool recordData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * sourceClock)
{
  LGMPAudio * audio = opaque;
  if (!generation || !data || !frames ||
      frames > KVMFR_AUDIO_MAX_FRAMES_PER_PACKET)
    return false;
  lockCaptureWriteGate(audio);

  if (!audio->publicDataAllowed || !audio->captureWriteBound ||
      !(audio->writeGrantedCapabilities & KVMFR_AUDIO_CAP_CAPTURE) ||
      !audio->writeBindingEpoch || !audio->writeRoleEpoch ||
      generation != audio->writeCaptureLocalGeneration ||
      !audio->writeCaptureGeneration)
  {
    unlockCaptureWriteGate(audio);
    return false;
  }
  const size_t bytes = frames * audio->writeCaptureFormat.blockAlign;
  if (!audio->writeCaptureFormat.blockAlign ||
      bytes > KVMFR_AUDIO_PCM_BYTES)
  {
    unlockCaptureWriteGate(audio);
    return false;
  }

  uint32_t flags = 0;
  uint64_t position = 0;
  int64_t timeNs = 0;
  uint64_t rateQ32 = 0;
  if (sourceClock)
  {
    if (!rateToQ32(sourceClock->rate, true, &rateQ32))
    {
      unlockCaptureWriteGate(audio);
      return false;
    }
    flags    |= KVMFR_AUDIO_MESSAGE_CLOCK_VALID;
    position  = sourceClock->position;
    timeNs    = sourceClock->time;
    if (sourceClock->stable)
      flags |= KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
    if (sourceClock->discontinuity)
      flags |= KVMFR_AUDIO_MESSAGE_DISCONTINUITY;
  }
  if (atomic_exchange_explicit(
        &audio->captureDiscontinuity, false, memory_order_acq_rel))
    flags |= KVMFR_AUDIO_MESSAGE_DISCONTINUITY;

  enum WriteResult result = writeMessageLocked(audio,
      audio->captureStream, &audio->captureSequence,
      KVMFR_AUDIO_MESSAGE_CAPTURE_DATA,
      audio->writeCaptureGeneration, flags, position, timeNs, rateQ32,
      (uint32_t)frames, data, (uint32_t)bytes);
  if (result == WRITE_FULL)
  {
    LGMPStreamPollState streamPoll;
    const LGMP_STATUS pollStatus = lgmpStreamPollInit(&streamPoll,
        (struct LGMPStreamPollConfig)
        {
          .spinCount = 64U,
          .minWaitUs = AUDIO_ACTIVE_POLL_US,
          .maxWaitUs = AUDIO_CAPTURE_WRITE_MAX_WAIT_US,
        });
    DEBUG_ASSERT(pollStatus == LGMP_OK);

    const uint64_t deadline =
      microtime() + AUDIO_CAPTURE_WRITE_TIMEOUT_US;
    while (result == WRITE_FULL && pollStatus == LGMP_OK)
    {
      const uint64_t now = microtime();
      if (now >= deadline)
        break;

      uint32_t waitUs = lgmpStreamPollIdle(&streamPoll);
      const uint64_t remainingUs = deadline - now;
      if (waitUs > remainingUs)
        waitUs = (uint32_t)remainingUs;
      if (waitUs)
        nsleep((uint64_t)waitUs * 1000U);

      result = writeMessageLocked(audio,
          audio->captureStream, &audio->captureSequence,
          KVMFR_AUDIO_MESSAGE_CAPTURE_DATA,
          audio->writeCaptureGeneration, flags, position, timeNs, rateQ32,
          (uint32_t)frames, data, (uint32_t)bytes);
    }
  }
  if (result != WRITE_OK)
  {
    if (result == WRITE_FULL)
      ++audio->captureSequence;
    atomic_store_explicit(
        &audio->captureDiscontinuity, true, memory_order_release);
  }
  unlockCaptureWriteGate(audio);
  return result == WRITE_OK;
}

static bool clockFeedback(void * opaque, uint32_t generation,
    const LG_AudioClock * playbackClock, double targetRate)
{
  LGMPAudio * audio = opaque;
  uint64_t targetRateQ32;
  uint64_t rateQ32;
  if (!generation || !playbackClock ||
      !rateToQ32(targetRate, false, &targetRateQ32) ||
      !rateToQ32(playbackClock->rate, true, &rateQ32))
    return false;

  if (!tryControlWriteGate(audio))
    return false;
  LG_LOCK(audio->lock);
  const bool supported = audio->phase == AUDIO_PHASE_BOUND &&
    !audio->releaseRequested && audio->statusAvailable &&
    audio->stateAcknowledged &&
    audio->stateAck.bindingEpoch == audio->bindingEpoch &&
    audio->stateAck.roleEpoch == audio->roleEpoch &&
    audio->stateAckGeneration ==
      audio->remoteStatus.playbackGeneration &&
    (audio->remoteStatus.grantedCapabilities &
      KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) != 0 &&
    audio->remoteStatus.feedbackAuthorityClientID == audio->clientID &&
    audio->remoteStatus.feedbackAuthorityEpoch &&
    audio->remoteStatus.bindingEpoch == audio->bindingEpoch &&
    audio->remoteStatus.roleEpoch == audio->roleEpoch &&
    audio->playback.active &&
    audio->playback.generation.local == generation;
  LG_UNLOCK(audio->lock);
  if (!supported)
  {
    unlockControlWriteGate(audio);
    return false;
  }
  if (!audio->publicDataAllowed || !audio->controlWriteBound ||
      !(audio->writeGrantedCapabilities &
        KVMFR_AUDIO_CAP_CLOCK_FEEDBACK) ||
      audio->writeBindingEpoch != audio->bindingEpoch ||
      audio->writeRoleEpoch != audio->roleEpoch ||
      generation != audio->writePlaybackLocalGeneration ||
      !audio->writePlaybackGeneration)
  {
    unlockControlWriteGate(audio);
    return false;
  }

  uint32_t flags = KVMFR_AUDIO_MESSAGE_CLOCK_VALID;
  if (playbackClock->stable)
    flags |= KVMFR_AUDIO_MESSAGE_CLOCK_STABLE;
  if (playbackClock->discontinuity)
    flags |= KVMFR_AUDIO_MESSAGE_DISCONTINUITY;
  const KVMFRAudioClockFeedback feedback =
  {
    .targetRateQ32 = targetRateQ32,
    .bindingEpoch  = audio->writeBindingEpoch,
    .roleEpoch     = audio->writeRoleEpoch,
  };
  const enum WriteResult result = writeMessageLocked(audio,
      audio->clientControlStream, &audio->clientControlSequence,
      KVMFR_AUDIO_MESSAGE_CLOCK_FEEDBACK,
      audio->writePlaybackGeneration, flags,
      playbackClock->position, playbackClock->time, rateQ32, 0,
      &feedback, sizeof(feedback));
  unlockControlWriteGate(audio);
  return result == WRITE_OK;
}

static bool clockFeedbackAvailable(void * opaque)
{
  LGMPAudio * audio = opaque;
  return atomic_load_explicit(
      &audio->clockFeedbackAvailable, memory_order_acquire);
}

static const LG_AudioOps l_audioOps =
{
  .name                     = "LGMP",
  .playbackArrivalReserveUs = AUDIO_ACTIVE_POLL_US,
  .setStatusListener        = setStatusListener,
  .attach                   = attach,
  .detach                   = detach,
  .recordData               = recordData,
  .clockFeedback            = clockFeedback,
  .clockFeedbackAvailable   = clockFeedbackAvailable,
};

const LG_AudioOps * lgmpAudio_getOps(void)
{
  return &l_audioOps;
}
