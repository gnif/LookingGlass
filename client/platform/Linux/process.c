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

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ProcessState
{
  LG_ProcessStopHandler   handler;
  void                  * opaque;
  struct sigaction        previousInt;
  struct sigaction        previousTerm;
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

static void stopSignalHandler(int signal)
{
  if (process.handler && process.handler(process.opaque))
    return;

  struct sigaction action = { 0 };
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  sigaction(signal, &action, NULL);
  raise(signal);
}

enum LG_ProcessPrivilege lgProcessGetPrivilege(void)
{
  const uid_t user      = getuid();
  const uid_t effective = geteuid();

  if (user == 0)
    return LG_PROCESS_PRIVILEGE_ROOT;

  if (user != effective)
    return LG_PROCESS_PRIVILEGE_SETUID;

  return LG_PROCESS_PRIVILEGE_NORMAL;
}

char * lgProcessGetExecutablePath(const char * argv0)
{
  size_t capacity = 256U;
  while (capacity <= SIZE_MAX / 2U)
  {
    char * path = malloc(capacity);
    if (!path)
      break;

    const ssize_t length = readlink("/proc/self/exe", path, capacity - 1U);
    if (length >= 0 && (size_t)length < capacity - 1U)
    {
      path[length] = '\0';
      return path;
    }

    free(path);
    if (length < 0)
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

  struct sigaction action = { 0 };
  action.sa_handler = stopSignalHandler;
  action.sa_flags   = SA_RESTART;
  sigemptyset(&action.sa_mask);

  if (sigaction(SIGINT, &action, &process.previousInt) < 0)
    goto fail;

  if (sigaction(SIGTERM, &action, &process.previousTerm) < 0)
  {
    sigaction(SIGINT, &process.previousInt, NULL);
    goto fail;
  }

  process.installed = true;
  return true;

fail:
  process.handler = NULL;
  process.opaque  = NULL;
  return false;
}

void lgProcessRemoveStopHandler(void)
{
  if (!process.installed)
    return;

  sigaction(SIGINT , &process.previousInt , NULL);
  sigaction(SIGTERM, &process.previousTerm, NULL);

  process.handler   = NULL;
  process.opaque    = NULL;
  process.installed = false;
}
