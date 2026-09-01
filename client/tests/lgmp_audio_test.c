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
#include <LGProtocol/LGMPConfig.h>
#include "common/debug.h"
#include "common/event.h"
#include "common/time.h"

#include <lgmp/client.h>
#include <lgmp/host.h>
#include <lgmp/stream.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RAM_SIZE     (8U * 1024U * 1024U)
#define WAIT_MS      3000U
#define MAX_MEMORIES 64U
#define MAX_EVENTS   256U

#define CHECK(condition) \
  do \
  { \
    if (!(condition)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", \
          __FILE__, __LINE__, #condition); \
      return false; \
    } \
  } \
  while (0)

enum TraceKind
{
  TRACE_PLAY_START,
  TRACE_PLAY_STOP,
  TRACE_PLAY_DATA,
  TRACE_REC_START,
  TRACE_REC_STOP,
};

struct TraceEvent
{
  enum TraceKind kind;
  uint32_t generation;
};

struct Trace
{
  pthread_mutex_t mutex;
  atomic_uint statusCount;
  atomic_uint playStartCount;
  atomic_uint playStopCount;
  atomic_uint playDataCount;
  atomic_uint recStartCount;
  atomic_uint recStopCount;
  LG_AudioStatus status;
  LG_AudioFormat playFormat;
  LG_AudioFormat recFormat;
  LG_AudioClock playClock;
  bool playClockValid;
  uint32_t playGeneration;
  uint32_t recGeneration;
  uint8_t playData[128];
  size_t playFrames;
  struct TraceEvent events[MAX_EVENTS];
  unsigned eventCount;
};

static void traceEvent(struct Trace * trace, enum TraceKind kind,
    uint32_t generation)
{
  pthread_mutex_lock(&trace->mutex);
  if (trace->eventCount < MAX_EVENTS)
    trace->events[trace->eventCount++] =
      (struct TraceEvent){ kind, generation };
  pthread_mutex_unlock(&trace->mutex);
}

static void statusCallback(void * opaque, const LG_AudioStatus * status)
{
  struct Trace * trace = opaque;
  pthread_mutex_lock(&trace->mutex);
  trace->status = *status;
  pthread_mutex_unlock(&trace->mutex);
  atomic_fetch_add_explicit(&trace->statusCount, 1, memory_order_release);
}

static void playbackStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format, const LG_AudioClock * clock)
{
  struct Trace * trace = opaque;
  pthread_mutex_lock(&trace->mutex);
  trace->playGeneration = generation;
  trace->playFormat = *format;
  trace->playClockValid = clock != NULL;
  if (clock)
    trace->playClock = *clock;
  pthread_mutex_unlock(&trace->mutex);
  traceEvent(trace, TRACE_PLAY_START, generation);
  atomic_fetch_add_explicit(&trace->playStartCount, 1,
      memory_order_release);
}

static void playbackStop(void * opaque, uint32_t generation)
{
  struct Trace * trace = opaque;
  traceEvent(trace, TRACE_PLAY_STOP, generation);
  atomic_fetch_add_explicit(&trace->playStopCount, 1,
      memory_order_release);
}

static void playbackData(void * opaque, uint32_t generation,
    const void * data, size_t frames, const LG_AudioClock * clock)
{
  struct Trace * trace = opaque;
  pthread_mutex_lock(&trace->mutex);
  trace->playGeneration = generation;
  trace->playFrames = frames;
  const size_t bytes = frames * trace->playFormat.channelCount * 3U;
  const size_t copy = bytes < sizeof(trace->playData) ?
    bytes : sizeof(trace->playData);
  memset(trace->playData, 0, sizeof(trace->playData));
  memcpy(trace->playData, data, copy);
  trace->playClockValid = clock != NULL;
  if (clock)
    trace->playClock = *clock;
  pthread_mutex_unlock(&trace->mutex);
  traceEvent(trace, TRACE_PLAY_DATA, generation);
  atomic_fetch_add_explicit(&trace->playDataCount, 1,
      memory_order_release);
}

static void recordStart(void * opaque, uint32_t generation,
    const LG_AudioFormat * format)
{
  struct Trace * trace = opaque;
  pthread_mutex_lock(&trace->mutex);
  trace->recGeneration = generation;
  trace->recFormat = *format;
  pthread_mutex_unlock(&trace->mutex);
  traceEvent(trace, TRACE_REC_START, generation);
  atomic_fetch_add_explicit(&trace->recStartCount, 1,
      memory_order_release);
}

static void recordStop(void * opaque, uint32_t generation)
{
  struct Trace * trace = opaque;
  traceEvent(trace, TRACE_REC_STOP, generation);
  atomic_fetch_add_explicit(&trace->recStopCount, 1,
      memory_order_release);
}

static const LG_AudioEventOps eventOps =
{
  .playbackStart = playbackStart,
  .playbackStop = playbackStop,
  .playbackData = playbackData,
  .recordStart = recordStart,
  .recordStop = recordStop,
};

struct Fixture
{
  void * ram;
  PLGMPHost host;
  PLGMPHostQueue pointerHost;
  PLGMPClient client;
  PLGMPClientQueue pointerClient;
  uint32_t clientID;
  LGEvent * pointerWake;
  LGMPAudio * audio;
  const LG_AudioOps * ops;
  PLGMPHostStream h2c;
  PLGMPHostStream c2h;
  PLGMPMemory memories[MAX_MEMORIES];
  unsigned memoryCount;
  KVMFRAudioClaim claim;
  uint64_t hostSequence;
  uint64_t endpointGeneration;
  struct Trace trace;
};

struct SelfUnregister
{
  LGMPAudio * audio;
  const LG_AudioOps * ops;
  atomic_uint count;
};

struct RecordStress
{
  LGMPAudio * audio;
  const LG_AudioOps * ops;
  uint32_t generation;
  atomic_bool stop;
  atomic_uint successes;
};

struct DisconnectTask
{
  LGMPAudio * audio;
  atomic_bool done;
};

static void * recordStress(void * opaque)
{
  struct RecordStress * stress = opaque;
  const uint8_t pcm[18] = { 0 };
  while (!atomic_load_explicit(&stress->stop, memory_order_acquire))
    if (stress->ops->recordData(stress->audio,
          stress->generation, pcm, 6, NULL))
      atomic_fetch_add_explicit(
          &stress->successes, 1, memory_order_release);
  return NULL;
}

static void * removeStatusListener(void * opaque)
{
  struct RecordStress * stress = opaque;
  stress->ops->setStatusListener(stress->audio, NULL, NULL);
  return NULL;
}

static void * disconnectAudio(void * opaque)
{
  struct DisconnectTask * task = opaque;
  lgmpAudio_disconnect(task->audio);
  atomic_store_explicit(&task->done, true, memory_order_release);
  return NULL;
}

static void selfUnregisterStatus(void * opaque,
    const LG_AudioStatus * status)
{
  struct SelfUnregister * state = opaque;
  (void)status;
  atomic_fetch_add_explicit(&state->count, 1, memory_order_release);
  state->ops->setStatusListener(state->audio, NULL, NULL);
}

static bool waitCount(struct Fixture * fixture, atomic_uint * value,
    unsigned expected, unsigned timeoutMs)
{
  const uint64_t deadline = microtime() + (uint64_t)timeoutMs * 1000U;
  while (microtime() < deadline)
  {
    lgmpHostProcess(fixture->host);
    if (atomic_load_explicit(value, memory_order_acquire) >= expected)
      return true;
    struct timespec delay = { .tv_nsec = 1000000L };
    nanosleep(&delay, NULL);
  }
  return atomic_load_explicit(value, memory_order_acquire) >= expected;
}

static bool waitDone(struct Fixture * fixture, atomic_bool * done,
    unsigned timeoutMs)
{
  const uint64_t deadline = microtime() + (uint64_t)timeoutMs * 1000U;
  while (microtime() < deadline)
  {
    lgmpHostProcess(fixture->host);
    if (atomic_load_explicit(done, memory_order_acquire))
      return true;
    const struct timespec delay = { .tv_nsec = 1000000L };
    nanosleep(&delay, NULL);
  }
  return atomic_load_explicit(done, memory_order_acquire);
}

static bool fixtureInit(struct Fixture * fixture)
{
  memset(fixture, 0, sizeof(*fixture));
  CHECK(!posix_memalign(&fixture->ram, 64, RAM_SIZE));
  memset(fixture->ram, 0, RAM_SIZE);
  CHECK(!pthread_mutex_init(&fixture->trace.mutex, NULL));

  KVMFR session = { 0 };
  memcpy(session.magic, KVMFR_MAGIC, sizeof(session.magic));
  session.version  = KVMFR_VERSION;
  session.features = KVMFR_FEATURE_AUDIO;
  CHECK(lgmpHostInit(fixture->ram, RAM_SIZE, &fixture->host,
        sizeof(session), (uint8_t *)&session) == LGMP_OK);
  const struct LGMPQueueConfig queueConfig =
  {
    .queueID = LGMP_Q_POINTER,
    .numMessages = LGMP_Q_POINTER_LEN,
    .subTimeout = 1000,
  };
  CHECK(lgmpHostQueueNew(fixture->host, queueConfig,
        &fixture->pointerHost) == LGMP_OK);
  CHECK(lgmpClientInit(fixture->ram, RAM_SIZE, &fixture->client) == LGMP_OK);
  struct timespec heartbeatDelay = { .tv_nsec = 300000000L };
  nanosleep(&heartbeatDelay, NULL);
  CHECK(lgmpHostProcess(fixture->host) == LGMP_OK);
  uint32_t udataSize = 0;
  uint8_t * udata = NULL;
  uint32_t remoteVersion = 0;
  CHECK(lgmpClientSessionInit(fixture->client, &udataSize, &udata,
        &fixture->clientID, &remoteVersion) == LGMP_OK);
  CHECK(fixture->clientID != 0);
  CHECK(lgmpClientSubscribe(fixture->client, LGMP_Q_POINTER,
        &fixture->pointerClient) == LGMP_OK);
  CHECK(lgmpHostProcess(fixture->host) == LGMP_OK);
  fixture->pointerWake = lgCreateEvent(true, 0);
  CHECK(fixture->pointerWake);
  CHECK(lgmpAudio_create(fixture->client, fixture->pointerWake,
        &fixture->audio));
  CHECK(lgmpAudio_connect(fixture->audio, fixture->clientID));
  fixture->ops = lgmpAudio_getOps();
  CHECK(fixture->ops);
  CHECK(fixture->ops->playbackFlags == 0);
  CHECK(fixture->ops->playbackArrivalReserveUs == 125);
  CHECK(fixture->ops->clockFeedbackAvailable);
  CHECK(!fixture->ops->clockFeedbackAvailable(fixture->audio));
  fixture->endpointGeneration = UINT64_C(0x100000001);
  return true;
}

static void fixtureCleanup(struct Fixture * fixture)
{
  if (fixture->audio)
    lgmpAudio_destroy(&fixture->audio);
  if (fixture->pointerWake)
    lgFreeEvent(fixture->pointerWake);
  if (fixture->pointerClient)
    lgmpClientUnsubscribe(&fixture->pointerClient);
  if (fixture->client)
    lgmpClientFree(&fixture->client);
  if (fixture->h2c)
    lgmpHostStreamFree(&fixture->h2c);
  if (fixture->c2h)
    lgmpHostStreamFree(&fixture->c2h);
  for (unsigned i = 0; i < fixture->memoryCount; ++i)
    lgmpHostMemFree(&fixture->memories[i]);
  if (fixture->host)
    lgmpHostFree(&fixture->host);
  pthread_mutex_destroy(&fixture->trace.mutex);
  free(fixture->ram);
}

static bool allocateMemory(struct Fixture * fixture, size_t size,
    PLGMPMemory * result)
{
  CHECK(fixture->memoryCount < MAX_MEMORIES);
  CHECK(size <= UINT32_MAX);
  CHECK(lgmpHostMemAlloc(fixture->host, (uint32_t)size, result) == LGMP_OK);
  fixture->memories[fixture->memoryCount++] = *result;
  return true;
}

static bool readClaim(struct Fixture * fixture, KVMFRAudioClaim * claim,
    unsigned timeoutMs)
{
  const uint64_t deadline = microtime() + (uint64_t)timeoutMs * 1000U;
  while (microtime() < deadline)
  {
    size_t size = sizeof(*claim);
    uint32_t clientID = 0;
    const LGMP_STATUS status = lgmpHostReadDataWithSource(
        fixture->pointerHost, claim, &size, &clientID);
    if (status == LGMP_OK)
    {
      CHECK(size == sizeof(*claim));
      CHECK(clientID == fixture->clientID);
      CHECK(lgmpHostAckData(fixture->pointerHost) == LGMP_OK);
      return true;
    }
    CHECK(status == LGMP_ERR_QUEUE_EMPTY);
    lgmpHostProcess(fixture->host);
    struct timespec delay = { .tv_nsec = 1000000L };
    nanosleep(&delay, NULL);
  }
  return false;
}

static bool createStreams(struct Fixture * fixture)
{
  const struct LGMPStreamConfig h2cConfig =
  {
    .direction = LGMP_STREAM_HOST_TO_CLIENT,
    .policy = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = KVMFR_AUDIO_STREAM_SLOT_COUNT,
    .slotSize = KVMFR_AUDIO_STREAM_SLOT_BYTES,
  };
  const struct LGMPStreamConfig c2hConfig =
  {
    .direction = LGMP_STREAM_CLIENT_TO_HOST,
    .policy = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = KVMFR_AUDIO_STREAM_SLOT_COUNT,
    .slotSize = KVMFR_AUDIO_STREAM_SLOT_BYTES,
  };
  CHECK(lgmpHostStreamNew(fixture->host, h2cConfig,
        &fixture->h2c) == LGMP_OK);
  CHECK(lgmpHostStreamNew(fixture->host, c2hConfig,
        &fixture->c2h) == LGMP_OK);
  return true;
}

static void copyDescriptor(KVMFRStreamDescriptor * output,
    PLGMPHostStream stream)
{
  struct LGMPStreamDescriptor descriptor;
  lgmpHostStreamGetDescriptor(stream, &descriptor);
  memcpy(output, &descriptor, sizeof(*output));
}

static bool bindStreams(struct Fixture * fixture)
{
  uint32_t epoch = 0;
  CHECK(lgmpHostStreamBind(fixture->h2c,
        fixture->clientID, &epoch) == LGMP_OK);
  CHECK(epoch != 0);
  CHECK(lgmpHostStreamBind(fixture->c2h,
        fixture->clientID, &epoch) == LGMP_OK);
  CHECK(epoch != 0);
  return true;
}

static bool postTagged(struct Fixture * fixture, uint64_t udata,
    const void * data, size_t size, LGMPMessage * clientMessage)
{
  PLGMPMemory memory;
  CHECK(allocateMemory(fixture, size, &memory));
  memcpy(lgmpHostMemPtr(memory), data, size);
  unsigned recipients = 0;
  CHECK(lgmpHostQueuePostForClients(fixture->pointerHost, udata, memory,
        &fixture->clientID, 1, &recipients) == LGMP_OK);
  CHECK(recipients == 1);
  const uint64_t deadline = microtime() + WAIT_MS * 1000U;
  LGMP_STATUS status;
  do
  {
    status = lgmpClientProcess(fixture->pointerClient, clientMessage);
    if (status == LGMP_OK)
      return true;
    CHECK(status == LGMP_ERR_QUEUE_EMPTY);
    lgmpHostProcess(fixture->host);
  }
  while (microtime() < deadline);
  return false;
}

static bool deliverBoundResponse(struct Fixture * fixture,
    const KVMFRAudioClaim * claim, bool correct)
{
  KVMFRAudioStreamResponse response =
  {
    .magic = KVMFR_AUDIO_RESPONSE_MAGIC,
    .version = KVMFR_AUDIO_CONTROL_VERSION,
    .size = sizeof(response),
    .requestGeneration = claim->requestGeneration,
    .boundClientID = fixture->clientID,
    .result = KVMFR_AUDIO_RESPONSE_BOUND,
    .protocolVersion = KVMFR_AUDIO_PROTOCOL_MAX,
    .endpointGeneration = fixture->endpointGeneration,
    .claimToken = correct ? claim->claimToken : claim->claimToken + 1,
  };
  copyDescriptor(&response.hostToClient, fixture->h2c);
  copyDescriptor(&response.clientToHost, fixture->c2h);

  LGMPMessage message;
  CHECK(postTagged(fixture, KVMFR_AUDIO_CONTROL_UDATA,
        &response, sizeof(response), &message));
  CHECK((uint32_t)message.udata == 0);
  CHECK((uint32_t)(message.udata >> 32) == KVMFR_AUDIO_CONTROL_TAG);
  CHECK(message.size == sizeof(response));
  KVMFRAudioStreamResponse copy;
  memcpy(&copy, message.mem, sizeof(copy));
  CHECK(lgmpClientMessageDone(fixture->pointerClient) == LGMP_OK);
  lgmpAudio_handleResponse(fixture->audio, &copy);
  return true;
}

static KVMFRAudioFormat makeFormat(uint32_t channelMask,
    KVMFRAudioSampleFormat sampleFormat)
{
  unsigned channels = 0;
  for (uint32_t mask = channelMask; mask; mask >>= 1)
    channels += mask & 1U;
  const unsigned bytes =
    sampleFormat == KVMFR_AUDIO_SAMPLE_S24_LE ? 3U : 2U;
  return (KVMFRAudioFormat)
  {
    .version = KVMFR_AUDIO_FORMAT_VERSION,
    .size = sizeof(KVMFRAudioFormat),
    .sampleFormat = sampleFormat,
    .sampleRate = 192000,
    .channelMask = channelMask,
    .channelCount = (uint16_t)channels,
    .validBits = (uint16_t)(bytes * 8U),
    .containerBits = (uint16_t)(bytes * 8U),
    .blockAlign = (uint16_t)(channels * bytes),
  };
}

static bool writeH2C(struct Fixture * fixture, uint32_t type,
    uint64_t generation, uint32_t flags, uint64_t position,
    int64_t timeNs, uint64_t rateQ32, uint32_t frames,
    const void * payload, uint32_t length, uint64_t sequence)
{
  LGMPStreamBuffer buffer = { 0 };
  CHECK(lgmpHostStreamWriteAcquire(fixture->h2c, &buffer) == LGMP_OK);
  CHECK(buffer.capacity >= sizeof(KVMFRAudioMessage) + length);
  const KVMFRAudioMessage message =
  {
    .version = KVMFR_AUDIO_MESSAGE_VERSION,
    .size = sizeof(KVMFRAudioMessage),
    .type = type,
    .streamGeneration = generation,
    .flags = flags,
    .sequence = sequence,
    .position = position,
    .timeNs = timeNs,
    .rateQ32 = rateQ32,
    .frames = frames,
    .length = length,
  };
  memcpy(buffer.data, &message, sizeof(message));
  if (length)
    memcpy((uint8_t *)buffer.data + sizeof(message), payload, length);
  CHECK(lgmpHostStreamWriteCommit(fixture->h2c, &buffer,
        (uint32_t)sizeof(message) + length) == LGMP_OK);
  return true;
}

static bool sendH2C(struct Fixture * fixture, uint32_t type,
    uint64_t generation, uint32_t flags, uint64_t position,
    int64_t timeNs, uint64_t rateQ32, uint32_t frames,
    const void * payload, uint32_t length)
{
  return writeH2C(fixture, type, generation, flags, position, timeNs,
      rateQ32, frames, payload, length, fixture->hostSequence++);
}

static bool sendStatus(struct Fixture * fixture, bool available,
    uint32_t capabilities,
    uint64_t playGeneration, const KVMFRAudioFormat * playFormat,
    uint64_t captureGeneration, const KVMFRAudioFormat * captureFormat)
{
  uint32_t flags = KVMFR_AUDIO_STATUS_HAS_OWNER;
  if (capabilities & KVMFR_AUDIO_CAP_PLAYBACK)
    flags |= KVMFR_AUDIO_STATUS_PLAYBACK_AVAILABLE;
  if (capabilities & KVMFR_AUDIO_CAP_CAPTURE)
    flags |= KVMFR_AUDIO_STATUS_CAPTURE_AVAILABLE;
  if (available)
    flags |= KVMFR_AUDIO_STATUS_AVAILABLE;
  if (playGeneration)
    flags |= KVMFR_AUDIO_STATUS_PLAYBACK_ACTIVE;
  if (captureGeneration)
    flags |= KVMFR_AUDIO_STATUS_CAPTURE_ACTIVE;
  KVMFRAudioStatus status =
  {
    .version = KVMFR_AUDIO_STATUS_VERSION,
    .size = sizeof(KVMFRAudioStatus),
    .flags = flags,
    .protocolVersion = KVMFR_AUDIO_PROTOCOL_MAX,
    .requestGeneration = fixture->claim.requestGeneration,
    .capabilities = capabilities,
    .ownerClientID = fixture->clientID,
    .ownerGeneration = fixture->claim.requestGeneration,
    .endpointGeneration = fixture->endpointGeneration,
    .playbackGeneration = playGeneration,
    .captureGeneration = captureGeneration,
    .playbackState = playGeneration ?
      KVMFR_AUDIO_STREAM_RUNNING : KVMFR_AUDIO_STREAM_STOPPED,
    .captureState = captureGeneration ?
      KVMFR_AUDIO_STREAM_RUNNING : KVMFR_AUDIO_STREAM_STOPPED,
  };
  if (playFormat)
    status.playbackFormat = *playFormat;
  if (captureFormat)
    status.captureFormat = *captureFormat;
  return sendH2C(fixture, KVMFR_AUDIO_MESSAGE_STATUS, 0, 0, 0, 0, 0,
      0, &status, sizeof(status));
}

static bool readC2H(struct Fixture * fixture, KVMFRAudioMessage * message,
    uint8_t payload[KVMFR_AUDIO_PCM_BYTES], unsigned timeoutMs)
{
  const uint64_t deadline = microtime() + (uint64_t)timeoutMs * 1000U;
  while (microtime() < deadline)
  {
    LGMPStreamBuffer buffer = { 0 };
    const LGMP_STATUS status =
      lgmpHostStreamReadPeek(fixture->c2h, &buffer);
    if (status == LGMP_OK)
    {
      CHECK(buffer.size >= sizeof(*message));
      memcpy(message, buffer.data, sizeof(*message));
      CHECK(message->length <= KVMFR_AUDIO_PCM_BYTES);
      CHECK(buffer.size == sizeof(*message) + message->length);
      if (message->length)
        memcpy(payload, (uint8_t *)buffer.data + sizeof(*message),
            message->length);
      CHECK(lgmpHostStreamReadRelease(fixture->c2h, &buffer) == LGMP_OK);
      return true;
    }
    CHECK(status == LGMP_ERR_STREAM_EMPTY);
    lgmpHostProcess(fixture->host);
    struct timespec delay = { .tv_nsec = 1000000L };
    nanosleep(&delay, NULL);
  }
  return false;
}

static bool readC2HType(struct Fixture * fixture, uint32_t type,
    KVMFRAudioMessage * message, uint8_t payload[KVMFR_AUDIO_PCM_BYTES],
    unsigned timeoutMs)
{
  const uint64_t deadline = microtime() + (uint64_t)timeoutMs * 1000U;
  while (microtime() < deadline)
  {
    const unsigned left = (unsigned)((deadline - microtime()) / 1000U + 1);
    CHECK(readC2H(fixture, message, payload, left));
    if (message->type == type)
      return true;
  }
  return false;
}

static bool unbindBoth(struct Fixture * fixture)
{
  const uint64_t deadline = microtime() + WAIT_MS * 1000U;
  bool h2cDone = false;
  bool c2hDone = false;
  while (microtime() < deadline && (!h2cDone || !c2hDone))
  {
    if (!h2cDone)
    {
      const LGMP_STATUS status = lgmpHostStreamUnbind(fixture->h2c);
      CHECK(status == LGMP_OK || status == LGMP_ERR_STREAM_BUSY);
      h2cDone = status == LGMP_OK;
    }
    if (!c2hDone)
    {
      LGMPStreamBuffer buffer = { 0 };
      LGMP_STATUS status;
      while ((status = lgmpHostStreamReadPeek(fixture->c2h,
              &buffer)) == LGMP_OK)
      {
        CHECK(lgmpHostStreamReadRelease(fixture->c2h, &buffer) == LGMP_OK);
        memset(&buffer, 0, sizeof(buffer));
      }
      CHECK(status == LGMP_ERR_STREAM_EMPTY ||
          status == LGMP_ERR_STREAM_UNBOUND ||
          status == LGMP_ERR_STREAM_STALE);
      status = lgmpHostStreamUnbind(fixture->c2h);
      CHECK(status == LGMP_OK || status == LGMP_ERR_STREAM_BUSY);
      c2hDone = status == LGMP_OK;
    }
    struct timespec delay = { .tv_nsec = 1000000L };
    nanosleep(&delay, NULL);
  }
  return h2cDone && c2hDone;
}

static bool beginClaim(struct Fixture * fixture, bool retry)
{
  fixture->ops->setStatusListener(fixture->audio,
      statusCallback, &fixture->trace);
  CHECK(waitCount(fixture, &fixture->trace.statusCount, 1, WAIT_MS));
  pthread_mutex_lock(&fixture->trace.mutex);
  const bool initiallyUnavailable = !fixture->trace.status.available;
  pthread_mutex_unlock(&fixture->trace.mutex);
  CHECK(initiallyUnavailable);

  lgmpAudio_pumpClaim(fixture->audio, fixture->pointerClient);
  CHECK(readClaim(fixture, &fixture->claim, WAIT_MS));
  CHECK(fixture->claim.messageType == KVMFR_MESSAGE_AUDIO_CLAIM);
  CHECK(fixture->claim.flags == KVMFR_AUDIO_CLAIM_BIND);
  CHECK(fixture->claim.clientCapabilities ==
      (KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE |
       KVMFR_AUDIO_CAP_CLOCK_FEEDBACK));

  if (retry)
  {
    struct timespec delay = { .tv_nsec = 210000000L };
    nanosleep(&delay, NULL);
    lgmpAudio_pumpClaim(fixture->audio, fixture->pointerClient);
    KVMFRAudioClaim second;
    CHECK(readClaim(fixture, &second, WAIT_MS));
    CHECK(!memcmp(&fixture->claim, &second, sizeof(second)));
  }
  return true;
}

static bool testLateBoundRelease(void)
{
  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));

  lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
  KVMFRAudioClaim ignored;
  CHECK(!readClaim(&fixture, &ignored, 30));

  CHECK(beginClaim(&fixture, true));
  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));

  fixture.ops->setStatusListener(fixture.audio, NULL, NULL);
  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));
  KVMFRAudioMessage message;
  uint8_t payload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(readC2H(&fixture, &message, payload, WAIT_MS));
  CHECK(message.type == KVMFR_AUDIO_MESSAGE_RELEASE);
  CHECK(message.sequence == 0);
  CHECK(unbindBoth(&fixture));
  const struct timespec settle = { .tv_nsec = 30000000L };
  nanosleep(&settle, NULL);

  fixtureCleanup(&fixture);
  return true;
}

static bool testRetiredClaim(void)
{
  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));

  struct SelfUnregister self =
  {
    .audio = fixture.audio,
    .ops = fixture.ops,
  };
  fixture.ops->setStatusListener(fixture.audio,
      selfUnregisterStatus, &self);
  CHECK(atomic_load_explicit(&self.count, memory_order_acquire) == 1);

  CHECK(beginClaim(&fixture, false));
  const KVMFRAudioClaim retired = fixture.claim;
  for (unsigned retry = 0; retry < 2; ++retry)
  {
    const struct timespec delay = { .tv_nsec = 210000000L };
    nanosleep(&delay, NULL);
    lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
    KVMFRAudioClaim duplicate;
    CHECK(readClaim(&fixture, &duplicate, WAIT_MS));
    CHECK(!memcmp(&retired, &duplicate, sizeof(retired)));
  }

  const struct timespec cooldownDelay = { .tv_nsec = 210000000L };
  nanosleep(&cooldownDelay, NULL);
  lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
  KVMFRAudioClaim unexpected;
  CHECK(!readClaim(&fixture, &unexpected, 30));

  fixture.ops->setStatusListener(fixture.audio, NULL, NULL);
  const struct timespec retirementDelay =
    { .tv_sec = 1, .tv_nsec = 250000000L };
  nanosleep(&retirementDelay, NULL);
  fixture.ops->setStatusListener(fixture.audio,
      statusCallback, &fixture.trace);
  const struct timespec reclaimDelay = { .tv_nsec = 220000000L };
  nanosleep(&reclaimDelay, NULL);
  lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
  CHECK(readClaim(&fixture, &fixture.claim, WAIT_MS));
  CHECK(fixture.claim.requestGeneration != retired.requestGeneration);
  CHECK(fixture.claim.claimToken != retired.claimToken);

  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));
  CHECK(deliverBoundResponse(&fixture, &retired, true));
  KVMFRAudioMessage message;
  uint8_t payload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(!readC2H(&fixture, &message, payload, 30));
  CHECK(unbindBoth(&fixture));

  CHECK(bindStreams(&fixture));
  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));
  CHECK(readC2H(&fixture, &message, payload, WAIT_MS));
  CHECK(message.type == KVMFR_AUDIO_MESSAGE_KEEPALIVE);
  CHECK(message.sequence == 0);

  fixture.ops->setStatusListener(fixture.audio, NULL, NULL);
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_RELEASE,
        &message, payload, WAIT_MS));
  CHECK(unbindBoth(&fixture));
  const struct timespec settle = { .tv_nsec = 30000000L };
  nanosleep(&settle, NULL);

  fixtureCleanup(&fixture);
  return true;
}

static bool testDataPlane(void)
{
  static const LG_AudioChannel expectedChannels[KVMFR_AUDIO_MAX_CHANNELS] =
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

  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));
  CHECK(beginClaim(&fixture, true));
  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));

  CHECK(deliverBoundResponse(&fixture, &fixture.claim, false));
  struct timespec shortDelay = { .tv_nsec = 30000000L };
  nanosleep(&shortDelay, NULL);

  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));
  KVMFRAudioMessage c2hMessage;
  uint8_t c2hPayload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(readC2H(&fixture, &c2hMessage, c2hPayload, WAIT_MS));
  CHECK(c2hMessage.type == KVMFR_AUDIO_MESSAGE_KEEPALIVE);
  CHECK(c2hMessage.sequence == 0);

  KVMFRAudioFormat playFormat = makeFormat(
      KVMFR_AUDIO_CHANNEL_FRONT_LEFT, KVMFR_AUDIO_SAMPLE_S24_LE);
  KVMFRAudioFormat captureFormat = makeFormat(
      KVMFR_AUDIO_CHANNEL_TOP_BACK_RIGHT, KVMFR_AUDIO_SAMPLE_S24_LE);
  const uint64_t playWire = UINT64_C(0x100000002);
  const uint64_t captureWire = UINT64_C(0x200000003);
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_START,
        playWire, KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
          KVMFR_AUDIO_MESSAGE_CLOCK_STABLE,
        99, 123456, UINT64_C(192000) << 32, 0,
        &playFormat, sizeof(playFormat)));
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_CAPTURE_START,
        captureWire, 0, 0, 0, 0, 0,
        &captureFormat, sizeof(captureFormat)));
  CHECK(sendStatus(&fixture, true,
        KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE,
        playWire, &playFormat,
        captureWire, &captureFormat));
  CHECK(waitCount(&fixture, &fixture.trace.statusCount, 2, WAIT_MS));
  pthread_mutex_lock(&fixture.trace.mutex);
  const bool available = fixture.trace.status.available;
  const uint32_t endpointLocal = fixture.trace.status.generation;
  pthread_mutex_unlock(&fixture.trace.mutex);
  CHECK(available && endpointLocal != 0);
  CHECK(fixture.ops->attach(fixture.audio, &eventOps, &fixture.trace));
  CHECK(waitCount(&fixture, &fixture.trace.playStartCount, 1, WAIT_MS));
  CHECK(waitCount(&fixture, &fixture.trace.recStartCount, 1, WAIT_MS));

  uint32_t previousPlayGeneration = 0;
  for (unsigned bit = 0; bit < KVMFR_AUDIO_MAX_CHANNELS; ++bit)
  {
    const uint32_t startCount =
      atomic_load_explicit(&fixture.trace.playStartCount,
          memory_order_acquire);
    KVMFRAudioFormat format = makeFormat(UINT32_C(1) << bit,
        KVMFR_AUDIO_SAMPLE_S24_LE);
    const uint64_t wire = UINT64_C(0x300000000) + bit;
    CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_START,
          wire, 0, 0, 0, 0, 0, &format, sizeof(format)));
    CHECK(waitCount(&fixture, &fixture.trace.playStartCount,
          startCount + 1, WAIT_MS));
    pthread_mutex_lock(&fixture.trace.mutex);
    CHECK(fixture.trace.playFormat.sampleFormat == LG_AUDIO_FMT_S24_LE);
    CHECK(fixture.trace.playFormat.channelCount == 1);
    CHECK(fixture.trace.playFormat.channels[0] == expectedChannels[bit]);
    CHECK(fixture.trace.playGeneration != 0);
    CHECK(!previousPlayGeneration ||
        fixture.trace.playGeneration != previousPlayGeneration);
    previousPlayGeneration = fixture.trace.playGeneration;
    pthread_mutex_unlock(&fixture.trace.mutex);
  }

  const uint32_t dataBefore =
    atomic_load_explicit(&fixture.trace.playDataCount, memory_order_acquire);
  uint8_t pcm[12];
  for (unsigned i = 0; i < sizeof(pcm); ++i)
    pcm[i] = (uint8_t)(0x40U + i);
  ++fixture.hostSequence;
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA,
        UINT64_C(0x300000000) + KVMFR_AUDIO_MAX_CHANNELS - 1,
        KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
          KVMFR_AUDIO_MESSAGE_CLOCK_STABLE,
        555, 987654321, UINT64_C(192000) << 32, 4,
        pcm, sizeof(pcm)));
  CHECK(waitCount(&fixture, &fixture.trace.playDataCount,
        dataBefore + 1, WAIT_MS));
  pthread_mutex_lock(&fixture.trace.mutex);
  CHECK(fixture.trace.playFrames == 4);
  CHECK(!memcmp(fixture.trace.playData, pcm, sizeof(pcm)));
  CHECK(fixture.trace.playClockValid);
  CHECK(fixture.trace.playClock.discontinuity);
  CHECK(fixture.trace.playClock.position == 555);
  pthread_mutex_unlock(&fixture.trace.mutex);

  pthread_mutex_lock(&fixture.trace.mutex);
  const uint32_t captureLocal = fixture.trace.recGeneration;
  const uint32_t playbackLocal = fixture.trace.playGeneration;
  pthread_mutex_unlock(&fixture.trace.mutex);
  CHECK(captureLocal && playbackLocal);
  const LG_AudioClock captureClock =
  {
    .position = 44,
    .time = 777,
    .rate = 192000.0,
    .stable = true,
  };
  uint8_t capturePcm[18];
  memset(capturePcm, 0x5a, sizeof(capturePcm));
  CHECK(fixture.ops->recordData(fixture.audio, captureLocal,
        capturePcm, 6, &captureClock));
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_CAPTURE_DATA,
        &c2hMessage, c2hPayload, WAIT_MS));
  CHECK(c2hMessage.streamGeneration == captureWire);
  CHECK(c2hMessage.frames == 6);
  CHECK(c2hMessage.length == sizeof(capturePcm));
  CHECK(!memcmp(c2hPayload, capturePcm, sizeof(capturePcm)));
  CHECK((c2hMessage.flags & (KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
          KVMFR_AUDIO_MESSAGE_CLOCK_STABLE)) ==
      (KVMFR_AUDIO_MESSAGE_CLOCK_VALID |
       KVMFR_AUDIO_MESSAGE_CLOCK_STABLE));

  const LG_AudioClock playbackClock =
  {
    .position = 888,
    .time = 999,
    .rate = 191999.75,
    .stable = true,
    .discontinuity = true,
  };
  CHECK(!fixture.ops->clockFeedback(fixture.audio, playbackLocal,
        &playbackClock, 192000.0));
  CHECK(!fixture.ops->clockFeedbackAvailable(fixture.audio));

  const uint64_t currentPlayWire =
    UINT64_C(0x300000000) + KVMFR_AUDIO_MAX_CHANNELS - 1;
  KVMFRAudioFormat currentPlayFormat = makeFormat(
      KVMFR_AUDIO_CHANNEL_TOP_BACK_RIGHT,
      KVMFR_AUDIO_SAMPLE_S24_LE);
  ++fixture.endpointGeneration;
  const unsigned clockStatusBefore = atomic_load_explicit(
      &fixture.trace.statusCount, memory_order_acquire);
  CHECK(sendStatus(&fixture, true,
        KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE |
          KVMFR_AUDIO_CAP_CLOCK_FEEDBACK,
        currentPlayWire, &currentPlayFormat,
        captureWire, &captureFormat));
  CHECK(waitCount(&fixture, &fixture.trace.statusCount,
        clockStatusBefore + 1, WAIT_MS));
  CHECK(fixture.ops->clockFeedbackAvailable(fixture.audio));
  CHECK(fixture.ops->clockFeedback(fixture.audio, playbackLocal,
        &playbackClock, 192000.0));
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_CLOCK_FEEDBACK,
        &c2hMessage, c2hPayload, WAIT_MS));
  CHECK(c2hMessage.streamGeneration ==
      UINT64_C(0x300000000) + KVMFR_AUDIO_MAX_CHANNELS - 1);
  CHECK(c2hMessage.length == sizeof(KVMFRAudioClockFeedback));
  CHECK(c2hMessage.flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY);
  KVMFRAudioClockFeedback feedback;
  memcpy(&feedback, c2hPayload, sizeof(feedback));
  CHECK(feedback.targetRateQ32 == (UINT64_C(192000) << 32));

  while (readC2H(&fixture, &c2hMessage, c2hPayload, 2))
  {
  }
  unsigned successful = 0;
  for (;;)
  {
    const uint64_t before = microtime();
    const bool ok = fixture.ops->recordData(fixture.audio, captureLocal,
        capturePcm, 6, NULL);
    const uint64_t elapsed = microtime() - before;
    CHECK(elapsed < 20000U);
    if (!ok)
      break;
    ++successful;
    CHECK(successful <= KVMFR_AUDIO_STREAM_SLOT_COUNT);
  }
  CHECK(successful > 0 && successful <= KVMFR_AUDIO_STREAM_SLOT_COUNT);
  uint64_t maxFullCall = 0;
  for (unsigned i = 0; i < 100; ++i)
  {
    const uint64_t before = microtime();
    CHECK(!fixture.ops->recordData(fixture.audio, captureLocal,
          capturePcm, 6, NULL));
    const uint64_t elapsed = microtime() - before;
    if (elapsed > maxFullCall)
      maxFullCall = elapsed;
  }
  CHECK(maxFullCall < 20000U);

  CHECK(readC2H(&fixture, &c2hMessage, c2hPayload, WAIT_MS));
  uint8_t resumedPcm[18];
  memset(resumedPcm, 0xef, sizeof(resumedPcm));
  CHECK(fixture.ops->recordData(fixture.audio, captureLocal,
        resumedPcm, 6, NULL));
  bool foundResumed = false;
  uint64_t previousSequence = c2hMessage.sequence;
  while (!foundResumed)
  {
    CHECK(readC2H(&fixture, &c2hMessage, c2hPayload, WAIT_MS));
    if (c2hMessage.type == KVMFR_AUDIO_MESSAGE_CAPTURE_DATA &&
        c2hMessage.length == sizeof(resumedPcm) &&
        !memcmp(c2hPayload, resumedPcm, sizeof(resumedPcm)))
    {
      CHECK(c2hMessage.sequence > previousSequence + 1);
      CHECK(c2hMessage.flags & KVMFR_AUDIO_MESSAGE_DISCONTINUITY);
      foundResumed = true;
    }
    previousSequence = c2hMessage.sequence;
  }

  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP,
        currentPlayWire, 0, 0, 0, 0, 0, NULL, 0));
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_CAPTURE_STOP,
        captureWire, 0, 0, 0, 0, 0, NULL, 0));
  const unsigned statusBefore = atomic_load_explicit(
      &fixture.trace.statusCount, memory_order_acquire);
  CHECK(sendStatus(&fixture, false, 0, 0, NULL, 0, NULL));
  CHECK(waitCount(&fixture, &fixture.trace.statusCount,
        statusBefore + 1, WAIT_MS));
  pthread_mutex_lock(&fixture.trace.mutex);
  CHECK(!fixture.trace.status.available);
  pthread_mutex_unlock(&fixture.trace.mutex);
  CHECK(readC2H(&fixture, &c2hMessage, c2hPayload, 350));
  CHECK(c2hMessage.type == KVMFR_AUDIO_MESSAGE_KEEPALIVE);

  const KVMFRAudioClaim originalClaim = fixture.claim;
  fixture.endpointGeneration++;
  playFormat = makeFormat(KVMFR_AUDIO_CHANNEL_STEREO,
      KVMFR_AUDIO_SAMPLE_S16_LE);
  const unsigned availableBefore = atomic_load_explicit(
      &fixture.trace.statusCount, memory_order_acquire);
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_START,
        UINT64_C(0x500000001), 0, 0, 0, 0, 0,
        &playFormat, sizeof(playFormat)));
  CHECK(sendStatus(&fixture, true,
        KVMFR_AUDIO_CAP_PLAYBACK | KVMFR_AUDIO_CAP_CAPTURE,
        UINT64_C(0x500000001),
        &playFormat, 0, NULL));
  CHECK(waitCount(&fixture, &fixture.trace.statusCount,
        availableBefore + 1, WAIT_MS));
  pthread_mutex_lock(&fixture.trace.mutex);
  CHECK(fixture.trace.status.available);
  CHECK(fixture.trace.status.generation != endpointLocal);
  pthread_mutex_unlock(&fixture.trace.mutex);
  lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
  KVMFRAudioClaim unexpectedClaim;
  CHECK(!readClaim(&fixture, &unexpectedClaim, 30));
  CHECK(!memcmp(&fixture.claim, &originalClaim, sizeof(originalClaim)));

  const unsigned playBeforeDetach = atomic_load_explicit(
      &fixture.trace.playDataCount, memory_order_acquire);
  for (unsigned i = 0; i < KVMFR_AUDIO_STREAM_SLOT_COUNT; ++i)
  {
    LGMPStreamBuffer buffer = { 0 };
    LGMP_STATUS status = lgmpHostStreamWriteAcquire(fixture.h2c, &buffer);
    if (status == LGMP_ERR_STREAM_FULL)
      break;
    CHECK(status == LGMP_OK);
    const KVMFRAudioMessage message =
    {
      .version = KVMFR_AUDIO_MESSAGE_VERSION,
      .size = sizeof(KVMFRAudioMessage),
      .type = KVMFR_AUDIO_MESSAGE_PLAYBACK_DATA,
      .streamGeneration = UINT64_C(0x500000001),
      .sequence = fixture.hostSequence++,
      .frames = 1,
      .length = playFormat.blockAlign,
    };
    memcpy(buffer.data, &message, sizeof(message));
    memset((uint8_t *)buffer.data + sizeof(message), 0x77,
        message.length);
    CHECK(lgmpHostStreamWriteCommit(fixture.h2c, &buffer,
          sizeof(message) + message.length) == LGMP_OK);
  }
  fixture.ops->detach(fixture.audio);
  const unsigned afterDetach = atomic_load_explicit(
      &fixture.trace.playDataCount, memory_order_acquire);
  CHECK(afterDetach >= playBeforeDetach);
  fixture.ops->setStatusListener(fixture.audio, NULL, NULL);
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_RELEASE,
        &c2hMessage, c2hPayload, WAIT_MS));
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_PLAYBACK_STOP,
        1, 0, 0, 0, 0, 0, NULL, 0));
  CHECK(unbindBoth(&fixture));
  struct timespec after = { .tv_nsec = 30000000L };
  nanosleep(&after, NULL);
  CHECK(atomic_load_explicit(&fixture.trace.playDataCount,
        memory_order_acquire) == afterDetach);

  fixtureCleanup(&fixture);
  printf("data-plane: full-ring-successes=%u max-full-call-us=%llu\n",
      successful, (unsigned long long)maxFullCall);
  return true;
}

static bool testReleaseIsFinal(void)
{
  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));
  CHECK(beginClaim(&fixture, false));
  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));
  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));

  KVMFRAudioMessage message;
  uint8_t payload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(readC2H(&fixture, &message, payload, WAIT_MS));
  CHECK(message.type == KVMFR_AUDIO_MESSAGE_KEEPALIVE);

  const uint64_t captureWire = UINT64_C(0x700000001);
  const KVMFRAudioFormat captureFormat = makeFormat(
      KVMFR_AUDIO_CHANNEL_TOP_BACK_RIGHT, KVMFR_AUDIO_SAMPLE_S24_LE);
  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_CAPTURE_START,
        captureWire, 0, 0, 0, 0, 0,
        &captureFormat, sizeof(captureFormat)));
  CHECK(sendStatus(&fixture, true, KVMFR_AUDIO_CAP_CAPTURE,
        0, NULL, captureWire, &captureFormat));
  CHECK(waitCount(&fixture, &fixture.trace.statusCount, 2, WAIT_MS));
  CHECK(fixture.ops->attach(fixture.audio, &eventOps, &fixture.trace));
  CHECK(waitCount(&fixture, &fixture.trace.recStartCount, 1, WAIT_MS));
  pthread_mutex_lock(&fixture.trace.mutex);
  const uint32_t captureLocal = fixture.trace.recGeneration;
  pthread_mutex_unlock(&fixture.trace.mutex);
  CHECK(captureLocal != 0);

  struct RecordStress stress =
  {
    .audio = fixture.audio,
    .ops = fixture.ops,
    .generation = captureLocal,
  };
  pthread_t stressThread;
  CHECK(!pthread_create(&stressThread, NULL, recordStress, &stress));

  CHECK(sendStatus(&fixture, true, KVMFR_AUDIO_CAP_CAPTURE,
        0, NULL, captureWire, &captureFormat));
  pthread_t listenerThread;
  CHECK(!pthread_create(&listenerThread, NULL,
        removeStatusListener, &stress));
  CHECK(!pthread_join(listenerThread, NULL));
  bool released = false;
  const uint64_t releaseDeadline = microtime() + WAIT_MS * 1000U;
  while (microtime() < releaseDeadline && !released)
  {
    CHECK(readC2H(&fixture, &message, payload, WAIT_MS));
    released = message.type == KVMFR_AUDIO_MESSAGE_RELEASE;
  }
  CHECK(released);

  const uint64_t trailingDeadline = microtime() + 30000U;
  while (microtime() < trailingDeadline)
  {
    if (!readC2H(&fixture, &message, payload, 2))
      continue;
    CHECK(message.type != KVMFR_AUDIO_MESSAGE_CAPTURE_DATA);
  }
  CHECK(!fixture.ops->recordData(fixture.audio,
        captureLocal, payload, 6, NULL));
  atomic_store_explicit(&stress.stop, true, memory_order_release);
  CHECK(!pthread_join(stressThread, NULL));
  CHECK(atomic_load_explicit(
        &stress.successes, memory_order_acquire) > 0);

  CHECK(sendH2C(&fixture, KVMFR_AUDIO_MESSAGE_CAPTURE_STOP,
        captureWire, 0, 0, 0, 0, 0, NULL, 0));
  CHECK(unbindBoth(&fixture));
  lgmpAudio_disconnect(fixture.audio);

  fixtureCleanup(&fixture);
  return true;
}

static bool testFaultReleaseReclaim(void)
{
  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));
  CHECK(beginClaim(&fixture, false));
  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));
  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));

  KVMFRAudioMessage message;
  uint8_t payload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(readC2H(&fixture, &message, payload, WAIT_MS));
  CHECK(message.type == KVMFR_AUDIO_MESSAGE_KEEPALIVE);
  CHECK(message.sequence == 0);

  CHECK(sendH2C(&fixture, UINT32_C(0xffffffff),
        0, 0, 0, 0, 0, 0, NULL, 0));
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_RELEASE,
        &message, payload, WAIT_MS));
  CHECK(unbindBoth(&fixture));

  const KVMFRAudioClaim first = fixture.claim;
  struct timespec retryDelay = { .tv_nsec = 250000000L };
  nanosleep(&retryDelay, NULL);
  lgmpAudio_pumpClaim(fixture.audio, fixture.pointerClient);
  CHECK(readClaim(&fixture, &fixture.claim, WAIT_MS));
  CHECK(fixture.claim.requestGeneration != first.requestGeneration);
  CHECK(fixture.claim.claimToken != first.claimToken);

  fixture.ops->setStatusListener(fixture.audio, NULL, NULL);
  fixtureCleanup(&fixture);
  return true;
}

static bool testBoundDisconnect(void)
{
  struct Fixture fixture;
  CHECK(fixtureInit(&fixture));
  CHECK(beginClaim(&fixture, false));
  CHECK(createStreams(&fixture));
  CHECK(bindStreams(&fixture));
  CHECK(deliverBoundResponse(&fixture, &fixture.claim, true));

  KVMFRAudioMessage message;
  uint8_t payload[KVMFR_AUDIO_PCM_BYTES];
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_KEEPALIVE,
        &message, payload, WAIT_MS));

  struct DisconnectTask task = { .audio = fixture.audio };
  atomic_init(&task.done, false);
  pthread_t thread;
  CHECK(!pthread_create(&thread, NULL, disconnectAudio, &task));
  CHECK(readC2HType(&fixture, KVMFR_AUDIO_MESSAGE_RELEASE,
        &message, payload, WAIT_MS));
  CHECK(!atomic_load_explicit(&task.done, memory_order_acquire));
  CHECK(unbindBoth(&fixture));
  CHECK(waitDone(&fixture, &task.done, WAIT_MS));
  CHECK(!pthread_join(thread, NULL));

  fixtureCleanup(&fixture);
  return true;
}

int main(void)
{
  debug_init();

  unsigned passed = 0;
  if (!testLateBoundRelease())
    return 1;
  ++passed;
  if (!testRetiredClaim())
    return 1;
  ++passed;
  if (!testDataPlane())
    return 1;
  ++passed;
  if (!testReleaseIsFinal())
    return 1;
  ++passed;
  if (!testFaultReleaseReclaim())
    return 1;
  ++passed;
  if (!testBoundDisconnect())
    return 1;
  ++passed;
  printf("PASS %u/6 client LGMP audio integration groups\n", passed);
  return 0;
}
