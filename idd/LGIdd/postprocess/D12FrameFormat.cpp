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

namespace
{
  bool NearlyEqual(float a, float b, float tolerance)
  {
    const float delta = a - b;
    return delta >= -tolerance && delta <= tolerance;
  }

  bool Identity(const D12ColorTransform& transform)
  {
    static const float matrixTolerance = 1.0f / 1048576.0f;
    static const float lutTolerance    = 1.0f / 65535.0f;

    if (transform.matrixEnabled)
    {
      for (unsigned row = 0; row < 3; ++row)
        for (unsigned column = 0; column < 4; ++column)
        {
          const float expected  = row == column ? 1.0f : 0.0f;
          const float effective =
            transform.matrix[row][column] * transform.scalar;
          if (!NearlyEqual(effective, expected, matrixTolerance))
            return false;
        }
    }

    if (transform.lutEnabled)
      for (unsigned i = 0; i < 4096; ++i)
      {
        const float expected = static_cast<float>(i) / 4095.0f;
        if (!NearlyEqual(transform.lut[i][0], expected, lutTolerance) ||
            !NearlyEqual(transform.lut[i][1], expected, lutTolerance) ||
            !NearlyEqual(transform.lut[i][2], expected, lutTolerance))
          return false;
      }

    return true;
  }
}

std::shared_ptr<const D12ColorTransform> D12::Transform(
  const std::shared_ptr<const D12ColorTransform>& transform)
{
  if (!transform || (!transform->matrixEnabled && !transform->lutEnabled) ||
      Identity(*transform))
    return nullptr;
  return transform;
}

DXGI_FORMAT D12::Dxgi(FramePixel pixel)
{
  switch (pixel)
  {
    case FramePixel::BGRA8:
      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case FramePixel::RGBA8:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case FramePixel::RGB10A2:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case FramePixel::RGBA16F:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
  }
  return DXGI_FORMAT_UNKNOWN;
}

FrameType D12::Type(FramePixel pixel)
{
  return Type(Dxgi(pixel));
}

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

bool D12::Profile(const D12FrameFormat& format, FrameStorage storage,
  FrameProfile& profile)
{
  if ((format.hdrPQ && !format.hdr) || !Frame::Valid(storage))
    return false;

  FrameProfile result;
  result.storage = storage;
  if (!format.hdr)
  {
    result.signal = FrameSignal::SRGB;
    if (format.format == FRAME_TYPE_BGRA)
      result.pixel = FramePixel::BGRA8;
    else if (format.format == FRAME_TYPE_RGBA)
      result.pixel = FramePixel::RGBA8;
    else
      return false;
  }
  else if (format.hdrPQ)
  {
    if (format.format != FRAME_TYPE_RGBA10)
      return false;
    result.pixel  = FramePixel::RGB10A2;
    result.signal = FrameSignal::PQ_BT2020;
  }
  else
  {
    if (format.format != FRAME_TYPE_RGBA16F)
      return false;
    result.pixel  = FramePixel::RGBA16F;
    result.signal = FrameSignal::SCRGB_LINEAR;
  }

  if (format.desc.Format != Dxgi(result.pixel))
    return false;

  profile = result;
  return true;
}

bool D12::Set(D12FrameFormat& format, const FrameProfile& profile,
  unsigned width, unsigned height, D3D12_RESOURCE_FLAGS flags)
{
  if (!width || !height || profile.storage != FrameStorage::D3D12_TEXTURE ||
      !Frame::Valid(profile))
    return false;

  format.desc                  = {};
  format.desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  format.desc.Width            = width;
  format.desc.Height           = height;
  format.desc.DepthOrArraySize = 1;
  format.desc.MipLevels        = 1;
  format.desc.Format           = Dxgi(profile.pixel);
  format.desc.SampleDesc.Count = 1;
  format.desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  format.desc.Flags            = flags;
  format.dataWidth             = width;
  format.dataHeight            = height;
  format.pitch                 = 0;
  format.width                 = width;
  format.height                = height;
  format.format                = Type(profile.pixel);
  format.hdr                   = profile.signal != FrameSignal::SRGB;
  format.hdrPQ                 = profile.signal == FrameSignal::PQ_BT2020;
  return format.desc.Format != DXGI_FORMAT_UNKNOWN &&
         format.format      != FRAME_TYPE_INVALID;
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
