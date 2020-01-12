/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "common/ivshmem.h"

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/option.h"
#include "common/stringutils.h"

struct IVSHMEMInfo
{
  int fd;
  int size;
};

static int uioOpenFile(const char * shmDevice, const char * file)
{
  char * path;
  alloc_sprintf(&path, "/sys/class/uio/%s/%s", shmDevice, file);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
  {
    free(path);
    return -1;
  }

  free(path);
  return fd;
}

static char * uioGetName(const char * shmDevice)
{
  int fd = uioOpenFile(shmDevice, "name");
  if (fd < 0)
    return NULL;

  char * name = malloc(32);
  int len = read(fd, name, 31);
  if (len <= 0)
  {
    free(name);
    close(fd);
    return NULL;
  }
  name[len] = '\0';
  close(fd);

  while(len > 0 && name[len-1] == '\n')
  {
    --len;
    name[len] = '\0';
  }

  return name;
}

static bool ivshmemDeviceValidator(struct Option * opt, const char ** error)
{
  // if it's not a uio device, it must be a file on disk
  if (strlen(opt->value.x_string) > 3 && memcmp(opt->value.x_string, "uio", 3) != 0)
  {
    struct stat st;
    if (stat(opt->value.x_string, &st) != 0)
    {
      *error = "Invalid path to the ivshmem file specified";
      return false;
    }
    return true;
  }

  char * name = uioGetName(opt->value.x_string);
  if (!name)
  {
    *error = "Failed to get the uio device name";
    return false;
  }

  if (strcmp(name, "KVMFR") != 0)
  {
    free(name);
    *error = "Device is not a KVMFR device";
    return false;
  }

  free(name);
  return true;
}

static StringList ivshmemDeviceGetValues(struct Option * option)
{
  StringList sl = stringlist_new(true);

  DIR * d = opendir("/sys/class/uio");
  if (!d)
    return sl;

  struct dirent * dir;
  while((dir = readdir(d)) != NULL)
  {
    if (dir->d_name[0] == '.')
      continue;

    char * name = uioGetName(dir->d_name);
    if (!name)
      continue;

    if (strcmp(name, "KVMFR") == 0)
    {
      char * devName;
      alloc_sprintf(&devName, "/dev/%s", dir->d_name);
      stringlist_push(sl, devName);
    }

    free(name);
  }

  closedir(d);
  return sl;
}

void ivshmemOptionsInit()
{
  struct Option options[] =
  {
    {
      .module         = "app",
      .name           = "shmFile",
      .shortopt       = 'f',
      .description    = "The path to the shared memory file, or the name of the uio device to use, ie: uio0",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "/dev/shm/looking-glass",
      .validator      = ivshmemDeviceValidator,
      .getValues      = ivshmemDeviceGetValues
    },
    {0}
  };

  option_register(options);
}

bool ivshmemOpen(struct IVSHMEM * dev)
{
  return ivshmemOpenDev(dev, option_get_string("app", "shmFile"));
}

bool ivshmemOpenDev(struct IVSHMEM * dev, const char * shmDevice)
{
  assert(dev);

  unsigned int devSize;
  int devFD;

  dev->opaque = NULL;

  DEBUG_INFO("KVMFR Device     : %s", shmDevice);

  if (strlen(shmDevice) > 8 && memcmp(shmDevice, "/dev/uio", 8) == 0)
  {
    const char * uioDev = shmDevice + 5;

    // get the device size
    int fd = uioOpenFile(uioDev, "maps/map0/size");
    if (fd < 0)
    {
      DEBUG_ERROR("Failed to open %s/size", uioDev);
      return false;
    }

    char size[32];
    int  len = read(fd, size, sizeof(size) - 1);
    if (len <= 0)
    {
      DEBUG_ERROR("Failed to read the device size");
      close(fd);
      return false;
    }
    size[len] = '\0';
    close(fd);
    devSize = strtoul(size, NULL, 16);

    devFD = open(shmDevice, O_RDWR, (mode_t)0600);
    if (devFD < 0)
    {
      DEBUG_ERROR("Failed to open: %s", shmDevice);
      DEBUG_ERROR("Do you have permission to access the device?");
      return false;
    }
  }
  else
  {
    struct stat st;
    if (stat(shmDevice, &st) != 0)
    {
      DEBUG_ERROR("Failed to stat: %s", shmDevice);
      return false;
    }

    devSize = st.st_size;
    devFD   = open(shmDevice, O_RDWR, (mode_t)0600);
    if (devFD < 0)
    {
      DEBUG_ERROR("Failed to open: %s", shmDevice);
      return false;
    }
  }

  void * map = mmap(0, devSize, PROT_READ | PROT_WRITE, MAP_SHARED, devFD, 0);
  if (map == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to map the shared memory device: %s", shmDevice);
    return false;
  }

  struct IVSHMEMInfo * info =
    (struct IVSHMEMInfo *)malloc(sizeof(struct IVSHMEMInfo));
  info->size = devSize;
  info->fd   = devFD;

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
  close(info->fd);

  free(info);
  dev->mem    = NULL;
  dev->size   = 0;
  dev->opaque = NULL;
}