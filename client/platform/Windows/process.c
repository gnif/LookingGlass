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

#include "platform/process.h"

#include "common/win32.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct ProcessState
{
  LG_ProcessStopHandler   handler;
  void                  * opaque;
  bool                    installed;
};

static struct ProcessState process;

static char * duplicateString(const char * value)
{
  if (!value)
    return NULL;

  const size_t length = strlen(value) + 1U;
  char       * result = malloc(length);
  if (result)
    memcpy(result, value, length);
  return result;
}

static BOOL WINAPI consoleHandler(DWORD type)
{
  switch (type)
  {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      if (process.handler && process.handler(process.opaque))
        return TRUE;
      return FALSE;

    default:
      return FALSE;
  }
}

enum LG_ProcessPrivilege lgProcessGetPrivilege(void)
{
  return LG_PROCESS_PRIVILEGE_NORMAL;
}

char * lgProcessGetExecutablePath(const char * argv0)
{
  DWORD capacity = MAX_PATH;
  while (capacity <= UINT32_MAX / 2U)
  {
    wchar_t * path = malloc((size_t)capacity * sizeof(*path));
    if (!path)
      break;

    const DWORD length = GetModuleFileNameW(NULL, path, capacity);
    if (length > 0 && length < capacity)
    {
      path[length] = L'\0';
      char * result = lgWideToUtf8(path);
      free(path);
      if (result)
        return result;
      break;
    }

    free(path);
    if (!length)
      break;
    capacity *= 2U;
  }

  return duplicateString(argv0);
}

bool lgProcessInstallStopHandler(LG_ProcessStopHandler handler, void * opaque)
{
  if (!handler || process.installed)
    return false;

  process.handler = handler;
  process.opaque  = opaque;
  if (!SetConsoleCtrlHandler(consoleHandler, TRUE))
  {
    process.handler = NULL;
    process.opaque  = NULL;
    return false;
  }

  process.installed = true;
  return true;
}

void lgProcessRemoveStopHandler(void)
{
  if (!process.installed)
    return;

  SetConsoleCtrlHandler(consoleHandler, FALSE);
  process.handler   = NULL;
  process.opaque    = NULL;
  process.installed = false;
}
