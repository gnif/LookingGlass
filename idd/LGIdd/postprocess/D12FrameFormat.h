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

#pragma once

#include <Windows.h>
#include <d3d12.h>
#include <memory>
#include <stdint.h>

extern "C" {
  #include "common/types.h"
}

struct D12ColorTransform
{
  bool  matrixEnabled = false;
  float matrix[3][4]  = {};
  float scalar        = 1.0f;
  bool  lutEnabled    = false;
  float lut[4096][4]  = {};
};

struct D12FrameFormat
{
  D3D12_RESOURCE_DESC                       desc          = {};
  unsigned                                  dataWidth     = 0;
  unsigned                                  dataHeight    = 0;
  unsigned                                  pitch         = 0;
  unsigned                                  width         = 0;
  unsigned                                  height        = 0;
  FrameType                                 format        = FRAME_TYPE_INVALID;
  bool                                      hdr           = false;
  bool                                      hdrPQ         = false;
  bool                                      hdrMetadata   = false;
  uint32_t                                  sdrWhiteLevel = LG_SDR_WHITE_LEVEL_DEFAULT;
  std::shared_ptr<const D12ColorTransform>  colorTransform;

  // HDR static metadata (SMPTE ST 2086)
  // Display color primaries in 0.00002 units (xy coordinates)
  uint16_t displayPrimary[3][2];
  // White point in 0.00002 units
  uint16_t whitePoint[2];
  // Max mastering display luminance in whole cd/m²
  uint32_t maxDisplayLuminance;
  // Min mastering display luminance in 0.0001 cd/m² units
  uint32_t minDisplayLuminance;
  // MaxCLL and MaxFALL in cd/m²
  uint32_t maxContentLightLevel;
  uint32_t maxFrameAverageLightLevel;
};

namespace D12
{
  enum class DescCmp : uint8_t
  {
    EXACT,
    CREATE,
    COPY,
    NO_FLAGS,
    VIEW,
  };

  enum class FormatCmp : uint8_t
  {
    EXACT,
    IMAGE,
    NO_FLAGS,
  };

  FrameType Type(DXGI_FORMAT format);
  void CopyHdr(D12FrameFormat& dst, const D12FrameFormat& src);
  std::shared_ptr<const D12ColorTransform> Transform(
    const std::shared_ptr<const D12ColorTransform>& transform);
  bool Same(const D3D12_RESOURCE_DESC& left,
    const D3D12_RESOURCE_DESC& right,
    DescCmp cmp = DescCmp::EXACT);
  bool Same(const D12FrameFormat& left, const D12FrameFormat& right,
    FormatCmp cmp = FormatCmp::EXACT);
}
