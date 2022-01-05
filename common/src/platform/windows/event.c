/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include "common/event.h"
#include "common/windebug.h"
#include "common/time.h"

#include <windows.h>
#include <stdatomic.h>

LGEvent * lgCreateEvent(bool autoReset, unsigned int msSpinTime)
{
  HANDLE handle = CreateEvent(NULL, autoReset ? FALSE : TRUE, FALSE, NULL);
  if (!handle)
  {
    DEBUG_WINERROR("Failed to create the event", GetLastError());
    return NULL;
  }

  return (LGEvent *)handle;
}

LGEvent * lgWrapEvent(void * handle)
{
  return (LGEvent *)handle;
}

void lgFreeEvent(LGEvent * event)
{
  CloseHandle((HANDLE)event);
}

bool lgWaitEvent(LGEvent * event, unsigned int timeout)
{
  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;
  do
  {
    switch(WaitForSingleObject((HANDLE)event, to))
    {
      case WAIT_OBJECT_0:
        return true;

      case WAIT_ABANDONED:
        continue;

      case WAIT_TIMEOUT:
        if (timeout == TIMEOUT_INFINITE)
          continue;
        return false;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for event failed", GetLastError());
        return false;
    }

    DEBUG_ERROR("Unknown wait event return code");
  }
  while(0);

  return false;
}

bool lgSignalEvent(LGEvent * event)
{
  return SetEvent((HANDLE)event);
}

bool lgResetEvent(LGEvent * event)
{
  return ResetEvent((HANDLE)event);
}
