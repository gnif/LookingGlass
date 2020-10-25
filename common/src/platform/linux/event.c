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

#include "common/event.h"

#include "common/debug.h"

#include <stdlib.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>

struct LGEvent
{
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  uint32_t        flag;
  bool            autoReset;
};

LGEvent * lgCreateEvent(bool autoReset, unsigned int msSpinTime)
{
  LGEvent * handle = (LGEvent *)calloc(sizeof(LGEvent), 1);
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

void lgFreeEvent(LGEvent * handle)
{
  assert(handle);

  pthread_cond_destroy (&handle->cond );
  pthread_mutex_destroy(&handle->mutex);
  free(handle);
}

bool lgWaitEventAbs(LGEvent * handle, struct timespec * ts)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  bool ret = true;
  int  res;
  while(ret && !atomic_load(&handle->flag))
  {
    if (!ts)
    {
      if ((res = pthread_cond_wait(&handle->cond, &handle->mutex)) != 0)
      {
        DEBUG_ERROR("Failed to wait on the condition (err: %d)", res);
        ret = false;
      }
    }
    else
    {
      switch((res = pthread_cond_timedwait(&handle->cond, &handle->mutex, ts)))
      {
        case 0:
          break;

        case ETIMEDOUT:
          ret = false;
          break;

        default:
          ret = false;
          DEBUG_ERROR("Timed wait failed (err: %d)", res);
          break;
      }
    }
  }

  if (handle->autoReset)
    atomic_store(&handle->flag, 0);

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  return ret;
}

bool lgWaitEventNS(LGEvent * handle, unsigned int timeout)
{
  if (timeout == TIMEOUT_INFINITE)
    return lgWaitEventAbs(handle, NULL);

  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t nsec = ts.tv_nsec + timeout;
  if(nsec > 1e9)
  {
    ts.tv_nsec = nsec - 1e9;
    ++ts.tv_sec;
  }
  else
    ts.tv_nsec = nsec;

  return lgWaitEventAbs(handle, &ts);
}

bool lgWaitEvent(LGEvent * handle, unsigned int timeout)
{
  if (timeout == TIMEOUT_INFINITE)
    return lgWaitEventAbs(handle, NULL);

  return lgWaitEventNS(handle, timeout * 1000000);
}

bool lgSignalEvent(LGEvent * handle)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  atomic_store(&handle->flag, 1);

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  if (pthread_cond_broadcast(&handle->cond) != 0)
  {
    DEBUG_ERROR("Failed to signal the condition");
    return false;
  }

  return true;
}

bool lgResetEvent(LGEvent * handle)
{
  assert(handle);

  if (pthread_mutex_lock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to lock the mutex");
    return false;
  }

  atomic_store(&handle->flag, 0);

  if (pthread_mutex_unlock(&handle->mutex) != 0)
  {
    DEBUG_ERROR("Failed to unlock the mutex");
    return false;
  }

  return true;
}
