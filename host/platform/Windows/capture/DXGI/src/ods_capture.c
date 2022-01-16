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

#include "ods_capture.h"
#include <stdbool.h>

#include "common/array.h"
#include "common/windebug.h"

struct ODSData {
  DWORD dwPid;
  char buffer[4096 - sizeof(DWORD)];
};

static HANDLE hThread        = NULL;
static HANDLE hStopThread    = NULL;
static HANDLE hBuffer        = NULL;
static HANDLE hDataReady     = NULL;
static HANDLE hBufferReady   = NULL;
static struct ODSData * data = NULL;

static bool ensureEvent(HANDLE * phEvent, LPSTR name)
{
  if (*phEvent)
    return true;

  *phEvent = CreateEventA(NULL, FALSE, FALSE, name);
  if (*phEvent)
    return true;

  DEBUG_WINERROR("CreateEvent failed", GetLastError());
  return false;
}

static DWORD CALLBACK captureThread(LPVOID arg)
{
  DWORD pid = GetCurrentProcessId();
  HANDLE wait[] = { hDataReady, hStopThread };
  SetEvent(hBufferReady);

  while (true)
  {
    switch (WaitForMultipleObjects(ARRAY_LENGTH(wait), wait, FALSE, INFINITE))
    {
      case WAIT_OBJECT_0:
      {
        if (data->dwPid != pid)
          SetEvent(hBufferReady);
        else
        {
          char buffer[sizeof(data->buffer) + 1];
          int size = 0;
          while (size < sizeof(data->buffer) && data->buffer[size])
            ++size;
          memcpy(buffer, data->buffer, size);
          SetEvent(hBufferReady);

          buffer[size] = '\0';
          if (size && isspace(buffer[size - 1]))
            buffer[--size] = '\0';
          DEBUG_ERROR("%s", buffer);
        }
        break;
      }

      case (WAIT_OBJECT_0 + 1):
        return 0;

      default:
        DEBUG_WINERROR("WaitForMultipleObjects failed", GetLastError());
    }
  }
}

void captureOutputDebugString(void)
{
  if (IsDebuggerPresent())
    return;

  if (!hBuffer)
  {
    hBuffer = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
      4096, "DBWIN_BUFFER");
    if (hBuffer == INVALID_HANDLE_VALUE)
    {
      DEBUG_WINERROR("CreateFileMapping failed", GetLastError());
      hBuffer = NULL;
      return;
    }
  }

  if (!data)
  {
    data = MapViewOfFile(hBuffer, SECTION_MAP_READ, 0, 0, 0);
    if (!data)
    {
      DEBUG_WINERROR("MapViewOfFile failed", GetLastError());
      return;
    }
  }

  if (!ensureEvent(&hBufferReady, "DBWIN_BUFFER_READY") ||
      !ensureEvent(&hDataReady,   "DBWIN_DATA_READY"))
  {
    DEBUG_ERROR("Failed to initialize OutputDebugString events");
    return;
  }

  if (!hStopThread)
  {
    hStopThread = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hStopThread)
    {
      DEBUG_WINERROR("CreateEvent failed", GetLastError());
      return;
    }
  }

  if (!hThread)
  {
    hThread = CreateThread(NULL, 0, captureThread, NULL, 0, NULL);
    if (!hThread)
      DEBUG_WINERROR("CreateThread failed", GetLastError());
  }
}

__attribute__((destructor))
static void stopCapture(void)
{
  if (hStopThread)
    SetEvent(hStopThread);

  if (hThread)
    WaitForSingleObject(hThread, INFINITE);

  if (hDataReady)
    CloseHandle(hDataReady);

  if (hBufferReady)
    CloseHandle(hBufferReady);

  if (data)
    UnmapViewOfFile(data);

  if (hBuffer)
    CloseHandle(hBuffer);
}
