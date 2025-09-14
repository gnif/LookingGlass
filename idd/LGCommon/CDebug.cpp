/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#include <Windows.h>
#include <string>

#include <stdio.h>
#include <malloc.h>
#include <strsafe.h>
#include <shlobj.h>

#include "CDebug.h"

CDebug g_debug;

const char *CDebug::s_levelStr[CDebug::LEVEL_MAX] =
{
  " ",
  "I",
  "W",
  "E",
  "T",
  "!",
  "F"
};

int vasprintf(char **pstr, const char *fmt, va_list args)
{
  int len = _vscprintf(fmt, args);
  if (len < 0)
    return -1;

  char *str = (char *)malloc(len + 1);
  if (!str)
    return -1;

  int r = vsprintf_s(str, len + 1, fmt, args);
  if (r < 0)
  {
    free(str);
    return -1;
  }

  *pstr = str;
  return r;
}

int vaswprintf(wchar_t **pstr, const wchar_t *fmt, va_list args)
{
  int len = _vscwprintf(fmt, args);
  if (len < 0)
    return -1;

  wchar_t *str = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
  if (!str)
    return -1;

  int r = vswprintf_s(str, len + 1, fmt, args);
  if (r < 0)
  {
    free(str);
    return -1;
  }

  *pstr = str;
  return r;
}

int aswprintf(wchar_t **pstr, const wchar_t *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vaswprintf(pstr, fmt, ap);
  va_end(ap);
  return r;
}

inline static void iso8601(wchar_t *buf, size_t count)
{
  struct tm utc;
  time_t unix;
  time(&unix);
  gmtime_s(&utc, &unix);
  wcsftime(buf, count, L"%Y-%m-%d %H:%M:%SZ", &utc);
}

inline static std::wstring getLogPath()
{
  PWSTR pszPath;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, NULL, &pszPath)))
  {
    DEBUG_ERROR("Failed to get ProgramData path");
    return L"";
  }

  std::wstring result(pszPath);
  CoTaskMemFree(pszPath);

  result += L"\\Looking Glass (IDD)\\";
  return result;
}

void CDebug::Init(const wchar_t * name)
{
  m_logDir = getLogPath();

  // don't redirect the debug output if running under a debugger
  if (IsDebuggerPresent())
    return;

  std::wstring baseName = name;
  std::wstring ext      = L".txt";
  std::wstring logFile  = m_logDir + baseName + ext;

  //rotate out old logs
  DeleteFileW((m_logDir + baseName + L".4" + ext).c_str());
  for (int i = 3; i >= 0; --i)
  {
    std::wstring oldPath;
    std::wstring newPath;

    if (i == 0)
    {
      oldPath = logFile;
      newPath = m_logDir + baseName + L".1" + ext;
    }
    else    
    {
      oldPath = m_logDir + baseName + L"." + std::to_wstring(i) + ext;
      newPath = m_logDir + baseName + L"." + std::to_wstring(i + 1) + ext;
    }

    MoveFileW(oldPath.c_str(), newPath.c_str());
  }

  /// open the new log file
  std::ofstream stream(logFile, std::ios::out | std::ios::trunc);
  if (!stream.is_open())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open the log file %s", logFile.c_str());
    return;
  }

  DEBUG_INFO(L"Logging to: %s", logFile.c_str());
  m_stream = std::move(stream);
}

void CDebug::LogStr(CDebug::Level level, const char *function, int line, bool wide, const void *str)
{
  if (level < 0 || level >= LEVEL_MAX)
    level = LEVEL_NONE;

  wchar_t timestamp[50];
  iso8601(timestamp, ARRAYSIZE(timestamp));

  wchar_t *result;
  if (aswprintf(&result, wide ? L"[%s] [%S] %40S:%-4d | %s\n" : L"[%s] [%S] %40S:%-4d | %S\n",
    timestamp, s_levelStr[level], function, line, str) < 0)
  {
    Write(L"Out of memory while logging");
    return;
  }

  Write(result);
  free(result);
}

void CDebug::Log_va(CDebug::Level level, const char *function, int line, const char *fmt, va_list args)
{
  char *result;
  if (vasprintf(&result, fmt, args) < 0)
  {
    Write(L"Out of memory while logging");
    return;
  }

  LogStr(level, function, line, false, result);
  free(result);
}

void CDebug::Log(CDebug::Level level, const char *function, int line, const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  Log_va(level, function, line, fmt, args);
  va_end(args);
}

void CDebug::Log_va(CDebug::Level level, const char *function, int line, const wchar_t *fmt, va_list args)
{
  wchar_t *result;
  if (vaswprintf(&result, fmt, args) < 0)
  {
    Write(L"Out of memory while logging");
    return;
  }

  LogStr(level, function, line, true, result);
  free(result);
}

void CDebug::Log(CDebug::Level level, const char *function, int line, const wchar_t *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  Log_va(level, function, line, fmt, args);
  va_end(args);
}

void CDebug::LogStrHR(CDebug::Level level, HRESULT hr, const char *function, int line, bool wide, const void *str)
{
  wchar_t *hrBuffer;
  if (!FormatMessageW(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    hr,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (LPWSTR)&hrBuffer,
    1024,
    NULL
  ))
  {
    DEBUG_ERROR("FormatMessage for 0x%08lX (%u) failed with code 0x%08x", hr, hr, GetLastError());
    LogStr(level, function, line, wide, str);
    return;
  }

  // Remove trailing CRLF in hrBuffer
  size_t len = wcslen(hrBuffer);
  while (len && (hrBuffer[len - 1] == L'\n' || hrBuffer[len - 1] == L'\r'))
    hrBuffer[--len] = L'\0';

  wchar_t *result;
  if (aswprintf(&result, wide ? L"%s (0x%08lX (%u): %s)" : L"%S (0x%08lX (%u): %s)", str, hr, hr, hrBuffer) < 0)
  {
    Write(L"Out of memory while logging");
    return;
  }

  LogStr(level, function, line, true, result);
  free(result);
}

void CDebug::LogHR(CDebug::Level level, HRESULT hr, const char *function, int line, const char *fmt, ...)
{
  char *result;
  va_list args;
  va_start(args, fmt);
  if (vasprintf(&result, fmt, args) < 0)
  {
    va_end(args);
    Write(L"Out of memory while logging");
    return;
  }

  va_end(args);
  LogStrHR(level, hr, function, line, false, result);
  free(result);
}

void CDebug::LogHR(CDebug::Level level, HRESULT hr, const char *function, int line, const wchar_t *fmt, ...)
{
  wchar_t *result;
  va_list args;
  va_start(args, fmt);
  if (vaswprintf(&result, fmt, args) < 0)
  {
    va_end(args);
    Write(L"Out of memory while logging");
    return;
  }

  va_end(args);
  LogStrHR(level, hr, function, line, true, result);
  free(result);
}

void CDebug::Write(const wchar_t *line)
{
  if (!m_stream.is_open())
  {
    OutputDebugStringW(line);
    return;
  }

  DWORD cbRequired = WideCharToMultiByte(CP_UTF8, 0, line, -1, NULL, 0, NULL, NULL);
  LPSTR utf8 = (LPSTR) malloc(cbRequired);
  if (!utf8)
  {
    m_stream << "Out of memory while logging" << std::endl;
    return;
  }

  WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, cbRequired, NULL, NULL);
  m_stream << utf8 << std::flush;
  free(utf8);
}
