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

#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <shellapi.h>

static bool bindStandardStream(FILE * stream, DWORD streamId)
{
  const HANDLE handle = GetStdHandle(streamId);
  if (!handle || handle == INVALID_HANDLE_VALUE)
    return false;

  HANDLE duplicate = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
        &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return false;

  const int sourceFd = _open_osfhandle((intptr_t)duplicate, _O_TEXT);
  if (sourceFd < 0)
  {
    CloseHandle(duplicate);
    return false;
  }

  FILE * reopened = NULL;
  if (freopen_s(&reopened, "NUL", "w", stream))
  {
    _close(sourceFd);
    return false;
  }

  const int targetFd = _fileno(reopened);
  if (targetFd < 0 || _dup2(sourceFd, targetFd))
  {
    _close(sourceFd);
    return false;
  }

  _close(sourceFd);
  clearerr(reopened);
  setvbuf(reopened, NULL, _IONBF, 0);
  return true;
}

static void bindStandardStreams(void)
{
  const bool stdoutBound = bindStandardStream(stdout, STD_OUTPUT_HANDLE);
  const bool stderrBound = bindStandardStream(stderr, STD_ERROR_HANDLE);
  if ((stdoutBound && stderrBound) || IsDebuggerPresent())
    return;

  if (!AttachConsole(ATTACH_PARENT_PROCESS))
    return;

  if (!stdoutBound)
    bindStandardStream(stdout, STD_OUTPUT_HANDLE);
  if (!stderrBound)
    bindStandardStream(stderr, STD_ERROR_HANDLE);
}

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

  bindStandardStreams();

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
