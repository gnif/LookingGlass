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

#include "../platform.h"

#include "common/debug.h"
#include "common/ivshmem_dmabuf.h"
#include "common/option.h"
#include "common/stringutils.h"

#include <dirent.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct LGMPExternalFrame
{
  LG_ExternalFrame base;
  atomic_uint      refs;
  int              fd;
}
LGMPExternalFrame;

static void lgmpExternalFrameRetain(LG_ExternalFrame * frame)
{
  LGMPExternalFrame  * this = (LGMPExternalFrame *)frame;
  const unsigned int   refs = atomic_fetch_add_explicit(
      &this->refs, 1, memory_order_relaxed);
  DEBUG_ASSERT(refs > 0 && refs < UINT_MAX);
}

static void lgmpExternalFrameRelease(LG_ExternalFrame * frame)
{
  LGMPExternalFrame  * this = (LGMPExternalFrame *)frame;
  const unsigned int   refs = atomic_fetch_sub_explicit(
      &this->refs, 1, memory_order_acq_rel);
  DEBUG_ASSERT(refs > 0);
  if (refs != 1)
    return;

  close(this->fd);
  free(this);
}

static bool lgmpExternalFrameQuery(const LG_ExternalFrame * frame,
    void * result, size_t resultSize)
{
  if (!result || resultSize != sizeof(int))
    return false;

  const LGMPExternalFrame * this = (const LGMPExternalFrame *)frame;
  *(int *)result = this->fd;
  return true;
}

static const LG_ExternalFrameOps lgmpExternalFrameOps =
{
  .type    = LG_EXTERNAL_FRAME_DMABUF,
  .retain  = lgmpExternalFrameRetain,
  .release = lgmpExternalFrameRelease,
  .query   = lgmpExternalFrameQuery,
};

static bool lgmpDeviceValidator(struct Option * opt, const char ** error)
{
  const char * transport = option_get_string("app", "transport");
  if (!transport || strcmp(transport, "lgmp") != 0)
    return true;

  if (strlen(opt->value.x_string) > 3 &&
      strncmp(opt->value.x_string, "kvmfr", 5) != 0)
  {
    struct stat st;
    if (stat(opt->value.x_string, &st) != 0)
    {
      *error = "Invalid path to the shared memory file";
      return false;
    }
  }
  return true;
}

static StringList lgmpDeviceValues(struct Option * option)
{
  StringList values = stringlist_new(true);
  DIR      * dir    = opendir("/sys/class/kvmfr");
  if (!dir)
    return values;

  struct dirent * entry;
  while ((entry = readdir(dir)))
  {
    if (entry->d_name[0] == '.')
      continue;

    char * name;
    alloc_sprintf(&name, "/dev/%s", entry->d_name);
    stringlist_push(values, name);
  }

  closedir(dir);
  return values;
}

void lgmpPlatformOptionsInit(void)
{
  struct stat  st;
  const char * defaultDevice = stat("/dev/kvmfr0", &st) == 0 ?
    "/dev/kvmfr0" : "/dev/shm/looking-glass";

  static struct Option options[] =
  {
    {
      .module         = "lgmp",
      .name           = "shmDevice",
      .old_module     = "app",
      .old_name       = "shmFile",
      .shortopt       = 'f',
      .description    = "Shared memory file or KVMFR device path",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = NULL,
      .validator      = lgmpDeviceValidator,
      .getValues      = lgmpDeviceValues,
    },
    {0}
  };

  options[0].value.x_string = (char *)defaultDevice;
  option_register(options);
}

bool lgmpPlatformOpenIVSHMEM(struct IVSHMEM * shm)
{
  if (!ivshmemInit(shm))
    return false;

  if (ivshmemOpenDev(shm, option_get_string("lgmp", "shmDevice"),
        IVSHMEM_CACHE_MODE_CACHED))
    return true;

  ivshmemFree(shm);
  return false;
}

bool lgmpPlatformSupportsExternalFrame(struct IVSHMEM * shm)
{
  return ivshmemHasDMA(shm);
}

LG_ExternalFrame * lgmpPlatformCreateExternalFrame(struct IVSHMEM * shm,
    size_t offset, size_t size)
{
  const int fd = ivshmemGetDMABuf(shm, offset, size);
  if (fd < 0)
    return NULL;

  LGMPExternalFrame * frame = malloc(sizeof(*frame));
  if (!frame)
  {
    close(fd);
    return NULL;
  }

  frame->base.ops = &lgmpExternalFrameOps;
  frame->fd       = fd;
  atomic_init(&frame->refs, 1);
  return &frame->base;
}
