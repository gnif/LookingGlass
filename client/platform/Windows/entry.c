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

#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

static void freeArguments(int argc, char * argv[])
{
  if (!argv)
    return;

  for (int i = 0; i < argc; ++i)
    free(argv[i]);
  free(argv);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance,
    LPSTR commandLine, int showCommand)
{
  (void)instance;
  (void)previousInstance;
  (void)commandLine;
  (void)showCommand;

  int        argc     = 0;
  wchar_t ** wideArgv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!wideArgv || argc < 1)
  {
    if (wideArgv)
      LocalFree(wideArgv);
    return EXIT_FAILURE;
  }

  char ** argv = calloc((size_t)argc + 1U, sizeof(*argv));
  if (!argv)
  {
    LocalFree(wideArgv);
    return EXIT_FAILURE;
  }

  for (int i = 0; i < argc; ++i)
  {
    argv[i] = lgWideToUtf8(wideArgv[i]);
    if (!argv[i])
    {
      freeArguments(argc, argv);
      LocalFree(wideArgv);
      return EXIT_FAILURE;
    }
  }
  LocalFree(wideArgv);

  const int result = lgClientMain(argc, argv);
  freeArguments(argc, argv);
  return result;
}
