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

#include "common/event.h"
#include "common/windebug.h"
#include "common/time.h"

#include <windows.h>

struct LGEvent
{
  volatile int  lock;
  bool          reset;
  HANDLE        handle;
  bool          wrapped;
  unsigned int  msSpinTime;
  volatile bool signaled;
};

LGEvent * lgCreateEvent(bool autoReset, unsigned int msSpinTime)
{
  LGEvent * event = (LGEvent *)malloc(sizeof(LGEvent));
  if (!event)
  {
    DEBUG_ERROR("out of ram");
    return NULL;
  }

  event->lock       = 0;
  event->reset      = autoReset;
  event->handle     = CreateEvent(NULL, autoReset ? FALSE : TRUE, FALSE, NULL);
  event->wrapped    = false;
  event->msSpinTime = msSpinTime;
  event->signaled   = false;

  if (!event->handle)
  {
    DEBUG_WINERROR("Failed to create the event", GetLastError());
    free(event);
    return NULL;
  }

  return event;
}

LGEvent * lgWrapEvent(void * handle)
{
  LGEvent * event = (LGEvent *)malloc(sizeof(LGEvent));
  if (!event)
  {
    DEBUG_ERROR("out of ram");
    return NULL;
  }

  event->lock       = 0;
  event->reset      = false;
  event->handle     = (HANDLE)handle;
  event->wrapped    = true;
  event->msSpinTime = 0;
  event->signaled   = false;
  return event;
}

void lgFreeEvent(LGEvent * event)
{
  CloseHandle(event->handle);
}

bool lgWaitEvent(LGEvent * event, unsigned int timeout)
{
  // wrapped events can't be enahnced
  if (!event->wrapped)
  {
    if (event->signaled)
    {
      if (event->reset)
        event->signaled = false;
      return true;
    }

    if (timeout == 0)
    {
      bool ret = event->signaled;
      if (event->reset)
        event->signaled = false;
      return ret;
    }

    if (event->msSpinTime)
    {
      unsigned int spinTime = event->msSpinTime;
      if (timeout != TIMEOUT_INFINITE)
      {
        if (timeout > event->msSpinTime)
          timeout -= event->msSpinTime;
        else
        {
          spinTime -= timeout;
          timeout   = 0;
        }
      }

      uint64_t now = microtime();
      uint64_t end = now + spinTime * 1000;
      while(!event->signaled)
      {
        now = microtime();
        if (now >= end)
          break;
      }

      if (event->signaled)
      {
        if (event->reset)
          event->signaled = false;
        return true;
      }
    }
  }

  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;
  while(true)
  {
    switch(WaitForSingleObject(event->handle, to))
    {
      case WAIT_OBJECT_0:
        if (!event->reset)
          event->signaled = true;
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
    return false;
  }
}

bool lgWaitEvents(LGEvent * events[], int count, bool waitAll, unsigned int timeout)
{
  const DWORD to = (timeout == TIMEOUT_INFINITE) ? INFINITE : (DWORD)timeout;

  HANDLE * handles = (HANDLE *)malloc(sizeof(HANDLE) * count);
  for(int i = 0; i < count; ++i)
    handles[i] = events[i]->handle;

  while(true)
  {
    DWORD result = WaitForMultipleObjects(count, handles, waitAll, to);
    if (result >= WAIT_OBJECT_0 && result < count)
    {
      // null non signaled events from the handle list
      for(int i = 0; i < count; ++i)
        if (i != result && !lgWaitEvent(events[i], 0))
          events[i] = NULL;
      free(handles);
      return true;
    }

    if (result >= WAIT_ABANDONED_0 && result - WAIT_ABANDONED_0 < count)
      continue;

    switch(result)
    {
      case WAIT_TIMEOUT:
        if (timeout == TIMEOUT_INFINITE)
          continue;

        free(handles);
        return false;

      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for event failed", GetLastError());
        free(handles);
        return false;
    }

    DEBUG_ERROR("Unknown wait event return code");
    free(handles);
    return false;
  }
}

bool lgSignalEvent(LGEvent * event)
{
  event->signaled = true;
  return SetEvent(event->handle);
}

bool lgResetEvent(LGEvent * event)
{
  event->signaled = false;
  return ResetEvent(event->handle);
}
