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

#include <malloc.h>
#include <strsafe.h>

#include "CDebug.h"

CDebug g_debug;

void CDebug::Init(const char * name)
{
  // don't redirect the debug output if running under a debugger
  if (IsDebuggerPresent())
    return;

  // get the system temp directory
  char tempPath[MAX_PATH];
  DWORD pathLen = GetTempPathA(sizeof(tempPath), tempPath);
  if (pathLen == 0)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to get the temp path");
    return;
  }

  std::string folder   = tempPath;
  std::string baseName = name;
  std::string ext      = ".txt";
  std::string logFile  = folder + baseName + ext;

  //rotate out old logs
  DeleteFileA((folder + baseName + ".4" + ext).c_str());
  for (int i = 3; i >= 0; --i)
  {
    std::string oldPath;
    std::string newPath;

    if (i == 0)
    {
      oldPath = logFile;
      newPath = folder + baseName + ".1" + ext;
    }
    else    
    {
      oldPath = folder + baseName + "." + std::to_string(i) + ext;
      newPath = folder + baseName + "." + std::to_string(i + 1) + ext;
    }

    MoveFileA(oldPath.c_str(), newPath.c_str());
  }

  /// open the new log file
  std::ofstream stream(logFile, std::ios::out | std::ios::trunc);
  if (!stream.is_open())
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to open the log file %s", logFile.c_str());
    return;
  }
  else
    DEBUG_INFO("Logging to: %s", logFile.c_str());

  m_stream = std::move(stream);
}

void CDebug::Log_va(CDebug::Level level, const char* function, int line, const char* fmt, va_list args)
{
  if (level < 0 || level >= LEVEL_MAX)
    level = LEVEL_NONE;

  static const char* fmtTemplate = "[%s] %40s:%-4d | ";
  const char* levelStr = m_levelStr[level];

  int length = 0;
  length = _scprintf(fmtTemplate, levelStr, function, line);
  length += _vscprintf(fmt, args);
  length += 2;

  /* Depending on the size of the format string, allocate space on the stack or the heap. */
  PCHAR buffer;
  buffer = (PCHAR)_malloca(length);
  if (!buffer)
    return;

  /* Populate the buffer with the contents of the format string. */
  StringCbPrintfA(buffer, length, fmtTemplate, levelStr, function, line);

  size_t offset = 0;
  StringCbLengthA(buffer, length, &offset);
  StringCbVPrintfA(&buffer[offset], length - offset, fmt, args);

  buffer[length - 2] = '\n';
  buffer[length - 1] = '\0';

  Write(buffer);

  _freea(buffer);
}

void CDebug::Log(CDebug::Level level, const char * function, int line, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  Log_va(level, function, line, fmt, args);
  va_end(args);
}

void CDebug::LogHR(CDebug::Level level, HRESULT hr, const char * function, int line, const char * fmt, ...)
{
  if (level < 0 || level >= LEVEL_MAX)
    level = LEVEL_NONE;

  char * hrBuffer;
  if (!FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    hr,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (char*)&hrBuffer,
    1024,
    NULL
  ))
  {
    DEBUG_INFO("FormatMessage failed with code 0x%08x", GetLastError());

    va_list args;
    va_start(args, fmt);
    Log_va(level, function, line, fmt, args);
    va_end(args);
    return;
  }

  // Remove trailing CRLF in hrBuffer
  size_t len = strlen(hrBuffer);
  while (len && (hrBuffer[len - 1] == '\n' || hrBuffer[len - 1] == '\r'))
    hrBuffer[--len] = '\0';

  static const char* fmtTemplate = "[%s] %40s:%-4d | ";
  const char* levelStr = m_levelStr[level];

  va_list args;
  va_start(args, fmt);

  int length = 0;
  length = _scprintf(fmtTemplate, levelStr, function, line);
  length += _vscprintf(fmt, args);
  length += 2 + 4 + (int)strlen(hrBuffer) + 1;

  /* Depending on the size of the format string, allocate space on the stack or the heap. */
  PCHAR buffer;
  buffer = (PCHAR)_malloca(length);
  if (!buffer)
  {
    va_end(args);
    return;
  }

  /* Populate the buffer with the contents of the format string. */
  StringCbPrintfA(buffer, length, fmtTemplate, levelStr, function, line);

  size_t offset = 0;
  StringCbLengthA(buffer, length, &offset);
  StringCbVPrintfA(&buffer[offset], length - offset, fmt, args);
  va_end(args);

  /* append the formatted error */
  StringCbLengthA(buffer, length, &offset);
  StringCbPrintfA(&buffer[offset], length - offset, " (%s)\n", hrBuffer);

  Write(buffer);

  _freea(buffer);
  LocalFree(hrBuffer);
}

void CDebug::Write(const char * line)
{
  if (m_stream.is_open())
  {
    m_stream << line;
    m_stream.flush();
  }
  else
    OutputDebugStringA(line);
}