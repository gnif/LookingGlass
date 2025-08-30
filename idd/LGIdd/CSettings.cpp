#include "CSettings.h"
#include "CDebug.h"

#include <wdf.h>

CSettings g_settings;
static const std::wstring subKey = L"SOFTWARE\\LookingGlass\\IDD";

static const DWORD DefaultDisplayModes[][3] =
{
  {7680, 4800, 120}, {7680, 4320, 120}, {6016, 3384, 120}, {5760, 3600, 120},
  {5760, 3240, 120}, {5120, 2800, 120}, {4096, 2560, 120}, {4096, 2304, 120},
  {3840, 2400, 120}, {3840, 2160, 120}, {3200, 2400, 120}, {3200, 1800, 120},
  {3008, 1692, 120}, {2880, 1800, 120}, {2880, 1620, 120}, {2560, 1600, 120},
  {2560, 1440, 120}, {1920, 1440, 120}, {1920, 1200, 120}, {1920, 1080, 120},
  {1600, 1200, 120}, {1600, 1024, 120}, {1600, 1050, 120}, {1600, 900 , 120},
  {1440, 900 , 120}, {1400, 1050, 120}, {1366, 768 , 120}, {1360, 768 , 120},
  {1280, 1024, 120}, {1280, 960 , 120}, {1280, 800 , 120}, {1280, 768 , 120},
  {1280, 720 , 120}, {1280, 600 , 120}, {1152, 864 , 120}, {1024, 768 , 120},
  {800 , 600 , 120}, {640 , 480 , 120}
};

static const DWORD DefaultPreferredDisplayMode = 19;

CSettings::CSettings()
{
}

void CSettings::LoadModes()
{
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
      m.refresh   = DefaultDisplayModes[i][2];
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

void CSettings::SetExtraMode(const DisplayMode & mode)
{
  WCHAR buf[64];
  _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%ux%u@%u%s",
    mode.width, mode.height, mode.refresh, mode.preferred ? L"*" : L"");

  WDFKEY paramsKey = nullptr;
  NTSTATUS status = WdfDriverOpenParametersRegistryKey(
    WdfGetDriver(),
    KEY_SET_VALUE,
    WDF_NO_OBJECT_ATTRIBUTES,
    &paramsKey
  );
  if (!NT_SUCCESS(status)) return;

  UNICODE_STRING valName;
  RtlInitUnicodeString(&valName, L"ExtraMode");

  UNICODE_STRING uData;
  RtlInitUnicodeString(&uData, buf);

  WDFSTRING hStr = nullptr;
  status = WdfStringCreate(&uData, WDF_NO_OBJECT_ATTRIBUTES, &hStr);
  if (NT_SUCCESS(status)) {
    status = WdfRegistryAssignString(paramsKey, &valName, hStr);
    WdfObjectDelete(hStr);
  }

  WdfRegistryClose(paramsKey);
}

bool CSettings::GetExtraMode(DisplayMode & mode)
{
  WDFKEY paramsKey = nullptr;
  NTSTATUS st = WdfDriverOpenParametersRegistryKey(
    WdfGetDriver(),
    KEY_QUERY_VALUE,
    WDF_NO_OBJECT_ATTRIBUTES,
    &paramsKey
  );
  if (!NT_SUCCESS(st))
    return false;

  UNICODE_STRING name;  RtlInitUnicodeString(&name, L"ExtraMode");
  UNICODE_STRING empty; RtlInitUnicodeString(&empty, L"");

  WDFSTRING hStr = nullptr;
  st = WdfStringCreate(&empty, WDF_NO_OBJECT_ATTRIBUTES, &hStr);
  if (!NT_SUCCESS(st))
  {
    WdfRegistryClose(paramsKey);
    return false;
  }

  st = WdfRegistryQueryString(paramsKey, &name, hStr);
  if (!NT_SUCCESS(st))
  {
    WdfObjectDelete(hStr);
    WdfRegistryClose(paramsKey);
    return false;
  }

  UNICODE_STRING us{};
  WdfStringGetUnicodeString(hStr, &us);

  std::wstring s(us.Buffer, us.Length / sizeof(wchar_t));

  WdfObjectDelete(hStr);
  WdfRegistryClose(paramsKey);

  return ParseModeString(s, mode);
}

bool CSettings::ReadModesValue(std::vector<std::wstring> &out) const
{
  HKEY hKey = nullptr;
  LONG st   = RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_QUERY_VALUE, &hKey);
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

  return true;
}
