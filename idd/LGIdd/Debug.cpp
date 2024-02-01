/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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
#include <malloc.h>
#include <strsafe.h>

/* credit: https://stackoverflow.com/questions/29049686/is-there-a-better-way-to-pass-formatted-output-to-outputdebugstring */
VOID _DBGPRINT(PCSTR kwszFunction, INT iLineNumber, LPCSTR kszDebugFormatString, ...) \
{
  INT cbFormatString = 0;
  va_list args;
  PCHAR szDebugString = NULL;
  size_t st_Offset = 0;

  va_start(args, kszDebugFormatString);

  cbFormatString = _scprintf("[%s:%d] ", kwszFunction, iLineNumber) * sizeof(CHAR);
  cbFormatString += _vscprintf(kszDebugFormatString, args) * sizeof(CHAR) + 2;

  /* Depending on the size of the format string, allocate space on the stack or the heap. */
  szDebugString = (PCHAR)_malloca(cbFormatString);
  if (!szDebugString)
    return;

  /* Populate the buffer with the contents of the format string. */
  StringCbPrintfA(szDebugString, cbFormatString, "[%s:%d] ", kwszFunction, iLineNumber);
  StringCbLengthA(szDebugString, cbFormatString, &st_Offset);
  StringCbVPrintfA(&szDebugString[st_Offset / sizeof(CHAR)], cbFormatString - st_Offset, kszDebugFormatString, args);

  OutputDebugStringA(szDebugString);

  _freea(szDebugString);
  va_end(args);
}