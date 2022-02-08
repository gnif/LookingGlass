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

#include "interface/platform.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/stringutils.h"
#include "common/thread.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/utsname.h>

struct app
{
  const char * executable;
  char * dataPath;
  char * osVersion;
};

struct app app = { 0 };

int main(int argc, char * argv[])
{
  // initialize for DEBUG_* macros
  debug_init();

  app.executable = argv[0];

  struct passwd * pw = getpwuid(getuid());
  alloc_sprintf(&app.dataPath, "%s/", pw->pw_dir);

  int result = app_main(argc, argv);

  free(app.dataPath);
  free(app.osVersion);
  return result;
}

void sigHandler(int signo)
{
  DEBUG_INFO("SIGINT");
  app_quit();
}

bool app_init(void)
{
  signal(SIGINT, sigHandler);
  return true;
}

const char * os_getExecutable(void)
{
  return app.executable;
}

const char * os_getDataPath(void)
{
  return app.dataPath;
}

bool os_getAndClearPendingActivationRequest(void)
{
  // TODO
  return false;
}

bool os_blockScreensaver(void)
{
  return false;
}

void os_showMessage(const char * caption, const char * msg)
{
  DEBUG_INFO("%s: %s", caption, msg);
}

bool os_hasSetCursorPos(void)
{
  return false;
}

void os_setCursorPos(int x, int y)
{
}

KVMFROS os_getKVMFRType(void)
{
  return KVMFR_OS_LINUX;
}

static const char * getPrettyName(void)
{
  static char buffer[256];

  FILE * fp = fopen("/etc/os-release", "r");
  if (fp == NULL)
  {
    fp = fopen("/usr/lib/os-release", "r");
    if (fp == NULL)
      return NULL;
  }

  while (fgets(buffer, sizeof(buffer), fp))
  {
    if (strstr(buffer, "PRETTY_NAME"))
    {
      char * ptr = strchr(buffer, '=') + 1;
      while (isspace(*ptr))
        ++ptr;

      size_t len = strlen(ptr);
      while (isspace(ptr[len - 1]))
        --len;

      if (*ptr == '"' || *ptr == '\'')
      {
        ++ptr;
        len -= 2;
      }

      ptr[len] = '\0';
      fclose(fp);
      return ptr;
    }

    // If a line is too long, skip it.
    while (buffer[strlen(buffer) - 1] != '\n')
      if (!fgets(buffer, sizeof(buffer), fp))
        goto done;
  }

done:
  fclose(fp);
  return NULL;
}

const char * os_getOSName(void)
{
  if (app.osVersion)
    return app.osVersion;

  const char * pretty = getPrettyName();
  struct utsname utsname;
  uname(&utsname);

  if (!pretty)
    alloc_sprintf(
      &app.osVersion,
      "%s %s on %s",
      utsname.sysname,
      utsname.release,
      utsname.machine
    );
  else
    alloc_sprintf(
      &app.osVersion,
      "%s, kernel: %s %s on %s",
      pretty,
      utsname.sysname,
      utsname.release,
      utsname.machine
    );

  return app.osVersion;
}

const uint8_t * os_getUUID(void)
{
  //TODO
  return NULL;
}
