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

#include "interface/transport.h"

#include "common/KVMFR.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"
#include "common/option.h"

#include <lgmp/host.h>

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_SHM_SIZE       (2U * 1024U * 1024U)
#define TEST_QUEUE_TIMEOUT  250U
#define TEST_WAIT_LOOPS     500U
#define TEST_FRAME_DATA     64U
#define TEST_MAX_ALLOCS     64U
#define TEST_FRAME_QUEUES   (LGMP_Q_FRAME_LEN + 1U)

extern const LG_TransportOps LGT_LGMP;

typedef struct TestFrame
{
  PLGMPMemory   memory;
  KVMFRFrame  * wire;
}
TestFrame;

typedef struct TestState
{
  int                 fd;
  char                path[64];
  bool                pathExists;
  void              * hostMemory;
  PLGMPHost           host;
  PLGMPHostQueue      queues[TEST_FRAME_QUEUES];
  PLGMPMemory         allocations[TEST_MAX_ALLOCS];
  unsigned            allocationCount;
  LG_Transport      * transport;
  const LG_FrameOps * frameOps;
}
TestState;

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

static bool waitForEmpty(TestState * state)
{
  for (unsigned attempt = 0; attempt < TEST_WAIT_LOOPS; ++attempt)
  {
    if (lgmpHostProcess(state->host) != LGMP_OK)
      return false;

    bool empty = true;
    for (unsigned i = 0; i < TEST_FRAME_QUEUES; ++i)
      empty &= lgmpHostQueuePending(state->queues[i]) == 0;
    if (empty)
      return true;

    usleep(1000);
  }

  return false;
}

static bool allocMemory(TestState * state, uint32_t size,
    PLGMPMemory * memory, void ** pointer)
{
  CHECK(state->allocationCount < TEST_MAX_ALLOCS);
  CHECK(lgmpHostMemAlloc(state->host, size, memory) == LGMP_OK);
  state->allocations[state->allocationCount++] = *memory;
  *pointer = lgmpHostMemPtr(*memory);
  return true;
}

static bool newFrame(TestState * state, uint32_t serial, TestFrame * result)
{
  const uint32_t size = sizeof(KVMFRFrame) + sizeof(FrameBuffer) +
    TEST_FRAME_DATA;
  void * pointer;
  CHECK(allocMemory(state, size, &result->memory, &pointer));

  result->wire = pointer;
  result->wire->formatVer     = 1;
  result->wire->frameSerial   = serial;
  result->wire->type          = FRAME_TYPE_BGRA;
  result->wire->screenWidth   = 4;
  result->wire->screenHeight  = 4;
  result->wire->dataWidth     = 4;
  result->wire->dataHeight    = 4;
  result->wire->frameWidth    = 4;
  result->wire->frameHeight   = 4;
  result->wire->rotation      = FRAME_ROT_0;
  result->wire->stride        = 4;
  result->wire->pitch         = 16;
  result->wire->offset        = sizeof(KVMFRFrame);
  result->wire->sdrWhiteLevel = KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  FrameBuffer * framebuffer = (FrameBuffer *)((uint8_t *)result->wire +
      result->wire->offset);
  atomic_store(&framebuffer->wp, TEST_FRAME_DATA);
  return true;
}

static void setDamage(TestFrame * frame, uint32_t count)
{
  frame->wire->damageRectsCount = count;
  for (uint32_t i = 0; i < count && i < KVMFR_MAX_DAMAGE_RECTS; ++i)
  {
    frame->wire->damageRects[i].x      = i + 1;
    frame->wire->damageRects[i].y      = i + 2;
    frame->wire->damageRects[i].width  = i + 3;
    frame->wire->damageRects[i].height = i + 4;
  }
}

static bool postShared(TestState * state, const TestFrame * frame)
{
  CHECK(lgmpHostQueuePost(state->queues[0], 0, frame->memory) == LGMP_OK);
  return true;
}

static bool postOwner(TestState * state, unsigned lane, uint64_t token,
    const TestFrame * frame)
{
  CHECK(lane < LGMP_Q_FRAME_LEN);

  uint32_t       clientIDs[32];
  unsigned       clientCount;
  unsigned       recipientCount;
  PLGMPHostQueue queue = state->queues[lane + 1];
  CHECK(lgmpHostGetClientIDs(queue, clientIDs, &clientCount) == LGMP_OK);
  CHECK(clientCount == 1);
  CHECK(lgmpHostQueuePostForClients(queue, token, frame->memory,
        clientIDs, clientCount, &recipientCount) == LGMP_OK);
  CHECK(recipientCount == 1);
  return true;
}

static bool resetFrames(TestState * state)
{
  state->frameOps->stopFrame(state->transport);
  CHECK(waitForEmpty(state));

  LG_TransportFrame frame;
  CHECK(state->frameOps->nextFrame(state->transport, false, &frame) ==
      LG_TRANSPORT_TIMEOUT);
  for (unsigned i = 0; i < TEST_FRAME_QUEUES; ++i)
    CHECK(lgmpHostQueueHasSubs(state->queues[i]));
  return true;
}

static bool releaseFrame(TestState * state, LG_TransportFrame * frame)
{
  state->frameOps->releaseFrame(state->transport, frame);
  CHECK(frame->releaseFn == NULL);
  CHECK(frame->releaseHandle == 0);
  CHECK(waitForEmpty(state));
  return true;
}

static bool testMalformed(TestState * state)
{
  CHECK(resetFrames(state));

  PLGMPMemory shortMemory;
  void * shortPointer;
  CHECK(allocMemory(state, sizeof(KVMFRFrame) - 4, &shortMemory,
        &shortPointer));
  CHECK(lgmpHostQueuePost(state->queues[0], 0, shortMemory) == LGMP_OK);
  LG_TransportFrame result;
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_ERROR);
  CHECK(waitForEmpty(state));

  TestFrame invalidType;
  CHECK(newFrame(state, 1, &invalidType));
  invalidType.wire->type = FRAME_TYPE_INVALID;
  CHECK(postShared(state, &invalidType));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_ERROR);
  CHECK(waitForEmpty(state));

  TestFrame invalidOffset;
  CHECK(newFrame(state, 2, &invalidOffset));
  invalidOffset.wire->offset = UINT32_MAX;
  CHECK(postShared(state, &invalidOffset));
  CHECK(state->frameOps->nextFrame(state->transport, true, &result) ==
      LG_TRANSPORT_ERROR);
  CHECK(waitForEmpty(state));

  TestFrame invalidSize;
  CHECK(newFrame(state, 3, &invalidSize));
  invalidSize.wire->dataHeight = UINT32_MAX;
  invalidSize.wire->pitch      = UINT32_MAX;
  CHECK(postShared(state, &invalidSize));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_ERROR);
  CHECK(waitForEmpty(state));

  TestFrame malformed;
  TestFrame valid;
  CHECK(newFrame(state, 4, &malformed));
  CHECK(newFrame(state, 4, &valid));
  malformed.wire->offset               = UINT32_MAX;
  valid.wire->formatVer                = 44;
  valid.wire->scheduleEpoch            = 2;
  valid.wire->scheduleDeadlineSerial   = 9;
  CHECK(postShared(state, &malformed));
  CHECK(postOwner(state, 0, (UINT64_C(2) << 32) | 9, &valid));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 4);
  CHECK(result.scheduleOwner);
  CHECK(result.format && result.format->version == 44);
  CHECK(releaseFrame(state, &result));
  return true;
}

static bool testSerialDamage(TestState * state)
{
  CHECK(resetFrames(state));

  LG_TransportFrame result;
  TestFrame first;
  CHECK(newFrame(state, UINT32_MAX - 1U, &first));
  setDamage(&first, 2);
  CHECK(postShared(state, &first));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == UINT32_MAX - 1U);
  CHECK(result.damageRects == NULL);
  CHECK(result.damageRectsCount == 0);
  CHECK(releaseFrame(state, &result));

  TestFrame last;
  CHECK(newFrame(state, UINT32_MAX, &last));
  setDamage(&last, 2);
  last.wire->flags = FRAME_FLAG_BLOCK_SCREENSAVER |
    FRAME_FLAG_REQUEST_ACTIVATION | FRAME_FLAG_TRUNCATED;
  CHECK(postShared(state, &last));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == UINT32_MAX);
  CHECK(result.damageRectsCount == 2);
  CHECK(result.damageRects);
  CHECK(result.damageRects[1].x == 2);
  CHECK(result.damageRects[1].height == 5);
  CHECK(result.flags == (LG_TRANSPORT_FRAME_BLOCK_SCREENSAVER |
        LG_TRANSPORT_FRAME_REQUEST_ACTIVATION |
        LG_TRANSPORT_FRAME_TRUNCATED));
  CHECK(releaseFrame(state, &result));

  TestFrame wrapped;
  CHECK(newFrame(state, 0, &wrapped));
  setDamage(&wrapped, 1);
  CHECK(postShared(state, &wrapped));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 0);
  CHECK(result.damageRectsCount == 1);
  CHECK(releaseFrame(state, &result));

  TestFrame stale;
  CHECK(newFrame(state, UINT32_MAX, &stale));
  CHECK(postShared(state, &stale));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_TIMEOUT);
  CHECK(waitForEmpty(state));

  TestFrame duplicate;
  CHECK(newFrame(state, 0, &duplicate));
  CHECK(postShared(state, &duplicate));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_TIMEOUT);
  CHECK(waitForEmpty(state));

  TestFrame gap;
  CHECK(newFrame(state, 2, &gap));
  setDamage(&gap, 2);
  CHECK(postShared(state, &gap));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.damageRects == NULL);
  CHECK(result.damageRectsCount == 0);
  CHECK(releaseFrame(state, &result));

  TestFrame badDamage;
  CHECK(newFrame(state, 3, &badDamage));
  setDamage(&badDamage, KVMFR_MAX_DAMAGE_RECTS + 1U);
  CHECK(postShared(state, &badDamage));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.damageRects == NULL);
  CHECK(result.damageRectsCount == 0);
  CHECK(releaseFrame(state, &result));
  return true;
}

static bool testLaneSelection(TestState * state)
{
  CHECK(resetFrames(state));

  LG_TransportFrame result;
  TestFrame shared;
  TestFrame owner;
  CHECK(newFrame(state, 10, &shared));
  CHECK(newFrame(state, 9, &owner));
  shared.wire->formatVer = 10;
  owner.wire->formatVer  = 9;
  CHECK(postShared(state, &shared));
  CHECK(postOwner(state, 0, 1, &owner));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 10);
  CHECK(!result.scheduleOwner);
  CHECK(result.format && result.format->version == 10);
  CHECK(releaseFrame(state, &result));

  TestFrame tiedShared;
  TestFrame tiedOwner;
  CHECK(newFrame(state, 11, &tiedShared));
  CHECK(newFrame(state, 11, &tiedOwner));
  tiedShared.wire->formatVer = 11;
  tiedOwner.wire->formatVer  = 12;
  CHECK(postShared(state, &tiedShared));
  CHECK(postOwner(state, 0, (UINT64_C(5) << 32) | 7, &tiedOwner));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 11);
  CHECK(result.scheduleOwner);
  CHECK(result.scheduleEpoch == 5);
  CHECK(result.scheduleDeadlineSerial == 7);
  CHECK(result.format && result.format->version == 12);
  CHECK(releaseFrame(state, &result));

  TestFrame wrongDeadline;
  TestFrame matchingDeadline;
  CHECK(newFrame(state, 12, &wrongDeadline));
  CHECK(newFrame(state, 12, &matchingDeadline));
  wrongDeadline.wire->formatVer                 = 21;
  wrongDeadline.wire->scheduleEpoch             = 7;
  wrongDeadline.wire->scheduleDeadlineSerial    = 9;
  matchingDeadline.wire->formatVer              = 22;
  matchingDeadline.wire->scheduleEpoch          = 7;
  matchingDeadline.wire->scheduleDeadlineSerial = 9;
  CHECK(postOwner(state, 0, (UINT64_C(6) << 32) | 9, &wrongDeadline));
  CHECK(postOwner(state, 1, (UINT64_C(7) << 32) | 9, &matchingDeadline));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 12);
  CHECK(result.scheduleEpoch == 7);
  CHECK(result.scheduleDeadlineSerial == 9);
  CHECK(result.format && result.format->version == 22);
  CHECK(releaseFrame(state, &result));

  TestFrame newerShared;
  TestFrame olderOwner;
  CHECK(newFrame(state, 13, &newerShared));
  CHECK(newFrame(state, 12, &olderOwner));
  newerShared.wire->formatVer = 31;
  olderOwner.wire->formatVer  = 32;
  CHECK(postShared(state, &newerShared));
  CHECK(postOwner(state, 0, 10, &olderOwner));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 13);
  CHECK(!result.scheduleOwner);
  CHECK(result.format && result.format->version == 31);
  CHECK(releaseFrame(state, &result));

  TestFrame sharedOld;
  TestFrame ownerNew;
  CHECK(newFrame(state, 14, &sharedOld));
  CHECK(newFrame(state, 15, &ownerNew));
  sharedOld.wire->formatVer = 41;
  ownerNew.wire->formatVer  = 42;
  CHECK(postShared(state, &sharedOld));
  CHECK(postOwner(state, 0, 11, &ownerNew));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 15);
  CHECK(result.scheduleOwner);
  CHECK(result.format && result.format->version == 42);
  CHECK(releaseFrame(state, &result));

  TestFrame duplicateShared;
  CHECK(newFrame(state, 15, &duplicateShared));
  CHECK(postShared(state, &duplicateShared));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_TIMEOUT);
  CHECK(waitForEmpty(state));

  TestFrame duplicateOwner;
  CHECK(newFrame(state, 15, &duplicateOwner));
  setDamage(&duplicateOwner, 1);
  CHECK(postOwner(state, 0, 12, &duplicateOwner));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.serial == 15);
  CHECK(result.scheduleOwner);
  CHECK(result.damageRects == NULL);
  CHECK(result.damageRectsCount == 0);
  CHECK(releaseFrame(state, &result));
  return true;
}

static bool testLeases(TestState * state)
{
  CHECK(resetFrames(state));

  TestFrame         first;
  TestFrame         second;
  LG_TransportFrame firstResult;
  LG_TransportFrame secondResult;
  CHECK(newFrame(state, 30, &first));
  CHECK(newFrame(state, 31, &second));
  CHECK(postShared(state, &first));
  CHECK(state->frameOps->nextFrame(state->transport, false, &firstResult) ==
      LG_TRANSPORT_OK);
  CHECK(firstResult.releaseFn);
  CHECK(firstResult.releaseHandle != 0);
  CHECK(lgmpHostQueueMessagePending(state->queues[0], first.memory, 0));

  CHECK(postShared(state, &second));
  CHECK(state->frameOps->nextFrame(state->transport, false, &secondResult) ==
      LG_TRANSPORT_TIMEOUT);
  CHECK(lgmpHostQueueMessagePending(state->queues[0], second.memory, 0));

  firstResult.releaseFn(firstResult.releaseOpaque, firstResult.releaseHandle);
  firstResult.releaseFn(firstResult.releaseOpaque, firstResult.releaseHandle);
  CHECK(state->frameOps->nextFrame(state->transport, false, &secondResult) ==
      LG_TRANSPORT_OK);
  CHECK(secondResult.serial == 31);
  CHECK(!lgmpHostQueueMessagePending(state->queues[0], first.memory, 0));
  state->frameOps->releaseFrame(state->transport, &firstResult);
  CHECK(releaseFrame(state, &secondResult));

  TestFrame stopped;
  LG_TransportFrame stoppedResult;
  CHECK(newFrame(state, 32, &stopped));
  CHECK(postShared(state, &stopped));
  CHECK(state->frameOps->nextFrame(state->transport, false, &stoppedResult) ==
      LG_TRANSPORT_OK);
  CHECK(stoppedResult.releaseFn);
  state->frameOps->stopFrame(state->transport);
  for (unsigned i = 0; i < TEST_FRAME_QUEUES; ++i)
    CHECK(!lgmpHostQueueHasSubs(state->queues[i]));
  stoppedResult.releaseFn(stoppedResult.releaseOpaque,
      stoppedResult.releaseHandle);
  stoppedResult.releaseFn(stoppedResult.releaseOpaque,
      stoppedResult.releaseHandle);
  state->frameOps->releaseFrame(state->transport, &stoppedResult);
  CHECK(waitForEmpty(state));

  CHECK(resetFrames(state));
  TestFrame resent;
  CHECK(newFrame(state, 32, &resent));
  CHECK(postShared(state, &resent));
  CHECK(state->frameOps->nextFrame(state->transport, false, &secondResult) ==
      LG_TRANSPORT_OK);
  CHECK(secondResult.serial == 32);
  CHECK(releaseFrame(state, &secondResult));

  TestFrame disconnected;
  LG_TransportFrame disconnectedResult;
  CHECK(newFrame(state, 33, &disconnected));
  CHECK(postShared(state, &disconnected));
  CHECK(state->frameOps->nextFrame(state->transport, false,
        &disconnectedResult) == LG_TRANSPORT_OK);
  CHECK(disconnectedResult.releaseFn);
  LGT_LGMP.disconnect(state->transport);
  CHECK(!LGT_LGMP.sessionValid(state->transport));
  disconnectedResult.releaseFn(disconnectedResult.releaseOpaque,
      disconnectedResult.releaseHandle);
  state->frameOps->releaseFrame(state->transport, &disconnectedResult);
  CHECK(waitForEmpty(state));
  return true;
}

static bool testDMAUnavailable(TestState * state)
{
  CHECK(resetFrames(state));
  CHECK(!state->frameOps->supportsDMA(state->transport));

  TestFrame         frame;
  LG_TransportFrame result;
  CHECK(newFrame(state, 50, &frame));
  CHECK(postShared(state, &frame));
  CHECK(state->frameOps->nextFrame(state->transport, false, &result) ==
      LG_TRANSPORT_OK);
  CHECK(result.dmaFD == -1);
  CHECK(releaseFrame(state, &result));
  return true;
}

static bool stateInit(TestState * state)
{
  memset(state, 0, sizeof(*state));
  state->fd         = -1;
  state->hostMemory = MAP_FAILED;
  memcpy(state->path, "/tmp/lgmp-frame-test-XXXXXX",
      sizeof("/tmp/lgmp-frame-test-XXXXXX"));

  state->fd = mkstemp(state->path);
  CHECK(state->fd >= 0);
  state->pathExists = true;
  CHECK(ftruncate(state->fd, TEST_SHM_SIZE) == 0);
  state->hostMemory = mmap(NULL, TEST_SHM_SIZE,
      PROT_READ | PROT_WRITE, MAP_SHARED, state->fd, 0);
  CHECK(state->hostMemory != MAP_FAILED);

  KVMFR session = { 0 };
  memcpy(session.magic, KVMFR_MAGIC, sizeof(session.magic));
  session.version  = KVMFR_VERSION;
  session.features = KVMFR_FEATURE_FRAME_SCHEDULE;
  memcpy(session.hostver, "frame-test", sizeof("frame-test"));
  CHECK(lgmpHostInit(state->hostMemory, TEST_SHM_SIZE, &state->host,
        sizeof(session), (uint8_t *)&session) == LGMP_OK);

  for (unsigned i = 0; i < TEST_FRAME_QUEUES; ++i)
  {
    const struct LGMPQueueConfig config = {
      .queueID     = i == 0 ? LGMP_Q_FRAME : LGMP_Q_FRAME_OWNER + i - 1,
      .numMessages = LGMP_Q_FRAME_LEN,
      .subTimeout  = TEST_QUEUE_TIMEOUT,
    };
    CHECK(lgmpHostQueueNew(state->host, config, &state->queues[i]) ==
        LGMP_OK);
  }

  LGT_LGMP.setup();
  option_set_string("lgmp", "shmDevice", state->path);
  option_set_bool("lgmp", "allowDMA", true);
  option_set_int("lgmp", "framePollInterval", 0);
  option_set_int("lgmp", "cursorPollInterval", 0);
  CHECK(LGT_LGMP.create(&state->transport));

  CHECK(unlink(state->path) == 0);
  state->pathExists = false;
  usleep(300000);
  CHECK(lgmpHostProcess(state->host) == LGMP_OK);

  LG_TransportSession transportSession;
  CHECK(LGT_LGMP.connect(state->transport, &transportSession) ==
      LG_TRANSPORT_OK);
  CHECK(transportSession.features & LG_TRANSPORT_FEATURE_FRAME_SCHEDULE);
  const LG_VideoOps * videoOps = LGT_LGMP.getVideoOps(state->transport);
  CHECK(videoOps && videoOps->type == LG_VIDEO_TYPE_FRAME && videoOps->frame);
  state->frameOps = videoOps->frame;
  CHECK(state->frameOps->stopFrame);
  return true;
}

static void stateFree(TestState * state)
{
  if (state->transport)
    LGT_LGMP.destroy(&state->transport);
  option_free();
  for (unsigned i = 0; i < state->allocationCount; ++i)
    lgmpHostMemFree(&state->allocations[i]);
  lgmpHostFree(&state->host);
  if (state->hostMemory != MAP_FAILED)
    munmap(state->hostMemory, TEST_SHM_SIZE);
  if (state->fd >= 0)
    close(state->fd);
  if (state->pathExists)
    unlink(state->path);
}

typedef bool (*TestFn)(TestState * state);

static const struct
{
  const char * name;
  TestFn       run;
}
tests[] =
{
  { "malformed"    , testMalformed      },
  { "serial-damage", testSerialDamage   },
  { "lanes"        , testLaneSelection  },
  { "leases"       , testLeases         },
  { "dma"          , testDMAUnavailable },
};

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <malformed|serial-damage|lanes|leases|dma>\n",
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
  TestState state;
  const bool initialized = stateInit(&state);
  const bool passed      = initialized && test(&state);

  stateFree(&state);
  return passed ? 0 : 1;
}
