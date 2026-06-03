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

#include "Devices.h"
#include <windows.h>
#include <devguid.h>
#include <setupapi.h>
#include <CDebug.h>

inline static bool wprefix(const wchar_t *pre, const wchar_t *str)
{
  return wcsncmp(pre, str, wcslen(pre)) == 0;
}

bool checkGPU(bool &found)
{
  found = false;

  HDEVINFO hDevInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, NULL, NULL, DIGCF_PRESENT);
  if (hDevInfo == INVALID_HANDLE_VALUE)
  {
    DEBUG_WARN_HR(GetLastError(), L"SetupDiGetClassDevsW");
    return false;
  }

  SP_DEVINFO_DATA devInfo = { 0 };
  devInfo.cbSize = sizeof devInfo;

  for (DWORD dwIndex = 0; SetupDiEnumDeviceInfo(hDevInfo, dwIndex, &devInfo); ++dwIndex)
  {
    DWORD dwSizeRequired;
    DWORD dwPropertyType;
    SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID, &dwPropertyType, NULL, 0, &dwSizeRequired);

    DWORD dwLastError = GetLastError();
    if (dwLastError == ERROR_INVALID_DATA)
      continue;
    else if (dwLastError != ERROR_INSUFFICIENT_BUFFER)
    {
      DEBUG_WARN_HR(GetLastError(), L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) size calculation");
      goto fail;
    }

    if (dwPropertyType != REG_MULTI_SZ)
    {
      DEBUG_WARN(L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) returned wrong type");
      goto fail;
    }

    LPWSTR lpBuffer = (LPWSTR)malloc(dwSizeRequired);
    if (!lpBuffer)
    {
      DEBUG_WARN(L"failed to allocate memory for SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID)");
      goto fail;
    }

    if (!SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfo, SPDRP_HARDWAREID, &dwPropertyType, (PBYTE)lpBuffer, dwSizeRequired, NULL))
    {
      DEBUG_WARN_HR(GetLastError(), L"SetupDiGetDeviceRegistryPropertyW(SPDRP_HARDWAREID) for real");
      free(lpBuffer);
      goto fail;
    }

    for (LPWSTR lpHwId = lpBuffer; *lpHwId; lpHwId += wcslen(lpBuffer) + 1)
    {
      if (
        wprefix(L"PCI\\VEN_10DE&", lpHwId) || // Nvidia
        wprefix(L"PCI\\VEN_1002&", lpHwId) || // AMD
        wprefix(L"PCI\\VEN_8086&", lpHwId)    // Intel
      )
      {
        found = true;
        break;
      }
    }

    free(lpBuffer);

    if (found)
      break;
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  return true;

fail:
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return false;
}
