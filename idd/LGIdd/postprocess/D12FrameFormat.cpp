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

#include "postprocess/D12FrameFormat.h"

#include <cstring>

FrameType D12::Type(DXGI_FORMAT format)
{
  switch (format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      return FRAME_TYPE_BGRA;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return FRAME_TYPE_RGBA;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      return FRAME_TYPE_RGBA10;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return FRAME_TYPE_RGBA16F;
    default:
      return FRAME_TYPE_INVALID;
  }
}

void D12::CopyHdr(D12FrameFormat& dst, const D12FrameFormat& src)
{
  dst.hdrMetadata   = src.hdrMetadata;
  dst.sdrWhiteLevel = src.sdrWhiteLevel;
  memcpy(dst.displayPrimary, src.displayPrimary, sizeof(dst.displayPrimary));
  memcpy(dst.whitePoint, src.whitePoint, sizeof(dst.whitePoint));
  dst.maxDisplayLuminance       = src.maxDisplayLuminance;
  dst.minDisplayLuminance       = src.minDisplayLuminance;
  dst.maxContentLightLevel      = src.maxContentLightLevel;
  dst.maxFrameAverageLightLevel = src.maxFrameAverageLightLevel;
}

bool D12::Same(const D3D12_RESOURCE_DESC& left,
  const D3D12_RESOURCE_DESC& right, DescCmp cmp)
{
  const bool alignment = cmp != DescCmp::CREATE &&
    cmp != DescCmp::COPY && cmp != DescCmp::VIEW;
  const bool layout    = cmp != DescCmp::COPY;
  const bool flags     = cmp != DescCmp::COPY &&
    cmp != DescCmp::NO_FLAGS && cmp != DescCmp::VIEW;
  return
    left.Dimension          == right.Dimension                          &&
    (!alignment || left.Alignment == right.Alignment)                  &&
    left.Width              == right.Width                              &&
    left.Height             == right.Height                             &&
    left.DepthOrArraySize   == right.DepthOrArraySize                   &&
    left.MipLevels          == right.MipLevels                          &&
    left.Format             == right.Format                             &&
    left.SampleDesc.Count   == right.SampleDesc.Count                   &&
    left.SampleDesc.Quality == right.SampleDesc.Quality                 &&
    (!layout || left.Layout == right.Layout)                            &&
    (!flags || left.Flags == right.Flags);
}

bool D12::Same(const D12FrameFormat& left, const D12FrameFormat& right,
  FormatCmp cmp)
{
  const DescCmp descCmp = cmp == FormatCmp::IMAGE ? DescCmp::CREATE :
    (cmp == FormatCmp::NO_FLAGS ? DescCmp::NO_FLAGS : DescCmp::EXACT);
  if (!Same(left.desc, right.desc, descCmp))
    return false;

  if (cmp == FormatCmp::IMAGE)
    return
      left.width  == right.width  &&
      left.height == right.height &&
      left.format == right.format &&
      left.hdr    == right.hdr    &&
      left.hdrPQ  == right.hdrPQ;

  return
    left.dataWidth      == right.dataWidth       &&
    left.dataHeight     == right.dataHeight      &&
    left.pitch          == right.pitch           &&
    left.width          == right.width           &&
    left.height         == right.height          &&
    left.format         == right.format          &&
    left.hdr            == right.hdr             &&
    left.hdrPQ          == right.hdrPQ           &&
    left.hdrMetadata    == right.hdrMetadata     &&
    left.sdrWhiteLevel  == right.sdrWhiteLevel   &&
    left.colorTransform == right.colorTransform  &&
    memcmp(left.displayPrimary, right.displayPrimary,
      sizeof(left.displayPrimary)) == 0          &&
    memcmp(left.whitePoint, right.whitePoint,
      sizeof(left.whitePoint)) == 0              &&

    left.maxDisplayLuminance       == right.maxDisplayLuminance       &&
    left.minDisplayLuminance       == right.minDisplayLuminance       &&
    left.maxContentLightLevel      == right.maxContentLightLevel      &&
    left.maxFrameAverageLightLevel == right.maxFrameAverageLightLevel;
}
