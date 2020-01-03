/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "interface/platform.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/thread.h"

#include <assert.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>

struct app
{
  const char  * executable;
  unsigned int  shmSize;
  int           shmFD;
  void        * shmMap;
};

static struct app app;

void sigHandler(int signo)
{
  DEBUG_INFO("SIGINT");
  app_quit();
}

static int uioOpenFile(const char * shmDevice, const char * file)
{
  int    len  = snprintf(NULL, 0, "/sys/class/uio/%s/%s", shmDevice, file);
  char * path = malloc(len + 1);
  sprintf(path, "/sys/class/uio/%s/%s", shmDevice, file);

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

static int shmOpenDev(const char * shmDevice)
{
  int    len  = snprintf(NULL, 0, "/dev/%s", shmDevice);
  char * path = malloc(len + 1);
  sprintf(path, "/dev/%s", shmDevice);

  int fd = open(path, O_RDWR, (mode_t)0600);
  if (fd < 0)
  {
    DEBUG_ERROR("Failed to open: %s", path);
    DEBUG_ERROR("Did you remmeber to modprobe the kvmfr module?");
    free(path);
    return -1;
  }

  free(path);
  return fd;
}

static bool shmDeviceValidator(struct Option * opt, const char ** error)
{
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

static StringList shmDeviceGetValues(struct Option * option)
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
      stringlist_push(sl, strdup(dir->d_name));

    free(name);
  }

  closedir(d);
  return sl;
}

int main(int argc, char * argv[])
{
  app.executable = argv[0];

  struct Option options[] =
  {
    {
      .module         = "os",
      .name           = "shmDevice",
      .description    = "The IVSHMEM device to use",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = "uio0",
      .validator      = shmDeviceValidator,
      .getValues      = shmDeviceGetValues
    },
    {0}
  };

  option_register(options);

  int result = app_main(argc, argv);
  os_shmemUnmap();
  close(app.shmFD);

  return result;
}

bool app_init()
{
  const char * shmDevice = option_get_string("os", "shmDevice");

  // get the device size
  int fd = uioOpenFile(shmDevice, "maps/map0/size");
  if (fd < 0)
  {
    DEBUG_ERROR("Failed to open %s/size", shmDevice);
    DEBUG_ERROR("Did you remmeber to modprobe the kvmfr module?");
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

  app.shmSize = strtoul(size, NULL, 16);

  // open the device
  app.shmFD  = shmOpenDev(shmDevice);
  app.shmMap = MAP_FAILED;
  if (app.shmFD < 0)
    return false;

  DEBUG_INFO("KVMFR Device     : %s", shmDevice);

  signal(SIGINT, sigHandler);
  return true;
}

const char * os_getExecutable()
{
  return app.executable;
}

unsigned int os_shmemSize()
{
  return app.shmSize;
}

bool os_shmemMmap(void **ptr)
{
  if (app.shmMap == MAP_FAILED)
  {
    app.shmMap = mmap(0, app.shmSize, PROT_READ | PROT_WRITE, MAP_SHARED, app.shmFD, 0);
    if (app.shmMap == MAP_FAILED)
    {
      const char * shmDevice = option_get_string("os", "shmDevice");
      DEBUG_ERROR("Failed to map the shared memory device: %s", shmDevice);
      return false;
    }
  }

  *ptr = app.shmMap;
  return true;
}

void os_shmemUnmap()
{
  if (app.shmMap == MAP_FAILED)
    return;

  munmap(app.shmMap, app.shmSize);
  app.shmMap = MAP_FAILED;
}