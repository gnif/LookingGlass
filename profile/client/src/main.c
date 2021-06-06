/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/debug.h"
#include "common/option.h"
#include "common/crash.h"
#include "common/KVMFR.h"
#include "common/locking.h"
#include "common/stringutils.h"
#include "common/ivshmem.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <pwd.h>
#include <string.h>
#include <time.h>

#include <lgmp/client.h>

#define min(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a < _b ? _a : _b; })
#define max(a,b) ({ __typeof__ (a) _a = (a); __typeof__ (b) _b = (b); _a > _b ? _a : _b; })

struct state
{
  bool           running;
  struct IVSHMEM shmDev;
};

struct state state;

static struct Option options[] =
{
  {
    .module         = "app",
    .name           = "configFile",
    .description    = "A file to read additional configuration from",
    .shortopt       = 'C',
    .type           = OPTION_TYPE_STRING,
    .value.x_string = NULL
  },
  {0}
};

static bool config_load(int argc, char * argv[])
{
  // load any global options first
  struct stat st;
  if (stat("/etc/looking-glass-client.ini", &st) >= 0)
  {
    DEBUG_INFO("Loading config from: /etc/looking-glass-client.ini");
    if (!option_load("/etc/looking-glass-client.ini"))
      return false;
  }

  // load user's local options
  struct passwd * pw = getpwuid(getuid());
  char * localFile;
  alloc_sprintf(&localFile, "%s/.looking-glass-client.ini", pw->pw_dir);
  if (stat(localFile, &st) >= 0)
  {
    DEBUG_INFO("Loading config from: %s", localFile);
    if (!option_load(localFile))
    {
      free(localFile);
      return false;
    }
  }
  free(localFile);

  if (!option_parse(argc, argv))
    return false;

  // if a file was specified to also load, do it
  const char * configFile = option_get_string("app", "configFile");
  if (configFile)
  {
    DEBUG_INFO("Loading config from: %s", configFile);
    if (!option_load(configFile))
      return false;
  }

  if (!option_validate())
    return false;

  return true;
}

static int run(void)
{
  PLGMPClient      lgmp;
  PLGMPClientQueue frameQueue;

  uint32_t udataSize;
  KVMFR *udata;

  LGMP_STATUS status;
  if ((status = lgmpClientInit(state.shmDev.mem, state.shmDev.size, &lgmp,
          &udataSize, (uint8_t **)&udata)) != LGMP_OK)
  {
    DEBUG_ERROR("lgmpClientInit: %s", lgmpStatusString(status));
    return -1;
  }

  if (udataSize != sizeof(KVMFR) ||
      memcmp(udata->magic, KVMFR_MAGIC, sizeof(udata->magic)) != 0 ||
      udata->version != KVMFR_VERSION)
  {
    DEBUG_BREAK();
    DEBUG_ERROR("The host application is not compatible with this client");
    DEBUG_ERROR("Expected KVMFR version %d", KVMFR_VERSION);
    DEBUG_BREAK();
    return -1;
  }

  if ((status = lgmpClientSubscribe(lgmp, LGMP_Q_FRAME, &frameQueue) != LGMP_OK))
  {
    DEBUG_ERROR("lgmpClientSubscribe: %s", lgmpStatusString(status));
    return -1;
  }

  struct perf
  {
    uint64_t min, max, ttl;
    unsigned int count;
  };

  unsigned int frameCount    = 0;
  uint64_t     lastFrameTime = 0;
  struct perf  p1  = {};
  struct perf  p5  = {};
  struct perf  p10 = {};
  struct perf  p30 = {};

  // start accepting frames
  while(state.running)
  {
    LGMPMessage msg;
    if ((status = lgmpClientProcess(frameQueue, &msg)) != LGMP_OK)
    {
      if (status == LGMP_ERR_QUEUE_EMPTY)
        continue;

      DEBUG_ERROR("lgmpClientProcess: %s", lgmpStatusString(status));
      return -1;
    }

    lgmpClientMessageDone(frameQueue);

    uint64_t frameTime = nanotime();
    uint64_t diff = frameTime - lastFrameTime;

    if (frameCount++ == 0)
    {
      lastFrameTime = frameTime;
      p1.min = p5.min = p10.min = p30.min = diff;
      continue;
    }

    ++p1 .count;
    ++p5 .count;
    ++p10.count;
    ++p30.count;

#define UPDATE(p, interval) \
    if (p.ttl + diff >= (1e9 * interval)) \
    { \
      fprintf(stdout, "%02d, min:%9lu ns (%5.2f ms) max:%9lu ns (%5.2f ms) avg:%9lu ns (%5.2f ms)\n", \
          interval, \
          p.min          , ((float)p.min / 1e6f), \
          p.max          , ((float)p.max / 1e6f), \
          p.ttl / p.count, (((float)p.ttl / p.count) / 1e6f)\
      ); \
      p.min = p.max = p.ttl = diff; p.count = 1; \
    } \
    else \
    { \
      p.min = min(p.min, diff); \
      p.max = max(p.max, diff); \
      p.ttl += diff; \
    }

    UPDATE(p1 , 1 );
    UPDATE(p5 , 5 );
    UPDATE(p10, 10);
    UPDATE(p30, 30);

    lastFrameTime = frameTime;
  }

  return 0;
}

int main(int argc, char * argv[])
{
  DEBUG_INFO("Looking Glass (" BUILD_VERSION ") - Client Profiler");

  if (!installCrashHandler("/proc/self/exe"))
    DEBUG_WARN("Failed to install the crash handler");

  option_register(options);
  ivshmemOptionsInit();

  if (!config_load(argc, argv))
  {
    option_free();
    return -1;
  }

  // init the global state vars
  state.running = true;

  int ret = -1;
  if (ivshmemOpen(&state.shmDev))
    ret = run();

  ivshmemClose(&state.shmDev);
  option_free();
  return ret;
}
