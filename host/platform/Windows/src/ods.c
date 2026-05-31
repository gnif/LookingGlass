/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include "ods.h"
#include "common/array.h"
#include "common/windebug.h"

#include <windows.h>

typedef struct ODSData
{
  DWORD dwPid;
  char buffer[4096 - sizeof(DWORD)];
}
ODSData;

typedef struct ODSState
{
  bool started;

  HANDLE hBuffer;
  HANDLE hBufferReady;
  HANDLE hDataReady;

  HANDLE hThread;
  HANDLE hStopEvent;

  ODSData * data;
}
ODSState;

static ODSState ods = {0};

static DWORD CALLBACK ods_thread(LPVOID arg)
{
  const DWORD pid = GetCurrentProcessId();
  HANDLE wait[] = { ods.hDataReady, ods.hStopEvent };
  char buffer[sizeof(ods.data->buffer) + 1];

  SetEvent(ods.hBufferReady);

  while(true)
  {
    switch(WaitForMultipleObjects(ARRAY_LENGTH(wait), wait, FALSE, INFINITE))
    {
      case WAIT_OBJECT_0:
      {
        if (ods.data->dwPid != pid)
        {
          SetEvent(ods.hBufferReady);
          break;
        }

        unsigned size = 0;
        while(size < sizeof(ods.data->buffer) && ods.data->buffer[size])
          ++size;
        memcpy(buffer, ods.data->buffer, size);
        SetEvent(ods.hBufferReady);

        buffer[size] = '\0';
        while (size && isspace(buffer[size - 1]))
          buffer[--size] = '\0';

        DEBUG_ERROR("ODS: %s", buffer);
        break;
      }

      case WAIT_OBJECT_0 + 1:
        return 0;

      default:
        DEBUG_WINERROR("WaitForMultipleObjects failed", GetLastError());
        break;
    }
  }

  return 0;
}

bool ods_start(void)
{
  if (IsDebuggerPresent() || ods.started)
    return true;

  ods.hBuffer = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
    0, 4096, "DBWIN_BUFFER");
  if (ods.hBuffer == INVALID_HANDLE_VALUE)
  {
    DEBUG_WINERROR("CreateFileMapping failed", GetLastError());
    return false;
  }

  ods.data = MapViewOfFile(ods.hBuffer, SECTION_MAP_READ, 0, 0, 0);
  if (!ods.data)
  {
    DEBUG_WINERROR("MapViewOfFile failed", GetLastError());
    goto fail_created;
  }

  ods.hBufferReady = CreateEventA(NULL, FALSE, FALSE, "DBWIN_BUFFER_READY");
  if (!ods.hBufferReady)
  {
    DEBUG_WINERROR("DBWIN_BUFFER_READY createEvent failed", GetLastError());
    goto fail_mapped;
  }

  ods.hDataReady = CreateEventA(NULL, FALSE, FALSE, "DBWIN_DATA_READY");
  if (!ods.hDataReady)
  {
    DEBUG_WINERROR("DBWIN_DATA_READY Create event failed", GetLastError());
    goto fail_buffer_event;
  }

  ods.hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!ods.hStopEvent)
  {
    DEBUG_WINERROR("Create thread stop event failed", GetLastError());
    goto fail_data_event;
  }

  ods.hThread = CreateThread(NULL, 0, ods_thread, NULL, 0, NULL);
  if (!ods.hThread)
  {
    DEBUG_WINERROR("Create ods thread failed", GetLastError());
    goto fail_stop_event;
  }

  ods.started = true;
  DEBUG_INFO("OutputDebugString Logging Started");
  return true;

fail_stop_event:
  CloseHandle(ods.hStopEvent);

fail_data_event:
  CloseHandle(ods.hDataReady);

fail_buffer_event:
  CloseHandle(ods.hBufferReady);

fail_mapped:
  UnmapViewOfFile(ods.data);

fail_created:
  CloseHandle(ods.hBuffer);

  return false;
}

void ods_stop(void)
{
  if (!ods.started)
    return;

  SetEvent(ods.hStopEvent);
  WaitForSingleObject(ods.hThread, INFINITE);

  CloseHandle(ods.hStopEvent);
  CloseHandle(ods.hDataReady);
  CloseHandle(ods.hBufferReady);
  UnmapViewOfFile(ods.data);
  CloseHandle(ods.hBuffer);

  DEBUG_INFO("OutputDebugString Logging Stopped");
  ods.started = false;
}
