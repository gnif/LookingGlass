/**
 * Looking Glass
 * Copyright © 2017-2023 The Looking Glass Authors
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

#include <d3d11.h>
#include <d3dcompiler.h>

static const char * DXGI_FORMAT_STR[] = {
  "DXGI_FORMAT_UNKNOWN",
  "DXGI_FORMAT_R32G32B32A32_TYPELESS",
  "DXGI_FORMAT_R32G32B32A32_FLOAT",
  "DXGI_FORMAT_R32G32B32A32_UINT",
  "DXGI_FORMAT_R32G32B32A32_SINT",
  "DXGI_FORMAT_R32G32B32_TYPELESS",
  "DXGI_FORMAT_R32G32B32_FLOAT",
  "DXGI_FORMAT_R32G32B32_UINT",
  "DXGI_FORMAT_R32G32B32_SINT",
  "DXGI_FORMAT_R16G16B16A16_TYPELESS",
  "DXGI_FORMAT_R16G16B16A16_FLOAT",
  "DXGI_FORMAT_R16G16B16A16_UNORM",
  "DXGI_FORMAT_R16G16B16A16_UINT",
  "DXGI_FORMAT_R16G16B16A16_SNORM",
  "DXGI_FORMAT_R16G16B16A16_SINT",
  "DXGI_FORMAT_R32G32_TYPELESS",
  "DXGI_FORMAT_R32G32_FLOAT",
  "DXGI_FORMAT_R32G32_UINT",
  "DXGI_FORMAT_R32G32_SINT",
  "DXGI_FORMAT_R32G8X24_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
  "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
  "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
  "DXGI_FORMAT_R10G10B10A2_TYPELESS",
  "DXGI_FORMAT_R10G10B10A2_UNORM",
  "DXGI_FORMAT_R10G10B10A2_UINT",
  "DXGI_FORMAT_R11G11B10_FLOAT",
  "DXGI_FORMAT_R8G8B8A8_TYPELESS",
  "DXGI_FORMAT_R8G8B8A8_UNORM",
  "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
  "DXGI_FORMAT_R8G8B8A8_UINT",
  "DXGI_FORMAT_R8G8B8A8_SNORM",
  "DXGI_FORMAT_R8G8B8A8_SINT",
  "DXGI_FORMAT_R16G16_TYPELESS",
  "DXGI_FORMAT_R16G16_FLOAT",
  "DXGI_FORMAT_R16G16_UNORM",
  "DXGI_FORMAT_R16G16_UINT",
  "DXGI_FORMAT_R16G16_SNORM",
  "DXGI_FORMAT_R16G16_SINT",
  "DXGI_FORMAT_R32_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT",
  "DXGI_FORMAT_R32_FLOAT",
  "DXGI_FORMAT_R32_UINT",
  "DXGI_FORMAT_R32_SINT",
  "DXGI_FORMAT_R24G8_TYPELESS",
  "DXGI_FORMAT_D24_UNORM_S8_UINT",
  "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
  "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
  "DXGI_FORMAT_R8G8_TYPELESS",
  "DXGI_FORMAT_R8G8_UNORM",
  "DXGI_FORMAT_R8G8_UINT",
  "DXGI_FORMAT_R8G8_SNORM",
  "DXGI_FORMAT_R8G8_SINT",
  "DXGI_FORMAT_R16_TYPELESS",
  "DXGI_FORMAT_R16_FLOAT",
  "DXGI_FORMAT_D16_UNORM",
  "DXGI_FORMAT_R16_UNORM",
  "DXGI_FORMAT_R16_UINT",
  "DXGI_FORMAT_R16_SNORM",
  "DXGI_FORMAT_R16_SINT",
  "DXGI_FORMAT_R8_TYPELESS",
  "DXGI_FORMAT_R8_UNORM",
  "DXGI_FORMAT_R8_UINT",
  "DXGI_FORMAT_R8_SNORM",
  "DXGI_FORMAT_R8_SINT",
  "DXGI_FORMAT_A8_UNORM",
  "DXGI_FORMAT_R1_UNORM",
  "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
  "DXGI_FORMAT_R8G8_B8G8_UNORM",
  "DXGI_FORMAT_G8R8_G8B8_UNORM",
  "DXGI_FORMAT_BC1_TYPELESS",
  "DXGI_FORMAT_BC1_UNORM",
  "DXGI_FORMAT_BC1_UNORM_SRGB",
  "DXGI_FORMAT_BC2_TYPELESS",
  "DXGI_FORMAT_BC2_UNORM",
  "DXGI_FORMAT_BC2_UNORM_SRGB",
  "DXGI_FORMAT_BC3_TYPELESS",
  "DXGI_FORMAT_BC3_UNORM",
  "DXGI_FORMAT_BC3_UNORM_SRGB",
  "DXGI_FORMAT_BC4_TYPELESS",
  "DXGI_FORMAT_BC4_UNORM",
  "DXGI_FORMAT_BC4_SNORM",
  "DXGI_FORMAT_BC5_TYPELESS",
  "DXGI_FORMAT_BC5_UNORM",
  "DXGI_FORMAT_BC5_SNORM",
  "DXGI_FORMAT_B5G6R5_UNORM",
  "DXGI_FORMAT_B5G5R5A1_UNORM",
  "DXGI_FORMAT_B8G8R8A8_UNORM",
  "DXGI_FORMAT_B8G8R8X8_UNORM",
  "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
  "DXGI_FORMAT_B8G8R8A8_TYPELESS",
  "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
  "DXGI_FORMAT_B8G8R8X8_TYPELESS",
  "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
  "DXGI_FORMAT_BC6H_TYPELESS",
  "DXGI_FORMAT_BC6H_UF16",
  "DXGI_FORMAT_BC6H_SF16",
  "DXGI_FORMAT_BC7_TYPELESS",
  "DXGI_FORMAT_BC7_UNORM",
  "DXGI_FORMAT_BC7_UNORM_SRGB",
  "DXGI_FORMAT_AYUV",
  "DXGI_FORMAT_Y410",
  "DXGI_FORMAT_Y416",
  "DXGI_FORMAT_NV12",
  "DXGI_FORMAT_P010",
  "DXGI_FORMAT_P016",
  "DXGI_FORMAT_420_OPAQUE",
  "DXGI_FORMAT_YUY2",
  "DXGI_FORMAT_Y210",
  "DXGI_FORMAT_Y216",
  "DXGI_FORMAT_NV11",
  "DXGI_FORMAT_AI44",
  "DXGI_FORMAT_IA44",
  "DXGI_FORMAT_P8",
  "DXGI_FORMAT_A8P8",
  "DXGI_FORMAT_B4G4R4A4_UNORM",

  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

  "DXGI_FORMAT_P208",
  "DXGI_FORMAT_V208",
  "DXGI_FORMAT_V408"
};

const char * getDXGIFormatStr(DXGI_FORMAT format)
{
  if (format > sizeof(DXGI_FORMAT_STR) / sizeof(const char *))
    return DXGI_FORMAT_STR[0];
  return DXGI_FORMAT_STR[format];
}

static const char * DXGI_COLOR_SPACE_TYPE_STR[] =
{
  "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709",
  "DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709",
  "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709",
  "DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020",
  "DXGI_COLOR_SPACE_RESERVED",
  "DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601",
  "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709",
  "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020",
  "DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020",
  "DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020",
  "DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020",
  "DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020",
  "DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020",
  "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709",
  "DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P2020",
  "DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020"
};

const char * getDXGIColorSpaceTypeStr(DXGI_COLOR_SPACE_TYPE type)
{
  if (type == DXGI_COLOR_SPACE_CUSTOM)
    return "DXGI_COLOR_SPACE_CUSTOM";

  if (type > sizeof(DXGI_COLOR_SPACE_TYPE_STR) / sizeof(const char *))
    return "Invalid or Unknown";

  return DXGI_COLOR_SPACE_TYPE_STR[type];
}

bool compileShader(ID3DBlob ** dst, const char * entry, const char * target,
  const char * code, const D3D_SHADER_MACRO * defines)
{
  ID3DBlob * errors;
  HRESULT status = D3DCompile(
    code,
    strlen(code),
    NULL,
    defines,
    NULL,
    entry,
    target,
    0,//D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION,
    0,
    dst,
    &errors);

  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to compile the shader");
    DEBUG_ERROR("%s", (const char *)ID3D10Blob_GetBufferPointer(errors));
    ID3D10Blob_Release(errors);
    return false;
  }

  return true;
}

bool getDisplayPathInfo(HMONITOR monitor, DISPLAYCONFIG_PATH_INFO * info)
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

float getSDRWhiteLevel(const DISPLAYCONFIG_PATH_INFO * displayPathInfo)
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
