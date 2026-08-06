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

#include "CRegistrySettings.h"

#include <optional>
#include <regex>
#include <CDebug.h>

#include "DefaultDisplayModes.h"
#include "RefreshRate.h"

#define LGIDD_REGKEY L"SOFTWARE\\LookingGlass\\IDD"

const DWORD DEFAULT_REFRESH = 120000;

CRegistrySettings::CRegistrySettings() : hKey(nullptr) {}

CRegistrySettings::~CRegistrySettings()
{
  if (hKey)
    RegCloseKey(hKey);
}

LSTATUS CRegistrySettings::open()
{
  HKEY key;
  LSTATUS result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, LGIDD_REGKEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);

  if (result == ERROR_SUCCESS)
    hKey = key;
  
  return result;
}

template<class T>
static std::basic_string<T> trim(const std::basic_string<T> &s)
{
  size_t b = 0, e = s.size();
  while (b < e && iswspace(s[b]))
    ++b;
  while (e > b && iswspace(s[e - 1]))
    --e;
  return s.substr(b, e - b);
}

static std::wregex displayMode(
  L"(\\d+)x(\\d+)@(\\d+(?:\\.\\d{1,3})?)(\\*)?");

static std::optional<DisplayMode> parseDisplayMode(const std::wstring &str)
{
  std::wstring trimmed = trim(str);
  std::wsmatch match;

  if (!std::regex_match(trimmed, match, displayMode))
    return {};

  DisplayMode mode;
  mode.width  = std::stoul(match[1]);
  mode.height = std::stoul(match[2]);
  if (!LGParseRefreshRate(match[3], mode.refreshMilliHz))
    return {};
  mode.preferred = match[4] == L"*";
  return mode;
}

std::vector<DisplayMode> CRegistrySettings::getDefaultModes()
{
  auto defaultRefresh = getDefaultRefresh();
  const unsigned refreshMilliHz =
    defaultRefresh ? *defaultRefresh : DEFAULT_REFRESH;

  std::vector<DisplayMode> result;
  for (int i = 0; i < ARRAYSIZE(DefaultDisplayModes); ++i)
  {
    DisplayMode mode;
    mode.width          = DefaultDisplayModes[i][0];
    mode.height         = DefaultDisplayModes[i][1];
    mode.refreshMilliHz = refreshMilliHz;
    mode.preferred      = i == DefaultPreferredDisplayMode;
    result.emplace_back(mode);
  }
  return result;
}

std::optional<std::vector<DisplayMode>> CRegistrySettings::getModes()
{
  LSTATUS status;

  DWORD type = 0, cb = 0;
  status = RegGetValue(hKey, nullptr, L"Modes", RRF_RT_REG_MULTI_SZ, &type, nullptr, &cb);
  switch (status)
  {
  case ERROR_SUCCESS:
    break;
  case ERROR_FILE_NOT_FOUND:
    return getDefaultModes();
  default:
    DEBUG_ERROR_HR(status, "RegGetValue(Modes) length computation");
    return {};
  }

  LPWSTR buf = (LPWSTR) malloc(cb);
  if (!buf)
  {
    DEBUG_ERROR("Failed to allocate memory for RegGetValue(Modes)");
    return {};
  }

  status = RegGetValueW(hKey, nullptr, L"Modes", RRF_RT_REG_MULTI_SZ, &type, buf, &cb);
  if (status != ERROR_SUCCESS)
  {
    DEBUG_ERROR_HR(status, "RegGetValue(Modes) read");
    free(buf);
    return {};
  }

  std::vector<DisplayMode> result;
  for (LPWSTR s = buf; *s; s += wcslen(s) + 1)
  {
    auto mode = parseDisplayMode(s);
    if (mode.has_value())
      result.emplace_back(std::move(mode.value()));
  }
  free(buf);
  return result;
}

LSTATUS CRegistrySettings::setModes(const std::vector<DisplayMode> &modes)
{
  std::wstring serialized;
  for (auto mode : modes)
  {
    serialized.append(mode.toString());
    serialized.push_back('\0');
  }

  return RegSetValueEx(hKey, L"Modes", 0, REG_MULTI_SZ, (PBYTE)serialized.c_str(),
    (DWORD)(serialized.length() + 1) * sizeof(wchar_t));
}

std::wstring DisplayMode::toString()
{
  std::wstring serialized;
  serialized.append(std::to_wstring(width));
  serialized.push_back('x');
  serialized.append(std::to_wstring(height));
  serialized.push_back('@');
  serialized.append(LGFormatRefreshRate(refreshMilliHz));
  if (preferred)
    serialized.push_back('*');
  return serialized;
}

std::optional<DWORD> CRegistrySettings::getDefaultRefresh()
{
  DWORD type = 0;
  DWORD size = 0;
  LSTATUS status = RegQueryValueExW(
    hKey, L"DefaultRefresh", nullptr, &type, nullptr, &size);
  if (status == ERROR_FILE_NOT_FOUND)
    return DEFAULT_REFRESH;
  if (status != ERROR_SUCCESS)
  {
    DEBUG_ERROR_HR(status, "RegQueryValueEx(DefaultRefresh)");
    return {};
  }

  if (type == REG_DWORD && size == sizeof(DWORD))
  {
    DWORD refresh = 0;
    status = RegQueryValueExW(hKey, L"DefaultRefresh", nullptr, &type,
      (LPBYTE)&refresh, &size);
    if (status == ERROR_SUCCESS && refresh >= 24 && refresh <= 1000)
      return refresh * 1000;
  }
  else if ((type == REG_SZ || type == REG_EXPAND_SZ) && size &&
      size % sizeof(wchar_t) == 0)
  {
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, L'\0');
    status = RegQueryValueExW(hKey, L"DefaultRefresh", nullptr, &type,
      (LPBYTE)value.data(), &size);
    if (status == ERROR_SUCCESS)
    {
      unsigned refreshMilliHz;
      if (LGParseRefreshRate(value.data(), refreshMilliHz))
        return refreshMilliHz;
    }
  }

  DEBUG_ERROR("Invalid DefaultRefresh value");
  return {};
}

LSTATUS CRegistrySettings::setDefaultRefresh(DWORD refreshMilliHz)
{
  const std::wstring value = LGFormatRefreshRate(refreshMilliHz);
  return RegSetValueExW(hKey, L"DefaultRefresh", 0, REG_SZ,
    (const BYTE *)value.c_str(),
    (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

std::optional<bool> CRegistrySettings::getNoGPU()
{
  DWORD result, cbData = sizeof result;

  LSTATUS status = RegGetValue(hKey, nullptr, L"NoGPU", RRF_RT_REG_DWORD, nullptr, &result, &cbData);
  switch (status)
  {
  case ERROR_SUCCESS:
    return !!result;
  case ERROR_FILE_NOT_FOUND:
    return false;
  default:
    DEBUG_ERROR_HR(status, "RegGetValue(NoGPU)");
    return {};
  }
}

LSTATUS CRegistrySettings::setNoGPU(bool noGPU)
{
  DWORD dwValue = noGPU;
  return RegSetValueEx(hKey, L"NoGPU", 0, REG_DWORD, (LPBYTE)&dwValue, sizeof(DWORD));
}

std::optional<bool> CRegistrySettings::getExclusiveMonitor()
{
  DWORD result, cbData = sizeof result;

  LSTATUS status = RegGetValue(hKey, nullptr, L"ExclusiveMonitor", RRF_RT_REG_DWORD, nullptr, &result, &cbData);
  switch (status)
  {
  case ERROR_SUCCESS:
    return !!result;
  case ERROR_FILE_NOT_FOUND:
    return true;
  default:
    DEBUG_ERROR_HR(status, "RegGetValue(ExclusiveMonitor)");
    return {};
  }
}

LSTATUS CRegistrySettings::setExclusiveMonitor(bool exclusive)
{
  DWORD dwValue = exclusive;
  return RegSetValueEx(hKey, L"ExclusiveMonitor", 0, REG_DWORD, (LPBYTE)&dwValue, sizeof(DWORD));
}
