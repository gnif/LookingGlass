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

#include <assert.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/mman.h>
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

struct osThreadHandle
{
  const char       * name;
  osThreadFunction   function;
  void             * opaque;
  pthread_t          handle;
  int                resultCode;
};

void sigHandler(int signo)
{
  DEBUG_INFO("SIGINT");
  app_quit();
}

int main(int argc, char * argv[])
{
  app.executable = argv[0];

  struct Option options[] =
  {
    {
      .module      = "os",
      .name        = "shmDevice",
      .description = "The IVSHMEM device to use",
      .value       = {
        .type       = OPTION_TYPE_STRING,
        .v.x_string = "uio0"
      },
      .validator   = NULL,
      .printHelp   = NULL
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

  // check the deice name
  {
    char file[100] = "/sys/class/uio/";
    strncat(file, shmDevice, sizeof(file) - 1);
    strncat(file, "/name"  , sizeof(file) - 1);

    int fd = open(file, O_RDONLY);
    if (fd < 0)
    {
      DEBUG_ERROR("Failed to open: %s", file);
      DEBUG_ERROR("Did you remmeber to modprobe the kvmfr module?");
      return false;
    }

    char name[32];
    int len = read(fd, name, sizeof(name) - 1);
    if (len <= 0)
    {
      DEBUG_ERROR("Failed to read: %s", file);
      close(fd);
      return false;
    }
    name[len] = '\0';
    close(fd);

    while(len > 0 && name[len-1] == '\n')
    {
      --len;
      name[len] = '\0';
    }

    if (strcmp(name, "KVMFR") != 0)
    {
      DEBUG_ERROR("Device is not a KVMFR device \"%s\" reports as: %s", file, name);
      return false;
    }
  }

  // get the device size
  {
    char file[100] = "/sys/class/uio/";
    strncat(file, shmDevice        , sizeof(file) - 1);
    strncat(file, "/maps/map0/size", sizeof(file) - 1);

    int fd = open(file, O_RDONLY);
    if (fd < 0)
    {
      DEBUG_ERROR("Failed to open: %s", file);
      return false;
    }

    char size[32];
    int  len = read(fd, size, sizeof(size) - 1);
    if (len <= 0)
    {
      DEBUG_ERROR("Failed to read: %s", file);
      close(fd);
      return false;
    }
    size[len] = '\0';
    close(fd);

    app.shmSize = strtoul(size, NULL, 16);
  }

  // open the device
  {
    char file[100] = "/dev/";
    strncat(file, shmDevice, sizeof(file) - 1);
    app.shmFD   = open(file, O_RDWR, (mode_t)0600);
    app.shmMap  = MAP_FAILED;
    if (app.shmFD < 0)
    {
      DEBUG_ERROR("Failed to open: %s", file);
      return false;
    }

    DEBUG_INFO("KVMFR Device     : %s", file);
  }

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

static void * threadWrapper(void * opaque)
{
  osThreadHandle * handle = (osThreadHandle *)opaque;
  handle->resultCode = handle->function(handle->opaque);
  return NULL;
}

bool os_createThread(const char * name, osThreadFunction function, void * opaque, osThreadHandle ** handle)
{
  *handle = (osThreadHandle*)malloc(sizeof(osThreadHandle));
  (*handle)->name     = name;
  (*handle)->function = function;
  (*handle)->opaque   = opaque;

  if (pthread_create(&(*handle)->handle, NULL, threadWrapper, *handle) != 0)
  {
    DEBUG_ERROR("pthread_create failed for thread: %s", name);
    free(*handle);
    *handle = NULL;
    return false;
  }
  return true;
}

bool os_joinThread(osThreadHandle * handle, int * resultCode)
{
  if (pthread_join(handle->handle, NULL) != 0)
  {
    DEBUG_ERROR("pthread_join failed for thread: %s", handle->name);
    free(handle);
    return false;
  }

  if (resultCode)
    *resultCode = handle->resultCode;

  free(handle);
  return true;
}

struct osEventHandle
{
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  bool            flag;
  bool            autoReset;
};

osEventHandle * os_createEvent(bool autoReset)
{
  osEventHandle * handle = (osEventHandle *)calloc(sizeof(osEventHandle), 1);
  if (!handle)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return NULL;
  }

  if (pthread_mutex_init(&handle->mutex, NULL) != 0)
  {
    DEBUG_ERROR("Failed to create the mutex");
    free(handle);
    return NULL;
  }

  if (pthread_cond_init(&handle->cond, NULL) != 0)
  {
    pthread_mutex_destroy(&handle->mutex);
    free(handle);
    return NULL;
  }

  handle->autoReset = autoReset;

  return handle;
}

void os_freeEvent(osEventHandle * handle)
{
  assert(handle);

  pthread_cond_destroy (&handle->cond );
  pthread_mutex_destroy(&handle->mutex);
  free(handle);
}

bool os_waitEvent(osEventHandle * handle, unsigned int timeout)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  while(!handle->flag)
  {
    if (timeout == TIMEOUT_INFINITE)
    {
      if (pthread_cond_wait(&handle->cond, &handle->mutex) != 0)
      {
        DEBUG_ERROR("Wait to wait on the condition");
        return false;
      }
    }
    else
    {
      struct timespec ts;
      ts.tv_sec  = timeout / 1000;
      ts.tv_nsec = (timeout % 1000) * 1000000;
      switch(pthread_cond_timedwait(&handle->cond, &handle->mutex, &ts))
      {
        case ETIMEDOUT:
          return false;

        default:
          DEBUG_ERROR("Timed wait failed");
          return false;
      }
    }
  }

  if (handle->autoReset)
    handle->flag = false;

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  return true;
}

bool os_signalEvent(osEventHandle * handle)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  handle->flag = true;

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  if (pthread_cond_signal(&handle->cond) != 0)
  {
    DEBUG_ERROR("Failed to signal the condition");
    return false;
  }

  return true;
}

bool os_resetEvent(osEventHandle * handle)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  handle->flag = false;

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  return true;
}