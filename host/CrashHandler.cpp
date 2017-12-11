/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
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

#include "CrashHandler.h"

typedef BOOL (WINAPI * PMiniDumpWriteDump)(
    _In_ HANDLE hProcess,
    _In_ DWORD ProcessId,
    _In_ HANDLE hFile,
    _In_ MINIDUMP_TYPE DumpType,
    _In_opt_ PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
    _In_opt_ PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
    _In_opt_ PMINIDUMP_CALLBACK_INFORMATION CallbackParam
  );

void CrashHandler::Initialize()
{
  SetUnhandledExceptionFilter(CrashHandler::ExceptionFilter);
}

LONG WINAPI CrashHandler::ExceptionFilter(struct _EXCEPTION_POINTERS * apExceptionInfo)
{
  HMODULE lib;
  PMiniDumpWriteDump fn_MiniDumpWriteDump;

  lib = LoadLibraryA("dbghelp.dll");
  if (!lib)
    return EXCEPTION_CONTINUE_SEARCH;

  fn_MiniDumpWriteDump = (PMiniDumpWriteDump)GetProcAddress(lib, "MiniDumpWriteDump");
  if (!fn_MiniDumpWriteDump)
    return EXCEPTION_CONTINUE_SEARCH;

  HANDLE hFile = CreateFileA(
    "looking-glass-host.dump",
    GENERIC_WRITE,
    FILE_SHARE_WRITE,
    NULL,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL,
    NULL
  );

  if (hFile == INVALID_HANDLE_VALUE)
    return EXCEPTION_CONTINUE_SEARCH;

  _MINIDUMP_EXCEPTION_INFORMATION info;
  info.ThreadId          = GetCurrentThreadId();
  info.ExceptionPointers = apExceptionInfo;
  info.ClientPointers    = FALSE;

  fn_MiniDumpWriteDump(
    GetCurrentProcess(),
    GetCurrentProcessId(),
    hFile,
    MiniDumpNormal,
    &info,
    NULL,
    NULL
  );

  CloseHandle(hFile);
  return EXCEPTION_CONTINUE_SEARCH;
}