#include "CRegistrySettings.h"

#include <optional>
#include <regex>
#include <CDebug.h>

#define LGIDD_REGKEY L"SOFTWARE\\LookingGlass\\IDD"

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

static std::wregex displayMode(L"(\\d+)x(\\d+)@(\\d+)(\\*)?");

static std::optional<DisplayMode> parseDisplayMode(const std::wstring &str)
{
  std::wstring trimmed = trim(str);
  std::wsmatch match;

  if (!std::regex_match(trimmed, match, displayMode))
    return {};

  DisplayMode mode;
  mode.width = std::stoul(match[1]);
  mode.height = std::stoul(match[2]);
  mode.refresh = std::stoul(match[3]);
  mode.preferred = match[4] == L"*";
  return mode;
}

std::optional<std::vector<DisplayMode>> CRegistrySettings::getModes()
{
  LSTATUS status;

  DWORD type = 0, cb = 0;
  status = RegGetValue(hKey, nullptr, L"Modes", RRF_RT_REG_MULTI_SZ, &type, nullptr, &cb);
  if (status != ERROR_SUCCESS)
  {
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
  serialized.append(std::to_wstring(refresh));
  if (preferred)
    serialized.push_back('*');
  return serialized;
}
