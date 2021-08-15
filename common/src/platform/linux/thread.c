/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "common/thread.h"

#include <stdlib.h>
#include <pthread.h>

#include "common/debug.h"

struct LGThread
{
  const char       * name;
  LGThreadFunction   function;
  void             * opaque;
  pthread_t          handle;
  int                resultCode;
};

static void * threadWrapper(void * opaque)
{
  LGThread * handle = (LGThread *)opaque;
  handle->resultCode = handle->function(handle->opaque);
  return NULL;
}

bool lgCreateThread(const char * name, LGThreadFunction function, void * opaque, LGThread ** handle)
{
  *handle = malloc(sizeof(**handle));
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

  pthread_setname_np((*handle)->handle, name);
  return true;
}

bool lgJoinThread(LGThread * handle, int * resultCode)
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
