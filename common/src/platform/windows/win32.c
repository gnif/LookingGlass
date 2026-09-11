/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "common/win32.h"

#include <stdlib.h>
#include <windows.h>

wchar_t * lgUtf8ToWide(const char * input)
{
  if (!input)
    return NULL;

  const int length = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, NULL, 0);
  if (!length)
    return NULL;

  wchar_t * output = malloc((size_t)length * sizeof(*output));
  if (!output)
    return NULL;

  if (!MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input, -1, output, length))
  {
    free(output);
    return NULL;
  }

  return output;
}

char * lgWideToUtf8(const wchar_t * input)
{
  if (!input)
    return NULL;

  const int length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, input, -1, NULL, 0, NULL, NULL);
  if (!length)
    return NULL;

  char * output = malloc((size_t)length);
  if (!output)
    return NULL;

  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input, -1, output, length, NULL, NULL))
  {
    free(output);
    return NULL;
  }

  return output;
}
