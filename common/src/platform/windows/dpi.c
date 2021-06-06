/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/dpi.h"
#include "common/windebug.h"

typedef enum MONITOR_DPI_TYPE {
  MDT_EFFECTIVE_DPI,
  MDT_ANGULAR_DPI,
  MDT_RAW_DPI,
  MDT_DEFAULT
} MONITOR_DPI_TYPE;
typedef HRESULT (WINAPI *GetDpiForMonitor_t)(HMONITOR hmonitor, MONITOR_DPI_TYPE dpiType, UINT * dpiX, UINT * dpiY);

UINT monitor_dpi(HMONITOR hMonitor)
{
  HMODULE shcore = LoadLibraryA("shcore.dll");
  if (!shcore)
  {
    DEBUG_ERROR("Could not load shcore.dll");
    return DPI_100_PERCENT;
  }

  GetDpiForMonitor_t GetDpiForMonitor = (GetDpiForMonitor_t) GetProcAddress(shcore, "GetDpiForMonitor");
  if (!GetDpiForMonitor)
  {
    DEBUG_ERROR("Could not find GetDpiForMonitor");
    return DPI_100_PERCENT;
  }

  UINT dpiX, dpiY;
  HRESULT status = GetDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  if (FAILED(status))
  {
    DEBUG_WINERROR("GetDpiForMonitor failed", status);
    return DPI_100_PERCENT;
  }

  return dpiX;
}
