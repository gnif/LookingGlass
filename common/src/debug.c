/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

inline static void debug_level(enum DebugLevel level, const char * file,
    unsigned int line, const char * function, const char * format, va_list va)
{
  const char * f = strrchr(file, DIRECTORY_SEPARATOR) + 1;
  fprintf(stderr, "%s%12" PRId64 "%20s:%-4u | %-30s | ",
      debug_lookup[level], microtime(), f,
      line, function);
  vfprintf(stderr, format, va);
  fprintf(stderr, "%s\n", debug_lookup[DEBUG_LEVEL_NONE]);
}


void debug_info(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_level(DEBUG_LEVEL_INFO, file, line, function, format, va);
  va_end(va);
}

void debug_warn(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_level(DEBUG_LEVEL_WARN, file, line, function, format, va);
  va_end(va);
}

void debug_error(const char * file, unsigned int line, const char * function,
    const char * format, ...)
{
  va_list va;
  va_start(va, format);
  debug_level(DEBUG_LEVEL_ERROR, file, line, function, format, va);
  va_end(va);
}
