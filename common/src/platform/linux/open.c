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

#include "common/open.h"
#include "common/debug.h"

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool xdgOpen(const char * path)
{
  pid_t pid = fork();
  if (pid == 0)
  {
    // setsid and fork again to detach the xdg-open process.
    setsid();
    pid_t pid = fork();
    if (pid == 0)
    {
      execlp("xdg-open", "xdg-open", path, NULL);
      _exit(127);
    }
    _exit(pid < 0);
  }
  else if (pid < 0)
  {
    DEBUG_ERROR("Failed to launch xdg-open: %s", strerror(errno));
    return false;
  }
  else
  {
    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
      DEBUG_ERROR("waitpid failed: %s", strerror(errno));
      return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
      return true;

    if (WIFEXITED(status))
      DEBUG_ERROR("helper process exited with code %d", WEXITSTATUS(status));
    else
      DEBUG_ERROR("helper process exited with signal: %s", strsignal(WTERMSIG(status)));
    return false;
  }
}

bool lgOpenURL(const char * url)
{
  return xdgOpen(url);
}
