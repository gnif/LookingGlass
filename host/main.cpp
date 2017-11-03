/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

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

#include <Windows.h>
#include "common\debug.h"

#include "Service.h"

#ifdef DEBUG
#include <io.h>
#include <fcntl.h>
#include <iostream>
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR szCmdParam, int iCmdShow)
{
#ifdef DEBUG
  {
    HANDLE _handle;
    int    _conout;
    FILE * fp;

    AllocConsole();

    CONSOLE_SCREEN_BUFFER_INFO conInfo;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &conInfo);
    conInfo.dwSize.Y = 500;
    SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), conInfo.dwSize);

    _handle = GetStdHandle(STD_INPUT_HANDLE);
    _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
    fp = _fdopen(_conout, "r");
    freopen_s(&fp, "CONIN$", "r", stdin);

    _handle = GetStdHandle(STD_OUTPUT_HANDLE);
    _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
    fp      = _fdopen(_conout, "w");
    freopen_s(&fp, "CONOUT$", "w", stdout);

    _handle = GetStdHandle(STD_ERROR_HANDLE);
    _conout = _open_osfhandle((intptr_t)_handle, _O_TEXT);
    fp = _fdopen(_conout, "w");
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::ios::sync_with_stdio();
    std::wcout.clear();
    std::cout.clear();
    std::wcerr.clear();
    std::cerr.clear();
    std::wcin.clear();
    std::cin.clear();
  }
#endif

  Service *svc = svc->Get();
  if (!svc->Initialize())
  {
    DEBUG_ERROR("Failed to initialize service");
    return -1;
  }

  while (true)
    if (!svc->Process())
      break;

  svc->DeInitialize();
#ifdef DEBUG
  getc(stdin);
#endif
  return 0;
}