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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_SHM_SIZE (2U * 1024U * 1024U)
#define TEST_TIMEOUT  20U
#define WAIT_TIMEOUT  (TEST_TIMEOUT + 100U)

extern const LG_TransportOps LGT_LGMP;

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
  PLGMPMemory pointerMemory = NULL;
  LG_Transport * transport = NULL;

  debug_init();

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
  CHECK(lgmpHostQueueNew(host, frameConfig, &frameQueue) == LGMP_OK);
  CHECK(lgmpHostQueueNew(host, pointerConfig, &pointerQueue) == LGMP_OK);

  const uint32_t frameSize =
    sizeof(KVMFRFrame) + sizeof(FrameBuffer) + sizeof(uint32_t);
  CHECK(lgmpHostMemAlloc(host, frameSize, &frameMemory) == LGMP_OK);
  KVMFRFrame * wireFrame = lgmpHostMemPtr(frameMemory);
  wireFrame->formatVer   = 1;
  wireFrame->frameSerial = 1;
  wireFrame->type        = FRAME_TYPE_BGRA;
  wireFrame->screenWidth = 1;
  wireFrame->screenHeight = 1;
  wireFrame->dataWidth   = 1;
  wireFrame->dataHeight  = 1;
  wireFrame->frameWidth  = 1;
  wireFrame->frameHeight = 1;
  wireFrame->rotation    = FRAME_ROT_0;
  wireFrame->stride      = 1;
  wireFrame->pitch       = sizeof(uint32_t);
  wireFrame->offset      = sizeof(*wireFrame);
  wireFrame->sdrWhiteLevel = KVMFR_SDR_WHITE_LEVEL_DEFAULT;
  FrameBuffer * framebuffer = (FrameBuffer *)((uint8_t *)wireFrame +
      wireFrame->offset);
  atomic_store(&framebuffer->wp, sizeof(uint32_t));

  CHECK(lgmpHostMemAlloc(host, sizeof(KVMFRCursor), &pointerMemory) ==
      LGMP_OK);
  KVMFRCursor * wirePointer = lgmpHostMemPtr(pointerMemory);
  wirePointer->x = 1;
  wirePointer->y = 1;

  LGT_LGMP.setup();
  option_set_string("lgmp", "shmDevice", path);
  option_set_bool("lgmp", "allowDMA", false);
  option_set_int("lgmp", "framePollInterval", 0);
  option_set_int("lgmp", "cursorPollInterval", 0);
  CHECK(LGT_LGMP.create(&transport));

  CHECK(unlink(path) == 0);
  pathExists = false;

  usleep(300000);
  CHECK(lgmpHostProcess(host) == LGMP_OK);

  LG_TransportSession transportSession;
  CHECK(LGT_LGMP.connect(transport, &transportSession) == LG_TRANSPORT_OK);

  LG_TransportFrame frame;
  LG_TransportPointer pointer;
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_TIMEOUT);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_TIMEOUT);
  CHECK(lgmpHostQueueHasSubs(frameQueue));
  CHECK(lgmpHostQueueHasSubs(pointerQueue));
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  LGT_LGMP.releaseFrame(transport, &frame);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  LGT_LGMP.releasePointer(transport, &pointer);

  /*
   * Stop with messages pending. Unsubscribing before the host timeout is the
   * lifecycle behavior that was lost when LGMP moved behind the transport API.
   */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(LGT_LGMP.stopFrame);
  CHECK(LGT_LGMP.stopPointer);
  LGT_LGMP.stopFrame(transport);
  LGT_LGMP.stopPointer(transport);
  CHECK(!lgmpHostQueueHasSubs(frameQueue));
  CHECK(!lgmpHostQueueHasSubs(pointerQueue));

  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  CHECK(lgmpHostQueuePending(frameQueue) == 0);
  CHECK(lgmpHostQueuePending(pointerQueue) == 0);

  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_TIMEOUT);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_TIMEOUT);
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  /* The host resends the same serial after a new subscription. */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  LGT_LGMP.releaseFrame(transport, &frame);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  LGT_LGMP.releasePointer(transport, &pointer);

  /*
   * Also recover when the host has already marked the cached handles bad.
   * LGMP leaves a timed-out handle non-NULL when unsubscribe fails.
   */
  wireFrame->frameSerial = 2;
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  CHECK(waitForQueuesEmpty(host, frameQueue, pointerQueue));
  CHECK(lgmpHostQueueHasSubs(frameQueue));
  CHECK(lgmpHostQueueHasSubs(pointerQueue));

  LGT_LGMP.stopFrame(transport);
  LGT_LGMP.stopPointer(transport);
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_TIMEOUT);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_TIMEOUT);
  CHECK(lgmpHostQueueNewSubs(frameQueue) == 1);
  CHECK(lgmpHostQueueNewSubs(pointerQueue) == 1);

  /* Resetting the transport serial makes the host's resend visible. */
  CHECK(lgmpHostQueuePost(frameQueue, 0, frameMemory) == LGMP_OK);
  CHECK(lgmpHostQueuePost(pointerQueue, CURSOR_FLAG_POSITION,
        pointerMemory) == LGMP_OK);
  CHECK(LGT_LGMP.nextFrame(transport, false, &frame) == LG_TRANSPORT_OK);
  LGT_LGMP.releaseFrame(transport, &frame);
  CHECK(LGT_LGMP.nextPointer(transport, &pointer) == LG_TRANSPORT_OK);
  LGT_LGMP.releasePointer(transport, &pointer);

  result = 0;

cleanup:
  if (transport)
    LGT_LGMP.destroy(&transport);
  option_free();
  lgmpHostMemFree(&pointerMemory);
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
