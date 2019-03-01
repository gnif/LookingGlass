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

#include "app.h"
#include "debug.h"
#include <stdlib.h>
#include <pthread.h>

struct osThreadHandle
{
  const char       * name;
  osThreadFunction   function;
  void             * opaque;
  pthread_t          handle;
  int                resultCode;
};

int main(int argc, char * argv[])
{
  bool termSig = false;
  int result = app_main(&termSig);
  os_shmemUnmap();
  return result;
}

unsigned int os_shmemSize()
{
  // TODO
  return 0;
}

bool os_shmemMmap(void **ptr)
{
  // TODO
  return false;
}

void os_shmemUnmap()
{
  // TODO
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