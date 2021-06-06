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

#include "common/ivshmem.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "common/debug.h"
#include "common/option.h"
#include "common/stringutils.h"
#include "module/kvmfr.h"

struct IVSHMEMInfo
{
  int  devFd;
  int  size;
  bool hasDMA;
};

static bool ivshmemDeviceValidator(struct Option * opt, const char ** error)
{
  // if it's not a kvmfr device, it must be a file on disk
  if (strlen(opt->value.x_string) > 3 && memcmp(opt->value.x_string, "kvmfr", 5) != 0)
  {
    struct stat st;
    if (stat(opt->value.x_string, &st) != 0)
    {
      *error = "Invalid path to the ivshmem file specified";
      return false;
    }
    return true;
  }

  return true;
}

static StringList ivshmemDeviceGetValues(struct Option * option)
{
  StringList sl = stringlist_new(true);

  DIR * d = opendir("/sys/class/kvmfr");
  if (!d)
    return sl;

  struct dirent * dir;
  while((dir = readdir(d)) != NULL)
  {
    if (dir->d_name[0] == '.')
      continue;

    char * devName;
    alloc_sprintf(&devName, "/dev/%s", dir->d_name);
    stringlist_push(sl, devName);
  }

  closedir(d);
  return sl;
}

void ivshmemOptionsInit(void)
{
  struct Option options[] =
  {
    {
      .module         = "app",
      .name           = "shmFile",
      .shortopt       = 'f',
      .description    = "The path to the shared memory file, or the name of the kvmfr device to use, ie: kvmfr0",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "/dev/shm/looking-glass",
      .validator      = ivshmemDeviceValidator,
      .getValues      = ivshmemDeviceGetValues
    },
    {0}
  };

  option_register(options);
}

bool ivshmemInit(struct IVSHMEM * dev)
{
  // FIXME: split code from ivshmemOpen
  return true;
}

bool ivshmemOpen(struct IVSHMEM * dev)
{
  return ivshmemOpenDev(dev, option_get_string("app", "shmFile"));
}

bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDevice)
{
  assert(dev);

  unsigned int devSize;
  int devFd = -1;
  bool hasDMA;

  dev->opaque = NULL;

  DEBUG_INFO("KVMFR Device     : %s", shmDevice);

  if (strlen(shmDevice) > 8 && memcmp(shmDevice, "/dev/kvmfr", 10) == 0)
  {
    devFd = open(shmDevice, O_RDWR, (mode_t)0600);
    if (devFd < 0)
    {
      DEBUG_ERROR("Failed to open: %s", shmDevice);
      DEBUG_ERROR("%s", strerror(errno));
      return false;
    }

    // get the device size
    devSize = ioctl(devFd, KVMFR_DMABUF_GETSIZE, 0);
    hasDMA = true;
  }
  else
  {
    struct stat st;
    if (stat(shmDevice, &st) != 0)
    {
      DEBUG_ERROR("Failed to stat: %s", shmDevice);
      DEBUG_ERROR("%s", strerror(errno));
      return false;
    }

    devSize = st.st_size;
    devFd   = open(shmDevice, O_RDWR, (mode_t)0600);
    if (devFd < 0)
    {
      DEBUG_ERROR("Failed to open: %s", shmDevice);
      DEBUG_ERROR("%s", strerror(errno));
      return false;
    }

    hasDMA = false;
  }

  void * map = mmap(0, devSize, PROT_READ | PROT_WRITE, MAP_SHARED, devFd, 0);
  if (map == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to map the shared memory device: %s", shmDevice);
    DEBUG_ERROR("%s", strerror(errno));
    return false;
  }

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)malloc(sizeof(struct IVSHMEMInfo));
  info->size   = devSize;
  info->devFd  = devFd;
  info->hasDMA = hasDMA;

  dev->opaque = info;
  dev->size   = devSize;
  dev->mem    = map;
  return true;
}

void ivshmemClose(struct IVSHMEM * dev)
{
  assert(dev);

  if (!dev->opaque)
    return;

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)dev->opaque;

  munmap(dev->mem, info->size);
  close(info->devFd);

  free(info);
  dev->mem    = NULL;
  dev->size   = 0;
  dev->opaque = NULL;
}

void ivshmemFree(struct IVSHMEM * dev)
{
  // FIXME: split code from ivshmemClose
}

bool ivshmemHasDMA(struct IVSHMEM * dev)
{
  assert(dev && dev->opaque);

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)dev->opaque;

  return info->hasDMA;
}

int ivshmemGetDMABuf(struct IVSHMEM * dev, uint64_t offset, uint64_t size)
{
  assert(ivshmemHasDMA(dev));
  assert(dev && dev->opaque);
  assert(offset + size <= dev->size);

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)dev->opaque;

  // align to the page size
  size = (size & ~(0x1000-1)) + 0x1000;

  const struct kvmfr_dmabuf_create create =
  {
    .flags  = KVMFR_DMABUF_FLAG_CLOEXEC,
    .offset = offset,
    .size   = size
  };

  int fd = ioctl(info->devFd, KVMFR_DMABUF_CREATE, &create);
  if (fd < 0)
    DEBUG_ERROR("Failed to create the dma buffer");

  return fd;
}
