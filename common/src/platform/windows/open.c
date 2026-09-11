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

#include "common/open.h"

#include "common/debug.h"
#include "common/win32.h"

#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

bool lgOpenURL(const char * url)
{
  wchar_t * wideUrl = lgUtf8ToWide(url);
  if (!wideUrl)
    return false;

  const INT_PTR result = (INT_PTR)ShellExecuteW(
      NULL, L"open", wideUrl, NULL, NULL, SW_SHOWNORMAL);
  free(wideUrl);
  if (result > 32)
    return true;

  DEBUG_ERROR("Failed to open URL (ShellExecute result: %lld)",
      (long long)result);
  return false;
}
