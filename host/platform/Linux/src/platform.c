/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#include "interface/platform.h"
#include "common/debug.h"
#include "common/option.h"
#include "common/thread.h"

#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

struct app
{
  const char * executable;
  char * dataPath;
};

struct app app = { 0 };

int main(int argc, char * argv[])
{
  app.executable = argv[0];

  struct passwd * pw = getpwuid(getuid());
  alloc_sprintf(&app.dataPath, "%s/", pw->pw_dir);

  int result = app_main(argc, argv);

  free(app.dataPath);
  return result;
}

void sigHandler(int signo)
{
  DEBUG_INFO("SIGINT");
  app_quit();
}

bool app_init()
{
  signal(SIGINT, sigHandler);
  return true;
}

const char * os_getExecutable()
{
  return app.executable;
}

const char * os_getDataPath()
{
  return app.dataPath;
}
