/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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
#include "common/windebug.h"
#include <stdio.h>

void DebugWinError(const char * file, const unsigned int line, const char * function, const char * desc, HRESULT status)
{
  char *buffer;
  if (!FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS,
    NULL,
    status,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (char*)&buffer,
    1024,
    NULL
  ))
  {
    DEBUG_ERROR("FormatMessage failed with code 0x%08lx", GetLastError());
    fprintf(stderr, "%12" PRId64 " [E] %20s:%-4u | %-30s | %s: 0x%08x\n", microtime(), file, line, function, desc, (int)status);
    return;
  }

  for(size_t i = strlen(buffer) - 1; i > 0; --i)
    if (buffer[i] == '\n' || buffer[i] == '\r')
      buffer[i] = 0;

  fprintf(stderr, "%12" PRId64 " [E] %20s:%-4u | %-30s | %s: 0x%08x (%s)\n", microtime(), file, line, function, desc, (int)status, buffer);
  LocalFree(buffer);
}
