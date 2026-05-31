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

#include "common/display.h"
#include "common/debug.h"

bool display_getPathInfo(HMONITOR monitor, DISPLAYCONFIG_PATH_INFO * info)
{
  bool result = false;
  UINT32 numPath, numMode;

  MONITORINFOEXW viewInfo = { .cbSize = sizeof(viewInfo) };
    if (!GetMonitorInfoW(monitor, (MONITORINFO*)&viewInfo))
  {
    DEBUG_ERROR("Failed to get the monitor info");
    goto err;
  }

err_retry:
  if (FAILED(GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPath, &numMode)))
    goto err;

  DISPLAYCONFIG_PATH_INFO * pathInfo = calloc(sizeof(*pathInfo), numPath);
  if (!pathInfo)
    goto err_mem_pathInfo;

  DISPLAYCONFIG_MODE_INFO * modeInfo = calloc(sizeof(*modeInfo), numMode);
  if (!modeInfo)
    goto err_mem_modeInfo;

  LONG status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,
    &numPath, pathInfo,
    &numMode, modeInfo,
    NULL);

  if (status != ERROR_SUCCESS)
  {
    if (status == ERROR_INSUFFICIENT_BUFFER)
    {
      free(modeInfo);
      free(pathInfo);
      goto err_retry;
    }

    DEBUG_ERROR("QueryDisplayConfig failed with 0x%lx", status);
    goto err_queryDisplay;
  }

  for(unsigned i = 0; i < numPath; ++i)
  {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName =
    {
      .header =
      {
        .type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,
        .size      = sizeof(sourceName),
        .adapterId = pathInfo[i].sourceInfo.adapterId,
        .id        = pathInfo[i].sourceInfo.id,
      }
    };

    if (FAILED(DisplayConfigGetDeviceInfo(&sourceName.header)))
      continue;

    if (wcscmp(viewInfo.szDevice, sourceName.viewGdiDeviceName) != 0)
      continue;

    *info = pathInfo[i];
    result = true;
    break;
  }

err_queryDisplay:
  free(modeInfo);

err_mem_modeInfo:
  free(pathInfo);

err_mem_pathInfo:

err:
  return result;
}

float display_getSDRWhiteLevel(const DISPLAYCONFIG_PATH_INFO * displayPathInfo)
{
  float nits = 80.0f;
  DISPLAYCONFIG_SDR_WHITE_LEVEL level =
  {
    .header =
    {
      .type      = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
      .size      = sizeof(level),
      .adapterId = displayPathInfo->targetInfo.adapterId,
      .id        = displayPathInfo->targetInfo.id,
    }
  };

  if (SUCCEEDED(DisplayConfigGetDeviceInfo(&level.header)))
    nits = level.SDRWhiteLevel / 1000.0f * 80.0f;

  return nits;
}
