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

#include <LGProtocol/KVMFR.h>
#include <LGProtocol/KVMFRRecovery.h>
#include <LGProtocol/LGMPConfig.h>
#include "common/debug.h"
#include "common/option.h"

#include <lgmp/host.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_SHM_SIZE (2U * 1024U * 1024U)
#define TEST_TIMEOUT  20U
#define WAIT_TIMEOUT  (TEST_TIMEOUT + 100U)
#define POLL_INTERVAL 50000U

extern const LG_TransportOps LGT_LGMP;
extern void lgmp_testSetVideoStatusDispatchGate(
    LG_Transport * target, atomic_bool * waiting, atomic_bool * hold);
extern LG_RecoveryError lgmp_testRecoveryError(uint32_t error);
extern bool lgmp_testRecoveryProbeCancellation(
    LG_TransportCancelledFn cancelled, void * opaque);

static bool cancelled(void * opaque)
{
  return *(const bool *)opaque;
}

struct CancelAfter
{
  unsigned int calls;
  unsigned int limit;
};

static bool cancelAfter(void * opaque)
{
  struct CancelAfter * state = opaque;
  return ++state->calls >= state->limit;
}

#define CHECK(x) \
  do \
  { \
    if (!(x)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
      goto cleanup; \
    } \
  } \
  while (0)

static bool waitForQueuesEmpty(PLGMPHost host, PLGMPHostQueue frameQueue,
    PLGMPHostQueue pointerQueue)
{
  for (unsigned i = 0; i < WAIT_TIMEOUT; ++i)
  {
    if (lgmpHostProcess(host) != LGMP_OK)
      return false;
    if (lgmpHostQueuePending(frameQueue) == 0 &&
        lgmpHostQueuePending(pointerQueue) == 0)
      return true;
    usleep(1000);
  }

  return false;
}

struct VideoStatusTrace;

struct WaitState
{
  LG_Transport      * transport;
  const LG_FrameOps * ops;
  LG_TransportStatus  status;
  atomic_bool         done;
  bool                frame;
};

struct UnregisterState
{
  LG_Transport      * transport;
  const LG_FrameOps * ops;
  atomic_bool         started;
  atomic_bool         done;
};

struct VideoStatusTrace
{
  atomic_uint count;
  atomic_uint_fast64_t frameEpoch;
  atomic_uint_fast64_t pointerEpoch;
  atomic_int frameReason;
  atomic_int pointerReason;
  atomic_bool frameAvailable;
  atomic_bool pointerAvailable;
  atomic_bool active;
  atomic_bool overlap;
  atomic_bool block;
  atomic_bool entered;
  atomic_bool release;
  atomic_bool * waitBeforeReplace;
  atomic_bool unregisterDone;
  atomic_bool replaceDone;
  LG_Transport      * unregisterTransport;
  LG_Transport     ** destroyTransport;
  const LG_FrameOps * unregisterOps;
  struct VideoStatusTrace * registerTrace;
  struct VideoStatusTrace * replaceTrace;
  unsigned int registerAtCount;
  LG_Transport      * transport;
  const LG_FrameOps * ops;
};

static void videoStatusChanged(void * opaque,
    const LG_VideoStatus * status)
{
  struct VideoStatusTrace * trace = opaque;
  if (atomic_exchange_explicit(&trace->active, true,
        memory_order_acq_rel))
    atomic_store_explicit(&trace->overlap, true, memory_order_release);

  atomic_store_explicit(&trace->frameEpoch,
      status->frame.epoch, memory_order_relaxed);
  atomic_store_explicit(&trace->pointerEpoch,
      status->pointer.epoch, memory_order_relaxed);
  atomic_store_explicit(&trace->frameReason,
      status->frame.reason, memory_order_relaxed);
  atomic_store_explicit(&trace->pointerReason,
      status->pointer.reason, memory_order_relaxed);
  atomic_store_explicit(&trace->frameAvailable,
      status->frame.available, memory_order_relaxed);
  atomic_store_explicit(&trace->pointerAvailable,
      status->pointer.available, memory_order_relaxed);
  const unsigned int count = atomic_fetch_add_explicit(
      &trace->count, 1, memory_order_release) + 1;

  if (trace->unregisterTransport)
  {
    trace->unregisterOps->setStatusListener(
        trace->unregisterTransport, NULL, NULL);
    atomic_store_explicit(
        &trace->unregisterDone, true, memory_order_release);
  }

  if (trace->destroyTransport)
  {
    LGT_LGMP.destroy(trace->destroyTransport);
    atomic_store_explicit(
        &trace->unregisterDone, true, memory_order_release);
  }

  if (trace->registerTrace &&
      (!trace->registerAtCount || count >= trace->registerAtCount))
    trace->registerTrace->ops->setStatusListener(
        trace->registerTrace->transport, videoStatusChanged,
        trace->registerTrace);

  if (atomic_load_explicit(&trace->block, memory_order_acquire))
  {
    atomic_store_explicit(&trace->entered, true, memory_order_release);
    while (!atomic_load_explicit(&trace->release, memory_order_acquire))
      usleep(1000);
  }

  if (trace->replaceTrace)
  {
    if (trace->waitBeforeReplace)
      while (!atomic_load_explicit(
          trace->waitBeforeReplace, memory_order_acquire))
        usleep(1000);
    trace->replaceTrace->ops->setStatusListener(
        trace->replaceTrace->transport, NULL, NULL);
    trace->replaceTrace->ops->setStatusListener(
        trace->replaceTrace->transport, videoStatusChanged,
        trace->replaceTrace);
    atomic_store_explicit(
        &trace->replaceDone, true, memory_order_release);
  }

  atomic_store_explicit(&trace->active, false, memory_order_release);
}

static bool waitStatusCount(
    const struct VideoStatusTrace * trace, unsigned int count)
{
  for (unsigned i = 0; i < WAIT_TIMEOUT; ++i)
  {
    if (atomic_load_explicit(&trace->count, memory_order_acquire) >= count)
      return true;
    usleep(1000);
  }
  return false;
}

static void * disconnectVideo(void * opaque)
{
  struct VideoStatusTrace * trace = opaque;
  LGT_LGMP.disconnect(trace->transport);
  return NULL;
}

static void * disconnectVideoSignaled(void * opaque)
{
  struct VideoStatusTrace * trace = opaque;
  LGT_LGMP.disconnect(trace->transport);
  return NULL;
}

static void * registerVideoStatus(void * opaque)
{
  struct VideoStatusTrace * trace = opaque;
  trace->ops->setStatusListener(
      trace->transport, videoStatusChanged, trace);
  return NULL;
}

static void * unregisterVideoStatus(void * opaque)
{
  struct UnregisterState * state = opaque;
  atomic_store_explicit(&state->started, true, memory_order_release);
  state->ops->setStatusListener(state->transport, NULL, NULL);
  atomic_store_explicit(&state->done, true, memory_order_release);
  return NULL;
}

static void * waitForVideo(void * opaque)
{
  struct WaitState * state = opaque;
  if (state->frame)
  {
    LG_TransportFrame frame;
    state->status = state->ops->nextFrame(
        state->transport, false, &frame);
  }
  else
  {
    LG_TransportPointer pointer;
    state->status = state->ops->nextPointer(state->transport, &pointer);
  }
  atomic_store_explicit(&state->done, true, memory_order_release);
  return NULL;
}

static bool checkWaitCancellation(LG_Transport * transport,
    const LG_FrameOps * ops, PLGMPHostQueue frameQueue,
    PLGMPHostQueue pointerQueue)
{
  struct WaitState frame = {
    .transport = transport,
    .ops       = ops,
    .status    = LG_TRANSPORT_ERROR,
    .frame     = true,
  };
  struct WaitState pointer = {
    .transport = transport,
    .ops       = ops,
    .status    = LG_TRANSPORT_ERROR,
  };
  pthread_t frameThread;
  pthread_t pointerThread;
  if (pthread_create(&frameThread, NULL, waitForVideo, &frame) != 0)
    return false;
  if (pthread_create(&pointerThread, NULL, waitForVideo, &pointer) != 0)
  {
    ops->cancelFrameWait(transport);
    pthread_join(frameThread, NULL);
    return false;
  }

  bool subscribed = false;
  for (unsigned i = 0; i < WAIT_TIMEOUT; ++i)
  {
    if (lgmpHostQueueHasSubs(frameQueue) &&
        lgmpHostQueueHasSubs(pointerQueue))
    {
      subscribed = true;
      break;
    }
    usleep(1000);
  }
  ops->cancelFrameWait(transport);
  ops->cancelPointerWait(transport);
  const int frameJoin   = pthread_join(frameThread, NULL);
  const int pointerJoin = pthread_join(pointerThread, NULL);
  return subscribed && frameJoin == 0 && pointerJoin == 0 &&
    frame.status == LG_TRANSPORT_TIMEOUT &&
    pointer.status == LG_TRANSPORT_TIMEOUT;
}

int main(void)
{
  int result = 1;
  int fd = -1;
  char path[] = "/tmp/lgmp-transport-test-XXXXXX";
  bool pathExists = false;
  void * hostMemory = MAP_FAILED;
  PLGMPHost host = NULL;
  PLGMPHostQueue frameQueue = NULL;
  PLGMPHostQueue pointerQueue = NULL;
  PLGMPMemory frameMemory = NULL;
  PLGMPMemory malformedFrameMemory = NULL;
  PLGMPMemory pointerMemory = NULL;
  PLGMPMemory malformedPointerMemory = NULL;
  LG_Transport * transport = NULL;
  LG_Transport * secondTransport = NULL;
  LG_Transport * thirdTransport = NULL;
  LG_Transport * nestedTransport = NULL;
  LG_Transport * admittedTransport = NULL;
  const LG_FrameOps * frameOps = NULL;
  struct VideoStatusTrace videoTrace;
  struct VideoStatusTrace secondVideoTrace;
  memset(&videoTrace, 0, sizeof(videoTrace));
  memset(&secondVideoTrace, 0, sizeof(secondVideoTrace));

  debug_init();

  CHECK(lgmp_testRecoveryError(KVMFR_R_ERR_BUSY) ==
      LG_RECOVERY_ERR_BUSY);
  CHECK(lgmp_testRecoveryError(KVMFR_R_ERR_CAPACITY) ==
      LG_RECOVERY_ERR_CAPACITY);
  struct CancelAfter cancelDuringProbe = { .limit = 3 };
  CHECK(!lgmp_testRecoveryProbeCancellation(
        cancelAfter, &cancelDuringProbe));
  CHECK(cancelDuringProbe.calls >= cancelDuringProbe.limit);

  fd = mkstemp(path);
  CHECK(fd >= 0);
  pathExists = true;
  CHECK(ftruncate(fd, TEST_SHM_SIZE) == 0);

  hostMemory = mmap(NULL, TEST_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
      fd, 0);
  CHECK(hostMemory != MAP_FAILED);

  KVMFR session = { 0 };
  memcpy(session.magic, KVMFR_MAGIC, sizeof(session.magic));
  session.version = KVMFR_VERSION;
  memcpy(session.hostver, "transport-test", sizeof("transport-test"));

  CHECK(lgmpHostInit(hostMemory, TEST_SHM_SIZE, &host, sizeof(session),
        (uint8_t *)&session) == LGMP_OK);

  const struct LGMPQueueConfig frameConfig = {
    .queueID     = LGMP_Q_FRAME,
    .numMessages = LGMP_Q_FRAME_LEN,
    .subTimeout  = TEST_TIMEOUT,
  };
  const struct LGMPQueueConfig pointerConfig = {
    .queueID     = LGMP_Q_POINTER,
    .numMessages = LGMP_Q_POINTER_LEN,
    .subTimeout  = TEST_TIMEOUT,
  };
  const uint32_t frameSize =
    sizeof(KVMFRFrame) + sizeof(KVMFRFrameBuffer) + sizeof(uint32_t);
  CHECK(lgmpHostMemAllocAligned(host, frameSize, _Alignof(KVMFRFrame),
        &frameMemory) == LGMP_OK);
  KVMFRFrame * wireFrame = lgmpHostMemPtr(frameMemory);
  wireFrame->formatVer   = 1;
  wireFrame->frameSerial = 1;
  wireFrame->type        = FRAME_TYPE_BGRA;
  wireFrame->screenWidth     = 1;
  wireFrame->screenHeight    = 1;
  wireFrame->dataWidth       = 1;
  wireFrame->dataHeight      = 1;
  wireFrame->frameWidth      = 1;
  wireFrame->frameHeight     = 1;
  wireFrame->rotation        = FRAME_ROT_0;
  wireFrame->stride          = 1;
  wireFrame->pitch           = sizeof(uint32_t);
  wireFrame->offset          = sizeof(*wireFrame);
  wireFrame->sdrWhiteLevel   = KVMFR_SDR_WHITE_LEVEL_DEFAULT;
  wireFrame->captureTime     = 100;
  wireFrame->postProcessTime = 200;
  wireFrame->copyTime        = 300;
  wireFrame->readyTime       = 400;
  wireFrame->timingSerial    = wireFrame->frameSerial;
  wireFrame->timingValid     = 1;
  KVMFRFrameBuffer * framebuffer =
    (KVMFRFrameBuffer *)((uint8_t *)wireFrame +
      wireFrame->offset);
  atomic_store(&framebuffer->wp, sizeof(uint32_t));
  CHECK(lgmpHostMemAlloc(host, sizeof(KVMFRFrame) - 1,
        &malformedFrameMemory) == LGMP_OK);

  CHECK(lgmpHostMemAlloc(host, sizeof(KVMFRCursor), &pointerMemory) ==
      LGMP_OK);
  CHECK(lgmpHostMemAlloc(host, sizeof(KVMFRCursor),
        &malformedPointerMemory) == LGMP_OK);
  KVMFRCursor * malformedPointer = lgmpHostMemPtr(malformedPointerMemory);
  malformedPointer->height = 2;
  malformedPointer->pitch  = UINT32_MAX;
  KVMFRCursor * wirePointer = lgmpHostMemPtr(pointerMemory);
  wirePointer->x = 1;
  wirePointer->y = 1;

  LGT_LGMP.setup();
  option_set_string("lgmp", "shmDevice", path);
  option_set_bool("lgmp", "allowDMA", false);
  option_set_int("lgmp", "framePollInterval", POLL_INTERVAL);
  option_set_int("lgmp", "cursorPollInterval", POLL_INTERVAL);
  CHECK(LGT_LGMP.create(&transport));
  const LG_VideoOps * videoOps = LGT_LGMP.getVideoOps(transport);
  CHECK(videoOps);
  CHECK(videoOps->type == LG_VIDEO_TYPE_FRAME);
  CHECK(videoOps->frame);
  frameOps = videoOps->frame;
  CHECK(frameOps->cancelFrameWait);
  CHECK(frameOps->cancelPointerWait);
  CHECK(frameOps->setStatusListener);

  CHECK(LGT_LGMP.create(&secondTransport));
  const LG_VideoOps * secondVideoOps =
    LGT_LGMP.getVideoOps(secondTransport);
  CHECK(secondVideoOps && secondVideoOps->type == LG_VIDEO_TYPE_FRAME);
  CHECK(secondVideoOps->frame && secondVideoOps->frame->setStatusListener);

  CHECK(LGT_LGMP.create(&thirdTransport));
  const LG_VideoOps * thirdVideoOps =
    LGT_LGMP.getVideoOps(thirdTransport);
  CHECK(thirdVideoOps && thirdVideoOps->type == LG_VIDEO_TYPE_FRAME);
  CHECK(thirdVideoOps->frame && thirdVideoOps->frame->setStatusListener);

  CHECK(LGT_LGMP.create(&nestedTransport));
  const LG_VideoOps * nestedVideoOps =
    LGT_LGMP.getVideoOps(nestedTransport);
  CHECK(nestedVideoOps && nestedVideoOps->type == LG_VIDEO_TYPE_FRAME);
  CHECK(nestedVideoOps->frame && nestedVideoOps->frame->setStatusListener);

  CHECK(LGT_LGMP.create(&admittedTransport));
  const LG_VideoOps * admittedVideoOps =
    LGT_LGMP.getVideoOps(admittedTransport);
  CHECK(admittedVideoOps && admittedVideoOps->type == LG_VIDEO_TYPE_FRAME);
  CHECK(admittedVideoOps->frame &&
      admittedVideoOps->frame->setStatusListener);

  usleep(300000);
  CHECK(lgmpHostProcess(host) == LGMP_OK);

  LG_TransportSession transportSession;
  bool cancelConnect = true;
  CHECK(LGT_LGMP.connectCancellable(transport, &transportSession,
        cancelled, &cancelConnect) == LG_TRANSPORT_DISCONNECTED);
  CHECK(!LGT_LGMP.sessionValid(transport));
  CHECK(LGT_LGMP.connectCancellable(
        transport, &transportSession, NULL, NULL) == LG_TRANSPORT_OK);
  LG_TransportSession secondSession;
  CHECK(LGT_LGMP.connectCancellable(
        secondTransport, &secondSession, NULL, NULL) == LG_TRANSPORT_OK);
  LG_TransportSession thirdSession;
  CHECK(LGT_LGMP.connectCancellable(
        thirdTransport, &thirdSession, NULL, NULL) == LG_TRANSPORT_OK);
  LG_TransportSession nestedSession;
  CHECK(LGT_LGMP.connectCancellable(
        nestedTransport, &nestedSession, NULL, NULL) == LG_TRANSPORT_OK);
  LG_TransportSession admittedSession;
  CHECK(LGT_LGMP.connectCancellable(
        admittedTransport, &admittedSession, NULL, NULL) == LG_TRANSPORT_OK);
  videoTrace.transport = transport;
  videoTrace.ops       = frameOps;
  frameOps->setStatusListener(
      transport, videoStatusChanged, &videoTrace);
  CHECK(atomic_load_explicit(&videoTrace.count, memory_order_acquire) == 1);
  CHECK(!atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  CHECK(!atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.frameReason, memory_order_acquire) ==
      LG_TRANSPORT_UNAVAILABLE);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerReason, memory_order_acquire) ==
      LG_TRANSPORT_UNAVAILABLE);

  CHECK(lgmpHostQueueNew(host, frameConfig, &frameQueue) == LGMP_OK);
  CHECK(lgmpHostQueueNew(host, pointerConfig, &pointerQueue) == LGMP_OK);

  LG_TransportFrame frame;
  LG_TransportPointer pointer;
  CHECK(checkWaitCancellation(
        transport, frameOps, frameQueue, pointerQueue));
  CHECK(waitStatusCount(&videoTrace, 3));
  CHECK(atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.frameReason, memory_order_acquire) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerReason, memory_order_acquire) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.frameEpoch, memory_order_acquire) != 0);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire) != 0);
  CHECK(!atomic_load_explicit(&videoTrace.overlap, memory_order_acquire));
  CHECK(lgmpHostQueueHasSubs(frameQueue));
  CHECK(lgmpHostQueueHasSubs(pointerQueue));
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  CHECK(frame.epoch == atomic_load_explicit(
        &videoTrace.frameEpoch, memory_order_acquire));
  LG_TransportFrameTiming timing;
  frameOps->getFrameTiming(transport, &frame, &timing);
  CHECK(timing.captureTime == wireFrame->captureTime);
  CHECK(timing.postProcessTime == wireFrame->postProcessTime);
  CHECK(timing.copyTime == wireFrame->copyTime);
  CHECK(timing.readyTime == wireFrame->readyTime);
  CHECK(timing.providerValid);
  frameOps->releaseFrame(transport, &frame);

  wireFrame->frameSerial  = 2;
  wireFrame->timingSerial = wireFrame->frameSerial;
  wireFrame->timingValid  = 0;
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  /* Work which began before producer timing became coherent is not reported
   * as an independent provider stage. */
  wireFrame->timingValid = 1;
  frameOps->getFrameTiming(transport, &frame, &timing);
  CHECK(timing.valid);
  CHECK(!timing.providerValid);
  CHECK(timing.receiveTime == 0);
  CHECK(timing.prepareTime == 0);
  frameOps->releaseFrame(transport, &frame);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  CHECK(pointer.epoch == atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire));
  frameOps->releasePointer(transport, &pointer);

  const uint64_t frameEpoch = atomic_load_explicit(
      &videoTrace.frameEpoch, memory_order_acquire);
  const uint64_t pointerEpoch = atomic_load_explicit(
      &videoTrace.pointerEpoch, memory_order_acquire);
  CHECK(lgmpHostQueuePost(frameQueue, 0, malformedFrameMemory) == LGMP_OK);
  frameOps->cancelFrameWait(transport);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_ERROR);
  CHECK(!atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.frameEpoch, memory_order_acquire) != frameEpoch);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire) == pointerEpoch);
  CHECK(atomic_load_explicit(
        &videoTrace.frameReason, memory_order_acquire) ==
      LG_TRANSPORT_ERROR);

  CHECK(lgmpHostQueuePost(frameQueue, 0, malformedFrameMemory) == LGMP_OK);
  struct WaitState frameErrorWait =
  {
    .transport = transport,
    .ops       = frameOps,
    .status    = LG_TRANSPORT_OK,
    .frame     = true,
  };
  pthread_t frameErrorThread;
  CHECK(pthread_create(&frameErrorThread, NULL,
        waitForVideo, &frameErrorWait) == 0);
  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  usleep(POLL_INTERVAL / 10U);
  CHECK(!atomic_load_explicit(
        &frameErrorWait.done, memory_order_acquire));
  frameOps->cancelFrameWait(transport);
  CHECK(pthread_join(frameErrorThread, NULL) == 0);
  CHECK(atomic_load_explicit(&frameErrorWait.done, memory_order_acquire));
  CHECK(frameErrorWait.status == LG_TRANSPORT_ERROR);

  wireFrame->frameSerial  = 3;
  wireFrame->timingSerial = wireFrame->frameSerial;
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  frameOps->releaseFrame(transport, &frame);

  const uint64_t frameEpochAfterRecovery = atomic_load_explicit(
      &videoTrace.frameEpoch, memory_order_acquire);
  const uint64_t pointerEpochBeforeLoss = atomic_load_explicit(
      &videoTrace.pointerEpoch, memory_order_acquire);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_SHAPE,
        malformedPointerMemory) == LGMP_OK);
  frameOps->cancelPointerWait(transport);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_ERROR);
  CHECK(atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  CHECK(!atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.frameEpoch, memory_order_acquire) ==
      frameEpochAfterRecovery);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire) !=
      pointerEpochBeforeLoss);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerReason, memory_order_acquire) ==
      LG_TRANSPORT_ERROR);

  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_SHAPE,
        malformedPointerMemory) == LGMP_OK);
  struct WaitState pointerErrorWait =
  {
    .transport = transport,
    .ops       = frameOps,
    .status    = LG_TRANSPORT_OK,
  };
  pthread_t pointerErrorThread;
  CHECK(pthread_create(&pointerErrorThread, NULL,
        waitForVideo, &pointerErrorWait) == 0);
  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  usleep(POLL_INTERVAL / 10U);
  CHECK(!atomic_load_explicit(
        &pointerErrorWait.done, memory_order_acquire));
  frameOps->cancelPointerWait(transport);
  CHECK(pthread_join(pointerErrorThread, NULL) == 0);
  CHECK(atomic_load_explicit(&pointerErrorWait.done, memory_order_acquire));
  CHECK(pointerErrorWait.status == LG_TRANSPORT_ERROR);

  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  frameOps->releasePointer(transport, &pointer);

  /*
   * Stop with messages pending. Unsubscribing before the host timeout is the
   * lifecycle behavior that was lost when LGMP moved behind the transport API.
   */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(frameOps->stopFrame);
  CHECK(frameOps->stopPointer);
  frameOps->stopFrame(transport);
  frameOps->stopPointer(transport);
  CHECK(!lgmpHostQueueHasSubs(frameQueue));
  CHECK(!lgmpHostQueueHasSubs(pointerQueue));

  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  CHECK(lgmpHostQueuePending(frameQueue) == 0);
  CHECK(lgmpHostQueuePending(pointerQueue) == 0);

  frameOps->cancelFrameWait(transport);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_TIMEOUT);
  frameOps->cancelPointerWait(transport);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_TIMEOUT);
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  /* The host resends the same serial after a new subscription. */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  frameOps->releaseFrame(transport, &frame);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  frameOps->releasePointer(transport, &pointer);

  /*
   * Also recover when the host has already marked the cached handles bad.
   * LGMP leaves a timed-out handle non-NULL when unsubscribe fails.
   */
  wireFrame->frameSerial  = 4;
  wireFrame->timingSerial = wireFrame->frameSerial;
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  CHECK(lgmpHostQueueHasSubs(frameQueue));
  CHECK(lgmpHostQueueHasSubs(pointerQueue));

  frameOps->stopFrame(transport);
  frameOps->stopPointer(transport);
  frameOps->cancelFrameWait(transport);
  const uint64_t oldFrameEpoch = atomic_load_explicit(
      &videoTrace.frameEpoch, memory_order_acquire);
  const uint64_t oldPointerEpoch = atomic_load_explicit(
      &videoTrace.pointerEpoch, memory_order_acquire);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_TIMEOUT);
  CHECK(atomic_load_explicit(
        &videoTrace.frameEpoch, memory_order_acquire) != oldFrameEpoch);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire) == oldPointerEpoch);
  frameOps->cancelPointerWait(transport);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_TIMEOUT);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerEpoch, memory_order_acquire) != oldPointerEpoch);
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  /* Resetting the transport serial makes the host's resend visible. */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  const bool pointerBeforeFrameRecovery = atomic_load_explicit(
      &videoTrace.pointerAvailable, memory_order_acquire);
  CHECK(frameOps->nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.frameAvailable, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire) ==
      pointerBeforeFrameRecovery);
  frameOps->releaseFrame(transport, &frame);
  CHECK(frameOps->nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  CHECK(atomic_load_explicit(
        &videoTrace.pointerAvailable, memory_order_acquire));
  frameOps->releasePointer(transport, &pointer);

  atomic_store_explicit(&videoTrace.block, true, memory_order_release);
  pthread_t disconnectThread;
  pthread_t crossUnregisterThread;
  CHECK(pthread_create(&disconnectThread, NULL,
        disconnectVideo, &videoTrace) == 0);
  for (unsigned i = 0; i < WAIT_TIMEOUT && !atomic_load_explicit(
        &videoTrace.entered, memory_order_acquire); ++i)
    usleep(1000);
  CHECK(atomic_load_explicit(&videoTrace.entered, memory_order_acquire));
  secondVideoTrace.transport           = secondTransport;
  secondVideoTrace.ops                 = secondVideoOps->frame;
  secondVideoTrace.unregisterTransport = transport;
  secondVideoTrace.unregisterOps       = frameOps;
  CHECK(pthread_create(&crossUnregisterThread, NULL,
        registerVideoStatus, &secondVideoTrace) == 0);
  usleep(POLL_INTERVAL);
  CHECK(atomic_load_explicit(
        &secondVideoTrace.count, memory_order_acquire) == 0);
  CHECK(!atomic_load_explicit(&secondVideoTrace.unregisterDone,
        memory_order_acquire));
  atomic_store_explicit(&videoTrace.release, true, memory_order_release);
  CHECK(pthread_join(disconnectThread, NULL) == 0);
  CHECK(pthread_join(crossUnregisterThread, NULL) == 0);
  CHECK(waitStatusCount(&secondVideoTrace, 1));
  CHECK(atomic_load_explicit(&secondVideoTrace.unregisterDone,
        memory_order_acquire));
  CHECK(!atomic_load_explicit(&videoTrace.overlap, memory_order_acquire));
  CHECK(!atomic_load_explicit(
        &secondVideoTrace.overlap, memory_order_acquire));
  frameOps->setStatusListener(transport, NULL, NULL);
  secondVideoOps->frame->setStatusListener(secondTransport, NULL, NULL);

  /* A callback can unregister another instance while a callback for that
   * instance is waiting to dispatch. The queued callback revalidates its
   * listener and is skipped. */
  struct VideoStatusTrace queuedTrace;
  struct VideoStatusTrace nestedTrace;
  struct VideoStatusTrace replacementTrace;
  memset(&queuedTrace, 0, sizeof(queuedTrace));
  memset(&nestedTrace, 0, sizeof(nestedTrace));
  memset(&replacementTrace, 0, sizeof(replacementTrace));
  queuedTrace.transport = transport;
  queuedTrace.ops       = frameOps;
  nestedTrace.transport           = secondTransport;
  nestedTrace.ops                 = secondVideoOps->frame;
  nestedTrace.unregisterTransport = transport;
  nestedTrace.unregisterOps       = frameOps;
  replacementTrace.transport      = secondTransport;
  replacementTrace.ops            = secondVideoOps->frame;
  nestedTrace.registerTrace       = &replacementTrace;
  frameOps->setStatusListener(
      transport, videoStatusChanged, &queuedTrace);
  const unsigned int queuedCount = atomic_load_explicit(
      &queuedTrace.count, memory_order_acquire);
  secondVideoOps->frame->setStatusListener(
      secondTransport, videoStatusChanged, &nestedTrace);
  CHECK(atomic_load_explicit(
        &nestedTrace.unregisterDone, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &replacementTrace.count, memory_order_acquire) == 1);
  LGT_LGMP.disconnect(transport);
  CHECK(atomic_load_explicit(
        &queuedTrace.count, memory_order_acquire) == queuedCount);

  /* Re-registering the identical callback pair creates a new listener
   * lifetime. A publication queued for the prior lifetime must not be
   * delivered after that replacement. */
  struct VideoStatusTrace gateTrace;
  struct VideoStatusTrace abaTrace;
  memset(&gateTrace, 0, sizeof(gateTrace));
  memset(&abaTrace, 0, sizeof(abaTrace));
  gateTrace.transport = transport;
  gateTrace.ops       = frameOps;
  abaTrace.transport  = secondTransport;
  abaTrace.ops        = secondVideoOps->frame;
  frameOps->setStatusListener(transport, NULL, NULL);
  secondVideoOps->frame->setStatusListener(
      secondTransport, videoStatusChanged, &abaTrace);
  const unsigned int abaCount = atomic_load_explicit(
      &abaTrace.count, memory_order_acquire);
  gateTrace.block = true;
  atomic_bool abaWaiting = false;
  gateTrace.replaceTrace      = &abaTrace;
  gateTrace.waitBeforeReplace = &abaWaiting;
  pthread_t gateThread;
  CHECK(pthread_create(&gateThread, NULL,
        registerVideoStatus, &gateTrace) == 0);
  for (unsigned i = 0; i < WAIT_TIMEOUT && !atomic_load_explicit(
        &gateTrace.entered, memory_order_acquire); ++i)
    usleep(1000);
  CHECK(atomic_load_explicit(&gateTrace.entered, memory_order_acquire));
  atomic_bool abaHold = true;
  lgmp_testSetVideoStatusDispatchGate(
      secondTransport, &abaWaiting, &abaHold);
  pthread_t abaPublishThread;
  CHECK(pthread_create(&abaPublishThread, NULL,
        disconnectVideoSignaled, &abaTrace) == 0);
  for (unsigned i = 0; i < WAIT_TIMEOUT && !atomic_load_explicit(
        &abaWaiting, memory_order_acquire); ++i)
    usleep(1000);
  CHECK(atomic_load_explicit(&abaWaiting, memory_order_acquire));
  atomic_store_explicit(&gateTrace.release, true, memory_order_release);
  usleep(POLL_INTERVAL);
  CHECK(atomic_load_explicit(
        &gateTrace.replaceDone, memory_order_acquire));

  /* External unregister waits for the stale accepted dispatch to drain. */
  struct UnregisterState unregisterState =
  {
    .transport = secondTransport,
    .ops       = secondVideoOps->frame,
  };
  pthread_t unregisterThread;
  CHECK(pthread_create(&unregisterThread, NULL,
        unregisterVideoStatus, &unregisterState) == 0);
  for (unsigned i = 0; i < WAIT_TIMEOUT && !atomic_load_explicit(
        &unregisterState.started, memory_order_acquire); ++i)
    usleep(1000);
  CHECK(atomic_load_explicit(
        &unregisterState.started, memory_order_acquire));
  usleep(POLL_INTERVAL);
  CHECK(!atomic_load_explicit(
        &unregisterState.done, memory_order_acquire));
  atomic_store_explicit(&abaHold, false, memory_order_release);
  CHECK(pthread_join(gateThread, NULL) == 0);
  CHECK(pthread_join(abaPublishThread, NULL) == 0);
  CHECK(pthread_join(unregisterThread, NULL) == 0);
  CHECK(atomic_load_explicit(&gateTrace.replaceDone, memory_order_acquire));
  CHECK(atomic_load_explicit(&unregisterState.done, memory_order_acquire));
  lgmp_testSetVideoStatusDispatchGate(NULL, NULL, NULL);
  CHECK(atomic_load_explicit(
        &abaTrace.count, memory_order_acquire) == abaCount + 1);

  /* Symmetric cross-instance unregisters are safe when both registrations
   * start together. Both callbacks run sequentially and complete their
   * cross-instance unregister. */
  struct VideoStatusTrace symmetricA;
  struct VideoStatusTrace symmetricB;
  memset(&symmetricA, 0, sizeof(symmetricA));
  memset(&symmetricB, 0, sizeof(symmetricB));
  symmetricA.transport           = transport;
  symmetricA.ops                 = frameOps;
  symmetricA.unregisterTransport = secondTransport;
  symmetricA.unregisterOps       = secondVideoOps->frame;
  symmetricB.transport           = secondTransport;
  symmetricB.ops                 = secondVideoOps->frame;
  symmetricB.unregisterTransport = transport;
  symmetricB.unregisterOps       = frameOps;
  pthread_t symmetricThreadA;
  pthread_t symmetricThreadB;
  CHECK(pthread_create(&symmetricThreadA, NULL,
        registerVideoStatus, &symmetricA) == 0);
  CHECK(pthread_create(&symmetricThreadB, NULL,
        registerVideoStatus, &symmetricB) == 0);
  CHECK(pthread_join(symmetricThreadA, NULL) == 0);
  CHECK(pthread_join(symmetricThreadB, NULL) == 0);
  CHECK(atomic_load_explicit(
        &symmetricA.count, memory_order_acquire) == 1);
  CHECK(atomic_load_explicit(
        &symmetricB.count, memory_order_acquire) == 1);
  CHECK(atomic_load_explicit(
        &symmetricA.unregisterDone, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &symmetricB.unregisterDone, memory_order_acquire));

  /* A callback may destroy the instance on which an admitted callback is
   * active. Physical teardown is deferred until that callback retires. */
  frameOps->setStatusListener(transport, NULL, NULL);
  secondVideoOps->frame->setStatusListener(secondTransport, NULL, NULL);
  admittedVideoOps->frame->setStatusListener(admittedTransport, NULL, NULL);
  struct VideoStatusTrace admittedDestroy;
  struct VideoStatusTrace admittedOuter;
  memset(&admittedDestroy, 0, sizeof(admittedDestroy));
  memset(&admittedOuter, 0, sizeof(admittedOuter));
  admittedDestroy.transport        = secondTransport;
  admittedDestroy.ops              = secondVideoOps->frame;
  admittedDestroy.destroyTransport = &admittedTransport;
  admittedOuter.transport          = admittedTransport;
  admittedOuter.ops                = admittedVideoOps->frame;
  admittedOuter.registerTrace      = &admittedDestroy;
  admittedOuter.registerAtCount    = 2;
  admittedVideoOps->frame->setStatusListener(
      admittedTransport, videoStatusChanged, &admittedOuter);
  CHECK(atomic_load_explicit(
        &admittedOuter.count, memory_order_acquire) == 1);
  LGT_LGMP.disconnect(admittedTransport);
  CHECK(!admittedTransport);
  CHECK(atomic_load_explicit(
        &admittedOuter.count, memory_order_acquire) == 2);
  CHECK(atomic_load_explicit(
        &admittedDestroy.count, memory_order_acquire) == 1);

  /* A synchronous callback stays pinned while it drops serialization to
   * drain a different instance, including when a third callback destroys it.
   */
  thirdVideoOps->frame->setStatusListener(thirdTransport, NULL, NULL);
  nestedVideoOps->frame->setStatusListener(nestedTransport, NULL, NULL);
  struct VideoStatusTrace drainTrace;
  struct VideoStatusTrace pinnedTrace;
  struct VideoStatusTrace thirdTrace;
  memset(&drainTrace, 0, sizeof(drainTrace));
  memset(&pinnedTrace, 0, sizeof(pinnedTrace));
  memset(&thirdTrace, 0, sizeof(thirdTrace));
  drainTrace.transport = thirdTransport;
  drainTrace.ops       = thirdVideoOps->frame;
  thirdVideoOps->frame->setStatusListener(
      thirdTransport, videoStatusChanged, &drainTrace);
  atomic_bool drainWaiting = false;
  atomic_bool drainHold = true;
  lgmp_testSetVideoStatusDispatchGate(
      thirdTransport, &drainWaiting, &drainHold);
  pthread_t drainThread;
  CHECK(pthread_create(&drainThread, NULL,
        disconnectVideoSignaled, &drainTrace) == 0);
  for (unsigned i = 0; i < WAIT_TIMEOUT && !atomic_load_explicit(
        &drainWaiting, memory_order_acquire); ++i)
    usleep(1000);
  CHECK(atomic_load_explicit(&drainWaiting, memory_order_acquire));

  pinnedTrace.transport           = transport;
  pinnedTrace.ops                 = frameOps;
  pinnedTrace.unregisterTransport = thirdTransport;
  pinnedTrace.unregisterOps       = thirdVideoOps->frame;
  pthread_t pinnedThread;
  CHECK(pthread_create(&pinnedThread, NULL,
        registerVideoStatus, &pinnedTrace) == 0);
  CHECK(waitStatusCount(&pinnedTrace, 1));

  thirdTrace.transport = nestedTransport;
  thirdTrace.ops       = nestedVideoOps->frame;
  nestedVideoOps->frame->setStatusListener(
      nestedTransport, videoStatusChanged, &thirdTrace);
  thirdTrace.destroyTransport = &transport;
  pthread_t thirdThread;
  CHECK(pthread_create(&thirdThread, NULL,
        disconnectVideoSignaled, &thirdTrace) == 0);
  CHECK(waitStatusCount(&thirdTrace, 2));
  atomic_store_explicit(&drainHold, false, memory_order_release);
  CHECK(pthread_join(drainThread, NULL) == 0);
  CHECK(pthread_join(pinnedThread, NULL) == 0);
  CHECK(pthread_join(thirdThread, NULL) == 0);
  lgmp_testSetVideoStatusDispatchGate(NULL, NULL, NULL);
  CHECK(!transport);
  CHECK(atomic_load_explicit(
        &pinnedTrace.unregisterDone, memory_order_acquire));
  CHECK(atomic_load_explicit(
        &thirdTrace.unregisterDone, memory_order_acquire));

  /* The same lifetime rule applies when nested synchronous callbacks destroy
   * the instance belonging to the enclosing callback. */
  secondVideoOps->frame->setStatusListener(secondTransport, NULL, NULL);
  struct VideoStatusTrace syncDestroy;
  struct VideoStatusTrace syncOuter;
  memset(&syncDestroy, 0, sizeof(syncDestroy));
  memset(&syncOuter, 0, sizeof(syncOuter));
  syncDestroy.transport        = secondTransport;
  syncDestroy.ops              = secondVideoOps->frame;
  syncDestroy.destroyTransport = &thirdTransport;
  syncOuter.transport          = thirdTransport;
  syncOuter.ops                = thirdVideoOps->frame;
  syncOuter.registerTrace      = &syncDestroy;
  thirdVideoOps->frame->setStatusListener(
      thirdTransport, videoStatusChanged, &syncOuter);
  CHECK(!thirdTransport);
  CHECK(atomic_load_explicit(
        &syncOuter.count, memory_order_acquire) == 1);
  CHECK(atomic_load_explicit(
        &syncDestroy.count, memory_order_acquire) == 1);

  result = 0;

cleanup:
  if (admittedTransport)
    LGT_LGMP.destroy(&admittedTransport);
  if (nestedTransport)
    LGT_LGMP.destroy(&nestedTransport);
  if (thirdTransport)
    LGT_LGMP.destroy(&thirdTransport);
  if (secondTransport)
    LGT_LGMP.destroy(&secondTransport);
  if (transport)
    LGT_LGMP.destroy(&transport);
  option_free();
  lgmpHostMemFree(&pointerMemory);
  lgmpHostMemFree(&malformedPointerMemory);
  lgmpHostMemFree(&malformedFrameMemory);
  lgmpHostMemFree(&frameMemory);
  lgmpHostFree(&host);
  if (hostMemory != MAP_FAILED)
    munmap(hostMemory, TEST_SHM_SIZE);
  if (fd >= 0)
    close(fd);
  if (pathExists)
    unlink(path);
  return result;
}
