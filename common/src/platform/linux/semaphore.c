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

#include "common/semaphore.h"

#include "common/debug.h"

#include <errno.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

struct LGSemaphore
{
  sem_t handle;
};

LGSemaphore * lgCreateSemaphore(unsigned int initialValue)
{
  LGSemaphore * semaphore = malloc(sizeof(*semaphore));
  if (!semaphore)
    return NULL;

  if (sem_init(&semaphore->handle, 0, initialValue) != 0)
  {
    DEBUG_ERROR("Failed to create semaphore: %s", strerror(errno));
    free(semaphore);
    return NULL;
  }

  return semaphore;
}

void lgFreeSemaphore(LGSemaphore * semaphore)
{
  if (!semaphore)
    return;

  if (sem_destroy(&semaphore->handle) != 0)
    DEBUG_ERROR("Failed to destroy semaphore: %s", strerror(errno));
  free(semaphore);
}

bool lgWaitSemaphore(LGSemaphore * semaphore)
{
  int result;
  do
    result = sem_wait(&semaphore->handle);
  while (result != 0 && errno == EINTR);

  if (result != 0)
    DEBUG_ERROR("Failed to wait on semaphore: %s", strerror(errno));
  return result == 0;
}

bool lgSignalSemaphore(LGSemaphore * semaphore)
{
  if (sem_post(&semaphore->handle) == 0)
    return true;

  DEBUG_ERROR("Failed to signal semaphore: %s", strerror(errno));
  return false;
}
