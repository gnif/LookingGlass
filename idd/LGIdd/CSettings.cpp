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

#include "CSettings.h"
#include "CDebug.h"
#include "DefaultDisplayModes.h"

#include <wdf.h>

CSettings g_settings;

#define LGIDD_REGKEY L"SOFTWARE\\LookingGlass\\IDD"

CSettings::CSettings()
{
}

void CSettings::LoadModes()
{
  const unsigned defaultRefresh = GetDefaultRefresh();
  m_displayModes.clear();

  bool hasPreferred = false;
  DisplayMode m;
  if (GetExtraMode(m))
  {
    DEBUG_INFO("ExtraMode: %ux%u@%u%s", m.width, m.height, m.refresh, m.preferred ? "*" : "");
    m_displayModes.push_back(m);
    hasPreferred = m.preferred;
  }

  std::vector<std::wstring> entries;
  if (!ReadModesValue(entries))
  {
    m_displayModes.reserve(m_displayModes.size() +
      ARRAYSIZE(DefaultDisplayModes));

    for (int i = 0; i < ARRAYSIZE(DefaultDisplayModes); ++i)
    {
      m.width     = DefaultDisplayModes[i][0];
      m.height    = DefaultDisplayModes[i][1];
      m.refresh   = defaultRefresh;
      m.preferred = !hasPreferred && (i == DefaultPreferredDisplayMode);
      m_displayModes.push_back(m);
    }
    return;
  }

  DEBUG_INFO("Parsed Modes:");
  for (const auto& line : entries)
    if (ParseModeString(line, m))
    {
      DEBUG_INFO("  %ux%u@%u%s", m.width, m.height, m.refresh, m.preferred ? "*" : "");

      if (hasPreferred)
        m.preferred = false;
      m_displayModes.push_back(m);
    }
}

void CSettings::SetExtraMode(const DisplayMode& mode)
{
  WCHAR buf[64];
  _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ux%u@%u%s",
    mode.width, mode.height, mode.refresh,
    mode.preferred ? L"*" : L"");

  HKEY  hKey = NULL;
  DWORD disp = 0;
  LONG  ec = RegCreateKeyExW(
    HKEY_LOCAL_MACHINE,
    LGIDD_REGKEY,
    0, NULL, REG_OPTION_NON_VOLATILE,
    KEY_SET_VALUE,
    NULL, &hKey, &disp);

  if (ec != ERROR_SUCCESS)
  {
    DEBUG_INFO("Failed to write key");
    return;
  }

  const WCHAR* valueName = L"ExtraMode";
  const DWORD  cb = (DWORD)((wcslen(buf) + 1) * sizeof(WCHAR));

  RegSetValueExW(hKey, valueName, 0, REG_SZ, (const BYTE*)buf, cb);
  RegCloseKey(hKey);
}

std::wstring CSettings::ReadStringValue(const wchar_t* name, const wchar_t* defaultValue)
{
  if (!defaultValue)
    defaultValue = L"";

  HKEY hKey = nullptr;
  LONG ec = RegOpenKeyExW(
    HKEY_LOCAL_MACHINE,
    LGIDD_REGKEY,
    0,
    KEY_QUERY_VALUE,
    &hKey
  );

  if (ec != ERROR_SUCCESS)
    return std::wstring(defaultValue);

  DWORD type = 0;
  DWORD cb = 0;

  ec = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb);
  if (ec != ERROR_SUCCESS ||
    (type != REG_SZ && type != REG_EXPAND_SZ) ||
    cb == 0 ||
    (cb % sizeof(wchar_t)) != 0)
  {
    RegCloseKey(hKey);
    return std::wstring(defaultValue);
  }

  std::vector<wchar_t> buf((cb / sizeof(wchar_t)) + 1, L'\0');

  DWORD type2 = 0;
  DWORD cb2 = cb;

  ec = RegQueryValueExW(
    hKey,
    name,
    nullptr,
    &type2,
    reinterpret_cast<LPBYTE>(buf.data()),
    &cb2
  );

  RegCloseKey(hKey);

  if (ec != ERROR_SUCCESS || (type2 != REG_SZ && type2 != REG_EXPAND_SZ) ||
    cb2 == 0 || (cb2 % sizeof(wchar_t)) != 0 || cb2 > cb)
    return std::wstring(defaultValue);

  buf[cb2 / sizeof(wchar_t)] = L'\0';
  return std::wstring(buf.data());
}

bool CSettings::ReadBoolValue(const wchar_t* name, bool defaultValue)
{
  HKEY hKey = nullptr;
  LONG ec = RegOpenKeyExW(
    HKEY_LOCAL_MACHINE,
    LGIDD_REGKEY,
    0,
    KEY_QUERY_VALUE,
    &hKey
  );

  if (ec != ERROR_SUCCESS)
    return defaultValue;

  DWORD type = 0;
  DWORD cb = 0;

  ec = RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &cb);
  if (ec != ERROR_SUCCESS || type != REG_DWORD || cb != sizeof(DWORD))
  {
    RegCloseKey(hKey);
    return defaultValue;
  }

  DWORD value = 0;
  DWORD type2 = 0;
  DWORD cb2 = sizeof(value);

  ec = RegQueryValueExW(
    hKey,
    name,
    nullptr,
    &type2,
    reinterpret_cast<LPBYTE>(&value),
    &cb2
  );

  RegCloseKey(hKey);

  if (ec != ERROR_SUCCESS || type2 != REG_DWORD || cb2 != sizeof(DWORD))
    return defaultValue;

  return value != 0;
}

bool CSettings::GetExtraMode(DisplayMode& mode)
{
  std::wstring extraMode = ReadStringValue(L"ExtraMode", NULL);
  if (extraMode.empty())
    return false;

  return ParseModeString(extraMode, mode);
}

unsigned CSettings::GetDefaultRefresh() const
{
  DWORD refresh = 60;
  DWORD cb      = sizeof(refresh);
  HKEY  hKey    = nullptr;

  LONG st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LGIDD_REGKEY, 0, KEY_QUERY_VALUE, &hKey);
  if (st == ERROR_SUCCESS)
  {
    DWORD type = 0;
    st = RegGetValueW(hKey, nullptr, L"DefaultRefresh", RRF_RT_REG_DWORD, &type, &refresh, &cb);
    RegCloseKey(hKey);
  }

  if (st != ERROR_SUCCESS || refresh < 30 || refresh > 1000)
    return 60;

  return refresh;
}

bool CSettings::ReadModesValue(std::vector<std::wstring> &out) const
{
  HKEY hKey = nullptr;
  LONG st   = RegOpenKeyExW(HKEY_LOCAL_MACHINE, LGIDD_REGKEY, 0, KEY_QUERY_VALUE, &hKey);
  if (st != ERROR_SUCCESS)
    return false;

  DWORD type = 0, cb = 0;
  st = RegGetValueW(hKey, nullptr, L"Modes", RRF_RT_REG_MULTI_SZ, &type, nullptr, &cb);
  if (st != ERROR_SUCCESS || cb == 0)  
  {
    RegCloseKey(hKey);
    return false;
  }

  std::vector<wchar_t> buf(cb / sizeof(wchar_t));
  st = RegGetValueW(hKey, nullptr, L"Modes", RRF_RT_REG_MULTI_SZ, &type, buf.data(), &cb);
  RegCloseKey(hKey);
  if (st != ERROR_SUCCESS)
    return false;

  const wchar_t* p = buf.data();
  while (*p)
  {
    out.emplace_back(p);
    p += (wcslen(p) + 1);
  }

  return !out.empty();
}

static std::wstring trim(const std::wstring &s)
{
  size_t b = 0, e = s.size();
  while (b < e && iswspace(s[b]))
    ++b;
  while (e > b && iswspace(s[e - 1]))
    --e;
  return s.substr(b, e - b);
}

static bool toUnsigned(const std::wstring &t, unsigned &v)
{
  if (t.empty())
    return false;

  wchar_t* end = nullptr;
  unsigned long tmp = wcstoul(t.c_str(), &end, 10);

  if (!end || *end != L'\0')
    return false;

  v = (unsigned)tmp;
  return true;
}

bool CSettings::ParseModeString(const std::wstring& in, DisplayMode& out)
{
  std::wstring s = trim(in);
  if (s.empty())
    return false;

  out.preferred = s[s.size() - 1] == L'*';
  if (out.preferred)
    s = trim(s.substr(0, s.size() - 1));

  size_t xPos  = s.find(L'x');
  size_t atPos = s.find(L'@', (xPos == std::wstring::npos ? 0 : xPos + 1));
  if (xPos == std::wstring::npos || atPos == std::wstring::npos ||
    xPos == 0 || atPos <= xPos + 1 || atPos + 1 >= s.size())
    return false;

  if (!toUnsigned(s.substr(0, xPos), out.width) ||
      !toUnsigned(s.substr(xPos + 1, atPos - (xPos + 1)), out.height) ||
      !toUnsigned(s.substr(atPos + 1), out.refresh))
      return false;

  // sanity check
  if (out.width   < 640   ||
      out.height  < 480   ||
      out.width   > 16384 ||
      out.height  > 16384 ||
      out.refresh < 30    ||
      out.refresh > 1000)
    return false;

  return true;
}
