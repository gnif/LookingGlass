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
#include "test.h"

#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopCount;
static volatile sig_atomic_t restoredCount;

static bool acceptStop(void * opaque)
{
  const int expected = 42;
  if (!opaque || *(const int *)opaque != expected)
    return false;

  ++stopCount;
  return true;
}

static bool rejectStop(void * opaque)
{
  (void)opaque;
  return false;
}

static void restoredHandler(int signal)
{
  (void)signal;
  ++restoredCount;
}

static void testPrivilege(void)
{
  enum LG_ProcessPrivilege expected = LG_PROCESS_PRIVILEGE_NORMAL;
  if (getuid() == 0)
    expected = LG_PROCESS_PRIVILEGE_ROOT;
  else if (getuid() != geteuid())
    expected = LG_PROCESS_PRIVILEGE_SETUID;

  CHECK(lgProcessGetPrivilege() == expected);
}

static void testExecutablePath(void)
{
  char * path = lgProcessGetExecutablePath(NULL);
  CHECK(path);
  CHECK(path[0] == '/');

  struct stat actual;
  struct stat expected;
  CHECK(stat(path, &actual) == 0);
  CHECK(stat("/proc/self/exe", &expected) == 0);
  CHECK(actual.st_dev == expected.st_dev);
  CHECK(actual.st_ino == expected.st_ino);
  free(path);
}

static void testHandledStop(void)
{
  const int opaque = 42;

  CHECK(!lgProcessInstallStopHandler(NULL, NULL));
  CHECK(lgProcessInstallStopHandler(acceptStop, (void *)&opaque));
  CHECK(!lgProcessInstallStopHandler(acceptStop, (void *)&opaque));

  struct sigaction action;
  CHECK(sigaction(SIGINT, NULL, &action) == 0);
  CHECK(action.sa_flags & SA_RESTART);

  raise(SIGINT);
  CHECK(stopCount == 1);

  raise(SIGTERM);
  CHECK(stopCount == 2);

  lgProcessRemoveStopHandler();
  lgProcessRemoveStopHandler();
}

static void testPreviousHandlerRestored(void)
{
  struct sigaction previous;
  struct sigaction action = { 0 };
  action.sa_handler = restoredHandler;
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGINT, &action, &previous) == 0);

  CHECK(lgProcessInstallStopHandler(acceptStop, NULL));
  lgProcessRemoveStopHandler();
  raise(SIGINT);
  CHECK(restoredCount == 1);

  CHECK(sigaction(SIGINT, &previous, NULL) == 0);
}

static void testRejectedStopTerminates(void)
{
  const pid_t child = fork();
  CHECK(child >= 0);
  if (child == 0)
  {
    if (!lgProcessInstallStopHandler(rejectStop, NULL))
      _exit(EXIT_FAILURE);
    raise(SIGTERM);
    _exit(EXIT_FAILURE);
  }

  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGTERM);
}

int main(void)
{
  testPrivilege();
  testExecutablePath();
  testHandledStop();
  testPreviousHandlerRestored();
  testRejectedStopTerminates();
  return 0;
}
