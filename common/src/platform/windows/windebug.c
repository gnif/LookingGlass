/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#include "common/windebug.h"
#include <stdio.h>

void DebugWinError(const char * file, const unsigned int line, const char * function, const char * desc, HRESULT status)
{
  char *buffer;
  FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
    NULL,
    status,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (char*)&buffer,
    1024,
    NULL
  );

  for(size_t i = strlen(buffer) - 1; i > 0; --i)
    if (buffer[i] == '\n' || buffer[i] == '\r')
      buffer[i] = 0;

  fprintf(stderr, "%12" PRId64 " [E] %20s:%-4u | %-30s | %s: 0x%08x (%s)\n", microtime(), file, line, function, desc, (int)status, buffer);
  LocalFree(buffer);
}

/* credit for this function to: https://stackoverflow.com/questions/17399302/how-can-i-detect-windows-8-1-in-a-desktop-application */
inline static BOOL CompareWindowsVersion(DWORD dwMajorVersion, DWORD dwMinorVersion)
{
  OSVERSIONINFOEX ver;
  DWORDLONG dwlConditionMask = 0;

  ZeroMemory(&ver, sizeof(OSVERSIONINFOEX));
  ver.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
  ver.dwMajorVersion = dwMajorVersion;
  ver.dwMinorVersion = dwMinorVersion;

  VER_SET_CONDITION(dwlConditionMask, VER_MAJORVERSION, VER_EQUAL);
  VER_SET_CONDITION(dwlConditionMask, VER_MINORVERSION, VER_EQUAL);

  return VerifyVersionInfo(&ver, VER_MAJORVERSION | VER_MINORVERSION, dwlConditionMask);
}

bool IsWindows8()
{
  return
    (CompareWindowsVersion(6, 3) == TRUE) ||
    (CompareWindowsVersion(6, 2) == TRUE);
}
