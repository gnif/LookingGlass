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

#pragma once
#include "transport/TransportConfig.h"

#include <windows.h>
#include <vector>
#include <string>

class CSettings
{
  public:
    struct DisplayMode
    {
      unsigned width;
      unsigned height;
      unsigned refreshMilliHz;
      bool     preferred;
      bool     extraMode;
    };
    typedef std::vector<DisplayMode> DisplayModes;

    CSettings();

    DisplayModes LoadModes();
    TransportInstances LoadTransportInstances() const;
    bool SetExtraMode(const DisplayMode & mode);
    bool GetExtraMode(DisplayMode & mode);
    unsigned GetDefaultRefreshMilliHz() const;

    std::wstring ReadStringValue(const wchar_t* name, const wchar_t* defaultValue = nullptr);
    static bool ReadBoolValue(
      const wchar_t * name, bool defaultValue = false)
    {
      HKEY hKey   = nullptr;
      LONG status = RegOpenKeyExW(
        HKEY_LOCAL_MACHINE,
        RegistryKey(),
        0,
        KEY_QUERY_VALUE,
        &hKey);
      if (status != ERROR_SUCCESS)
        return defaultValue;

      DWORD type = 0;
      DWORD size = 0;
      status = RegQueryValueExW(
        hKey, name, nullptr, &type, nullptr, &size);
      if (status != ERROR_SUCCESS ||
          type   != REG_DWORD     ||
          size   != sizeof(DWORD))
      {
        RegCloseKey(hKey);
        return defaultValue;
      }

      DWORD value     = 0;
      DWORD valueType = 0;
      DWORD valueSize = sizeof(value);
      status = RegQueryValueExW(
        hKey,
        name,
        nullptr,
        &valueType,
        reinterpret_cast<LPBYTE>(&value),
        &valueSize);
      RegCloseKey(hKey);
      if (status    != ERROR_SUCCESS ||
          valueType != REG_DWORD     ||
          valueSize != sizeof(value))
        return defaultValue;

      return value != 0;
    }

    static bool ShouldLogStatistics()
    {
      return ReadBoolValue(L"LogStatistics");
    }

  private:
    static const wchar_t * RegistryKey()
    {
      return L"SOFTWARE\\LookingGlass\\IDD";
    }

    bool ReadMultiStringValue(const wchar_t * name,
      std::vector<std::wstring>& out) const;
    bool ReadModesValue(std::vector<std::wstring> &out) const;
    bool ParseModeString(const std::wstring& in, DisplayMode& out);
};

extern CSettings g_settings;
