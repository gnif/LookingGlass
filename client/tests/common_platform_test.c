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

#include "test.h"

#include "common/debug.h"
#include "common/event.h"
#include "common/filesystem.h"
#include "common/random.h"
#include "common/semaphore.h"
#include "common/stringutils.h"
#include "common/thread.h"
#include "common/time.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_WAIT_NS UINT64_C(1000000000)

struct SignalState
{
  LGEvent * event;
};

struct TimerState
{
  LGEvent    * event;
  atomic_uint  calls;
};

struct SemaphoreState
{
  LGSemaphore * semaphore;
  LGEvent     * complete;
};

static int signalThread(void * opaque)
{
  const struct SignalState * state = opaque;
  nsleep(UINT64_C(2000000));
  CHECK(lgSignalEvent(state->event));
  return 0;
}

static bool timerCallback(void * opaque)
{
  struct TimerState * state = opaque;
  const unsigned int calls = atomic_fetch_add_explicit(
      &state->calls, 1, memory_order_relaxed) + 1;
  if (calls < 2)
    return true;

  CHECK(lgSignalEvent(state->event));
  return false;
}

static int semaphoreThread(void * opaque)
{
  const struct SemaphoreState * state = opaque;
  CHECK(lgWaitSemaphore(state->semaphore));
  CHECK(lgSignalEvent(state->complete));
  return 0;
}

static void testEventWaits(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  CHECK(event);

  const uint64_t timeoutStart = nanotime();
  CHECK(!lgWaitEventNS(event, UINT64_C(1000000)));
  CHECK(nanotime() >= timeoutStart);

  const struct SignalState state = { .event = event };
  LGThread * thread;
  CHECK(lgCreateThread("eventSignal", signalThread, (void *)&state, &thread));
  CHECK(lgWaitEventDeadline(event, nanotime() + TEST_WAIT_NS));
  CHECK(lgJoinThread(thread, NULL));

  CHECK(!lgWaitEvent(event, 0));
  lgFreeEvent(event);
}

static void testManualReset(void)
{
  LGEvent * event = lgCreateEvent(false, 0);
  CHECK(event);
  CHECK(lgSignalEvent(event));
  CHECK(lgWaitEvent(event, 0));
  CHECK(lgWaitEvent(event, 0));
  CHECK(lgResetEvent(event));
  CHECK(!lgWaitEvent(event, 0));
  lgFreeEvent(event);
}

static void testTimer(void)
{
  LGEvent * event = lgCreateEvent(true, 0);
  CHECK(event);

  struct TimerState state = { .event = event };
  atomic_init(&state.calls, 0);

  LGTimer * timer;
  CHECK(lgCreateTimer(1, timerCallback, &state, &timer));
  CHECK(lgWaitEventNS(event, TEST_WAIT_NS));
  lgTimerDestroy(timer);
  CHECK(atomic_load_explicit(&state.calls, memory_order_relaxed) == 2);
  lgFreeEvent(event);
}

static void testSemaphore(void)
{
  LGSemaphore * semaphore = lgCreateSemaphore(0);
  LGEvent     * complete  = lgCreateEvent(true, 0);
  CHECK(semaphore);
  CHECK(complete);

  const struct SemaphoreState state =
  {
    .semaphore = semaphore,
    .complete  = complete,
  };
  LGThread * thread;
  CHECK(lgCreateThread(
        "semaphore", semaphoreThread, (void *)&state, &thread));
  CHECK(!lgWaitEventNS(complete, UINT64_C(1000000)));
  CHECK(lgSignalSemaphore(semaphore));
  CHECK(lgWaitEventNS(complete, TEST_WAIT_NS));
  CHECK(lgJoinThread(thread, NULL));

  CHECK(lgSignalSemaphore(semaphore));
  CHECK(lgSignalSemaphore(semaphore));
  CHECK(lgWaitSemaphore(semaphore));
  CHECK(lgWaitSemaphore(semaphore));
  lgFreeEvent(complete);
  lgFreeSemaphore(semaphore);
}

static void testRandom(void)
{
  uint8_t first [32] = { 0 };
  uint8_t second[32] = { 0 };
  CHECK(lgRandomBytes(first, sizeof(first)));
  CHECK(lgRandomBytes(second, sizeof(second)));
  CHECK(memcmp(first, second, sizeof(first)) != 0);
}

static void testFilesystem(void)
{
  char * directory;
  char * file;
  CHECK(alloc_sprintf(&directory, "%s/filesystem-test", LG_TEST_TMP_DIR) > 0);
  CHECK(alloc_sprintf(&file, "%s/entry.txt", directory) > 0);

  lgDeleteFile(file);
  lgDeleteDirectory(directory);
  CHECK(lgPathType(directory) == LG_FILE_TYPE_MISSING);
  CHECK(lgCreateDirectory(directory));
  CHECK(lgCreateDirectory(directory));
  CHECK(lgPathType(directory) == LG_FILE_TYPE_DIRECTORY);

  FILE * stream = lgOpenFile(file, "wb");
  CHECK(stream);
  CHECK(fputs("Looking Glass", stream) >= 0);
  CHECK(fclose(stream) == 0);
  CHECK(lgPathType(file) == LG_FILE_TYPE_REGULAR);

  StringList entries = lgListDirectory(directory);
  bool       found   = false;
  CHECK(entries);
  for (unsigned int i = 0; i < stringlist_count(entries); ++i)
    if (!strcmp(stringlist_at(entries, i), "entry.txt"))
      found = true;
  CHECK(found);
  stringlist_free(&entries);

  CHECK(lgDeleteFile(file));
  CHECK(lgDeleteDirectory(directory));
  CHECK(lgPathType(directory) == LG_FILE_TYPE_MISSING);
  free(file);
  free(directory);
}

int main(void)
{
  debug_init();
  lgTimeInit();
  testEventWaits();
  testManualReset();
  testTimer();
  testSemaphore();
  testRandom();
  testFilesystem();
  return 0;
}
