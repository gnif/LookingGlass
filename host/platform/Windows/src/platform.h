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

#include <stdbool.h>
#include <windows.h>

/*
 * Windows 10 provides this API via kernel32.dll as well as advapi32.dll and
 * mingw opts for linking against the kernel32.dll version which is fine
 * provided you don't intend to run this on earlier versions of windows. As such
 * we need to lookup this method at runtime. */
typedef WINBOOL WINAPI (*CreateProcessAsUserA_t)(HANDLE hToken,
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    WINBOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation);
extern CreateProcessAsUserA_t f_CreateProcessAsUserA;

#define WM_CALL_FUNCTION (WM_USER+1)
#define WM_TRAYICON      (WM_USER+2)

typedef LRESULT (*CallFunction)(WPARAM wParam, LPARAM lParam);
struct MSG_CALL_FUNCTION
{
  CallFunction fn;
  WPARAM       wParam;
  LPARAM       lParam;
};

bool windowsSetupAPI(void);
const char *getSystemLogDirectory(void);
LRESULT sendAppMessage(UINT Msg, WPARAM wParam, LPARAM lParam);
