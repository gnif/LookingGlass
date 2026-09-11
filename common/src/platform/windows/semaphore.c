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
#include "common/windebug.h"

#include <limits.h>
#include <stdlib.h>
#include <windows.h>

struct LGSemaphore
{
  HANDLE handle;
};

LGSemaphore * lgCreateSemaphore(unsigned int initialValue)
{
  if (initialValue > (unsigned int)LONG_MAX)
    return NULL;

  LGSemaphore * semaphore = malloc(sizeof(*semaphore));
  if (!semaphore)
    return NULL;

  semaphore->handle = CreateSemaphoreW(
      NULL, (LONG)initialValue, LONG_MAX, NULL);
  if (!semaphore->handle)
  {
    DEBUG_WINERROR("Failed to create semaphore", GetLastError());
    free(semaphore);
    return NULL;
  }

  return semaphore;
}

void lgFreeSemaphore(LGSemaphore * semaphore)
{
  if (!semaphore)
    return;

  if (!CloseHandle(semaphore->handle))
    DEBUG_WINERROR("Failed to destroy semaphore", GetLastError());
  free(semaphore);
}

bool lgWaitSemaphore(LGSemaphore * semaphore)
{
  const DWORD result = WaitForSingleObject(semaphore->handle, INFINITE);
  if (result == WAIT_OBJECT_0)
    return true;

  DEBUG_WINERROR("Failed to wait on semaphore", GetLastError());
  return false;
}

bool lgSignalSemaphore(LGSemaphore * semaphore)
{
  if (ReleaseSemaphore(semaphore->handle, 1, NULL))
    return true;

  DEBUG_WINERROR("Failed to signal semaphore", GetLastError());
  return false;
}
