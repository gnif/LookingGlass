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

#include "common/debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint64_t startTime;
static bool     traceEnabled = false;

void debug_init(void)
{
  startTime = microtime();
  platform_debugInit();
}

void debug_enableTracing(void)
{
  traceEnabled = true;
}

inline static void debug_levelVA(enum DebugLevel level, const char * file,
    unsigned int line, const char * function, const char * format, va_list va)
{
  if (level == DEBUG_LEVEL_TRACE && !traceEnabled)
    return;

  const char * f = strrchr(file, DIRECTORY_SEPARATOR);
  if (!f)
    f = file;
  else
    ++f;

  uint64_t elapsed = microtime() - startTime;
  uint64_t sec     = elapsed / 1000000UL;
  uint64_t us      = elapsed % 1000000UL;

  fprintf(stderr, "%02u:%02u:%02u.%03u %s %18s:%-4u | %-30s | ",
      (unsigned)(sec / 60 / 60),
      (unsigned)(sec / 60 % 60),
      (unsigned)(sec % 60),
      (unsigned)(us / 1000),
      debug_lookup[level],
      f,
      line, function);

  vfprintf(stderr, format, va);
  fprintf(stderr, "%s\n", debug_lookup[DEBUG_LEVEL_NONE]);
}


void debug_level(enum DebugLevel level, const char * file, unsigned int line,
    const char * function, const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_levelVA(level, file, line, function, format, va);
  va_end(va);
}

void debug_info(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_levelVA(DEBUG_LEVEL_INFO, file, line, function, format, va);
  va_end(va);
}

void debug_warn(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_levelVA(DEBUG_LEVEL_WARN, file, line, function, format, va);
  va_end(va);
}

void debug_error(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_levelVA(DEBUG_LEVEL_ERROR, file, line, function, format, va);
  va_end(va);
}

void debug_trace(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_levelVA(DEBUG_LEVEL_INFO, file, line, function, format, va);
  va_end(va);
}
