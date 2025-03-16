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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <fstream>

class CDebug
{
  private:
    std::ofstream m_stream;
    void Write(const char * line);

  public:
    CDebug();

    enum Level
    {
      LEVEL_NONE = 0,
      LEVEL_INFO,
      LEVEL_WARN,
      LEVEL_ERROR,
      LEVEL_TRACE,
      LEVEL_FIXME,
      LEVEL_FATAL,
      
      LEVEL_MAX
    };

    void Log(CDebug::Level level, const char * function, int line, const char * fmt, ...);
    void LogHR(CDebug::Level level, HRESULT hr, const char* function, int line, const char* fmt, ...);

  private:
    const char* m_levelStr[LEVEL_MAX] =
    {
      " ",
      "I",
      "W",
      "E",
      "T",
      "!",
      "F"
    };
};

extern CDebug g_debug;

#define DBGPRINT(kszDebugFormatString, ...) \
  g_debug.Log(CDebug::LEVEL_INFO, __FUNCTION__, __LINE__, kszDebugFormatString, __VA_ARGS__)

#define DBGPRINT_HR(status, kszDebugFormatString, ...) \
  g_debug.LogHR(CDebug::LEVEL_INFO, status, __FUNCTION__, __LINE__, kszDebugFormatString, __VA_ARGS__)
