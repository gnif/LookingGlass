/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "windows/debug.h"
#include <stdio.h>

void DebugWinError(const char * file, const unsigned int line, const char * function, const char * desc, HRESULT status)
{
  char *buffer;
  FormatMessageA(
    FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER,
    NULL,
    status,
    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
    (char*)&buffer,
    1024,
    NULL
  );

  for(size_t i = strlen(buffer) - 1; i > 0; --i)
    if (buffer[i] == '\n' || buffer[i] == '\r')
      buffer[i] = 0;

  fprintf(stderr, "[E] %20s:%-4u | %-30s | %s: 0x%08x (%s)\n", file, line, function, desc, (int)status, buffer);
  LocalFree(buffer);
}