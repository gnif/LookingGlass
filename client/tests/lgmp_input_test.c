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

#include "../transports/LGMP/input.h"

#include "common/KVMFRInput.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"

#include <lgmp/client.h>
#include <lgmp/host.h>
#include <lgmp/stream.h>

#include <linux/input.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_SHM_SIZE       (2U * 1024U * 1024U)
#define TEST_QUEUE_TIMEOUT  1000U
#define TEST_WAIT_MS        2500U
#define TEST_IDLE_WAIT_MS   1500U
#define TEST_QUIET_MS       50U
#define TEST_MAX_STATUSES   32U
#define TEST_MOTION_COUNT   200U

typedef struct StatusTrace
{
  atomic_uint count;
  atomic_bool available;
  atomic_uint generation;
}
StatusTrace;

typedef struct TestState
{
  void                    * memory;
  PLGMPHost                 host;
  PLGMPHostQueue            queue;
  PLGMPClient               client;
  PLGMPHostStream           streams[KVMFR_INPUT_STREAM_ENDPOINT_COUNT];
  KVMFRStreamDescriptor     streamDescriptors[
    KVMFR_INPUT_STREAM_ENDPOINT_COUNT];
  uint32_t                  streamEpoch;
  uint32_t                  streamGeneration;
  LGMPInput               * input;
  const LG_InputOps       * ops;
  uint32_t                  clientID;
  uint32_t                  statusSerial;
  PLGMPMemory               statuses[TEST_MAX_STATUSES];
  unsigned                  statusCount;
  StatusTrace               status;
}
TestState;

typedef struct DisconnectTask
{
  LGMPInput  * input;
  atomic_bool  done;
}
DisconnectTask;

#define CHECK(x) \
  do \
  { \
    if (!(x)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", \
          __FILE__, __LINE__, #x); \
      return false; \
    } \
  } \
  while (0)

static void statusChanged(void * opaque, const LG_InputStatus * status)
{
  StatusTrace * trace = opaque;
  if (!status)
    return;

  atomic_store(&trace->available, status->available);
  atomic_store(&trace->generation, status->generation);
  atomic_fetch_add(&trace->count, 1);
}

static bool hostProcess(TestState * state)
{
  CHECK(lgmpHostProcess(state->host) == LGMP_OK);
  return true;
}

static KVMFRStreamDescriptor exportStreamDescriptor(
    const struct LGMPStreamDescriptor * descriptor)
{
  return (KVMFRStreamDescriptor)
  {
    .magic      = descriptor->magic,
    .version    = descriptor->version,
    .size       = descriptor->size,
    .offset     = descriptor->offset,
    .regionSize = descriptor->regionSize,
    .direction  = descriptor->direction,
    .policy     = descriptor->policy,
    .slotCount  = descriptor->slotCount,
    .slotSize   = descriptor->slotSize,
  };
}

static bool waitStatus(TestState * state, unsigned count,
    bool available, uint32_t generation)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (atomic_load(&state->status.count) >= count &&
        atomic_load(&state->status.available) == available &&
        atomic_load(&state->status.generation) == generation)
      return true;
    usleep(1000);
  }

  return false;
}

static bool waitStatusEmpty(TestState * state)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (lgmpHostQueuePending(state->queue) == 0)
    {
      usleep(1000);
      return true;
    }
    usleep(1000);
  }

  return false;
}

static bool postStatus(TestState * state, uint32_t endpointGeneration,
    uint32_t ownerClientID, uint32_t ownerGeneration)
{
  CHECK(state->statusCount < TEST_MAX_STATUSES);
  CHECK(waitStatusEmpty(state));

  PLGMPMemory memory;
  CHECK(lgmpHostMemAlloc(state->host, sizeof(KVMFRInputStatus), &memory) ==
      LGMP_OK);
  state->statuses[state->statusCount++] = memory;

  KVMFRInputStatus * status = lgmpHostMemPtr(memory);
  *status = (KVMFRInputStatus)
  {
    .version             = KVMFR_INPUT_VERSION,
    .capabilities        = KVMFR_INPUT_CAP_MOUSE_RELATIVE |
      KVMFR_INPUT_CAP_MOUSE_ABSOLUTE | KVMFR_INPUT_CAP_KEYBOARD,
    .flags               = KVMFR_INPUT_STATUS_AVAILABLE |
      (ownerClientID ? KVMFR_INPUT_STATUS_HAS_OWNER : 0),
    .generation          = endpointGeneration,
    .ownerClientID       = ownerClientID,
    .ownerGeneration     = ownerGeneration,
    .lease               = 1000,
    .maxButtons          = KVMFR_INPUT_MOUSE_BUTTON_COUNT,
    .streamVersion       = KVMFR_INPUT_STREAM_VERSION,
    .streamEndpointCount = KVMFR_INPUT_STREAM_ENDPOINT_COUNT,
    .streamGeneration    = state->streamGeneration,
  };

  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    status->streamEndpoint[i].stream = state->streamDescriptors[i];
    status->streamEndpoint[i].flags =
      KVMFR_INPUT_STREAM_ENDPOINT_AVAILABLE;
  }
  status->streamEndpoint[0].boundClientID = state->clientID;
  status->streamEndpoint[0].bindingGeneration = state->streamEpoch;
  status->streamEndpoint[0].flags |=
    KVMFR_INPUT_STREAM_ENDPOINT_BOUND;

  if (++state->statusSerial == 0)
    ++state->statusSerial;
  CHECK(lgmpHostQueuePost(state->queue, state->statusSerial, memory) ==
      LGMP_OK);
  return true;
}

static bool postAvailable(TestState * state, uint32_t endpointGeneration)
{
  const unsigned nextStatus = atomic_load(&state->status.count) + 1;
  CHECK(postStatus(state, endpointGeneration, 0, 0));
  CHECK(waitStatus(state, nextStatus, true, endpointGeneration));
  return true;
}

static bool postOwner(TestState * state, uint32_t endpointGeneration,
    uint32_t ownerClientID, uint32_t ownerGeneration)
{
  CHECK(postStatus(state, endpointGeneration, ownerClientID,
        ownerGeneration));
  CHECK(waitStatusEmpty(state));
  return true;
}

static bool expectInputQueueEmpty(TestState * state)
{
  KVMFRInputMessage message;
  size_t            size = sizeof(message);
  uint32_t          sourceClientID = 0;
  const LGMP_STATUS status = lgmpHostReadDataWithSource(state->queue,
    &message, &size, &sourceClientID);
  if (status == LGMP_OK)
    lgmpHostAckData(state->queue);
  CHECK(status == LGMP_ERR_QUEUE_EMPTY);
  return true;
}

static bool readInput(TestState * state, KVMFRInputMessage * result)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    CHECK(expectInputQueueEmpty(state));
    LGMPStreamBuffer buffer = { 0 };
    const LGMP_STATUS status = lgmpHostStreamReadPeek(
      state->streams[0], &buffer);
    if (status == LGMP_ERR_STREAM_EMPTY)
    {
      usleep(1000);
      continue;
    }

    CHECK(status == LGMP_OK);
    CHECK(buffer.size == sizeof(*result));
    memcpy(result, buffer.data, sizeof(*result));
    CHECK(result->reserved == 0);
    CHECK(result->generation != 0);
    CHECK(result->sequence != 0);
    CHECK(lgmpHostStreamReadRelease(state->streams[0], &buffer) ==
      LGMP_OK);
    return true;
  }

  return false;
}

static bool expectType(TestState * state, KVMFRInputMessageType type,
    KVMFRInputMessage * result)
{
  CHECK(readInput(state, result));
  if (result->type != type)
  {
    fprintf(stderr, "expected input message %u, got %u\n",
        type, result->type);
    return false;
  }
  return true;
}

static bool readUntilType(TestState * state, KVMFRInputMessageType type,
    unsigned limit, KVMFRInputMessage * result, unsigned * skipped)
{
  for (unsigned i = 0; i < limit; ++i)
  {
    CHECK(readInput(state, result));
    if (result->type == type)
      return true;
    if (skipped)
      ++*skipped;
  }

  return false;
}

static bool expectNoInput(TestState * state)
{
  for (unsigned i = 0; i < TEST_QUIET_MS; ++i)
  {
    CHECK(hostProcess(state));
    CHECK(expectInputQueueEmpty(state));
    LGMPStreamBuffer buffer = { 0 };
    const LGMP_STATUS status = lgmpHostStreamReadPeek(
      state->streams[0], &buffer);
    if (status == LGMP_ERR_STREAM_EMPTY)
    {
      usleep(1000);
      continue;
    }

    if (status == LGMP_OK)
      lgmpHostStreamReadRelease(state->streams[0], &buffer);
    CHECK(status == LGMP_ERR_STREAM_EMPTY);
  }

  return true;
}

static bool keyboardHas(const KVMFRInputMessage * message, uint8_t usage)
{
  if (message->type != KVMFR_INPUT_MESSAGE_KEYBOARD)
    return false;

  for (unsigned i = 0; i < KVMFR_INPUT_KEYBOARD_KEY_COUNT; ++i)
    if (message->payload.keyboard.keys[i] == usage)
      return true;
  return false;
}

static bool keyboardEmpty(const KVMFRInputMessage * message)
{
  if (message->type != KVMFR_INPUT_MESSAGE_KEYBOARD ||
      message->payload.keyboard.modifiers)
    return false;

  for (unsigned i = 0; i < KVMFR_INPUT_KEYBOARD_KEY_COUNT; ++i)
    if (message->payload.keyboard.keys[i])
      return false;
  return true;
}

static bool resetAndRelease(TestState * state, uint32_t generation)
{
  state->ops->reset(state->input);
  KVMFRInputMessage message;
  unsigned          skipped = 0;
  CHECK(readUntilType(state, KVMFR_INPUT_MESSAGE_RELEASE, 8,
        &message, &skipped));
  CHECK(message.generation == generation);
  const uint32_t endpointGeneration =
    atomic_load(&state->status.generation);
  CHECK(endpointGeneration != 0);
  CHECK(postOwner(state, endpointGeneration, 0, 0));
  return true;
}

static bool testClaim(TestState * state)
{
  CHECK(postAvailable(state, 10));
  CHECK(state->ops->supports(state->input,
        LG_INPUT_SUPPORT_MOUSE_ABSOLUTE));
  CHECK(state->ops->keyDown(state->input, KEY_A));

  KVMFRInputMessage claim;
  KVMFRInputMessage keyboard;
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_CLAIM, &claim));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &keyboard));
  CHECK(claim.sequence == 1);
  CHECK(keyboard.generation == claim.generation);
  CHECK(keyboard.sequence == 2);
  CHECK(keyboardHas(&keyboard, 4));

  CHECK(postOwner(state, 10, state->clientID, claim.generation));
  CHECK(state->ops->keyUp(state->input, KEY_A));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &keyboard));
  CHECK(keyboard.generation == claim.generation);
  CHECK(keyboard.sequence == 3);
  CHECK(keyboardEmpty(&keyboard));

  CHECK(resetAndRelease(state, claim.generation));
  CHECK(expectNoInput(state));
  return true;
}

static bool testBlocked(TestState * state)
{
  const uint32_t otherClient = state->clientID == UINT32_MAX ?
    state->clientID - 1 : state->clientID + 1;
  CHECK(postStatus(state, 20, otherClient, 77));
  CHECK(waitStatus(state, 2, true, 20));

  CHECK(state->ops->keyDown(state->input, KEY_A));
  CHECK(state->ops->mousePress(state->input, 1));
  CHECK(expectNoInput(state));

  CHECK(postOwner(state, 20, 0, 0));
  KVMFRInputMessage claim;
  KVMFRInputMessage keyboard;
  KVMFRInputMessage mouse;
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_CLAIM, &claim));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &keyboard));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE, &mouse));
  CHECK(keyboardHas(&keyboard, 4));
  CHECK(mouse.payload.mouseRelative.buttons == 1);
  CHECK(mouse.payload.mouseRelative.deltaX == 0);
  CHECK(mouse.payload.mouseRelative.deltaY == 0);

  CHECK(postOwner(state, 20, state->clientID, claim.generation));
  CHECK(resetAndRelease(state, claim.generation));
  return true;
}

static bool testRestart(TestState * state)
{
  CHECK(postAvailable(state, 30));
  CHECK(state->ops->keyDown(state->input, KEY_A));
  CHECK(state->ops->mousePress(state->input, 1));

  KVMFRInputMessage oldClaim;
  KVMFRInputMessage message;
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_CLAIM, &oldClaim));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &message));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE, &message));
  CHECK(postOwner(state, 30, state->clientID, oldClaim.generation));

  const unsigned nextStatus = atomic_load(&state->status.count) + 1;
  CHECK(postStatus(state, 31, 0, 0));
  CHECK(waitStatus(state, nextStatus, true, 31));

  KVMFRInputMessage newClaim;
  KVMFRInputMessage keyboard;
  KVMFRInputMessage mouse;
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_CLAIM, &newClaim));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &keyboard));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE, &mouse));
  CHECK(newClaim.generation != oldClaim.generation);
  CHECK(newClaim.sequence == 1);
  CHECK(keyboard.generation == newClaim.generation);
  CHECK(keyboard.sequence == 2);
  CHECK(keyboardHas(&keyboard, 4));
  CHECK(mouse.generation == newClaim.generation);
  CHECK(mouse.sequence == 3);
  CHECK(mouse.payload.mouseRelative.buttons == 1);

  CHECK(postOwner(state, 31, state->clientID, newClaim.generation));
  CHECK(resetAndRelease(state, newClaim.generation));
  return true;
}

static bool testPressure(TestState * state)
{
  CHECK(postAvailable(state, 40));
  CHECK(state->ops->mousePress(state->input, 1));
  for (unsigned i = 0; i < TEST_MOTION_COUNT; ++i)
    CHECK(state->ops->mouseMotion(state->input, 1, 1));
  CHECK(state->ops->mouseRelease(state->input, 1));

  bool     sawClaim     = false;
  bool     sawPressed   = false;
  bool     sawAggregate = false;
  bool     sawRelease   = false;
  uint32_t generation   = 0;
  uint32_t sequence     = 0;
  for (unsigned i = 0; i < TEST_MOTION_COUNT + 16; ++i)
  {
    KVMFRInputMessage message;
    CHECK(readInput(state, &message));
    if (!generation)
      generation = message.generation;
    CHECK(message.generation == generation);
    CHECK(message.sequence == sequence + 1);
    sequence = message.sequence;

    if (message.type == KVMFR_INPUT_MESSAGE_CLAIM)
    {
      CHECK(!sawClaim);
      sawClaim = true;
      continue;
    }
    CHECK(message.type == KVMFR_INPUT_MESSAGE_MOUSE_RELATIVE);
    if (message.payload.mouseRelative.buttons)
      sawPressed = true;
    if (message.payload.mouseRelative.deltaX > 1 ||
        message.payload.mouseRelative.deltaY > 1)
      sawAggregate = true;
    if (!message.payload.mouseRelative.buttons &&
        message.payload.mouseRelative.deltaX == 0 &&
        message.payload.mouseRelative.deltaY == 0 &&
        message.payload.mouseRelative.wheel == 0)
    {
      sawRelease = true;
      break;
    }
  }

  CHECK(sawClaim);
  CHECK(sawPressed);
  CHECK(sawAggregate);
  CHECK(sawRelease);
  CHECK(resetAndRelease(state, generation));
  return true;
}

static bool testIdle(TestState * state)
{
  CHECK(postAvailable(state, 50));
  CHECK(state->ops->keyDown(state->input, KEY_A));

  KVMFRInputMessage claim;
  KVMFRInputMessage message;
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_CLAIM, &claim));
  CHECK(expectType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, &message));

  unsigned skipped = 0;
  CHECK(readUntilType(state, KVMFR_INPUT_MESSAGE_KEEPALIVE,
        TEST_IDLE_WAIT_MS, &message, &skipped));
  CHECK(message.generation == claim.generation);
  CHECK(message.sequence > 2);

  CHECK(state->ops->keyUp(state->input, KEY_A));
  CHECK(readUntilType(state, KVMFR_INPUT_MESSAGE_KEYBOARD, 8,
        &message, &skipped));
  CHECK(keyboardEmpty(&message));

  const uint32_t keyUpSequence = message.sequence;
  CHECK(readUntilType(state, KVMFR_INPUT_MESSAGE_RELEASE,
        TEST_IDLE_WAIT_MS, &message, &skipped));
  CHECK(message.generation == claim.generation);
  CHECK(message.sequence > keyUpSequence);
  return true;
}

static bool stateInit(TestState * state)
{
  memset(state, 0, sizeof(*state));
  state->memory = MAP_FAILED;
  atomic_init(&state->status.count, 0);
  atomic_init(&state->status.available, false);
  atomic_init(&state->status.generation, 0);

  state->memory = mmap(NULL, TEST_SHM_SIZE, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(state->memory != MAP_FAILED);

  const uint32_t sessionData = UINT32_C(0x12345678);
  CHECK(lgmpHostInit(state->memory, TEST_SHM_SIZE, &state->host,
        sizeof(sessionData), (uint8_t *)&sessionData) == LGMP_OK);
  const struct LGMPQueueConfig config =
  {
    .queueID     = LGMP_Q_INPUT,
    .numMessages = LGMP_Q_INPUT_LEN,
    .subTimeout  = TEST_QUEUE_TIMEOUT,
  };
  CHECK(lgmpHostQueueNew(state->host, config, &state->queue) == LGMP_OK);
  CHECK(lgmpClientInit(state->memory, TEST_SHM_SIZE, &state->client) ==
      LGMP_OK);

  usleep(300000);
  CHECK(hostProcess(state));
  uint32_t   dataSize;
  uint32_t   remoteVersion;
  uint8_t  * data;
  CHECK(lgmpClientSessionInit(state->client, &dataSize, &data,
        &state->clientID, &remoteVersion) == LGMP_OK);
  CHECK(state->clientID != 0);
  CHECK(dataSize == sizeof(sessionData));
  CHECK(memcmp(data, &sessionData, sizeof(sessionData)) == 0);

  const struct LGMPStreamConfig streamConfig =
  {
    .direction = LGMP_STREAM_CLIENT_TO_HOST,
    .policy    = LGMP_STREAM_RELIABLE_FIFO,
    .slotCount = KVMFR_INPUT_STREAM_SLOT_COUNT,
    .slotSize  = KVMFR_INPUT_STREAM_SLOT_SIZE,
  };
  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
  {
    CHECK(lgmpHostStreamNew(state->host, streamConfig,
          &state->streams[i]) == LGMP_OK);
    struct LGMPStreamDescriptor descriptor;
    lgmpHostStreamGetDescriptor(state->streams[i], &descriptor);
    state->streamDescriptors[i] = exportStreamDescriptor(&descriptor);
  }
  CHECK(lgmpHostStreamBind(state->streams[0], state->clientID,
        &state->streamEpoch) == LGMP_OK);
  state->streamGeneration = 1;

  CHECK(lgmpInput_create(state->client, &state->input));
  CHECK(lgmpInput_connect(state->input, state->clientID));
  state->ops = lgmpInput_getOps();
  CHECK(state->ops);
  state->ops->setStatusListener(state->input, statusChanged, &state->status);
  CHECK(atomic_load(&state->status.count) == 1);
  CHECK(!atomic_load(&state->status.available));
  CHECK(atomic_load(&state->status.generation) == 0);

  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    if (lgmpHostQueueHasSubs(state->queue))
      return true;
    usleep(1000);
  }
  return false;
}

static void * disconnectThread(void * opaque)
{
  DisconnectTask * task = opaque;
  lgmpInput_disconnect(task->input);
  atomic_store(&task->done, true);
  return NULL;
}

static bool disconnectAndDrain(TestState * state)
{
  if (!state->input)
    return true;

  if (state->ops)
    state->ops->setStatusListener(state->input, NULL, NULL);

  DisconnectTask task =
  {
    .input = state->input,
  };
  atomic_init(&task.done, false);
  pthread_t thread;
  if (pthread_create(&thread, NULL, disconnectThread, &task) != 0)
  {
    lgmpInput_disconnect(state->input);
    return false;
  }

  bool valid = true;
  bool releaseAcknowledged = false;
  for (unsigned i = 0;
      i < TEST_WAIT_MS && !atomic_load(&task.done); ++i)
  {
    if (lgmpHostProcess(state->host) != LGMP_OK)
      valid = false;
    if (!expectInputQueueEmpty(state))
      valid = false;

    for (;;)
    {
      LGMPStreamBuffer buffer = { 0 };
      const LGMP_STATUS status = lgmpHostStreamReadPeek(
        state->streams[0], &buffer);
      if (status == LGMP_ERR_STREAM_EMPTY)
        break;
      if (status != LGMP_OK)
      {
        valid = false;
        break;
      }

      KVMFRInputMessage message;
      valid &= buffer.size == sizeof(message);
      if (buffer.size == sizeof(message))
        memcpy(&message, buffer.data, sizeof(message));
      else
        memset(&message, 0, sizeof(message));
      valid &= message.type == KVMFR_INPUT_MESSAGE_RELEASE;
      valid &= message.reserved == 0;
      if (lgmpHostStreamReadRelease(state->streams[0], &buffer) !=
          LGMP_OK)
        valid = false;
      if (!releaseAcknowledged &&
          message.type == KVMFR_INPUT_MESSAGE_RELEASE)
      {
        const uint32_t endpointGeneration =
          atomic_load(&state->status.generation);
        if (endpointGeneration && !postStatus(
              state, endpointGeneration, 0, 0))
          valid = false;
        releaseAcknowledged = true;
      }
    }
    usleep(1000);
  }

  if (pthread_join(thread, NULL) != 0)
    valid = false;
  if (!atomic_load(&task.done))
    valid = false;
  return valid;
}

static bool stateFree(TestState * state)
{
  const bool drained = disconnectAndDrain(state);
  lgmpInput_destroy(&state->input);
  lgmpClientFree(&state->client);
  for (unsigned i = 0; i < state->statusCount; ++i)
    lgmpHostMemFree(&state->statuses[i]);
  for (unsigned i = 0; i < KVMFR_INPUT_STREAM_ENDPOINT_COUNT; ++i)
    lgmpHostStreamFree(&state->streams[i]);
  lgmpHostFree(&state->host);
  if (state->memory != MAP_FAILED)
    munmap(state->memory, TEST_SHM_SIZE);
  return drained;
}

typedef bool (*TestFn)(TestState * state);

static const struct
{
  const char * name;
  TestFn       run;
}
tests[] =
{
  { "claim"   , testClaim    },
  { "blocked" , testBlocked  },
  { "restart" , testRestart  },
  { "pressure", testPressure },
  { "idle"    , testIdle     },
};

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <claim|blocked|restart|pressure|idle>\n",
        argv[0]);
    return 2;
  }

  TestFn test = NULL;
  for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    if (strcmp(argv[1], tests[i].name) == 0)
    {
      test = tests[i].run;
      break;
    }
  if (!test)
  {
    fprintf(stderr, "unknown test case: %s\n", argv[1]);
    return 2;
  }

  debug_init();
  TestState  state;
  const bool initialized = stateInit(&state);
  bool       passed      = initialized && test(&state);
  if (!stateFree(&state))
    passed = false;
  return passed ? 0 : 1;
}
