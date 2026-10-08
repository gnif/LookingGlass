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

/*
 * The PyroWave packet encoder, CDF 9/7 transform, and quantization logic in
 * this file derive from PyroWave at commit
 * 89f7e47d4abbf650c91fae766728af866c5e32a0.
 *
 * Copyright (c) 2025 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "transport/network/CNetworkVideo.h"
#include "transport/network/NetworkSettings.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "capture/CFrameScheduler.h"
#include "d3d/CD3D12CommandQueue.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <limits>
#include <new>
#include <utility>
#include <vector>

using namespace Microsoft::WRL;

namespace
{
  constexpr uint32_t STREAM_ID                  = 1;
  constexpr uint32_t MAX_WIDTH                  = 8192;
  constexpr uint32_t MAX_HEIGHT                 = 8192;
  constexpr uint32_t MAX_FRAME_LENGTH           = LG_NET_VIDEO_MAX_FRAME_LENGTH;
  constexpr uint32_t MAX_BLOCKS                 = 262144;
  constexpr uint32_t MAX_PENDING_FRAMES         = 1;
  constexpr uint32_t MAX_SEND_ITEMS             = 8192;
  constexpr size_t   MAX_SEND_BYTES             = 96U * 1024U * 1024U;
  constexpr uint32_t MAX_FRAME_LATENCY_MS       = 50;
  constexpr uint32_t ENCODER_SLOT_COUNT         = 2;
  constexpr uint32_t PYROWAVE_LEVELS            = 5;
  constexpr uint32_t PYROWAVE_ALIGNMENT         = 1U << PYROWAVE_LEVELS;
  constexpr uint32_t PYROWAVE_MIN_DIMENSION     = 4U << PYROWAVE_LEVELS;
  constexpr uint32_t PYROWAVE_PACKET_BYTES      = 2488;
  constexpr uint32_t PYROWAVE_BLOCK_BYTES_MAX   =
    PYROWAVE_PACKET_BYTES + 8U;
  // Reserve an equal fragment budget for every possible codec block so a
  // negotiated delivery path can always represent the encoded frame.
  constexpr uint32_t PYROWAVE_FRAGS_PER_BLOCK   =
    LG_NET_VIDEO_MAX_FRAGMENTS / MAX_BLOCKS;
  constexpr uint32_t PYROWAVE_FRAGMENT_DATA_MIN =
    (PYROWAVE_BLOCK_BYTES_MAX + PYROWAVE_FRAGS_PER_BLOCK - 1U) /
      PYROWAVE_FRAGS_PER_BLOCK;
  constexpr uint32_t PYROWAVE_FRAGMENT_MIN      =
    LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE + PYROWAVE_FRAGMENT_DATA_MIN;
  constexpr uint32_t PYROWAVE_PACKET_META_WORDS = 2;
  constexpr uint32_t PYROWAVE_SEQUENCE_MASK     = 7;
  constexpr uint32_t PYROWAVE_HEADER_CODE_MASK  =
    LG_NET_VIDEO_PYROWAVE_MODE_MASK <<
      LG_NET_VIDEO_PYROWAVE_MODE_SHIFT;
  constexpr uint32_t PYROWAVE_HEADER_CODE_DELTA =
    LG_NET_VIDEO_PYROWAVE_MODE_DELTA <<
      LG_NET_VIDEO_PYROWAVE_MODE_SHIFT;
  constexpr uint32_t PYROWAVE_QUANT_WORDS_8X8   = 40;
  constexpr uint32_t PYROWAVE_STATE_BYTES       = 16;
  constexpr uint32_t PYROWAVE_QUANT_LEVELS      = 19;
  constexpr uint32_t PYROWAVE_STAT_WORDS        =
    1U + PYROWAVE_QUANT_LEVELS * 2U;
  constexpr uint32_t PYROWAVE_RDO_BUCKETS       = 128;
  constexpr uint32_t PYROWAVE_RDO_DIVISIONS     = 16;
  constexpr uint32_t PYROWAVE_RDO_WORDS         =
    16U + PYROWAVE_RDO_BUCKETS * PYROWAVE_RDO_DIVISIONS;
  constexpr uint32_t PYROWAVE_MAX_8X8_BLOCKS    = MAX_BLOCKS * 16U;
  constexpr uint64_t MAX_PLANE_ELEMENTS         = UINT64_C(1) <<
    D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
  constexpr uint32_t PYROWAVE_MIN_FRAME_BYTES   = 20;

  // Use capture cadence for an aggregate payload target. Limit idle credit to
  // one second so an inactive source cannot create an unbounded burst.
  constexpr uint64_t VIDEO_RATE_BYTES_PER_MBIT     = 125000;
  constexpr uint64_t VIDEO_RATE_NS_PER_SECOND      = 1000000000ULL;
  constexpr uint64_t VIDEO_RATE_DEFAULT_PERIOD_NS  =
    VIDEO_RATE_NS_PER_SECOND / 60U;
  constexpr uint64_t VIDEO_RATE_MAX_PERIOD_NS      =
    VIDEO_RATE_NS_PER_SECOND;

  constexpr LGNetVideoChromaSubsampling CHROMA_420 =
    static_cast<LGNetVideoChromaSubsampling>(LG_NET_VIDEO_CHROMA_420);
  constexpr LGNetVideoChromaSubsampling CHROMA_444 =
    static_cast<LGNetVideoChromaSubsampling>(LG_NET_VIDEO_CHROMA_444);

  static_assert(PYROWAVE_FRAGS_PER_BLOCK > 0,
    "The transport must represent every codec block");
  static_assert(PYROWAVE_BLOCK_BYTES_MAX <=
      PYROWAVE_FRAGS_PER_BLOCK * LG_NET_VIDEO_MAX_FRAGMENT_LENGTH,
    "The fragment payload limit must represent every codec block");

  constexpr LGNetEnvelopeFlags DELIVERY_MASK    =
    LG_NET_ENVELOPE_RESPONSE | LG_NET_ENVELOPE_FINAL |
    LG_NET_ENVELOPE_RELIABLE | LG_NET_ENVELOPE_DATAGRAM |
    LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_ERROR |
    LG_NET_ENVELOPE_EARLY_DATA;

  const FrameProfile VIDEO_PROFILES[] =
  {
    { FrameStorage::D3D12_TEXTURE, FramePixel::BGRA8,   FrameSignal::SRGB },
    { FrameStorage::D3D12_TEXTURE, FramePixel::RGBA8,   FrameSignal::SRGB },
    { FrameStorage::D3D12_TEXTURE, FramePixel::RGB10A2, FrameSignal::PQ_BT2020 },
  };

  uint32_t Align(uint32_t value, uint32_t alignment)
  {
    return (value + alignment - 1U) & ~(alignment - 1U);
  }

  bool PlaneElementsFit(uint32_t width, uint32_t height,
    LGNetVideoChromaSubsampling chroma)
  {
    if (!width || !height || width > MAX_WIDTH || height > MAX_HEIGHT)
      return false;
    if (chroma == CHROMA_420 && ((width | height) & 1U))
      return false;
    const uint32_t alignedWidth  = (std::max)(
      Align(width, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
    const uint32_t alignedHeight = (std::max)(
      Align(height, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
    if (alignedWidth > MAX_WIDTH || alignedHeight > MAX_HEIGHT)
      return false;
    const uint64_t lumaPixels    =
      static_cast<uint64_t>(alignedWidth) * alignedHeight;
    const uint64_t planeElements =
      chroma == CHROMA_420 ?
        lumaPixels + lumaPixels / 2U : lumaPixels * 3U;
    return planeElements <= MAX_PLANE_ELEMENTS;
  }

  class NetworkVideoFrameCaps final : public FrameCaps
  {
  private:
    const LGNetVideoChromaSubsampling m_chroma;

  public:
    explicit NetworkVideoFrameCaps(LGNetVideoChromaSubsampling chroma) :
      m_chroma(chroma)
    {
    }

    bool CanUseMode(const FrameMode& mode,
      uint32_t * requiredSizeMiB = nullptr) const override
    {
      if (requiredSizeMiB)
        *requiredSizeMiB = 0;
      return mode.width && mode.height && mode.refresh100uHz &&
        PlaneElementsFit(mode.width, mode.height, m_chroma);
    }
  };

  bool SamePeer(const NetworkVideoPeer& left, const NetworkVideoPeer& right)
  {
    return left.client == right.client &&
      left.generation == right.generation;
  }

  bool SameCfg(const FrameCfg& left, const FrameCfg& right)
  {
    return Frame::Same(left, right);
  }

  void MergeFrameDamage(FrameDesc& current, const FrameDesc& skipped)
  {
    if (skipped.damage == FrameDamage::NONE ||
        current.damage == FrameDamage::FULL)
      return;
    if (skipped.damage == FrameDamage::FULL)
    {
      current.damage = FrameDamage::FULL;
      current.count  = 0;
      return;
    }
    if (current.damage == FrameDamage::NONE)
    {
      current.damage = skipped.damage;
      current.count  = skipped.count;
      for (unsigned index = 0; index < skipped.count; ++index)
        current.rects[index] = skipped.rects[index];
      return;
    }

    if (current.count <= FRAME_DAMAGE_MAX - skipped.count)
    {
      for (unsigned index = 0; index < skipped.count; ++index)
        current.rects[current.count + index] = skipped.rects[index];
      current.count += skipped.count;
      return;
    }

    RECT bounds = current.rects[0];
    for (unsigned index = 1; index < current.count; ++index)
    {
      const RECT& rect = current.rects[index];
      bounds.left      = (std::min)(bounds.left, rect.left);
      bounds.top       = (std::min)(bounds.top, rect.top);
      bounds.right     = (std::max)(bounds.right, rect.right);
      bounds.bottom    = (std::max)(bounds.bottom, rect.bottom);
    }
    for (unsigned index = 0; index < skipped.count; ++index)
    {
      const RECT& rect = skipped.rects[index];
      bounds.left      = (std::min)(bounds.left, rect.left);
      bounds.top       = (std::min)(bounds.top, rect.top);
      bounds.right     = (std::max)(bounds.right, rect.right);
      bounds.bottom    = (std::max)(bounds.bottom, rect.bottom);
    }
    current.rects[0] = bounds;
    current.count    = 1;
  }

  bool SupportedProfile(const FrameProfile& profile)
  {
    for (const FrameProfile& supported : VIDEO_PROFILES)
      if (Frame::Same(profile, supported))
        return true;
    return false;
  }

  uint32_t NextPowerOfTwo(uint32_t value)
  {
    if (value <= 1U)
      return 1U;
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
  }

  uint64_t Next64(uint64_t value)
  {
    ++value;
    return value ? value : 1;
  }

  uint32_t LoadLE32(const uint8_t * data)
  {
    return static_cast<uint32_t>(data[0])       |
      static_cast<uint32_t>(data[1]) << 8U      |
      static_cast<uint32_t>(data[2]) << 16U     |
      static_cast<uint32_t>(data[3]) << 24U;
  }

  void StoreLE32(uint8_t * data, uint32_t value)
  {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8U);
    data[2] = static_cast<uint8_t>(value >> 16U);
    data[3] = static_cast<uint8_t>(value >> 24U);
  }

  LGNetEnvelopeFlags DeliveryFlag(NetworkVideoDelivery delivery)
  {
    switch (delivery)
    {
      case NetworkVideoDelivery::RELIABLE:
        return LG_NET_ENVELOPE_RELIABLE;

      case NetworkVideoDelivery::DATAGRAM:
        return LG_NET_ENVELOPE_DATAGRAM;

      case NetworkVideoDelivery::BULK:
        return 0;
    }
    return 0;
  }

  bool FragmentMessage(uint16_t messageType)
  {
    return messageType == LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT ||
      messageType == LG_NET_VIDEO_MESSAGE_FRAME_UPDATE_FRAGMENT;
  }

  unsigned DeliveryIndex(NetworkVideoDelivery delivery)
  {
    switch (delivery)
    {
      case NetworkVideoDelivery::RELIABLE:
        return 0;

      case NetworkVideoDelivery::DATAGRAM:
        return 1;

      case NetworkVideoDelivery::BULK:
        return 2;
    }
    return 0;
  }

  uint8_t DeliveryMask(NetworkVideoDelivery delivery)
  {
    return static_cast<uint8_t>(1U << DeliveryIndex(delivery));
  }

  bool DeliveryFlags(const LGNetEnvelope& envelope,
    NetworkVideoDelivery delivery, LGNetEnvelopeFlags expected)
  {
    return (envelope.flags & DELIVERY_MASK) ==
      (expected | DeliveryFlag(delivery));
  }

  bool DeliverySupported(const NetworkVideoPeerInfo& info,
    NetworkVideoDelivery delivery)
  {
    switch (delivery)
    {
      case NetworkVideoDelivery::RELIABLE:
        return info.capabilityFlags & LG_NET_CAPABILITY_RELIABLE;

      case NetworkVideoDelivery::DATAGRAM:
        return info.capabilityFlags & LG_NET_CAPABILITY_DATAGRAM;

      case NetworkVideoDelivery::BULK:
        return info.capabilityFlags & LG_NET_CAPABILITY_BULK;
    }
    return false;
  }

  uint32_t DeliveryPayload(const NetworkVideoPeerInfo& info,
    NetworkVideoDelivery delivery)
  {
    switch (delivery)
    {
      case NetworkVideoDelivery::RELIABLE:
        return info.maxReliablePayload;

      case NetworkVideoDelivery::DATAGRAM:
        return info.maxDatagramPayload;

      case NetworkVideoDelivery::BULK:
        return info.maxBulkPayload;
    }
    return 0;
  }

  bool DatagramFragmentsSupported(const NetworkVideoPeerInfo& info)
  {
    return DeliverySupported(info, NetworkVideoDelivery::RELIABLE) &&
      info.maxReliablePayload >= PYROWAVE_FRAGMENT_MIN &&
      DeliverySupported(info, NetworkVideoDelivery::DATAGRAM) &&
      info.maxDatagramPayload >= PYROWAVE_FRAGMENT_MIN;
  }

  bool BulkFragmentsSupported(const NetworkVideoPeerInfo& info)
  {
    return DeliverySupported(info, NetworkVideoDelivery::BULK) &&
      info.maxBulkPayload >= PYROWAVE_FRAGMENT_MIN;
  }

  bool Multiply(size_t left, size_t right, size_t& result)
  {
    if (right && left > (std::numeric_limits<size_t>::max)() / right)
      return false;
    result = left * right;
    return true;
  }

  bool Add(size_t left, size_t right, size_t& result)
  {
    if (left > (std::numeric_limits<size_t>::max)() - right)
      return false;
    result = left + right;
    return true;
  }

  DXGI_FORMAT ProfileFormat(FramePixel pixel)
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

  // The tiled transform, quantization, rate control, and packing pipeline is
  // derived from PyroWave commit 89f7e47d4abbf650c91fae766728af866c5e32a0.
  // Copyright (c) 2025 Hans-Kristian Arntzen, provided under the MIT license.
  constexpr char ENCODER_SHADER[] = R"hlsl(
Texture2D<float4>         Source       : register(t0);
RWStructuredBuffer<float> Wave         : register(u0);
RWStructuredBuffer<float> Scratch      : register(u1);
RWStructuredBuffer<float> Atlas        : register(u2);
RWStructuredBuffer<float> Planes       : register(u3);
RWByteAddressBuffer       Packed       : register(u4);
RWByteAddressBuffer       PackState    : register(u5);
RWStructuredBuffer<uint>  Aux          : register(u6);
RWStructuredBuffer<uint>  QuantPayload : register(u7);

cbuffer Parameters : register(b0)
{
  uint4  Dim0;
  uint4  Op0;
  uint4  Op1;
  uint4  AuxBase;
  uint4  Work;
  uint4  State;
  float4 Values;
};

uint mirrorCoord(int coord, uint length)
{
  if (length <= 1u)
    return 0u;
  const int period  = int(2u * (length - 1u));
  int       wrapped = coord % period;
  if (wrapped < 0)
    wrapped += period;
  return uint(wrapped) < length ? uint(wrapped) : uint(period - wrapped);
}

float3 rgbToYCbCr(float3 rgb, bool bt2020)
{
  const float kr = bt2020 ? 0.2627 : 0.2126;
  const float kb = bt2020 ? 0.0593 : 0.0722;
  const float kg = 1.0 - kr - kb;
  const float y  = dot(rgb, float3(kr, kg, kb));
  return float3(y, (rgb.b - y) / (2.0 * (1.0 - kb)),
    (rgb.r - y) / (2.0 * (1.0 - kr)));
}

float3 loadSourceYCbCr(int2 coord, bool bt2020)
{
  coord.x = clamp(coord.x, 0, int(Dim0.x) - 1);
  coord.y = clamp(coord.y, 0, int(Dim0.y) - 1);
  return rgbToYCbCr(Source.Load(int3(coord, 0)).rgb, bt2020);
}

float lumaValue(float value)
{
  return (64.0 / 1023.0 + value * (876.0 / 1023.0)) - 0.5;
}

float chromaValue(float value)
{
  return (512.0 / 1023.0 - 0.5) + value * (896.0 / 1023.0);
}

[numthreads(8, 8, 1)]
void ColorMain(uint3 tid : SV_DispatchThreadID)
{
  const uint2 pixel      = tid.xy + Work.xy * 8u;
  const bool  subsampled = Op0.w != 0u;
  const bool  bt2020     = Op1.x != 0u;
  if (subsampled)
  {
    const uint chromaWidth  = Dim0.z >> 1u;
    const uint chromaHeight = Dim0.w >> 1u;
    if (pixel.x >= chromaWidth || pixel.y >= chromaHeight)
      return;
    const uint2  chromaLimit = Dim0.xy >> 1u;
    const uint2  chromaCoord = min(pixel, chromaLimit - 1u);
    const int2   chromaSrc   = int2(chromaCoord * 2u);
    const float3 a           = loadSourceYCbCr(chromaSrc, bt2020);
    const float3 b           = loadSourceYCbCr(
      chromaSrc + int2(1, 0), bt2020);
    const float3 c           = loadSourceYCbCr(
      chromaSrc + int2(0, 1), bt2020);
    const float3 d           = loadSourceYCbCr(
      chromaSrc + int2(1, 1), bt2020);
    float3       ya          = a;
    float3       yb          = b;
    float3       yc          = c;
    float3       yd          = d;
    const int2   lumaSrc     = int2(pixel * 2u);
    if (any(pixel != chromaCoord))
    {
      ya = loadSourceYCbCr(lumaSrc, bt2020);
      yb = loadSourceYCbCr(lumaSrc + int2(1, 0), bt2020);
      yc = loadSourceYCbCr(lumaSrc + int2(0, 1), bt2020);
      yd = loadSourceYCbCr(lumaSrc + int2(1, 1), bt2020);
    }
    const uint   y0  = (pixel.y * 2u) * Dim0.z + pixel.x * 2u;
    const uint   y1  = y0 + Dim0.z;
    Planes[Op0.x + y0]      = lumaValue(ya.x);
    Planes[Op0.x + y0 + 1u] = lumaValue(yb.x);
    Planes[Op0.x + y1]      = lumaValue(yc.x);
    Planes[Op0.x + y1 + 1u] = lumaValue(yd.x);
    const uint chroma = pixel.y * chromaWidth + pixel.x;
    Planes[Op0.y + chroma] = chromaValue(
      (a.y + b.y + c.y + d.y) * 0.25);
    Planes[Op0.z + chroma] = chromaValue(
      (a.z + b.z + c.z + d.z) * 0.25);
    return;
  }

  if (pixel.x >= Dim0.z || pixel.y >= Dim0.w)
    return;
  const float3 yuv   = loadSourceYCbCr(int2(pixel), bt2020);
  const uint   index = pixel.y * Dim0.z + pixel.x;
  Planes[Op0.x + index] = lumaValue(yuv.x);
  Planes[Op0.y + index] = chromaValue(yuv.y);
  Planes[Op0.z + index] = chromaValue(yuv.z);
}

groupshared float2 DWTShared[20 * 41];

uint2 unswizzle8x8(uint index)
{
  uint y = index & 1u;
  uint x = (index >> 1u) & 3u;
  y |= ((index >> 3u) & 3u) << 1u;
  x |= ((index >> 5u) & 1u) << 2u;
  return uint2(x, y);
}

float loadPlaneValue(int2 coord)
{
  const uint x = mirrorCoord(coord.x, Op0.x);
  const uint y = mirrorCoord(coord.y, Op0.y);
  if (Op1.x == 0u)
    return Planes[Op1.w + y * Dim0.z + x];
  const uint index = Op1.y + y * Dim0.z + x;
  return (Op1.x & 1u) != 0u ? Wave[index] : Scratch[index];
}

void storeDWTValue(uint x, uint y, float value, bool low)
{
  if (x >= Op0.x || y >= Op0.y)
    return;
  const uint index = Op1.w + y * Dim0.z + x;
  // Intermediate low-pass values feed the next level while their atlas
  // region retains the deeper-level coefficients from the prior frame.
  if (!low || Work.z != 0u)
    Atlas[index] = value;
  if (!low)
    return;
  const uint lowIndex = Op1.z + y * Dim0.z + x;
  if ((Op1.x & 1u) != 0u)
    Scratch[lowIndex] = value;
  else
    Wave[lowIndex] = value;
}

float2 loadDWTShared(uint y, uint x)
{
  return DWTShared[y * 41u + x];
}

void storeDWTShared(uint y, uint x, float2 value)
{
  DWTShared[y * 41u + x] = value;
}

void loadDWTQuad(uint2 group, uint2 local)
{
  const int2 coord = int2(group * 32u + local) - 4;
  storeDWTShared(local.y >> 1u, local.x,
    float2(loadPlaneValue(coord), loadPlaneValue(coord + int2(0, 1))));
  storeDWTShared(local.y >> 1u, local.x + 1u,
    float2(loadPlaneValue(coord + int2(1, 0)),
      loadPlaneValue(coord + int2(1, 1))));
}

void forwardTransform8x2(uint lane)
{
  const float alpha = -1.586134342059924;
  const float beta  = -0.052980118572961;
  const float gamma =  0.882911075530934;
  const float delta =  0.443506852043971;
  const float k     =  1.230174104914001;
  const float invK  =  1.0 / k;
  const uint2 local = uint2(8u * (lane & 3u), lane >> 2u);
  float2      values[16];
  [unroll]
  for (uint i = 0u; i < 16u; ++i)
    values[i] = loadDWTShared(local.y, local.x + i);
  [unroll]
  for (uint i = 1u; i < 15u; i += 2u)
    values[i] += alpha * (values[i - 1u] + values[i + 1u]);
  [unroll]
  for (uint i = 2u; i < 14u; i += 2u)
    values[i] += beta * (values[i - 1u] + values[i + 1u]);
  [unroll]
  for (uint i = 3u; i < 13u; i += 2u)
    values[i] += gamma * (values[i - 1u] + values[i + 1u]);
  [unroll]
  for (uint i = 4u; i < 12u; i += 2u)
    values[i] += delta * (values[i - 1u] + values[i + 1u]);

  GroupMemoryBarrierWithGroupSync();

  [unroll]
  for (uint i = 2u; i < 6u; ++i)
  {
    const float2 a = values[2u * i] * invK;
    const float2 b = values[2u * i + 1u] * k;
    const uint   y = (local.x >> 1u) + i - 2u;
    storeDWTShared(y, 2u * local.y, float2(a.x, b.x));
    storeDWTShared(y, 2u * local.y + 1u, float2(a.y, b.y));
  }
}

void forwardTransform4x2(uint lane, bool active, uint yOffset)
{
  const float alpha = -1.586134342059924;
  const float beta  = -0.052980118572961;
  const float gamma =  0.882911075530934;
  const float delta =  0.443506852043971;
  const float k     =  1.230174104914001;
  const float invK  =  1.0 / k;
  const uint2 local = uint2(4u * (lane & 7u), (lane >> 3u) + yOffset);
  float2      values[12];
  if (active)
  {
    [unroll]
    for (uint i = 0u; i < 12u; ++i)
      values[i] = loadDWTShared(local.y, local.x + i);
    [unroll]
    for (uint i = 1u; i < 11u; i += 2u)
      values[i] += alpha * (values[i - 1u] + values[i + 1u]);
    [unroll]
    for (uint i = 2u; i < 10u; i += 2u)
      values[i] += beta * (values[i - 1u] + values[i + 1u]);
    [unroll]
    for (uint i = 3u; i < 9u; i += 2u)
      values[i] += gamma * (values[i - 1u] + values[i + 1u]);
    [unroll]
    for (uint i = 4u; i < 8u; i += 2u)
      values[i] += delta * (values[i - 1u] + values[i + 1u]);
  }

  GroupMemoryBarrierWithGroupSync();

  if (active)
  {
    [unroll]
    for (uint i = 2u; i < 4u; ++i)
    {
      const float2 a = values[2u * i] * invK;
      const float2 b = values[2u * i + 1u] * k;
      const uint   y = (local.x >> 1u) + i - 2u;
      storeDWTShared(y, 2u * local.y, float2(a.x, b.x));
      storeDWTShared(y, 2u * local.y + 1u, float2(a.y, b.y));
    }
  }
}

[numthreads(64, 1, 1)]
void DWTMain(uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
  const uint2 workGroup = groupID.xy + Work.xy;
  const uint2 local     = 2u * unswizzle8x8(lane);
  loadDWTQuad(workGroup, local);
  loadDWTQuad(workGroup, local + uint2(16u, 0u));
  loadDWTQuad(workGroup, local + uint2(0u, 16u));
  loadDWTQuad(workGroup, local + uint2(16u, 16u));

  const uint2 right = uint2(32u + 2u * (lane & 3u),
    2u * (lane >> 2u));
  loadDWTQuad(workGroup, right);

  const uint2 bottom = uint2(2u * (lane & 15u),
    32u + 2u * (lane >> 4u));
  loadDWTQuad(workGroup, bottom);

  if (lane < 16u)
  {
    const uint2 corner = uint2(32u + 2u * (lane & 3u),
      32u + 2u * (lane >> 2u));
    loadDWTQuad(workGroup, corner);
  }
  GroupMemoryBarrierWithGroupSync();

  forwardTransform8x2(lane);
  forwardTransform4x2(lane, lane < 32u, 16u);
  GroupMemoryBarrierWithGroupSync();

  forwardTransform8x2(lane);
  GroupMemoryBarrierWithGroupSync();

  const uint2 outputLocal = unswizzle8x8(lane);
  const uint  bandWidth   = Op0.x >> 1u;
  const uint  bandHeight  = Op0.y >> 1u;
  for (uint y = outputLocal.y; y < 16u; y += 8u)
  {
    for (uint x = outputLocal.x * 2u; x < 32u; x += 16u)
    {
      const float2 v0   = loadDWTShared(y, x);
      const float2 v1   = loadDWTShared(y, x + 1u);
      const uint   outX = workGroup.x * 16u + (x >> 1u);
      const uint   outY = workGroup.y * 16u + y;
      if (outX < bandWidth && outY < bandHeight)
      {
        storeDWTValue(outX, outY, v0.x, true);
        storeDWTValue(outX + bandWidth, outY, v1.x, false);
        storeDWTValue(outX, outY + bandHeight, v0.y, false);
        storeDWTValue(outX + bandWidth, outY + bandHeight, v1.y, false);
      }
    }
  }
}

static const uint QuantLevels       = 19u;
static const uint StatWords         = 1u + QuantLevels * 2u;
static const uint RDOBuckets        = 128u;
static const uint RDODivisions      = 16u;
static const uint RDOStateBase      = 16u;
static const uint RDOStateWords     =
  RDOStateBase + RDOBuckets * RDODivisions;
static const uint PacketWordsPerMax = 622u;

uint magnitudeBits(uint magnitude)
{
  return magnitude != 0u ? uint(firstbithigh(magnitude)) + 1u : 0u;
}

void computeQuantScale(float maxValue, out uint code, out float scale)
{
  if (maxValue < 1.0)
  {
    code  = 6u;
    scale = 1.0;
    return;
  }

  const int   exponent = int(floor(log2(maxValue - 0.25))) + 1;
  const float target   = exp2(float(exponent)) - 0.25;
  const float invScale = maxValue / target;
  code  = min(uint(ceil((invScale - 0.25) * 8.0)), 15u);
  scale = 1.0 / (float(code) / 8.0 + 0.25);
}

float bandCoefficient(uint x, uint y)
{
  const uint index = Dim0.y + (Op0.y + y) * Dim0.x + Op0.x + x;
  return Atlas[index] * Values.x;
}

[numthreads(256, 1, 1)]
void ClearMain(uint3 tid : SV_DispatchThreadID)
{
  if (tid.x == 0u)
    QuantPayload[0] = 1u;
  if (tid.x < Dim0.x)
    Aux[AuxBase.w + tid.x] = 0u;
  if (Work.x != 0u && tid.x < Dim0.y)
    QuantPayload[State.x + tid.x] = 0u;
}
)hlsl"
R"hlsl(
groupshared uint  QuantMax         [128];
groupshared uint  QuantScaleCode   [16];
groupshared float QuantScaleValue  [16];
groupshared uint  QuantQuality     [16];
groupshared uint  QuantControl     [16];
groupshared uint  QuantPlanes      [128];
groupshared uint  QuantSigns       [128];
groupshared float QuantError       [128];
groupshared uint  QuantPayloadBase [16];
groupshared uint  QuantPayloadValid[16];

[numthreads(128, 1, 1)]
void QuantMain(uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
  const uint2 workGroup = groupID.xy + Work.xy;
  const uint  block     = lane >> 3u;
  const uint  subblock  = lane & 7u;
  const uint  blockX    = workGroup.x * 4u + (block & 3u);
  const uint  blockY    = workGroup.y * 4u + (block >> 2u);
  const bool  valid     = blockX < Op0.z && blockY < Op0.w;
  const uint  baseX     = blockX * 8u + (subblock >> 2u) * 4u;
  const uint  baseY     = blockY * 8u + (subblock & 3u) * 2u;
  float       values   [8];
  float       localMax  = 0.0;
  for (uint x = 0u; x < 4u; ++x)
  {
    const uint xCoord = baseX + x;
    const bool top    = valid && xCoord < Dim0.z && baseY < Dim0.w;
    const bool bottom = valid && xCoord < Dim0.z && baseY + 1u < Dim0.w;
    values[2u * x]      = top ? bandCoefficient(xCoord, baseY) : 0.0;
    values[2u * x + 1u] = bottom ?
      bandCoefficient(xCoord, baseY + 1u) : 0.0;
    localMax = max(localMax, abs(values[2u * x]));
    localMax = max(localMax, abs(values[2u * x + 1u]));
  }
  QuantMax[lane] = asuint(localMax);
  GroupMemoryBarrierWithGroupSync();

  if (subblock == 0u)
  {
    float blockMax = 0.0;
    for (uint index = 0u; index < 8u; ++index)
      blockMax = max(blockMax, asfloat(QuantMax[block * 8u + index]));
    uint  code;
    float scale;
    computeQuantScale(blockMax, code, scale);
    QuantScaleCode[block]  = code;
    QuantScaleValue[block] = scale;
  }
  GroupMemoryBarrierWithGroupSync();

  const float adaptive = QuantScaleValue[block];
  const float invQuant = 1.0 / max(Values.x * adaptive, 1e-20);
  uint        magnitudes[8];
  uint        signMask = 0u;
  uint        maximum  = 0u;
  for (uint index = 0u; index < 8u; ++index)
  {
    const float scaled = abs(values[index] * adaptive);
    magnitudes[index]  = uint(min(floor(scaled), 262143.0));
    maximum            = max(maximum, magnitudes[index]);
    if (values[index] < 0.0)
      signMask |= 1u << index;
  }
  QuantMax[lane] = maximum;
  GroupMemoryBarrierWithGroupSync();

  const uint blockIndex = Op1.z + blockY * Op0.z + blockX;
  if (subblock == 0u)
  {
    uint maxPlanes = 0u;
    uint control   = 0u;
    for (uint index = 0u; index < 8u; ++index)
      maxPlanes = max(maxPlanes,
        magnitudeBits(QuantMax[block * 8u + index]));
    const uint quality = maxPlanes > 3u ? maxPlanes - 3u : 0u;
    for (uint index = 0u; index < 8u; ++index)
    {
      const uint planes = magnitudeBits(QuantMax[block * 8u + index]);
      const uint coded  = planes > quality ?
        min(planes - quality, 3u) : 0u;
      control |= coded << (index * 2u);
    }
    QuantQuality[block] = quality;
    QuantControl[block] = valid && maxPlanes != 0u ?
      control | (quality << 16u) | (QuantScaleCode[block] << 20u) : 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  const uint quality      = QuantQuality[block];
  const uint planes       = magnitudeBits(maximum);
  const uint coded        = planes > quality ?
    min(planes - quality, 3u) : 0u;
  const uint count        = quality + coded;
  const uint payloadBytes = count != 0u ? count + 1u : 0u;
  const uint payloadWords = (payloadBytes + 3u) >> 2u;
  uint       payload[5];
  [unroll]
  for (uint word = 0u; word < 5u; ++word)
    payload[word] = 0u;
  if (payloadWords != 0u)
  {
    payload[0] = signMask;
    for (uint plane = 0u; plane < count; ++plane)
    {
      const uint shift = count - plane - 1u;
      uint       value = 0u;
      for (uint index = 0u; index < 8u; ++index)
        value |= ((magnitudes[index] >> shift) & 1u) << index;
      const uint byte = plane + 1u;
      payload[byte >> 2u] |= value << ((byte & 3u) * 8u);
    }
  }
  QuantPlanes[lane] = valid ? payloadWords : 0u;
  GroupMemoryBarrierWithGroupSync();

  if (subblock == 0u)
  {
    uint totalWords = 0u;
    for (uint index = 0u; index < 8u; ++index)
      totalWords += QuantPlanes[block * 8u + index];

    uint payloadBase  = 0u;
    uint payloadValid = 0u;
    if (valid && totalWords != 0u)
    {
      InterlockedAdd(QuantPayload[0], totalWords, payloadBase);
      if (payloadBase <= Op1.w && totalWords <= Op1.w - payloadBase)
        payloadValid = 1u;
      else
      {
        uint ignored;
        PackState.InterlockedOr(8u, 1u, ignored);
      }
    }
    QuantPayloadBase [block] = payloadBase;
    QuantPayloadValid[block] = payloadValid;
    if (valid)
    {
      Aux[AuxBase.y + blockIndex * 2u]      = payloadValid != 0u ?
        QuantControl[block] : 0u;
      Aux[AuxBase.y + blockIndex * 2u + 1u] = payloadValid != 0u ?
        payloadBase : 0u;
    }
  }
  GroupMemoryBarrierWithGroupSync();

  if (valid && QuantPayloadValid[block] != 0u)
  {
    uint payloadBase = QuantPayloadBase[block];
    for (uint previous = 0u; previous < subblock; ++previous)
      payloadBase += QuantPlanes[block * 8u + previous];
    for (uint word = 0u; word < payloadWords; ++word)
      QuantPayload[payloadBase + word] = payload[word];
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint q = 0u; q < QuantLevels; ++q)
  {
    uint        maximum     = 0u;
    uint        signs       = 0u;
    float       squareError = 0.0;
    const float step        = exp2(float(q));
    for (uint index = 0u; index < 8u; ++index)
    {
      const float scaled    = abs(values[index] * adaptive);
      const uint  magnitude = magnitudes[index] >> q;
      maximum  = max(maximum, magnitude);
      signs   += magnitude != 0u ? 1u : 0u;
      const float reconstructed = magnitude != 0u ?
        (float(magnitude) + 0.5) * step : 0.0;
      const float error         = (scaled - reconstructed) * invQuant;
      squareError += error * error * Values.y;
    }
    QuantPlanes[lane] = magnitudeBits(maximum);
    QuantSigns[lane]  = signs;
    QuantError[lane]  = q == 0u ? 0.0 : min(squareError, 60000.0);
    GroupMemoryBarrierWithGroupSync();

    if (subblock == 0u && valid)
    {
      uint  maxPlanes      = 0u;
      uint  magnitudeBytes = 0u;
      uint  signBits       = 0u;
      float error          = 0.0;
      for (uint index = 0u; index < 8u; ++index)
      {
        const uint source = block * 8u + index;
        maxPlanes  = max(maxPlanes, QuantPlanes[source]);
        signBits  += QuantSigns[source];
        error     += QuantError[source];
      }
      const uint qualityPlanes = maxPlanes > 3u ? maxPlanes - 3u : 0u;
      for (uint index = 0u; index < 8u; ++index)
      {
        const uint planes = QuantPlanes[block * 8u + index];
        const uint coded  = planes > qualityPlanes ?
          min(planes - qualityPlanes, 3u) : 0u;
        magnitudeBytes += coded + qualityPlanes;
      }
      const uint stat       = blockIndex * StatWords;
      if (q == 0u)
        Aux[AuxBase.x + stat] = maxPlanes;
      Aux[AuxBase.x + stat + 1u + q * 2u] =
        asuint(min(error, 60000.0));
      Aux[AuxBase.x + stat + 2u + q * 2u] =
        magnitudeBytes * 8u + signBits;
    }
    GroupMemoryBarrierWithGroupSync();
  }
}

uint distortionBucket(float distortion, uint cost,
  float baseDistortion, uint baseCost)
{
  if (cost >= baseCost)
    return 0u;
  const float delta = max(distortion - baseDistortion, 0.0);
  if (delta <= 0.0)
    return 0u;
  const float index = 60.0 + 2.0 * log2(delta / float(baseCost - cost));
  return min(uint(max(index + 0.5, 0.0)), RDOBuckets - 1u);
}

groupshared uint  AnalyzeCostLane       [64];
groupshared float AnalyzeDistortionLane [64];
groupshared uint  AnalyzeCost           [QuantLevels];
groupshared float AnalyzeDistortion     [QuantLevels];

[numthreads(64, 1, 1)]
void AnalyzeMain(uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
  const uint2 workGroup   = groupID.xy + Work.xy;
  const uint  blockSlot   = lane & 15u;
  const uint  quantGroup  = lane >> 4u;
  const uint  blockX      = workGroup.x * 4u + (blockSlot & 3u);
  const uint  blockY      = workGroup.y * 4u + (blockSlot >> 2u);
  const bool  valid       = blockX < Op0.z && blockY < Op0.w;
  const uint  blockIndex8 = Op1.z + blockY * Op0.z + blockX;

  for (uint round = 0u; round < (QuantLevels + 3u) / 4u; ++round)
  {
    const uint quant      = quantGroup + 4u * round;
    const bool quantLive  = quant < QuantLevels;
    uint       cost       = 0u;
    float      distortion = 0.0;
    if (valid && quantLive)
    {
      const uint stat = blockIndex8 * StatWords + 1u + quant * 2u;
      cost       = Aux[AuxBase.x + stat + 1u];
      distortion = asfloat(Aux[AuxBase.x + stat]);
      if (cost != 0u)
        cost += 24u;
    }
    AnalyzeCostLane[lane]       = cost;
    AnalyzeDistortionLane[lane] = distortion;
    GroupMemoryBarrierWithGroupSync();

    if (blockSlot == 0u && quantLive)
    {
      uint  totalCost       = 0u;
      float totalDistortion = 0.0;
      for (uint index = 0u; index < 16u; ++index)
      {
        const uint source = quantGroup * 16u + index;
        totalCost       += AnalyzeCostLane[source];
        totalDistortion += AnalyzeDistortionLane[source];
      }
      if (totalCost != 0u)
        totalCost += 64u;
      AnalyzeCost[quant]       = (totalCost + 31u) >> 5u;
      AnalyzeDistortion[quant] = totalDistortion;
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (lane == 0u)
  {
    const uint blockIndex  = Op1.y + workGroup.y * Op1.x + workGroup.x;
    const uint perDivision = max(uint(Values.w), 1u);
    const uint division    = min(
      blockIndex / perDivision, RDODivisions - 1u);
    uint       ignored;
    InterlockedAdd(Aux[AuxBase.w], AnalyzeCost[0], ignored);
    InterlockedAdd(Aux[AuxBase.w + 2u],
      QuantPayload[State.x + blockIndex + 1u], ignored);

    uint previous = 0u;
    for (uint quant = 1u; quant < QuantLevels; ++quant)
    {
      const uint raw    = distortionBucket(
        AnalyzeDistortion[quant], AnalyzeCost[quant],
        AnalyzeDistortion[0], AnalyzeCost[0]);
      const uint bucket = min(max(raw, previous + 1u),
        RDOBuckets - QuantLevels + quant);
      const uint saving = AnalyzeCost[quant - 1u] > AnalyzeCost[quant] ?
        AnalyzeCost[quant - 1u] - AnalyzeCost[quant] : 0u;
      Aux[AuxBase.z + blockIndex * (QuantLevels - 1u) + quant - 1u] =
        saving != 0u ? (saving << 7u) | bucket : 0u;
      if (saving != 0u)
        InterlockedAdd(
          Aux[AuxBase.w + RDOStateBase +
            bucket * RDODivisions + division],
          saving, ignored);
      previous = bucket;
    }
  }
}

groupshared uint RDOScan[512];

[numthreads(512, 1, 1)]
void FinalizeRDOMain(uint lane : SV_GroupIndex)
{
  const uint base  = AuxBase.w + RDOStateBase + lane * 4u;
  uint4      value = uint4(Aux[base], Aux[base + 1u],
    Aux[base + 2u], Aux[base + 3u]);
  value.y += value.x;
  value.z += value.y;
  value.w += value.z;
  RDOScan[lane] = value.w;
  GroupMemoryBarrierWithGroupSync();

  for (uint step = 1u; step < 512u; step <<= 1u)
  {
    const uint up = lane >= step ? RDOScan[lane - step] : 0u;
    GroupMemoryBarrierWithGroupSync();
    value += up;
    RDOScan[lane] = value.w;
    GroupMemoryBarrierWithGroupSync();
  }
  Aux[base]      = value.x;
  Aux[base + 1u] = value.y;
  Aux[base + 2u] = value.z;
  Aux[base + 3u] = value.w;
}

[numthreads(64, 1, 1)]
void ResolveMain(uint3 tid : SV_DispatchThreadID)
{
  if (tid.x >= Work.y)
    return;
  const uint blockIndex = Work.x + tid.x;

  const uint canonicalTotal = QuantPayload[State.x];
  const uint oldDirty       = Aux[AuxBase.w + 2u];
  const uint fixed          = canonicalTotal >= oldDirty ?
    canonicalTotal - oldDirty : 0u;
  const uint consumed       = fixed + Aux[AuxBase.w];
  const uint target         = min(Work.z, fixed + Work.w);
  if (consumed <= target)
  {
    Aux[AuxBase.w + RDOStateWords + blockIndex] = 0u;
    return;
  }

  const uint required    = consumed - target;
  const uint subdivision = min(
    blockIndex / max(Dim0.z, 1u), RDODivisions - 1u);
  uint       selected    = 0u;
  for (uint quant = 1u; quant < QuantLevels; ++quant)
  {
    const uint operation =
      Aux[AuxBase.z + blockIndex * (QuantLevels - 1u) + quant - 1u];
    const uint saving    = operation >> 7u;
    if (saving == 0u)
      continue;

    const uint bucket  = operation & (RDOBuckets - 1u);
    const uint segment = bucket * RDODivisions + subdivision;
    const uint after   = Aux[AuxBase.w + RDOStateBase + segment];
    const uint before  = segment != 0u ?
      Aux[AuxBase.w + RDOStateBase + segment - 1u] : 0u;
    if (after <= required)
      selected = quant;
    else if (before < required)
    {
      uint cursor;
      InterlockedAdd(Aux[AuxBase.w + 1u], saving, cursor);
      if (cursor < required - before)
        selected = quant;
    }
  }
  Aux[AuxBase.w + RDOStateWords + blockIndex] = selected;
}
)hlsl"
R"hlsl(
groupshared uint PackControl        [64];
groupshared uint PackScale          [64];
groupshared uint PackMagnitudeBytes [64];
groupshared uint PackMagnitudeOffset[64];
groupshared uint PackSignBits       [64];
groupshared uint PackSignificant    [64 * 8];
groupshared uint PackWords          [4 * PacketWordsPerMax];
groupshared uint PackBallot         [4];
groupshared uint PackPlaneBase      [4];
groupshared uint PackSignBase       [4];
groupshared uint PackSize           [4];
groupshared uint PackOffset         [4];
groupshared uint PackValid          [4];

void packByteOr(uint record, uint offset, uint value)
{
  InterlockedOr(PackWords[record * PacketWordsPerMax + (offset >> 2u)],
    (value & 0xffu) << ((offset & 3u) * 8u));
}

void packControlOr(uint record, uint compact, uint value)
{
  const uint offset = 8u + compact * 2u;
  InterlockedOr(PackWords[record * PacketWordsPerMax + (offset >> 2u)],
    (value & 0xffffu) << ((offset & 2u) * 8u));
}

uint modifyQuantCode(uint code, uint quant)
{
  const uint exponent = code >> 3u;
  return ((exponent > quant ? exponent - quant : 0u) << 3u) |
    (code & 7u);
}

uint quantizeControlWord(uint code, uint quant)
{
  if (quant == 0u || code == 0u)
    return code;

  uint       quality      = (code >> 16u) & 15u;
  const uint qualityQuant = min(quality, quant);
  quality -= qualityQuant;
  quant   -= qualityQuant;
  if (quant != 0u)
  {
    quant = min(quant, 3u);
    uint plane0 = code & 0x5555u;
    uint plane1 = (code & 0xaaaau) >> 1u;
    uint plane2 = plane0 & plane1;
    do
    {
      plane0 = plane1;
      plane1 = plane2;
      plane2 = 0u;
      --quant;
    }
    while (quant != 0u);
    plane0 &= ~plane1;
    code = (code & ~0xffffu) | plane0 | (plane1 << 1u);
  }
  return (code & ~(15u << 16u)) | (quality << 16u);
}

uint quantPayloadByte(uint wordOffset, uint byteOffset)
{
  return (QuantPayload[wordOffset + (byteOffset >> 2u)] >>
    ((byteOffset & 3u) * 8u)) & 0xffu;
}

uint quantPayloadSubblockOffset(
  uint codeWord, uint payloadBase, uint subblock)
{
  const uint quality = (codeWord >> 16u) & 15u;
  uint       offset  = payloadBase;
  for (uint previous = 0u; previous < subblock; ++previous)
  {
    const uint planes =
      ((codeWord >> (previous * 2u)) & 3u) + quality;
    if (planes != 0u)
      offset += (planes + 4u) >> 2u;
  }
  return offset;
}

[numthreads(64, 1, 1)]
void PackMain(uint3 groupID : SV_GroupID, uint lane : SV_GroupIndex)
{
  const uint2 workGroup   = groupID.xy + Work.xy;
  const uint  record      = lane >> 4u;
  const uint  block       = lane & 15u;
  const uint  packetX     = workGroup.x * 2u + (record & 1u);
  const uint  packetY     = workGroup.y * 2u + (record >> 1u);
  const uint  blocksY     = (Dim0.w + 31u) / 32u;
  const bool  packetLive  = (Work.z & (1u << record)) != 0u &&
    packetX < Op1.x && packetY < blocksY;
  const uint  blockIndex  = Op1.y + packetY * Op1.x + packetX;
  const uint  quant       = packetLive ? min(
    Aux[AuxBase.w + RDOStateWords + blockIndex], QuantLevels - 1u) : 0u;
  const uint  blockX      = packetX * 4u + (block & 3u);
  const uint  blockY      = packetY * 4u + (block >> 2u);
  const bool  blockLive   = packetLive && blockX < Op0.z && blockY < Op0.w;
  const uint  blockIndex8 = Op1.z + blockY * Op0.z + blockX;
  const uint2 meta        = blockLive ?
    uint2(Aux[AuxBase.y + blockIndex8 * 2u],
      Aux[AuxBase.y + blockIndex8 * 2u + 1u]) : uint2(0u, 0u);
  const uint  codeWord    = quantizeControlWord(meta.x, quant);
  const uint  controlWord = codeWord & 0xffffu;
  const uint  quality     = (codeWord >> 16u) & 15u;
  const uint  scaleCode   = (codeWord >> 20u) & 15u;

  if (block == 0u)
  {
    PackBallot[record] = 0u;
    PackValid[record]  = 0u;
    if (packetLive)
    {
      Aux[AuxBase.z + blockIndex * 2u]      = 0u;
      Aux[AuxBase.z + blockIndex * 2u + 1u] = 0u;
    }
  }
  for (uint word = block; word < PacketWordsPerMax; word += 16u)
    PackWords[record * PacketWordsPerMax + word] = 0u;
  GroupMemoryBarrierWithGroupSync();

  uint magnitude = 0u;
  uint signBits  = 0u;
  for (uint subblock = 0u; subblock < 8u; ++subblock)
  {
    const uint coded =
      ((controlWord >> (subblock * 2u)) & 3u) + quality;
    magnitude += coded;

    uint significant = 0u;
    if (blockLive && coded != 0u)
    {
      const uint payloadOffset = quantPayloadSubblockOffset(
        meta.x, meta.y, subblock);
      for (uint plane = 0u; plane < coded; ++plane)
        significant |= quantPayloadByte(payloadOffset, plane + 1u);
    }
    PackSignificant[lane * 8u + subblock] = significant;
    signBits += countbits(significant);
  }
  PackControl[lane]        = controlWord;
  PackScale[lane]          = (scaleCode << 4u) | quality;
  PackMagnitudeBytes[lane] = magnitude;
  PackSignBits[lane]       = signBits;
  if (blockLive && controlWord != 0u)
    InterlockedOr(PackBallot[record], 1u << block);
  GroupMemoryBarrierWithGroupSync();

  if (block == 0u && packetLive && PackBallot[record] != 0u)
  {
    const uint ballot         = PackBallot[record];
    const uint groups         = countbits(ballot);
    uint       magnitudeBytes = 0u;
    uint       packetSignBits = 0u;
    for (uint group = 0u; group < 16u; ++group)
    {
      const uint index = record * 16u + group;
      PackMagnitudeOffset[index] = magnitudeBytes;
      if ((ballot & (1u << group)) != 0u)
      {
        magnitudeBytes += PackMagnitudeBytes[index];
        packetSignBits += PackSignBits[index];
      }
    }
    PackPlaneBase[record] = 8u + groups * 3u;
    PackSignBase[record]  = PackPlaneBase[record] + magnitudeBytes;
    PackSize[record]      =
      (PackSignBase[record] + (packetSignBits + 7u) / 8u + 3u) & ~3u;
    PackWords[record * PacketWordsPerMax] = ballot |
      ((PackSize[record] >> 2u) << 16u) | (uint(Values.z) << 28u);
    PackWords[record * PacketWordsPerMax + 1u] =
      modifyQuantCode(uint(Values.w), quant) | (blockIndex << 8u);

    uint offset;
    PackState.InterlockedAdd(0u, PackSize[record], offset);
    if (Op1.w >= 8u && offset <= Op1.w - 8u &&
        PackSize[record] <= Op1.w - 8u - offset)
    {
      uint ignored;
      PackState.InterlockedAdd(4u, 1u, ignored);
      PackOffset[record] = offset + 8u;
      PackValid[record]  = 1u;
      Aux[AuxBase.z + blockIndex * 2u]      = offset + 8u;
      Aux[AuxBase.z + blockIndex * 2u + 1u] = PackSize[record];
    }
    else
    {
      uint ignored;
      PackState.InterlockedOr(8u, 1u, ignored);
    }
  }
  GroupMemoryBarrierWithGroupSync();

  if (block == 0u && packetLive &&
      (PackBallot[record] == 0u || PackValid[record] != 0u))
  {
    const uint packetWords = PackBallot[record] != 0u ?
      PackSize[record] / 4u : 0u;
    uint       oldWords;
    uint       ignored;
    InterlockedExchange(
      QuantPayload[State.x + blockIndex + 1u], packetWords, oldWords);
    InterlockedAdd(QuantPayload[State.x], packetWords - oldWords, ignored);
  }
  GroupMemoryBarrierWithGroupSync();

  const uint ballot = PackBallot[record];
  const bool active = PackValid[record] != 0u &&
    (ballot & (1u << block)) != 0u;
  if (active)
  {
    const uint compact     = countbits(ballot & ((1u << block) - 1u));
    const uint controlWord = PackControl[lane];
    packControlOr(record, compact, controlWord);
    packByteOr(record, 8u + countbits(ballot) * 2u + compact,
      PackScale[lane]);
  }
  GroupMemoryBarrierWithGroupSync();

  if (active)
    for (uint subblock = 0u; subblock < 8u; ++subblock)
    {
      const uint outputQuality  = PackScale[lane] & 15u;
      const uint coded          =
        ((PackControl[lane] >> (subblock * 2u)) & 3u) + outputQuality;
      const uint payloadOffset  = quantPayloadSubblockOffset(
        meta.x, meta.y, subblock);
      uint       subblockOffset = 0u;
      for (uint previous = 0u; previous < subblock; ++previous)
        subblockOffset +=
          ((PackControl[lane] >> (previous * 2u)) & 3u) + outputQuality;
      for (uint plane = 0u; plane < coded; ++plane)
      {
        const uint byte = quantPayloadByte(payloadOffset, plane + 1u);
        packByteOr(record, PackPlaneBase[record] +
          PackMagnitudeOffset[lane] + subblockOffset + plane, byte);
      }
    }
  GroupMemoryBarrierWithGroupSync();

  if (active)
  {
    uint signBit   = 0u;
    uint localSign = 0u;
    for (uint group = 0u; group < block; ++group)
      if ((ballot & (1u << group)) != 0u)
        signBit += PackSignBits[record * 16u + group];
    for (uint subblock = 0u; subblock < 8u; ++subblock)
    {
      const uint payloadOffset = quantPayloadSubblockOffset(
        meta.x, meta.y, subblock);
      uint       significant   = PackSignificant[lane * 8u + subblock];
      uint       signMask      = 0u;
      if (significant != 0u)
        signMask = quantPayloadByte(payloadOffset, 0u);
      for (uint pixel = 0u; pixel < 8u; ++pixel)
      {
        if ((significant & (1u << pixel)) == 0u)
          continue;
        if ((signMask & (1u << pixel)) != 0u)
          packByteOr(record,
            PackSignBase[record] + (signBit + localSign) / 8u,
            1u << ((signBit + localSign) & 7u));
        ++localSign;
      }
    }
  }
  GroupMemoryBarrierWithGroupSync();

  if (PackValid[record] != 0u)
    for (uint word = block; word < PackSize[record] / 4u; word += 16u)
      Packed.Store(PackOffset[record] + word * 4u,
        PackWords[record * PacketWordsPerMax + word]);
}

[numthreads(1, 1, 1)]
void FinalizeMain()
{
  uint packetCount = PackState.Load(4u);
  if (packetCount == 0u && QuantPayload[State.x] == 0u)
  {
    QuantPayload[State.x]      = 3u;
    QuantPayload[State.x + 1u] = 3u;
    if (Work.x != 0u)
    {
      const uint packetWord0 = 1u | (3u << 16u) | (Op0.x << 28u);
      Packed.Store(8u, packetWord0);
      Packed.Store(12u, Op0.w);
      Packed.Store(16u, 6u << 20u);
      Aux[AuxBase.z]      = 8u;
      Aux[AuxBase.z + 1u] = 12u;
      PackState.Store(0u, 12u);
      PackState.Store(4u, 1u);
      packetCount = 1u;
    }
  }
  const uint hdr         = Op0.z != 0u ? 1u : 0u;
  const uint word0       = (Dim0.x - 1u) |
    ((Dim0.y - 1u) << 14u) | (Op0.x << 28u) | (1u << 31u);
  const uint word1       = packetCount | (Op0.y << 26u) |
    (hdr << 27u) | (hdr << 28u) | (hdr << 29u) | (1u << 30u);
  Packed.Store(0u, word0);
  Packed.Store(4u, word1);
  PackState.Store(0u, PackState.Load(0u) + 8u);
}
)hlsl";

  struct EncodedBlock
  {
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t index  = 0;
  };

  struct EncodedFrame
  {
    uint64_t                                    configEpoch   = 0;
    uint64_t                                    sourceSerial  = 0;
    uint64_t                                    frameID       = 0;
    uint64_t                                    captureTimeNs = 0;
    uint32_t                                    deadlineMs    = 0;
    uint32_t                                    blockCapacity = 0;
    uint32_t                                    hardLimit     = 0;
    std::vector<uint8_t>                        damagedBlocks;
    std::vector<uint8_t>                        data;
    std::vector<EncodedBlock>                   blocks;
    std::shared_ptr<const std::vector<uint8_t>> wholePayload;
  };

  struct EncodedPacket
  {
    const uint8_t * data   = nullptr;
    uint32_t        length = 0;
  };

  EncodedPacket Packet(const EncodedFrame& frame,
    const EncodedBlock& block)
  {
    const uint32_t prefix = block.offset ? 0U : 8U;
    if (block.length <= prefix || block.offset > frame.data.size() ||
        block.length > frame.data.size() - block.offset)
      return {};
    return
    {
      frame.data.data() + block.offset + prefix,
      block.length - prefix,
    };
  }

  bool SamePacket(const EncodedPacket& left, const EncodedPacket& right)
  {
    if (!left.data || !right.data || left.length != right.length ||
        left.length < 8U)
      return left.data == right.data && left.length == right.length;

    constexpr uint32_t SEQUENCE_BITS = PYROWAVE_SEQUENCE_MASK << 28U;
    if ((LoadLE32(left.data) & ~SEQUENCE_BITS) !=
          (LoadLE32(right.data) & ~SEQUENCE_BITS))
      return false;
    return std::equal(left.data + sizeof(uint32_t),
      left.data + left.length, right.data + sizeof(uint32_t));
  }

  struct ShaderParameters
  {
    uint32_t dim0   [4] = {};
    uint32_t op0    [4] = {};
    uint32_t op1    [4] = {};
    uint32_t auxBase[4] = {};
    uint32_t work   [4] = {};
    uint32_t state  [4] = {};
    float    values [4] = {};
  };

  static_assert(sizeof(ShaderParameters) == 28U * sizeof(uint32_t),
    "Unexpected shader parameter layout");
  constexpr UINT SHADER_PARAMETER_DWORDS = static_cast<UINT>(
    sizeof(ShaderParameters) / sizeof(uint32_t));

  class PyroWaveD3D12Encoder
  {
  private:
    enum Pipeline
    {
      PIPELINE_CLEAR,
      PIPELINE_COLOR,
      PIPELINE_DWT,
      PIPELINE_QUANT,
      PIPELINE_ANALYZE,
      PIPELINE_RDO_FINALIZE,
      PIPELINE_RESOLVE,
      PIPELINE_PACK,
      PIPELINE_FINALIZE,
      PIPELINE_COUNT,
    };

    struct Plane
    {
      uint32_t width                          = 0;
      uint32_t height                         = 0;
      uint32_t levels                         = 0;
      uint32_t offset                         = 0;
      uint32_t lowpassOffset[PYROWAVE_LEVELS] = {};
    };

    struct PlaneRect
    {
      uint32_t left   = 0;
      uint32_t top    = 0;
      uint32_t right  = 0;
      uint32_t bottom = 0;
    };

    struct GroupRange
    {
      uint32_t begin = 0;
      uint32_t end   = 0;
    };

    struct RowRun
    {
      uint32_t x      = 0;
      uint32_t y      = 0;
      uint32_t width  = 0;
      uint32_t height = 0;
    };

    struct PackRun
    {
      uint32_t x          = 0;
      uint32_t y          = 0;
      uint32_t width      = 0;
      uint32_t height     = 0;
      uint32_t activeMask = 0;
    };

    struct BandWork
    {
      uint32_t             blockBase  = 0;
      uint32_t             block8Base = 0;
      uint32_t             blocksX    = 0;
      uint32_t             blocksY    = 0;
      std::vector<RowRun>  blockRuns;
      std::vector<PackRun> packRuns;
    };

    struct FrameWork
    {
      std::vector<uint8_t> damagedBlocks;
      std::vector<RowRun>  colorRuns;
      std::vector<RowRun>  dwtRuns[3][PYROWAVE_LEVELS];
      std::vector<BandWork> bands;
      bool                  full = false;
    };

    struct Slot
    {
      ComPtr<ID3D12DescriptorHeap> descriptors;
      ComPtr<ID3D12DescriptorHeap> clearDescriptors;
      ComPtr<ID3D12Resource>       packed;
      ComPtr<ID3D12Resource>       packState;
      ComPtr<ID3D12Resource>       aux;
      ComPtr<ID3D12Resource>       readback;
      D12Sync                      computeSync;
      D12Sync                      sync;
      TexLease                     lease;
      FrameIn                      frame;
      FrameWork                    work;
      const uint8_t              * mapped      = nullptr;
      uint64_t                     captureTime = 0;
      uint64_t                     order       = 0;
      uint32_t                     sequence    = 0;
      uint32_t                     frameLimit  = 0;
      uint32_t                     hardLimit   = 0;
      bool                         inFlight    = false;
      bool                         used        = false;
    };

    ComPtr<ID3D12Device3>       m_device;
    CD3D12CommandQueue          m_computeQueue;
    CD3D12CommandQueue          m_copyQueue;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelines[PIPELINE_COUNT];
    ComPtr<ID3D12Resource>      m_wave;
    ComPtr<ID3D12Resource>      m_scratch;
    ComPtr<ID3D12Resource>      m_atlas;
    ComPtr<ID3D12Resource>      m_planeData;
    ComPtr<ID3D12Resource>      m_quantPayload;
    Slot                        m_slots    [ENCODER_SLOT_COUNT];
    FrameCfg                    m_cfg;
    Plane                       m_planes   [3]    = {};
    LGNetVideoChromaSubsampling m_chroma          = 0;
    uint64_t                    m_packedOffset    = 0;
    uint64_t                    m_readbackBytes   = 0;
    uint64_t                    m_submitOrder     = 0;
    uint32_t                    m_blockCapacity   = 0;
    uint32_t                    m_block8Capacity  = 0;
    uint32_t                    m_blocksPerRDO    = 0;
    uint32_t                    m_packedCapacity  = 0;
    uint32_t                    m_auxBase  [4]    = {};
    uint32_t                    m_auxWords        = 0;
    uint32_t                    m_payloadWords    = 0;
    uint32_t                    m_sequence        = 0;
    uint32_t                    m_hardLimit       = 0;
    bool                        m_persistentValid = false;
    bool                        m_ready           = false;
    HANDLE                      m_wakeEvent       = nullptr;

    static void CompletionFunction(CD3D12CommandSlot * slot,
      bool result, void * param1, void * param2)
    {
      (void)slot;
      (void)result;
      (void)param2;
      if (param1)
        SetEvent(static_cast<HANDLE>(param1));
    }

    static bool CreateBuffer(ID3D12Device3 * device, uint64_t bytes,
      D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state,
      D3D12_RESOURCE_FLAGS flags, ComPtr<ID3D12Resource>& resource)
    {
      if (!device || !bytes)
        return false;

      D3D12_HEAP_PROPERTIES heap = {};

      heap.Type                 = heapType;
      heap.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      heap.CreationNodeMask     = 1;
      heap.VisibleNodeMask      = 1;

      D3D12_RESOURCE_DESC desc = {};

      desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
      desc.Width              = bytes;
      desc.Height             = 1;
      desc.DepthOrArraySize   = 1;
      desc.MipLevels          = 1;
      desc.SampleDesc.Count   = 1;
      desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      desc.Flags              = flags;

      const HRESULT hr = device->CreateCommittedResource(&heap,
        D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc, state, nullptr,
        IID_PPV_ARGS(&resource));
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr, "Failed to create network video buffer");
        return false;
      }
      return true;
    }

    static bool Compile(const char * entry, ComPtr<ID3DBlob>& shader)
    {
      ComPtr<ID3DBlob> errors;
      const HRESULT    hr = D3DCompile(ENCODER_SHADER,
        sizeof(ENCODER_SHADER) - 1U, "NetworkPyroWave", nullptr, nullptr,
        entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr, "Failed to compile the network video shader");
        if (errors)
          DEBUG_ERROR("%s",
            static_cast<const char *>(errors->GetBufferPointer()));
        return false;
      }
      return true;
    }

    bool CreatePipelines()
    {
      D3D12_DESCRIPTOR_RANGE ranges[2] = {};

      ranges[0].RangeType                         =
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors                    = 1;
      ranges[0].BaseShaderRegister                = 0;
      ranges[0].OffsetInDescriptorsFromTableStart = 0;
      ranges[1].RangeType                         =
        D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      ranges[1].NumDescriptors                    = 8;
      ranges[1].BaseShaderRegister                = 0;
      ranges[1].OffsetInDescriptorsFromTableStart = 1;

      D3D12_ROOT_PARAMETER parameters[2] = {};

      parameters[0].ParameterType                       =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[0].ShaderVisibility                    =
        D3D12_SHADER_VISIBILITY_ALL;
      parameters[0].DescriptorTable.NumDescriptorRanges = 2;
      parameters[0].DescriptorTable.pDescriptorRanges   = ranges;
      parameters[1].ParameterType                       =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      parameters[1].ShaderVisibility                    =
        D3D12_SHADER_VISIBILITY_ALL;
      parameters[1].Constants.ShaderRegister            = 0;
      parameters[1].Constants.RegisterSpace             = 0;
      parameters[1].Constants.Num32BitValues            =
        SHADER_PARAMETER_DWORDS;

      D3D12_ROOT_SIGNATURE_DESC signature = {};

      signature.NumParameters = 2;
      signature.pParameters   = parameters;
      signature.Flags         = D3D12_ROOT_SIGNATURE_FLAG_NONE;

      ComPtr<ID3DBlob> blob;
      ComPtr<ID3DBlob> errors;
      HRESULT          hr = D3D12SerializeRootSignature(&signature,
        D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr,
          "Failed to serialize the network video root signature");
        if (errors)
          DEBUG_ERROR("%s",
            static_cast<const char *>(errors->GetBufferPointer()));
        return false;
      }

      hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(),
        blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr,
          "Failed to create the network video root signature");
        return false;
      }

      static const char * ENTRIES[PIPELINE_COUNT] =
      {
        "ClearMain",
        "ColorMain",
        "DWTMain",
        "QuantMain",
        "AnalyzeMain",
        "FinalizeRDOMain",
        "ResolveMain",
        "PackMain",
        "FinalizeMain",
      };
      for (unsigned i = 0; i < PIPELINE_COUNT; ++i)
      {
        ComPtr<ID3DBlob> shader;
        if (!Compile(ENTRIES[i], shader))
          return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};

        desc.pRootSignature     = m_rootSignature.Get();
        desc.CS.pShaderBytecode = shader->GetBufferPointer();
        desc.CS.BytecodeLength  = shader->GetBufferSize();
        hr = m_device->CreateComputePipelineState(
          &desc, IID_PPV_ARGS(&m_pipelines[i]));
        if (FAILED(hr))
        {
          DEBUG_ERROR_HR(hr,
            "Failed to create a network video compute pipeline");
          return false;
        }
      }

      return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Descriptor(
      const Slot& slot, unsigned index) const
    {
      D3D12_CPU_DESCRIPTOR_HANDLE result =
        slot.descriptors->GetCPUDescriptorHandleForHeapStart();
      result.ptr += static_cast<SIZE_T>(index) *
        m_device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return result;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GPUDescriptor(
      const Slot& slot, unsigned index) const
    {
      D3D12_GPU_DESCRIPTOR_HANDLE result =
        slot.descriptors->GetGPUDescriptorHandleForHeapStart();
      result.ptr += static_cast<UINT64>(index) *
        m_device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return result;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE ClearDescriptor(
      const Slot& slot, unsigned index) const
    {
      D3D12_CPU_DESCRIPTOR_HANDLE result =
        slot.clearDescriptors->GetCPUDescriptorHandleForHeapStart();
      result.ptr += static_cast<SIZE_T>(index) *
        m_device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return result;
    }

    void CreateSourceDescriptor(Slot& slot, ID3D12Resource * source)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};

      srv.Format                  = ProfileFormat(m_cfg.profile.pixel);
      srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels     = 1;
      m_device->CreateShaderResourceView(source, &srv, Descriptor(slot, 0));
    }

    bool CreateStaticDescriptors(Slot& slot)
    {
      ID3D12Resource * resources[] =
      {
        m_wave.Get(),
        m_scratch.Get(),
        m_atlas.Get(),
        m_planeData.Get(),
        slot.packed.Get(),
        slot.packState.Get(),
        slot.aux.Get(),
        m_quantPayload.Get(),
      };
      const uint64_t elements[] =
      {
        m_wave->GetDesc().Width / sizeof(float),
        m_scratch->GetDesc().Width / sizeof(float),
        m_atlas->GetDesc().Width / sizeof(float),
        m_planeData->GetDesc().Width / sizeof(float),
        slot.packed->GetDesc().Width / sizeof(uint32_t),
        slot.packState->GetDesc().Width / sizeof(uint32_t),
        slot.aux->GetDesc().Width / sizeof(uint32_t),
        m_quantPayload->GetDesc().Width / sizeof(uint32_t),
      };
      constexpr uint64_t MAX_ELEMENTS =
        1ULL << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
      for (unsigned i = 0; i < 8; ++i)
      {
        if (!elements[i] || elements[i] > MAX_ELEMENTS ||
            elements[i] > UINT_MAX)
          return false;

        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        const bool                       raw = i == 4 || i == 5;

        uav.Format                      = raw ?
          DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements          = static_cast<UINT>(elements[i]);
        uav.Buffer.StructureByteStride  = raw ? 0U :
          static_cast<UINT>(sizeof(uint32_t));
        uav.Buffer.Flags                = raw ?
          D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
        m_device->CreateUnorderedAccessView(
          resources[i], nullptr, &uav, Descriptor(slot, i + 1));
        if (i == 5)
          m_device->CreateUnorderedAccessView(
            resources[i], nullptr, &uav, ClearDescriptor(slot, 0));
      }
      return true;
    }

    static void UAVBarrier(ID3D12GraphicsCommandList * list,
      ID3D12Resource * resource)
    {
      D3D12_RESOURCE_BARRIER barrier = {};

      barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
      barrier.UAV.pResource = resource;
      list->ResourceBarrier(1, &barrier);
    }

    static void Transition(ID3D12GraphicsCommandList * list,
      ID3D12Resource * resource, D3D12_RESOURCE_STATES before,
      D3D12_RESOURCE_STATES after)
    {
      D3D12_RESOURCE_BARRIER barrier = {};

      barrier.Type                   =
        D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      barrier.Transition.pResource   = resource;
      barrier.Transition.StateBefore = before;
      barrier.Transition.StateAfter  = after;
      barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      list->ResourceBarrier(1, &barrier);
    }

    void Dispatch(ID3D12GraphicsCommandList * list, Pipeline pipeline,
      const ShaderParameters& parameters, uint32_t x, uint32_t y)
    {
      ShaderParameters values = parameters;
      std::copy(std::begin(m_auxBase), std::end(m_auxBase),
        std::begin(values.auxBase));
      values.state[0] = m_payloadWords;
      list->SetPipelineState(m_pipelines[pipeline].Get());
      list->SetComputeRoot32BitConstants(1,
        SHADER_PARAMETER_DWORDS, &values, 0);
      list->Dispatch(x, y, 1);
    }

    static bool CalculateBlockCapacity(const Plane (&planes)[3],
      LGNetVideoChromaSubsampling chroma, uint32_t& capacity,
      uint32_t& capacity8)
    {
      uint64_t blocks  = 0;
      uint64_t blocks8 = 0;
      for (int level = static_cast<int>(PYROWAVE_LEVELS) - 1;
           level >= 0; --level)
        for (unsigned component = 0; component < 3; ++component)
        {
          if (chroma == CHROMA_420 && component && !level)
            continue;
          const uint32_t localLevel =
            chroma == CHROMA_420 && component ?
              static_cast<uint32_t>(level - 1) :
              static_cast<uint32_t>(level);
          const uint32_t bandWidth  =
            planes[component].width >> (localLevel + 1U);
          const uint32_t bandHeight =
            planes[component].height >> (localLevel + 1U);
          const uint32_t bands      =
            level == static_cast<int>(PYROWAVE_LEVELS) - 1 ? 4U : 3U;
          const uint64_t blocksX    =
            (static_cast<uint64_t>(bandWidth) + 31U) / 32U;
          const uint64_t blocksY    =
            (static_cast<uint64_t>(bandHeight) + 31U) / 32U;
          const uint64_t blocks8X   =
            (static_cast<uint64_t>(bandWidth) + 7U) / 8U;
          const uint64_t blocks8Y   =
            (static_cast<uint64_t>(bandHeight) + 7U) / 8U;
          blocks  += blocksX * blocksY * bands;
          blocks8 += blocks8X * blocks8Y * bands;
        }
      if (!blocks || blocks > MAX_BLOCKS || !blocks8 ||
          blocks8 > PYROWAVE_MAX_8X8_BLOCKS)
        return false;
      capacity  = static_cast<uint32_t>(blocks);
      capacity8 = static_cast<uint32_t>(blocks8);
      return true;
    }

    bool CreateSlot(Slot& slot)
    {
      D3D12_DESCRIPTOR_HEAP_DESC heap = {};

      heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      heap.NumDescriptors = 9;
      heap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      HRESULT hr = m_device->CreateDescriptorHeap(
        &heap, IID_PPV_ARGS(&slot.descriptors));
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr,
          "Failed to create the network video descriptor heap");
        return false;
      }

      D3D12_DESCRIPTOR_HEAP_DESC clearHeap = {};

      clearHeap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      clearHeap.NumDescriptors = 1;
      clearHeap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
      hr = m_device->CreateDescriptorHeap(
        &clearHeap, IID_PPV_ARGS(&slot.clearDescriptors));
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr,
          "Failed to create the network video clear descriptor heap");
        return false;
      }

      if (!CreateBuffer(m_device.Get(), m_packedCapacity,
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.packed) ||
          !CreateBuffer(m_device.Get(), PYROWAVE_STATE_BYTES,
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.packState) ||
          !CreateBuffer(m_device.Get(),
            static_cast<uint64_t>(m_auxWords) * sizeof(uint32_t),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, slot.aux) ||
          !CreateBuffer(m_device.Get(), m_readbackBytes,
            D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, slot.readback))
        return false;

      void          * mapped = nullptr;
      const HRESULT   mapHr  = slot.readback->Map(0, nullptr, &mapped);
      if (FAILED(mapHr) || !mapped)
      {
        DEBUG_ERROR_HR(mapHr,
          "Failed to map the network video readback buffer");
        return false;
      }
      slot.mapped = static_cast<const uint8_t *>(mapped);
      return CreateStaticDescriptors(slot);
    }

    bool Configure(const FrameCfg& cfg, LGNetVideoChromaSubsampling chroma,
      ID3D12Resource * source)
    {
      if (!source || !SupportedProfile(cfg.profile) || !cfg.width ||
          !cfg.height || cfg.width > MAX_WIDTH || cfg.height > MAX_HEIGHT)
        return false;

      ComPtr<ID3D12Device3> device;
      HRESULT               hr = source->GetDevice(IID_PPV_ARGS(&device));
      if (FAILED(hr) || !device)
        return false;

      const uint32_t alignedWidth  = (std::max)(
        Align(cfg.width, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
      const uint32_t alignedHeight = (std::max)(
        Align(cfg.height, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
      if (alignedWidth > MAX_WIDTH || alignedHeight > MAX_HEIGHT)
        return false;

      Plane    planes[3]   = {};
      uint64_t planePixels = 0;
      for (unsigned i = 0; i < 3; ++i)
      {
        const bool subsampled = chroma == CHROMA_420 && i;
        planes[i].width       = subsampled ? alignedWidth / 2U : alignedWidth;
        planes[i].height      = subsampled ? alignedHeight / 2U : alignedHeight;
        planes[i].levels      = subsampled ?
          PYROWAVE_LEVELS - 1U : PYROWAVE_LEVELS;
        const uint64_t pixels =
          static_cast<uint64_t>(planes[i].width) * planes[i].height;
        if (planePixels > UINT32_MAX || pixels > UINT32_MAX - planePixels)
          return false;
        planes[i].offset = static_cast<uint32_t>(planePixels);
        // Sparse transforms retain untouched LL samples between frames, so
        // every level needs a disjoint row range in its parity buffer.
        uint32_t levelHeight    = planes[i].height;
        uint32_t lowpassRows[2] = {};
        for (uint32_t level = 0; level < planes[i].levels; ++level)
        {
          levelHeight >>= 1U;
          const unsigned buffer = level & 1U;
          if (lowpassRows[buffer] > planes[i].height ||
              levelHeight > planes[i].height - lowpassRows[buffer])
            return false;
          const uint64_t lowpassOffset = planePixels +
            static_cast<uint64_t>(lowpassRows[buffer]) * planes[i].width;
          if (lowpassOffset > UINT32_MAX)
            return false;
          planes[i].lowpassOffset[level] =
            static_cast<uint32_t>(lowpassOffset);
          lowpassRows[buffer] += levelHeight;
        }
        planePixels += pixels;
      }
      constexpr uint64_t MAX_ELEMENTS =
        1ULL << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;

      uint32_t blockCapacity  = 0;
      uint32_t block8Capacity = 0;
      if (!planePixels || planePixels > MAX_ELEMENTS ||
          !CalculateBlockCapacity(
            planes, chroma, blockCapacity, block8Capacity))
        return false;

      const uint64_t statsBase    = 0;
      const uint64_t metaBase     =
        statsBase + static_cast<uint64_t>(block8Capacity) *
          PYROWAVE_STAT_WORDS;
      const uint64_t rdoOpsBase   =
        metaBase + static_cast<uint64_t>(block8Capacity) * 2U;
      const uint64_t rdoStateBase =
        rdoOpsBase + static_cast<uint64_t>(blockCapacity) *
          (static_cast<uint64_t>(PYROWAVE_QUANT_LEVELS) - 1U);
      const uint64_t auxWords     = rdoStateBase +
        PYROWAVE_RDO_WORDS + blockCapacity;
      const uint64_t payloadWords =
        1U + static_cast<uint64_t>(block8Capacity) *
          PYROWAVE_QUANT_WORDS_8X8;
      const uint64_t payloadTotal =
        payloadWords + static_cast<uint64_t>(blockCapacity) + 1U;
      if (auxWords > MAX_ELEMENTS || payloadWords > UINT32_MAX ||
          payloadTotal > MAX_ELEMENTS ||
          rdoStateBase > UINT32_MAX)
        return false;

      const uint64_t worstPacked     =
        static_cast<uint64_t>(blockCapacity) * PYROWAVE_PACKET_BYTES + 8U;
      const uint64_t targetPacked    =
        static_cast<uint64_t>(alignedWidth) * alignedHeight * 2U + 8U;
      const uint64_t packedCapacity  = (std::min)(worstPacked,
        (std::min)(targetPacked,
          static_cast<uint64_t>(MAX_FRAME_LENGTH)));
      const uint64_t packetMetaBytes =
        static_cast<uint64_t>(blockCapacity) *
          PYROWAVE_PACKET_META_WORDS * sizeof(uint32_t);
      const uint64_t packedOffset    =
        PYROWAVE_STATE_BYTES + packetMetaBytes;
      const uint64_t readbackBytes   = packedOffset + packedCapacity;
      if (!packedCapacity || packedCapacity > UINT32_MAX)
        return false;

      Reset();
      m_device          = device;
      m_cfg             = cfg;
      m_chroma          = chroma;
      m_packedOffset    = packedOffset;
      m_readbackBytes   = readbackBytes;
      m_blockCapacity   = blockCapacity;
      m_block8Capacity  = block8Capacity;
      m_blocksPerRDO    = NextPowerOfTwo(
        (blockCapacity + PYROWAVE_RDO_DIVISIONS - 1U) /
        PYROWAVE_RDO_DIVISIONS);
      m_packedCapacity  = static_cast<uint32_t>(packedCapacity);
      m_auxBase[0]      = static_cast<uint32_t>(statsBase);
      m_auxBase[1]      = static_cast<uint32_t>(metaBase);
      m_auxBase[2]      = static_cast<uint32_t>(rdoOpsBase);
      m_auxBase[3]      = static_cast<uint32_t>(rdoStateBase);
      m_auxWords        = static_cast<uint32_t>(auxWords);
      m_payloadWords    = static_cast<uint32_t>(payloadWords);
      std::copy(std::begin(planes), std::end(planes), std::begin(m_planes));

      if (!m_computeQueue.Init(m_device.Get(),
            D3D12_COMMAND_LIST_TYPE_COMPUTE, L"Network Video Compute",
            CD3D12CommandSlot::FAST, ENCODER_SLOT_COUNT) ||
          !m_copyQueue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COPY,
            L"Network Video Copy", CD3D12CommandSlot::FAST,
            ENCODER_SLOT_COUNT) ||
          !CreatePipelines())
      {
        Reset();
        return false;
      }
      if (!CreateBuffer(m_device.Get(), planePixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_wave) ||
          !CreateBuffer(m_device.Get(), planePixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_scratch) ||
          !CreateBuffer(m_device.Get(), planePixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_atlas) ||
          !CreateBuffer(m_device.Get(), planePixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_planeData) ||
          !CreateBuffer(m_device.Get(),
            payloadTotal * sizeof(uint32_t),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_quantPayload))
      {
        Reset();
        return false;
      }
      for (Slot& slot : m_slots)
        if (!CreateSlot(slot))
        {
          Reset();
          return false;
        }

      m_ready = true;
      return true;
    }

    bool SameDevice(ID3D12Resource * source) const
    {
      if (!m_device || !source)
        return false;
      ComPtr<ID3D12Device3> device;
      return SUCCEEDED(source->GetDevice(IID_PPV_ARGS(&device))) &&
        device.Get() == m_device.Get();
    }

    static uint32_t QuantBits(
      int level, unsigned component, unsigned band)
    {
      const uint32_t bits = 8U + static_cast<uint32_t>(level) +
        (band == 0 ? 2U : band < 3 ? 1U : 0U) - (component ? 1U : 0U);
      return (std::min)(12U, bits);
    }

    float DistortionScale(
      int level, unsigned component, unsigned band) const
    {
      const float     horizontal  = band & 1U ? 0.75f : 0.25f;
      const float     vertical    = band & 2U ? 0.75f : 0.25f;
      constexpr float DPI         = 96.0f;
      constexpr float CPD         = 0.34f * DPI;
      float           cycles      =
        std::sqrt(horizontal * horizontal + vertical * vertical) *
        CPD * std::exp2(-static_cast<float>(level));
      cycles = (std::max)(cycles, 8.0f);
      float           sensitivity =
        2.6f * (0.0192f + 0.114f * cycles) *
        std::exp(-std::pow(0.114f * cycles, 1.1f));
      if (component && level != static_cast<int>(PYROWAVE_LEVELS) - 1 &&
          m_chroma == CHROMA_420)
        sensitivity *= 0.6f;
      const uint32_t normalizedBits = 8U + static_cast<uint32_t>(level) +
        (band == 0 ? 2U : band < 3 ? 1U : 0U) - (component ? 1U : 0U);
      const float    resolution     =
        static_cast<float>(1U << normalizedBits);
      const float    weighted       = sensitivity * resolution;
      return weighted * weighted / 256.0f;
    }

    bool RecordBands(ID3D12GraphicsCommandList * list, Pipeline pipeline,
      const FrameWork& work, uint32_t sequence, uint32_t packedLimit)
    {
      uint32_t blockBase  = 0;
      uint32_t blockBase8 = 0;
      size_t   bandIndex  = 0;
      for (int level = static_cast<int>(PYROWAVE_LEVELS) - 1;
           level >= 0; --level)
        for (unsigned component = 0; component < 3; ++component)
        {
          if (m_chroma == CHROMA_420 && component && !level)
            continue;
          const uint32_t localLevel =
            m_chroma == CHROMA_420 && component ?
              static_cast<uint32_t>(level - 1) :
              static_cast<uint32_t>(level);
          const uint32_t bandWidth  =
            m_planes[component].width >> (localLevel + 1U);
          const uint32_t bandHeight =
            m_planes[component].height >> (localLevel + 1U);
          const uint32_t blocksX    = (bandWidth + 31U) / 32U;
          const uint32_t blocksY    = (bandHeight + 31U) / 32U;
          const uint32_t blocks8X   = (bandWidth + 7U) / 8U;
          const uint32_t blocks8Y   = (bandHeight + 7U) / 8U;
          const unsigned firstBand  =
            level == static_cast<int>(PYROWAVE_LEVELS) - 1 ? 0 : 1;
          for (unsigned band = firstBand; band < 4; ++band)
          {
            if (bandIndex >= work.bands.size())
              return false;
            const BandWork& bandWork  = work.bands[bandIndex++];
            const uint32_t  quantBits =
              QuantBits(level, component, band);
            if (bandWork.blockBase != blockBase ||
                bandWork.block8Base != blockBase8 ||
                bandWork.blocksX != blocksX || bandWork.blocksY != blocksY)
              return false;
            ShaderParameters parameters = {};
            parameters.dim0[0]   = m_planes[component].width;
            parameters.dim0[1]   = m_planes[component].offset;
            parameters.dim0[2]   = bandWidth;
            parameters.dim0[3]   = bandHeight;
            parameters.op0[0]    = band & 1U ? bandWidth : 0;
            parameters.op0[1]    = band & 2U ? bandHeight : 0;
            parameters.op0[2]    = blocks8X;
            parameters.op0[3]    = blocks8Y;
            parameters.op1[0]    = blocksX;
            parameters.op1[1]    = blockBase;
            parameters.op1[2]    = blockBase8;
            parameters.op1[3]    = pipeline == PIPELINE_QUANT ?
              m_payloadWords : packedLimit;
            parameters.values[0] = static_cast<float>(1U << quantBits);
            parameters.values[1] = DistortionScale(
              level, component, band);
            parameters.values[2] = static_cast<float>(
              pipeline == PIPELINE_PACK ? sequence : m_blockCapacity);
            parameters.values[3] = static_cast<float>(
              pipeline == PIPELINE_PACK ? 8U * (4U + quantBits) :
                m_blocksPerRDO);

            if (pipeline == PIPELINE_QUANT || pipeline == PIPELINE_ANALYZE)
            {
              if (work.full)
                Dispatch(list, pipeline, parameters, blocksX, blocksY);
              else
                for (const RowRun& run : bandWork.blockRuns)
                {
                  parameters.work[0] = run.x;
                  parameters.work[1] = run.y;
                  Dispatch(list, pipeline, parameters,
                    run.width, run.height);
                }
            }
            else if (pipeline == PIPELINE_PACK)
            {
              if (work.full)
              {
                parameters.work[2] = 0xfU;
                Dispatch(list, pipeline, parameters,
                  (blocksX + 1U) / 2U, (blocksY + 1U) / 2U);
              }
              else
                for (const PackRun& run : bandWork.packRuns)
                {
                  parameters.work[0] = run.x;
                  parameters.work[1] = run.y;
                  parameters.work[2] = run.activeMask;
                  Dispatch(list, pipeline, parameters,
                    run.width, run.height);
                }
            }
            else
              return false;

            blockBase  += blocksX * blocksY;
            blockBase8 += blocks8X * blocks8Y;
          }
        }
      return bandIndex == work.bands.size() &&
        blockBase == m_blockCapacity &&
        blockBase8 == m_block8Capacity;
    }

    void RecordTransforms(ID3D12GraphicsCommandList * list,
      const FrameWork& work)
    {
      uint32_t activeWidth [3] = {};
      uint32_t activeHeight[3] = {};
      for (unsigned component = 0; component < 3; ++component)
      {
        activeWidth [component] = m_planes[component].width;
        activeHeight[component] = m_planes[component].height;
      }

      for (unsigned level = 0; level < PYROWAVE_LEVELS; ++level)
      {
        for (unsigned component = 0; component < 3; ++component)
        {
          const Plane& plane = m_planes[component];
          if (level >= plane.levels)
            continue;

          ShaderParameters transform = {};
          transform.dim0[2] = plane.width;
          transform.dim0[3] = plane.height;
          transform.op0[0]  = activeWidth [component];
          transform.op0[1]  = activeHeight[component];
          transform.op1[0]  = level;
          transform.op1[1]  = level ?
            plane.lowpassOffset[level - 1U] : 0U;
          transform.op1[2]  = plane.lowpassOffset[level];
          transform.op1[3]  = plane.offset;
          transform.work[2] = level + 1U == plane.levels ? 1U : 0U;
          if (work.full)
            Dispatch(list, PIPELINE_DWT, transform,
              (activeWidth[component] + 31U) / 32U,
              (activeHeight[component] + 31U) / 32U);
          else
            for (const RowRun& run : work.dwtRuns[component][level])
            {
              transform.work[0] = run.x;
              transform.work[1] = run.y;
              Dispatch(list, PIPELINE_DWT, transform,
                run.width, run.height);
            }
          activeWidth [component] >>= 1;
          activeHeight[component] >>= 1;
        }
        UAVBarrier(list, nullptr);
      }
    }

    void PrepareReadback(ID3D12GraphicsCommandList * list, Slot& slot)
    {
      UAVBarrier(list, slot.packed.Get());
      UAVBarrier(list, slot.packState.Get());
      UAVBarrier(list, slot.aux.Get());
      ID3D12Resource * resources[] =
      {
        slot.packed.Get(),
        slot.packState.Get(),
        slot.aux.Get(),
      };
      for (ID3D12Resource * resource : resources)
        Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COMMON);
    }

    void RecordReadback(
      ID3D12GraphicsCommandList * list, Slot& slot) const
    {
      list->CopyBufferRegion(slot.readback.Get(), 0,
        slot.packState.Get(), 0, PYROWAVE_STATE_BYTES);
      list->CopyBufferRegion(slot.readback.Get(), PYROWAVE_STATE_BYTES,
        slot.aux.Get(),
        static_cast<uint64_t>(m_auxBase[2]) * sizeof(uint32_t),
        static_cast<uint64_t>(m_blockCapacity) *
          PYROWAVE_PACKET_META_WORDS * sizeof(uint32_t));
      list->CopyBufferRegion(slot.readback.Get(), m_packedOffset,
        slot.packed.Get(), 0, slot.frameLimit);
    }

    static GroupRange AffectedGroups(
      uint32_t begin, uint32_t end, uint32_t length,
      uint32_t groupCount)
    {
      GroupRange result;
      begin = (std::min)(begin, length);
      end   = (std::min)(end, length);
      if (begin >= end || !length || !groupCount)
        return result;
      result.begin = begin < 36U ? 0U : (begin - 36U) / 32U + 1U;
      result.end   = (std::min)(groupCount, (end + 35U) / 32U);
      const uint32_t mirroredEdge = length > 36U ? length - 36U : 0U;
      if (end > mirroredEdge)
      {
        result.begin = (std::min)(result.begin, groupCount - 1U);
        result.end   = groupCount;
      }
      result.begin = (std::min)(result.begin, result.end);
      return result;
    }

    static bool BuildRowRuns(const std::vector<uint8_t>& mask,
      size_t base, uint32_t width, uint32_t height,
      std::vector<RowRun>& runs)
    {
      const size_t count = static_cast<size_t>(width) * height;
      if (base > mask.size() || count > mask.size() - base)
        return false;

      runs.clear();
      std::vector<size_t> previous;
      std::vector<size_t> current;
      for (uint32_t y = 0; y < height; ++y)
      {
        current.clear();
        size_t   previousPosition = 0;
        uint32_t x                = 0;
        while (x < width)
        {
          while (x < width && !mask[base + y * width + x])
            ++x;
          const uint32_t begin = x;
          while (x < width && mask[base + y * width + x])
            ++x;
          if (begin != x)
          {
            const uint32_t runWidth = x - begin;
            while (previousPosition < previous.size() &&
                runs[previous[previousPosition]].x < begin)
              ++previousPosition;
            if (previousPosition < previous.size())
            {
              RowRun& run = runs[previous[previousPosition]];
              if (run.x == begin && run.width == runWidth &&
                  run.y + run.height == y)
              {
                ++run.height;
                current.push_back(previous[previousPosition]);
                ++previousPosition;
                continue;
              }
            }
            runs.push_back({ begin, y, runWidth, 1U });
            current.push_back(runs.size() - 1U);
          }
        }
        previous.swap(current);
      }
      return true;
    }

    static void MarkGroups(std::vector<uint8_t>& mask,
      uint32_t width, const GroupRange& x, const GroupRange& y)
    {
      for (uint32_t row = y.begin; row < y.end; ++row)
        std::fill(mask.begin() + static_cast<size_t>(row) * width + x.begin,
          mask.begin() + static_cast<size_t>(row) * width + x.end,
          static_cast<uint8_t>(1U));
    }

    bool BuildFrameWork(const FrameDesc& frame, bool forceFull,
      FrameWork& work) const
    {
      work      = {};
      work.full = forceFull || frame.damage == FrameDamage::FULL;
      work.damagedBlocks.assign(
        m_blockCapacity, static_cast<uint8_t>(work.full));
      if (!work.full && frame.damage != FrameDamage::NONE &&
          (frame.damage != FrameDamage::RECTS || !frame.count ||
           frame.count > FRAME_DAMAGE_MAX))
        return false;

      constexpr size_t INVALID_BAND =
        (std::numeric_limits<size_t>::max)();
      size_t           bandIndices[PYROWAVE_LEVELS][3][4];
      uint32_t         blockBase    = 0;
      uint32_t         blockBase8   = 0;
      for (auto& levelBands : bandIndices)
        for (auto& componentBands : levelBands)
          std::fill(std::begin(componentBands), std::end(componentBands),
            INVALID_BAND);
      for (int level = static_cast<int>(PYROWAVE_LEVELS) - 1;
           level >= 0; --level)
        for (unsigned component = 0; component < 3; ++component)
        {
          if (m_chroma == CHROMA_420 && component && !level)
            continue;
          const uint32_t localLevel =
            m_chroma == CHROMA_420 && component ?
              static_cast<uint32_t>(level - 1) :
              static_cast<uint32_t>(level);
          const uint32_t bandWidth  =
            m_planes[component].width >> (localLevel + 1U);
          const uint32_t bandHeight =
            m_planes[component].height >> (localLevel + 1U);
          const uint32_t blocksX    = (bandWidth + 31U) / 32U;
          const uint32_t blocksY    = (bandHeight + 31U) / 32U;
          const uint32_t blocks8X   = (bandWidth + 7U) / 8U;
          const uint32_t blocks8Y   = (bandHeight + 7U) / 8U;
          const unsigned firstBand  =
            level == static_cast<int>(PYROWAVE_LEVELS) - 1 ? 0U : 1U;
          for (unsigned band = firstBand; band < 4; ++band)
          {
            bandIndices[level][component][band] = work.bands.size();
            work.bands.push_back(
            {
              blockBase,
              blockBase8,
              blocksX,
              blocksY,
              {},
              {},
            });
            blockBase  += blocksX * blocksY;
            blockBase8 += blocks8X * blocks8Y;
          }
        }
      if (blockBase != m_blockCapacity ||
          blockBase8 != m_block8Capacity)
        return false;

      if (!work.full && frame.damage == FrameDamage::RECTS)
      {
        for (unsigned index = 0; index < frame.count; ++index)
        {
          const RECT& rect = frame.rects[index];
          if (rect.left < 0 || rect.top < 0 ||
              rect.right <= rect.left || rect.bottom <= rect.top ||
              rect.right > static_cast<LONG>(m_cfg.width) ||
              rect.bottom > static_cast<LONG>(m_cfg.height))
          {
            work.full = true;
            std::fill(work.damagedBlocks.begin(),
              work.damagedBlocks.end(), static_cast<uint8_t>(1U));
            return true;
          }
        }

        const uint32_t colorDivisor = m_chroma == CHROMA_420 ? 2U : 1U;
        const uint32_t colorWidth   = m_planes[0].width / colorDivisor;
        const uint32_t colorHeight  = m_planes[0].height / colorDivisor;
        const uint32_t colorGroupsX = (colorWidth + 7U) / 8U;
        const uint32_t colorGroupsY = (colorHeight + 7U) / 8U;
        std::vector<uint8_t> colorMask(
          static_cast<size_t>(colorGroupsX) * colorGroupsY);
        for (unsigned index = 0; index < frame.count; ++index)
        {
          const RECT& source = frame.rects[index];
          uint32_t    left   =
            static_cast<uint32_t>(source.left) / colorDivisor;
          uint32_t    top    =
            static_cast<uint32_t>(source.top) / colorDivisor;
          uint32_t    right  =
            (static_cast<uint32_t>(source.right) + colorDivisor - 1U) /
              colorDivisor;
          uint32_t    bottom =
            (static_cast<uint32_t>(source.bottom) + colorDivisor - 1U) /
              colorDivisor;
          if (source.right == static_cast<LONG>(m_cfg.width))
            right = colorWidth;
          if (source.bottom == static_cast<LONG>(m_cfg.height))
            bottom = colorHeight;
          const GroupRange x =
          {
            left / 8U,
            (std::min)(colorGroupsX, (right + 7U) / 8U),
          };
          const GroupRange y =
          {
            top / 8U,
            (std::min)(colorGroupsY, (bottom + 7U) / 8U),
          };
          MarkGroups(colorMask, colorGroupsX, x, y);
        }
        if (!BuildRowRuns(colorMask, 0, colorGroupsX, colorGroupsY,
              work.colorRuns))
          return false;
        bool completeWork = std::all_of(colorMask.begin(), colorMask.end(),
          [](uint8_t active) { return active != 0; });

        std::vector<uint8_t> dwtMasks[3][PYROWAVE_LEVELS];
        for (unsigned component = 0; component < 3; ++component)
        {
          const Plane&   plane        = m_planes[component];
          const bool     subsampled   =
            m_chroma == CHROMA_420 && component;
          const uint32_t divisor      = subsampled ? 2U : 1U;
          const uint32_t sourceWidth  =
            (m_cfg.width + divisor - 1U) / divisor;
          const uint32_t sourceHeight =
            (m_cfg.height + divisor - 1U) / divisor;

          std::vector<PlaneRect> currentRects;
          currentRects.reserve(frame.count);
          for (unsigned index = 0; index < frame.count; ++index)
          {
            const RECT& source = frame.rects[index];
            PlaneRect   rect;
            rect.left   = static_cast<uint32_t>(source.left) / divisor;
            rect.top    = static_cast<uint32_t>(source.top) / divisor;
            rect.right  =
              (static_cast<uint32_t>(source.right) + divisor - 1U) /
                divisor;
            rect.bottom =
              (static_cast<uint32_t>(source.bottom) + divisor - 1U) /
                divisor;
            if (rect.right == sourceWidth)
              rect.right = plane.width;
            if (rect.bottom == sourceHeight)
              rect.bottom = plane.height;
            currentRects.push_back(rect);
          }

          uint32_t activeWidth  = plane.width;
          uint32_t activeHeight = plane.height;
          for (uint32_t localLevel = 0; localLevel < plane.levels;
               ++localLevel)
          {
            const uint32_t globalLevel = localLevel +
              (subsampled ? 1U : 0U);
            const uint32_t bandWidth   = activeWidth / 2U;
            const uint32_t bandHeight  = activeHeight / 2U;
            const uint32_t groupCountX = (activeWidth + 31U) / 32U;
            const uint32_t groupCountY = (activeHeight + 31U) / 32U;
            const uint32_t blocksX     = (bandWidth + 31U) / 32U;
            const uint32_t blocksY     = (bandHeight + 31U) / 32U;
            const unsigned firstBand   =
              localLevel + 1U == plane.levels ? 0U : 1U;
            std::vector<uint8_t>& dwtMask =
              dwtMasks[component][localLevel];
            dwtMask.assign(
              static_cast<size_t>(groupCountX) * groupCountY,
              static_cast<uint8_t>(0U));

            std::vector<PlaneRect> nextRects;
            nextRects.reserve(currentRects.size());
            for (const PlaneRect& rect : currentRects)
            {
              const GroupRange x = AffectedGroups(
                rect.left, rect.right, activeWidth, groupCountX);
              const GroupRange y = AffectedGroups(
                rect.top, rect.bottom, activeHeight, groupCountY);
              if (x.begin == x.end || y.begin == y.end)
                continue;
              MarkGroups(dwtMask, groupCountX, x, y);

              const uint32_t blockBeginX = x.begin / 2U;
              const uint32_t blockBeginY = y.begin / 2U;
              const uint32_t blockEndX   = (x.end + 1U) / 2U;
              const uint32_t blockEndY   = (y.end + 1U) / 2U;
              for (unsigned band = firstBand; band < 4; ++band)
              {
                const size_t    bandIndex =
                  bandIndices[globalLevel][component][band];
                if (bandIndex == INVALID_BAND ||
                    bandIndex >= work.bands.size())
                  return false;
                const BandWork& bandWork = work.bands[bandIndex];
                for (uint32_t blockY = blockBeginY;
                     blockY < blockEndY; ++blockY)
                  for (uint32_t blockX = blockBeginX;
                       blockX < blockEndX; ++blockX)
                  {
                    if (blockX >= blocksX || blockY >= blocksY)
                      return false;
                    work.damagedBlocks[bandWork.blockBase +
                      blockY * blocksX + blockX] =
                        static_cast<uint8_t>(1U);
                  }
              }

              nextRects.push_back(
              {
                x.begin * 16U,
                y.begin * 16U,
                (std::min)(bandWidth, x.end * 16U),
                (std::min)(bandHeight, y.end * 16U),
              });
            }
            if (!BuildRowRuns(dwtMask, 0, groupCountX, groupCountY,
                  work.dwtRuns[component][localLevel]))
              return false;
            completeWork = completeWork &&
              std::all_of(dwtMask.begin(), dwtMask.end(),
                [](uint8_t active) { return active != 0; });
            currentRects = std::move(nextRects);
            activeWidth  = bandWidth;
            activeHeight = bandHeight;
          }
        }
        if (completeWork &&
            std::all_of(work.damagedBlocks.begin(),
              work.damagedBlocks.end(),
              [](uint8_t damaged) { return damaged != 0; }))
        {
          work.full = true;
          return true;
        }
      }

      for (BandWork& band : work.bands)
      {
        if (work.full)
          continue;
        if (!BuildRowRuns(work.damagedBlocks, band.blockBase,
              band.blocksX, band.blocksY, band.blockRuns))
          return false;

        const uint32_t packWidth  = (band.blocksX + 1U) / 2U;
        const uint32_t packHeight = (band.blocksY + 1U) / 2U;
        std::vector<size_t> previous;
        std::vector<size_t> current;
        for (uint32_t y = 0; y < packHeight; ++y)
        {
          current.clear();
          size_t   previousPosition = 0;
          uint32_t x                = 0;
          while (x < packWidth)
          {
            const auto activeMask = [&](uint32_t groupX)
            {
              uint32_t result = 0;
              for (uint32_t record = 0; record < 4U; ++record)
              {
                const uint32_t blockX = groupX * 2U + (record & 1U);
                const uint32_t blockY = y * 2U + (record >> 1U);
                if (blockX < band.blocksX && blockY < band.blocksY &&
                    work.damagedBlocks[band.blockBase +
                      blockY * band.blocksX + blockX])
                  result |= 1U << record;
              }
              return result;
            };

            const uint32_t mask = activeMask(x);
            if (!mask)
            {
              ++x;
              continue;
            }
            const uint32_t begin = x++;
            while (x < packWidth && activeMask(x) == mask)
              ++x;
            const uint32_t runWidth = x - begin;
            while (previousPosition < previous.size() &&
                band.packRuns[previous[previousPosition]].x < begin)
              ++previousPosition;
            if (previousPosition < previous.size())
            {
              PackRun& run = band.packRuns[previous[previousPosition]];
              if (run.x == begin && run.width == runWidth &&
                  run.activeMask == mask && run.y + run.height == y)
              {
                ++run.height;
                current.push_back(previous[previousPosition]);
                ++previousPosition;
                continue;
              }
            }
            band.packRuns.push_back(
              { begin, y, runWidth, 1U, mask });
            current.push_back(band.packRuns.size() - 1U);
          }
          previous.swap(current);
        }
      }
      return true;
    }

    bool Compact(Slot& slot, const uint8_t * readback,
      EncodedFrame& output) const
    {
      const uint32_t packedBytes = LoadLE32(readback);
      const uint32_t packetCount = LoadLE32(readback + 4U);
      const uint32_t overflow    = LoadLE32(readback + 8U);
      if (overflow || packetCount > m_blockCapacity ||
          packedBytes < 8U || packedBytes > m_packedCapacity ||
          packedBytes > slot.frameLimit ||
          packedBytes > MAX_FRAME_LENGTH)
        return false;

      const uint8_t * packetMeta    = readback + PYROWAVE_STATE_BYTES;
      const uint8_t * packed        = readback + m_packedOffset;
      const bool      hdr           =
        m_cfg.profile.signal == FrameSignal::PQ_BT2020;
      const uint32_t  expectedWord0 = (m_cfg.width - 1U) |
        ((m_cfg.height - 1U) << 14U) | (slot.sequence << 28U) |
        (1U << 31U);
      const uint32_t  expectedWord1 = packetCount |
        ((m_chroma == CHROMA_444 ? 1U : 0U) << 26U) |
        (static_cast<uint32_t>(hdr) << 27U) |
        (static_cast<uint32_t>(hdr) << 28U) |
        (static_cast<uint32_t>(hdr) << 29U) | (1U << 30U);
      if (LoadLE32(packed) != expectedWord0 ||
          LoadLE32(packed + 4U) != expectedWord1)
        return false;

      try
      {
        output.blocks.clear();
        output.blocks.reserve(packetCount);
        output.data.resize(packedBytes);
        std::copy_n(packed, 8U, output.data.begin());

        uint32_t outputOffset = 8U;
        for (uint32_t blockIndex = 0;
             blockIndex < m_blockCapacity; ++blockIndex)
        {
          if (blockIndex >= slot.work.damagedBlocks.size())
            return false;
          const uint8_t * meta = packetMeta +
            static_cast<size_t>(blockIndex) *
              PYROWAVE_PACKET_META_WORDS * sizeof(uint32_t);
          const uint32_t sourceOffset = LoadLE32(meta);
          const uint32_t length       = LoadLE32(meta + sizeof(uint32_t));
          const bool     damaged      = slot.work.damagedBlocks[blockIndex];
          const bool     dummy        = !damaged && blockIndex == 0U &&
            packetCount == 1U && packedBytes >= 20U &&
            sourceOffset == 8U && length == 12U &&
            LoadLE32(packed + 8U) ==
              (1U | (3U << 16U) | (slot.sequence << 28U)) &&
            LoadLE32(packed + 12U) == 8U * (4U + 12U) &&
            LoadLE32(packed + 16U) == 6U << 20U;
          if (!damaged && !dummy)
            continue;
          if (!length)
          {
            if (sourceOffset)
              return false;
            continue;
          }
          if (sourceOffset < 8U || sourceOffset > packedBytes ||
              length < 8U || length > PYROWAVE_PACKET_BYTES ||
              length > packedBytes - sourceOffset ||
              length > packedBytes - outputOffset)
            return false;

          const uint8_t * packet       = packed + sourceOffset;
          const uint32_t  header       = LoadLE32(packet);
          const uint32_t  headerLength =
            ((header >> 16U) & 0xfffU) * sizeof(uint32_t);
          const uint32_t  packetBlock  = LoadLE32(
            packet + sizeof(uint32_t)) >> 8U;
          if (headerLength != length || packetBlock != blockIndex)
            return false;

          std::copy_n(packet, length,
            output.data.begin() + outputOffset);
          output.blocks.push_back({ outputOffset, length, blockIndex });
          outputOffset += length;
        }
        if (output.blocks.size() != packetCount ||
            outputOffset != packedBytes)
          return false;
      }
      catch (...)
      {
        return false;
      }

      if (!output.blocks.empty())
      {
        output.blocks[0].offset  = 0;
        output.blocks[0].length += 8U;
      }
      output.blockCapacity     = m_blockCapacity;
      output.damagedBlocks     = std::move(slot.work.damagedBlocks);
      return output.damagedBlocks.size() == output.blockCapacity;
    }

    Slot * OldestSlot()
    {
      Slot * oldest = nullptr;
      for (Slot& slot : m_slots)
        if (slot.inFlight && (!oldest || slot.order < oldest->order))
          oldest = &slot;
      return oldest;
    }

    const Slot * OldestSlot() const
    {
      const Slot * oldest = nullptr;
      for (const Slot& slot : m_slots)
        if (slot.inFlight && (!oldest || slot.order < oldest->order))
          oldest = &slot;
      return oldest;
    }

    static void ReleaseSlot(Slot& slot)
    {
      slot.computeSync = {};
      slot.sync        = {};
      slot.inFlight    = false;
      slot.captureTime = 0;
      slot.order       = 0;
      slot.frameLimit  = 0;
      slot.hardLimit   = 0;
      slot.frame       = {};
      slot.work        = {};

      slot.lease.Reset();
    }

  public:
    ~PyroWaveD3D12Encoder()
    {
      Reset();
    }

    bool Busy() const
    {
      for (const Slot& slot : m_slots)
        if (!slot.inFlight)
          return false;
      return true;
    }

    void SetWakeEvent(HANDLE event)
    {
      m_wakeEvent = event;
    }

    bool HasInFlight() const
    {
      return OldestSlot() != nullptr;
    }

    void ReleaseCompletedLeases()
    {
      for (Slot& slot : m_slots)
        if (slot.inFlight && slot.lease &&
            slot.computeSync.State() == D12SyncState::READY)
          slot.lease.Reset();
    }

    bool Failed() const
    {
      if (m_computeQueue.Failed() || m_copyQueue.Failed())
        return true;
      for (const Slot& slot : m_slots)
        if (slot.inFlight &&
            (slot.computeSync.State() == D12SyncState::FAILED ||
             slot.sync.State() == D12SyncState::FAILED))
          return true;
      return false;
    }

    bool Submit(FrameIn frame, TexLease lease, const FrameCfg& cfg,
      LGNetVideoChromaSubsampling chroma, uint32_t hardFrameLimit,
      uint32_t updateFrameLimit)
    {
      if (!lease || !lease.Get() ||
          lease->State() != D3D12_RESOURCE_STATE_COMMON ||
          hardFrameLimit < PYROWAVE_MIN_FRAME_BYTES ||
          updateFrameLimit < PYROWAVE_MIN_FRAME_BYTES)
        return false;
      ID3D12Resource * source = lease->Get();
      if (!m_ready || !SameCfg(m_cfg, cfg) || m_chroma != chroma ||
          !SameDevice(source))
        if (!Configure(cfg, chroma, source))
          return false;

      const uint32_t hardLimit   = (std::min)(
        m_packedCapacity, hardFrameLimit & ~UINT32_C(3));
      const uint32_t packedLimit = (std::min)(hardLimit,
        updateFrameLimit & ~UINT32_C(3));
      if (hardLimit < PYROWAVE_MIN_FRAME_BYTES ||
          packedLimit < PYROWAVE_MIN_FRAME_BYTES)
        return false;

      FrameWork work;
      try
      {
        if (!BuildFrameWork(frame.desc.frame,
              !m_persistentValid || hardLimit != m_hardLimit, work))
          return false;
      }
      catch (...)
      {
        return false;
      }

      unsigned slotIndex = 0;
      while (slotIndex < ENCODER_SLOT_COUNT && m_slots[slotIndex].inFlight)
        ++slotIndex;
      if (slotIndex == ENCODER_SLOT_COUNT)
        return false;
      Slot&               slot           = m_slots[slotIndex];
      CD3D12CommandSlot * computeCommand =
        m_computeQueue.Acquire(slotIndex);
      if (!computeCommand)
        return false;
      CD3D12CommandSlot * copyCommand    =
        m_copyQueue.Acquire(slotIndex);
      if (!copyCommand)
      {
        computeCommand->Cancel();
        return false;
      }
      if (!lease->Wait(*computeCommand))
      {
        copyCommand->Cancel();
        computeCommand->Cancel();
        return false;
      }

      slot.captureTime   = lease->time.postStart;
      slot.sequence      = m_sequence;
      slot.frameLimit    = packedLimit;
      slot.hardLimit     = hardLimit;
      slot.work          = std::move(work);
      CreateSourceDescriptor(slot, source);
      ID3D12DescriptorHeap * heaps[] = { slot.descriptors.Get() };

      ComPtr<ID3D12GraphicsCommandList> computeList =
        computeCommand->GetGfxList();
      ComPtr<ID3D12GraphicsCommandList> copyList    =
        copyCommand->GetGfxList();
      computeList->SetDescriptorHeaps(1, heaps);
      computeList->SetComputeRootSignature(m_rootSignature.Get());
      computeList->SetComputeRootDescriptorTable(
        0, slot.descriptors->GetGPUDescriptorHandleForHeapStart());
      UAVBarrier(computeList.Get(), nullptr);
      if (slot.used)
      {
        Transition(computeList.Get(), slot.packed.Get(),
          D3D12_RESOURCE_STATE_COMMON,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(computeList.Get(), slot.packState.Get(),
          D3D12_RESOURCE_STATE_COMMON,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(computeList.Get(), slot.aux.Get(),
          D3D12_RESOURCE_STATE_COMMON,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      }
      const UINT clear[4] = {};
      computeList->ClearUnorderedAccessViewUint(GPUDescriptor(slot, 6),
        ClearDescriptor(slot, 0), slot.packState.Get(), clear, 0, nullptr);
      UAVBarrier(computeList.Get(), slot.packState.Get());

      ShaderParameters clearState = {};
      clearState.dim0[0] = PYROWAVE_RDO_WORDS;
      clearState.dim0[1] = m_blockCapacity + 1U;
      clearState.work[0] = slot.work.full;
      const uint32_t clearWords = slot.work.full ?
        (std::max)(PYROWAVE_RDO_WORDS, m_blockCapacity + 1U) :
        PYROWAVE_RDO_WORDS;
      Dispatch(computeList.Get(), PIPELINE_CLEAR, clearState,
        (clearWords + 255U) / 256U, 1);
      UAVBarrier(computeList.Get(), slot.aux.Get());
      UAVBarrier(computeList.Get(), m_quantPayload.Get());

      ShaderParameters color = {};
      color.dim0[0] = m_cfg.width;
      color.dim0[1] = m_cfg.height;
      color.dim0[2] = m_planes[0].width;
      color.dim0[3] = m_planes[0].height;
      color.op0[0]  = m_planes[0].offset;
      color.op0[1]  = m_planes[1].offset;
      color.op0[2]  = m_planes[2].offset;
      color.op0[3]  = m_chroma == CHROMA_420;
      color.op1[0]  = m_cfg.profile.signal == FrameSignal::PQ_BT2020;
      const uint32_t colorWidth  = m_chroma == CHROMA_420 ?
        m_planes[0].width / 2U : m_planes[0].width;
      const uint32_t colorHeight = m_chroma == CHROMA_420 ?
        m_planes[0].height / 2U : m_planes[0].height;
      if (slot.work.full)
        Dispatch(computeList.Get(), PIPELINE_COLOR, color,
          (colorWidth + 7U) / 8U, (colorHeight + 7U) / 8U);
      else
        for (const RowRun& run : slot.work.colorRuns)
        {
          color.work[0] = run.x;
          color.work[1] = run.y;
          Dispatch(computeList.Get(), PIPELINE_COLOR, color,
            run.width, run.height);
        }
      UAVBarrier(computeList.Get(), m_planeData.Get());

      RecordTransforms(computeList.Get(), slot.work);
      if (!RecordBands(computeList.Get(), PIPELINE_QUANT,
            slot.work, slot.sequence, m_packedCapacity))
      {
        copyCommand->Cancel();
        computeCommand->Cancel();
        return false;
      }
      UAVBarrier(computeList.Get(), slot.aux.Get());
      UAVBarrier(computeList.Get(), m_quantPayload.Get());
      UAVBarrier(computeList.Get(), slot.packState.Get());
      if (!RecordBands(computeList.Get(), PIPELINE_ANALYZE,
            slot.work, slot.sequence, m_packedCapacity))
      {
        copyCommand->Cancel();
        computeCommand->Cancel();
        return false;
      }
      UAVBarrier(computeList.Get(), slot.aux.Get());

      ShaderParameters finalizeRDO = {};
      Dispatch(computeList.Get(), PIPELINE_RDO_FINALIZE, finalizeRDO, 1, 1);
      UAVBarrier(computeList.Get(), slot.aux.Get());

      ShaderParameters resolve = {};
      resolve.dim0[0] = (packedLimit - 8U) / UINT32_C(4);
      resolve.dim0[1] = m_blockCapacity;
      resolve.dim0[2] = m_blocksPerRDO;
      resolve.work[2] = (hardLimit - 8U) / UINT32_C(4);
      resolve.work[3] = (packedLimit - 8U) / UINT32_C(4);
      if (slot.work.full)
      {
        resolve.work[1] = m_blockCapacity;
        Dispatch(computeList.Get(), PIPELINE_RESOLVE, resolve,
          (m_blockCapacity + 63U) / 64U, 1);
      }
      else
        for (const BandWork& band : slot.work.bands)
          for (const RowRun& run : band.blockRuns)
            for (uint32_t row = 0; row < run.height; ++row)
            {
              resolve.work[0] = band.blockBase +
                (run.y + row) * band.blocksX + run.x;
              resolve.work[1] = run.width;
              Dispatch(computeList.Get(), PIPELINE_RESOLVE, resolve,
                (run.width + 63U) / 64U, 1);
            }
      UAVBarrier(computeList.Get(), slot.aux.Get());

      if (!RecordBands(computeList.Get(), PIPELINE_PACK,
            slot.work, slot.sequence, packedLimit))
      {
        copyCommand->Cancel();
        computeCommand->Cancel();
        return false;
      }
      UAVBarrier(computeList.Get(), slot.packed.Get());
      UAVBarrier(computeList.Get(), slot.packState.Get());
      UAVBarrier(computeList.Get(), slot.aux.Get());
      UAVBarrier(computeList.Get(), m_quantPayload.Get());
      ShaderParameters finalize = {};
      finalize.dim0[0] = m_cfg.width;
      finalize.dim0[1] = m_cfg.height;
      finalize.op0[0]  = slot.sequence;
      finalize.op0[1]  = m_chroma == CHROMA_444;
      finalize.op0[2]  =
        m_cfg.profile.signal == FrameSignal::PQ_BT2020;
      finalize.op0[3]  = 8U * (4U + QuantBits(
        static_cast<int>(PYROWAVE_LEVELS) - 1, 0, 0));
      finalize.work[0] = slot.work.full;
      Dispatch(computeList.Get(), PIPELINE_FINALIZE, finalize, 1, 1);
      PrepareReadback(computeList.Get(), slot);
      RecordReadback(copyList.Get(), slot);

      computeCommand->SetCompletionCallback(
        &CompletionFunction, m_wakeEvent, nullptr);
      if (!computeCommand->Execute(&slot.computeSync))
      {
        copyCommand->Cancel();
        Reset();
        return false;
      }
      m_persistentValid = true;
      m_hardLimit       = hardLimit;
      slot.used         = true;
      if (!copyCommand->WaitFor(
            slot.computeSync.fence.Get(), slot.computeSync.value))
      {
        copyCommand->Cancel();
        Reset();
        return false;
      }
      copyCommand->SetCompletionCallback(
        &CompletionFunction, m_wakeEvent, nullptr);
      if (!copyCommand->Execute(&slot.sync))
      {
        Reset();
        return false;
      }

      slot.frame    = std::move(frame);
      slot.lease    = std::move(lease);
      slot.order    = Next64(m_submitOrder);
      slot.inFlight = true;
      m_submitOrder = slot.order;
      m_sequence    = (m_sequence + 1U) & 7U;
      return true;
    }

    D12SyncState State() const
    {
      const Slot * slot = OldestSlot();
      return slot ? slot->sync.State() : D12SyncState::PENDING;
    }

    bool Complete(uint64_t configEpoch, uint64_t frameID,
      EncodedFrame& output)
    {
      Slot * slot = OldestSlot();
      if (!slot || slot->sync.State() != D12SyncState::READY)
        return false;
      slot->lease.Reset();

      if (!slot->mapped)
      {
        ReleaseSlot(*slot);
        return false;
      }

      output = {};

      const bool result = Compact(*slot, slot->mapped, output);

      if (result)
      {
        output.configEpoch   = configEpoch;
        output.sourceSerial  = slot->frame.desc.frame.content ?
          slot->frame.desc.frame.content->serial : 0;
        output.frameID       = frameID;
        output.captureTimeNs = slot->captureTime;
        output.deadlineMs    = MAX_FRAME_LATENCY_MS;
        output.hardLimit     = slot->hardLimit;
      }
      ReleaseSlot(*slot);
      return result;
    }

    void Reset()
    {
      m_computeQueue.WaitForIdle();
      m_copyQueue.WaitForIdle();
      m_ready = false;
      for (Slot& slot : m_slots)
      {
        ReleaseSlot(slot);
        if (slot.mapped)
        {
          const D3D12_RANGE writeRange = { 0, 0 };
          slot.readback->Unmap(0, &writeRange);
          slot.mapped = nullptr;
        }
        slot.readback.Reset();
        slot.aux.Reset();
        slot.packState.Reset();
        slot.packed.Reset();
        slot.clearDescriptors.Reset();
        slot.descriptors.Reset();
        slot.used = false;
      }
      m_planeData.Reset();
      m_atlas.Reset();
      m_scratch.Reset();
      m_wave.Reset();
      m_quantPayload.Reset();
      for (ComPtr<ID3D12PipelineState>& pipeline : m_pipelines)
        pipeline.Reset();
      m_rootSignature.Reset();
      m_copyQueue.DeInit();
      m_computeQueue.DeInit();
      m_device.Reset();
      m_cfg             = {};
      m_chroma          = 0;
      m_packedOffset    = 0;
      m_readbackBytes   = 0;
      m_submitOrder     = 0;
      m_blockCapacity   = 0;
      m_block8Capacity  = 0;
      m_blocksPerRDO    = 0;
      m_packedCapacity  = 0;
      m_auxWords        = 0;
      m_payloadWords    = 0;
      m_hardLimit       = 0;
      m_persistentValid = false;
      std::fill(std::begin(m_auxBase), std::end(m_auxBase), 0U);
      std::fill(std::begin(m_planes), std::end(m_planes), Plane {});
    }
  };
}

struct CNetworkVideo::Impl
{
  struct ReferenceFrame
  {
    uint64_t                            frameID = 0;
    std::shared_ptr<const EncodedFrame> frame;
    std::vector<uint8_t>                subsequentBlocks;
  };

  struct PeerState
  {
    NetworkVideoPeer                    peer                   = {};
    NetworkVideoPeerInfo                info                   = {};
    LGNetVideoSubscribe                 subscription           = {};
    LGNetVideoFeedback                  feedback               = {};
    LGNetVideoState                     lastStatusState        = {};
    uint64_t                            serial                 = 0;
    uint64_t                            componentEpoch         = 0;
    uint64_t                            statusSequence         = 0;
    uint64_t                            statusEpoch            = 0;
    uint64_t                            lastFrameID            = 0;
    uint64_t                            lastCompleteID         = 0;
    uint64_t                            lastCaptureTimeNs      = 0;
    uint64_t                            referenceFrameID       = 0;
    uint64_t                            sendReadyGeneration[3] = {};
    uint32_t                            lastStatusDetail       = 0;
    uint32_t                            lastStatusFlags        = 0;
    uint32_t                            droppedFrames          = 0;
    uint8_t                             blockedDeliveries      = 0;
    std::shared_ptr<const EncodedFrame> referenceFrame;
    std::deque<ReferenceFrame>          referenceHistory;
    std::vector<uint8_t>                updateBlocks;
    bool                                statusSent             = false;
    bool                                subscribed             = false;
    bool                                configured             = false;
    bool                                congested              = false;
    bool                                keyframePending        = false;
    bool                                referencePending       = false;
    bool                                sendFailed             = false;
  };

  struct PendingFrame
  {
    FrameIn   frame;
    TexLease  lease;
    FrameCfg  cfg;
    uint64_t  configEpoch = 0;
  };

  struct SendItem
  {
    uint64_t                                    serial         = 0;
    NetworkVideoPeer                            peer           = {};
    NetworkVideoDelivery                        delivery       =
      NetworkVideoDelivery::RELIABLE;
    uint16_t                                    messageType    = 0;
    LGNetEnvelopeFlags                          flags          =
      LG_NET_ENVELOPE_FINAL;
    uint64_t                                    componentEpoch = 0;
    uint64_t                                    requestID      = 0;
    uint64_t                                    frameID        = 0;
    uint64_t                                    baseFrameID    = 0;
    std::shared_ptr<const std::vector<uint8_t>> payload;
    std::shared_ptr<const EncodedFrame>         encoded;
    uint32_t                                    fragmentIndex  = 0;
    uint32_t                                    fragmentCount  = 0;
    uint32_t                                    blockIndex     = 0;
    uint32_t                                    blockOffset    = 0;
    uint32_t                                    payloadLimit   = 0;
    bool                                        started        = false;
  };

  CSRWLock m_processLock;
  CSRWLock m_senderCallbackLock;
  CSRWLock m_lock;

  HANDLE            m_workerWake   = nullptr;
  HANDLE            m_workerThread = nullptr;
  std::atomic<bool> m_workerStop   = false;

  INetworkVideoSender          * m_sender                  = nullptr;
  uint64_t                       m_lastBudgetCaptureTimeNs = 0;
  uint32_t                       m_videoBitrateMbps        =
    NETWORK_DEFAULT_VIDEO_BITRATE_MBPS;
  LGNetVideoChromaSubsampling    m_videoChroma             = CHROMA_444;
  bool                           m_started                 = false;
  bool                           m_stopped                 = false;
  bool                           m_failed                  = false;
  bool                           m_changing                = false;

  FrameCfg m_activeCfg  = {};
  FrameCfg m_pendingCfg = {};
  bool     m_active     = false;
  bool     m_pending    = false;

  uint64_t m_configEpoch        = 0;
  uint64_t m_latestSourceSerial = 0;
  uint64_t m_nextFrameID        = 0;
  uint64_t m_nextPeerSerial     = 0;
  uint64_t m_nextSendSerial     = 0;

  std::vector<PeerState>                m_peers;
  std::deque<PendingFrame>              m_frames;
  std::deque<SendItem>                  m_sends;
  std::shared_ptr<const EncodedFrame>   m_canonicalFrame;
  std::shared_ptr<const EncodedFrame>   m_latestFrame;
  size_t                                m_sendBytes = 0;

  PyroWaveD3D12Encoder m_encoder;

  Impl()
  {
    m_workerWake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_workerWake)
      return;
    m_encoder.SetWakeEvent(m_workerWake);
    m_workerThread = CreateThread(
      nullptr, 0, WorkerProc, this, 0, nullptr);
    if (!m_workerThread)
    {
      m_encoder.SetWakeEvent(nullptr);
      CloseHandle(m_workerWake);
      m_workerWake = nullptr;
    }
    else if (!SetThreadPriority(m_workerThread, THREAD_PRIORITY_HIGHEST))
      DEBUG_WARN_HR(GetLastError(),
        "Failed to raise the network video worker priority");
  }

  ~Impl()
  {
    m_workerStop.store(true, std::memory_order_release);
    if (m_workerWake)
      SetEvent(m_workerWake);
    if (m_workerThread)
    {
      WaitForSingleObject(m_workerThread, INFINITE);
      CloseHandle(m_workerThread);
      m_workerThread = nullptr;
    }
    m_encoder.SetWakeEvent(nullptr);
    if (m_workerWake)
    {
      CloseHandle(m_workerWake);
      m_workerWake = nullptr;
    }
  }

  static DWORD WINAPI WorkerProc(void * opaque)
  {
    Impl * self = static_cast<Impl *>(opaque);
    for (;;)
    {
      if (WaitForSingleObject(self->m_workerWake, INFINITE) != WAIT_OBJECT_0 ||
          self->m_workerStop.load(std::memory_order_acquire))
        break;
      try
      {
        self->DriveEncoder();
      }
      catch (...)
      {
        DEBUG_ERROR("Unhandled failure in the network video worker");
        {
          CSRWExclusiveLock processLock(self->m_processLock);
          self->m_encoder.Reset();
          CSRWExclusiveLock lock(self->m_lock);
          self->FailNL(8);
        }
        self->Wake();
      }
    }
    return 0;
  }

  bool WorkerReady() const
  {
    return m_workerWake && m_workerThread;
  }

  void Wake() const
  {
    if (m_workerWake)
      SetEvent(m_workerWake);
  }

  PeerState * FindPeerNL(const NetworkVideoPeer& peer)
  {
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
      [&peer](const PeerState& current)
      {
        return SamePeer(current.peer, peer);
      });
    return found == m_peers.end() ? nullptr : &*found;
  }

  const PeerState * FindPeerNL(const NetworkVideoPeer& peer) const
  {
    const auto found = std::find_if(m_peers.begin(), m_peers.end(),
      [&peer](const PeerState& current)
      {
        return SamePeer(current.peer, peer);
      });
    return found == m_peers.end() ? nullptr : &*found;
  }

  bool HasSubscribersNL() const
  {
    for (const PeerState& peer : m_peers)
      if (peer.subscribed)
        return true;
    return false;
  }

  static void ResetPeerReferenceNL(PeerState& peer)
  {
    peer.referenceFrame.reset();
    peer.referenceHistory.clear();
    peer.updateBlocks.clear();
    peer.referenceFrameID = 0;
    peer.referencePending = false;
  }

  static bool PeerNeedsReferenceNL(
    const PeerState& peer, const EncodedFrame& frame)
  {
    return peer.keyframePending || !peer.referenceFrame ||
      peer.referenceFrame->configEpoch != frame.configEpoch ||
      peer.referenceFrame->blockCapacity != frame.blockCapacity;
  }

  uint32_t FrameLimitNL(uint64_t captureTimeNs)
  {
    uint64_t period = VIDEO_RATE_DEFAULT_PERIOD_NS;
    if (captureTimeNs && m_lastBudgetCaptureTimeNs)
      period = captureTimeNs > m_lastBudgetCaptureTimeNs ?
        (std::min)(captureTimeNs - m_lastBudgetCaptureTimeNs,
          VIDEO_RATE_MAX_PERIOD_NS) : 0;
    if (captureTimeNs)
      m_lastBudgetCaptureTimeNs = captureTimeNs;

    const uint64_t bytes   =
      static_cast<uint64_t>(m_videoBitrateMbps) *
        VIDEO_RATE_BYTES_PER_MBIT * period / VIDEO_RATE_NS_PER_SECOND;
    const uint64_t limited = (std::max)(
      static_cast<uint64_t>(PYROWAVE_MIN_FRAME_BYTES),
      (std::min)(bytes, static_cast<uint64_t>(MAX_FRAME_LENGTH)));
    return static_cast<uint32_t>(limited);
  }

  LGNetVideoChromaSubsampling ChromaNL() const
  {
    if (!m_active)
      return static_cast<LGNetVideoChromaSubsampling>(0);
    return m_videoChroma;
  }

  LGNetVideoStreamConfig StreamConfigNL() const
  {
    LGNetVideoStreamConfig config = {};
    if (!m_active || !m_configEpoch)
      return config;
    const bool                        hdr    =
      m_activeCfg.profile.signal == FrameSignal::PQ_BT2020;
    const LGNetVideoChromaSubsampling chroma = ChromaNL();
    config.streamID            = STREAM_ID;
    config.codec               = LG_NET_VIDEO_CODEC_PYROWAVE;
    config.codecVersion        =
      LG_NET_VIDEO_PYROWAVE_CODEC_VERSION_CURRENT;
    config.configEpoch         = m_configEpoch;
    config.width               = m_activeCfg.width;
    config.height              = m_activeCfg.height;
    config.pixelFormat         = static_cast<LGNetVideoPixelFormat>(
      chroma == CHROMA_420 ?
        LG_NET_VIDEO_PIXEL_FORMAT_YUV420P10 :
        LG_NET_VIDEO_PIXEL_FORMAT_YUV444P10);
    config.chromaSubsampling   = chroma;
    config.colorPrimaries      = static_cast<LGNetColorPrimaries>(hdr ?
      LG_NET_COLOR_PRIMARIES_BT2020 : LG_NET_COLOR_PRIMARIES_BT709);
    config.colorTransfer       = static_cast<LGNetColorTransfer>(hdr ?
      LG_NET_COLOR_TRANSFER_PQ : LG_NET_COLOR_TRANSFER_BT709);
    config.colorMatrix         = static_cast<LGNetColorMatrix>(hdr ?
      LG_NET_COLOR_MATRIX_BT2020 : LG_NET_COLOR_MATRIX_BT709);
    config.colorRange          = LG_NET_COLOR_RANGE_LIMITED;
    config.planeCount          = 3;
    config.bitDepth            = 10;
    config.flags               = LG_NET_VIDEO_STREAM_GPU_PLANES_ONLY;
    config.maxFrameLength      = MAX_FRAME_LENGTH;
    config.maxFragmentLength   = LG_NET_VIDEO_MAX_FRAGMENT_LENGTH;
    config.maxFrameLatencyMs   = MAX_FRAME_LATENCY_MS;
    if (hdr)
      config.flags |= LG_NET_VIDEO_STREAM_HDR;
    return config;
  }

  bool PeerCanStreamNL(const PeerState& peer) const
  {
    if (!peer.subscribed || !m_active || !m_configEpoch)
      return false;
    const LGNetVideoSubscribe& request = peer.subscription;
    const bool                 hdr     =
      m_activeCfg.profile.signal == FrameSignal::PQ_BT2020;
    return (!request.preferredCodec ||
        request.preferredCodec == LG_NET_VIDEO_CODEC_PYROWAVE) &&
      request.maxWidth >= m_activeCfg.width &&
      request.maxHeight >= m_activeCfg.height &&
      request.maxFrameLength >= 20U &&
      (!(request.flags & LG_NET_VIDEO_SUBSCRIBE_REQUIRE_HDR) || hdr);
  }

  bool QueueNL(const NetworkVideoPeer& peer,
    NetworkVideoDelivery delivery, uint16_t messageType,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    uint64_t frameID,
    std::shared_ptr<const std::vector<uint8_t>> payload)
  {
    if (!payload || payload->size() > UINT32_MAX ||
        m_sends.size() >= MAX_SEND_ITEMS ||
        payload->size() > MAX_SEND_BYTES -
          (std::min)(m_sendBytes, MAX_SEND_BYTES))
      return false;
    try
    {
      SendItem     item;
      const size_t bytes = payload->size();
      item.serial         = Next64(m_nextSendSerial);
      item.peer           = peer;
      item.delivery       = delivery;
      item.messageType    = messageType;
      item.flags          = flags;
      item.componentEpoch = componentEpoch;
      item.requestID      = requestID;
      item.frameID        = frameID;
      item.payload        = std::move(payload);
      const uint64_t serial = item.serial;
      m_sends.push_back(std::move(item));
      m_nextSendSerial = serial;
      m_sendBytes      += bytes;
    }
    catch (...)
    {
      return false;
    }
    return true;
  }

  static bool EncodedQueueBytes(
    const EncodedFrame& encoded, size_t& bytes)
  {
    size_t blockBytes;
    size_t frameBytes;
    size_t encodedBytes;
    return Multiply(encoded.blocks.size(), sizeof(EncodedBlock), blockBytes) &&
      Add(encoded.data.size(), encoded.damagedBlocks.size(), frameBytes) &&
      Add(frameBytes, blockBytes, encodedBytes) &&
      Add(encodedBytes, encoded.wholePayload ?
        encoded.wholePayload->size() : 0, bytes);
  }

  static std::shared_ptr<EncodedFrame> BuildCanonicalFrame(
    const std::shared_ptr<const EncodedFrame>& previous,
    EncodedFrame&& current)
  {
    if (!current.blockCapacity || current.data.size() < 8U ||
        current.damagedBlocks.size() != current.blockCapacity ||
        current.hardLimit < 20U || current.data.size() > current.hardLimit)
      return {};
    if (!previous || previous->configEpoch != current.configEpoch ||
        previous->blockCapacity != current.blockCapacity ||
        previous->data.size() < 8U ||
        std::all_of(current.damagedBlocks.begin(),
          current.damagedBlocks.end(),
          [](uint8_t damaged) { return damaged != 0; }))
    {
      try
      {
        return std::make_shared<EncodedFrame>(std::move(current));
      }
      catch (...)
      {
        return {};
      }
    }

    try
    {
      std::shared_ptr<EncodedFrame> result =
        std::make_shared<EncodedFrame>();
      result->configEpoch   = current.configEpoch;
      result->sourceSerial  = current.sourceSerial;
      result->frameID       = current.frameID;
      result->captureTimeNs = current.captureTimeNs;
      result->deadlineMs    = current.deadlineMs;
      result->blockCapacity = current.blockCapacity;
      result->hardLimit     = current.hardLimit;
      result->damagedBlocks = std::move(current.damagedBlocks);
      result->blocks.reserve((std::max)(
        previous->blocks.size(), current.blocks.size()));
      result->data.reserve((std::max)(
        previous->data.size(), current.data.size()));
      result->data.resize(8U);

      constexpr uint32_t SEQUENCE_BITS    = PYROWAVE_SEQUENCE_MASK << 28U;
      const uint32_t     firstWord        = LoadLE32(current.data.data());
      const uint32_t     sequence         =
        (firstWord >> 28U) & PYROWAVE_SEQUENCE_MASK;
      size_t             previousPosition = 0;
      size_t             currentPosition  = 0;
      for (uint32_t index = 0; index < current.blockCapacity; ++index)
      {
        while (previousPosition < previous->blocks.size() &&
            previous->blocks[previousPosition].index < index)
          ++previousPosition;
        while (currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index < index)
          ++currentPosition;
        const EncodedBlock * previousBlock =
          previousPosition < previous->blocks.size() &&
            previous->blocks[previousPosition].index == index ?
              &previous->blocks[previousPosition] : nullptr;
        const EncodedBlock * currentBlock  =
          currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index == index ?
              &current.blocks[currentPosition] : nullptr;
        const bool           damaged       =
          result->damagedBlocks[index] != 0;
        const EncodedFrame&  sourceFrame   = damaged ? current : *previous;
        const EncodedBlock * sourceBlock   = damaged ?
          currentBlock : previousBlock;
        if (!sourceBlock)
          continue;

        const EncodedPacket packet = Packet(sourceFrame, *sourceBlock);
        if (!packet.data || packet.length < 8U ||
            packet.length > PYROWAVE_PACKET_BYTES ||
            result->data.size() > current.hardLimit ||
            packet.length > current.hardLimit - result->data.size() ||
            (LoadLE32(packet.data + sizeof(uint32_t)) >> 8U) != index)
          return {};
        const uint32_t offset = static_cast<uint32_t>(result->data.size());
        result->data.insert(result->data.end(),
          packet.data, packet.data + packet.length);
        const uint32_t packetFirst =
          (LoadLE32(result->data.data() + offset) & ~SEQUENCE_BITS) |
          (sequence << 28U);
        StoreLE32(result->data.data() + offset, packetFirst);
        result->blocks.push_back({ offset, packet.length, index });
      }
      if (result->blocks.empty())
      {
        constexpr uint32_t DUMMY_PACKET_BYTES = 12U;
        constexpr uint32_t DUMMY_QUANT_CODE   = 8U * (4U + 12U);
        if (result->data.size() > current.hardLimit ||
            DUMMY_PACKET_BYTES > current.hardLimit - result->data.size())
          return {};
        const uint32_t offset = static_cast<uint32_t>(result->data.size());
        result->data.resize(result->data.size() + DUMMY_PACKET_BYTES);
        StoreLE32(result->data.data() + offset,
          1U | (3U << 16U) | (sequence << 28U));
        StoreLE32(result->data.data() + offset + sizeof(uint32_t),
          DUMMY_QUANT_CODE);
        StoreLE32(result->data.data() + offset + 2U * sizeof(uint32_t),
          6U << 20U);
        result->blocks.push_back({ offset, DUMMY_PACKET_BYTES, 0U });
      }
      if (result->blocks.size() > UINT32_C(0x00ffffff) ||
          result->data.size() > current.hardLimit)
        return {};

      const uint32_t secondWord =
        (LoadLE32(current.data.data() + sizeof(uint32_t)) &
          ~(PYROWAVE_HEADER_CODE_MASK | UINT32_C(0x00ffffff))) |
        static_cast<uint32_t>(result->blocks.size());
      StoreLE32(result->data.data(), firstWord);
      StoreLE32(result->data.data() + sizeof(uint32_t), secondWord);
      result->blocks[0].offset  = 0;
      result->blocks[0].length += 8U;
      return result;
    }
    catch (...)
    {
      return {};
    }
  }

  static bool AccumulateUpdateBlocks(const EncodedFrame& reference,
    const EncodedFrame& current, std::vector<uint8_t>& updateBlocks)
  {
    if (!reference.blockCapacity ||
        reference.blockCapacity != current.blockCapacity ||
        reference.configEpoch != current.configEpoch ||
        reference.data.size() < 8U || current.data.size() < 8U)
      return false;

    try
    {
      updateBlocks.resize(current.blockCapacity, 0);
      size_t referencePosition = 0;
      size_t currentPosition   = 0;
      for (uint32_t index = 0; index < current.blockCapacity; ++index)
      {
        while (referencePosition < reference.blocks.size() &&
            reference.blocks[referencePosition].index < index)
          ++referencePosition;
        while (currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index < index)
          ++currentPosition;
        const EncodedBlock * referenceBlock =
          referencePosition < reference.blocks.size() &&
            reference.blocks[referencePosition].index == index ?
              &reference.blocks[referencePosition] : nullptr;
        const EncodedBlock * currentBlock   =
          currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index == index ?
              &current.blocks[currentPosition] : nullptr;
        if (!SamePacket(referenceBlock ? Packet(reference, *referenceBlock) :
              EncodedPacket {},
            currentBlock ? Packet(current, *currentBlock) :
              EncodedPacket {}))
          updateBlocks[index] = 1;
      }
    }
    catch (...)
    {
      return false;
    }
    return true;
  }

  static std::shared_ptr<const EncodedFrame> BuildUpdateFrame(
    const EncodedFrame& reference, const EncodedFrame& current,
    const std::vector<uint8_t>& previousBlocks,
    std::vector<uint8_t>& updateBlocks)
  {
    try
    {
      updateBlocks = previousBlocks;
    }
    catch (...)
    {
      updateBlocks.clear();
      return {};
    }
    if (!AccumulateUpdateBlocks(reference, current, updateBlocks))
      return {};

    try
    {
      uint32_t operationCount = 0;
      for (uint8_t changed : updateBlocks)
        operationCount += changed != 0;
      if (!operationCount || operationCount > UINT32_C(0x00ffffff))
        return {};

      std::shared_ptr<EncodedFrame> update =
        std::make_shared<EncodedFrame>();
      update->configEpoch   = current.configEpoch;
      update->sourceSerial  = current.sourceSerial;
      update->frameID       = current.frameID;
      update->captureTimeNs = current.captureTimeNs;
      update->deadlineMs    = current.deadlineMs;
      update->blockCapacity = current.blockCapacity;
      update->blocks.reserve(operationCount);
      update->data.reserve(current.data.size());
      update->data.resize(8U);

      const uint32_t firstWord  = LoadLE32(current.data.data());
      const uint32_t secondWord =
        (LoadLE32(current.data.data() + sizeof(uint32_t)) &
          ~(PYROWAVE_HEADER_CODE_MASK | UINT32_C(0x00ffffff))) |
        PYROWAVE_HEADER_CODE_DELTA | operationCount;
      StoreLE32(update->data.data(), firstWord);
      StoreLE32(update->data.data() + sizeof(uint32_t), secondWord);

      const uint32_t sequence        =
        (firstWord >> 28U) & PYROWAVE_SEQUENCE_MASK;
      size_t         currentPosition = 0;
      for (uint32_t index = 0; index < current.blockCapacity; ++index)
      {
        while (currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index < index)
          ++currentPosition;
        if (!updateBlocks[index])
          continue;
        const EncodedBlock * block  =
          currentPosition < current.blocks.size() &&
            current.blocks[currentPosition].index == index ?
              &current.blocks[currentPosition] : nullptr;
        const uint32_t       offset =
          static_cast<uint32_t>(update->data.size());
        if (block)
        {
          const EncodedPacket packet = Packet(current, *block);
          if (!packet.data || packet.length < 8U ||
              packet.length > LG_NET_VIDEO_MAX_FRAME_UPDATE_LENGTH -
                update->data.size())
            return {};
          update->data.insert(update->data.end(),
            packet.data, packet.data + packet.length);
          update->blocks.push_back({ offset, packet.length, index });
        }
        else
        {
          if (update->data.size() > LG_NET_VIDEO_MAX_FRAME_UPDATE_LENGTH -
              LG_NET_VIDEO_PYROWAVE_CLEAR_PACKET_SIZE)
            return {};
          update->data.resize(update->data.size() +
            LG_NET_VIDEO_PYROWAVE_CLEAR_PACKET_SIZE);
          uint8_t * packet = update->data.data() + offset;
          StoreLE32(packet,
            (LG_NET_VIDEO_PYROWAVE_CLEAR_PACKET_WORDS << 16U) |
              (sequence << 28U));
          StoreLE32(packet + sizeof(uint32_t), index << 8U);
          update->blocks.push_back(
            { offset, LG_NET_VIDEO_PYROWAVE_CLEAR_PACKET_SIZE, index });
        }
      }
      if (update->blocks.size() != operationCount ||
          update->data.size() > LG_NET_VIDEO_MAX_FRAME_UPDATE_LENGTH)
        return {};

      update->blocks[0].offset  = 0;
      update->blocks[0].length += 8U;
      return update;
    }
    catch (...)
    {
      updateBlocks.clear();
      return {};
    }
  }

  static void RecordReferenceFrameNL(PeerState& peer, uint64_t frameID,
    const std::shared_ptr<const EncodedFrame>& frame)
  {
    if (!frameID || !frame)
      return;

    if (!peer.referenceHistory.empty())
    {
      ReferenceFrame& candidate = peer.referenceHistory.front();
      if (frameID <= candidate.frameID)
        return;
      if (candidate.frame && AccumulateUpdateBlocks(
            *candidate.frame, *frame, candidate.subsequentBlocks))
        return;
      if (candidate.subsequentBlocks.size() == frame->blockCapacity)
        std::fill(candidate.subsequentBlocks.begin(),
          candidate.subsequentBlocks.end(), static_cast<uint8_t>(1U));
      return;
    }

    try
    {
      ReferenceFrame candidate;
      candidate.frameID = frameID;
      candidate.frame   = frame;
      candidate.subsequentBlocks.assign(
        frame->blockCapacity, static_cast<uint8_t>(0U));
      peer.referenceHistory.push_back(std::move(candidate));
    }
    catch (...)
    {
    }
  }

  static void RemoveReferenceFrameNL(PeerState& peer, uint64_t frameID)
  {
    peer.referenceHistory.erase(std::remove_if(
      peer.referenceHistory.begin(), peer.referenceHistory.end(),
      [frameID](const ReferenceFrame& retained)
      {
        return retained.frameID == frameID;
      }), peer.referenceHistory.end());
  }

  static bool AdvancePeerReferenceNL(PeerState& peer,
    uint64_t lastCompleteFrameID, uint64_t lastDecodedFrameID)
  {
    const uint64_t   acknowledgedID = (std::min)(
      lastCompleteFrameID, lastDecodedFrameID);
    ReferenceFrame * candidate      = nullptr;
    for (auto retained = peer.referenceHistory.rbegin();
         retained != peer.referenceHistory.rend(); ++retained)
      if (retained->frameID >= peer.referenceFrameID &&
          retained->frameID <= acknowledgedID)
      {
        candidate = &*retained;
        break;
      }

    if (!candidate)
    {
      if (!peer.referencePending || !peer.referenceFrameID ||
          peer.referenceFrameID > acknowledgedID)
        return false;
      peer.referencePending = false;
      peer.keyframePending  = false;
      return true;
    }

    const uint64_t                            promotedID    =
      candidate->frameID;
    const std::shared_ptr<const EncodedFrame> promotedFrame =
      candidate->frame;
    if (!promotedFrame)
      return false;

    if (candidate->subsequentBlocks.size() != promotedFrame->blockCapacity)
      return false;

    std::vector<uint8_t> subsequentBlocks =
      std::move(candidate->subsequentBlocks);

    peer.referenceFrame   = promotedFrame;
    peer.referenceFrameID = promotedID;
    peer.referencePending = false;
    peer.keyframePending  = false;
    peer.updateBlocks     = std::move(subsequentBlocks);
    while (!peer.referenceHistory.empty() &&
        peer.referenceHistory.front().frameID <= promotedID)
      peer.referenceHistory.pop_front();
    return true;
  }

  bool EncodedQueuedNL(const EncodedFrame * encoded,
    uint64_t excludingSerial = 0) const
  {
    return std::any_of(m_sends.begin(), m_sends.end(),
      [encoded, excludingSerial](const SendItem& item)
      {
        return item.serial != excludingSerial &&
          item.encoded.get() == encoded;
      });
  }

  void ReleaseSendAccountingNL(const SendItem& item)
  {
    if (item.payload)
    {
      m_sendBytes -= item.payload->size();
      return;
    }
    if (!item.encoded || EncodedQueuedNL(item.encoded.get(), item.serial))
      return;

    size_t bytes;
    if (EncodedQueueBytes(*item.encoded, bytes))
      m_sendBytes -= bytes;
  }

  template<typename T, bool (*Encode)(void *, size_t, const T *)>
  bool QueueFixedNL(const NetworkVideoPeer& peer,
    NetworkVideoDelivery delivery, uint16_t messageType,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    uint64_t frameID, const T& value, size_t size)
  {
    std::shared_ptr<std::vector<uint8_t>> payload;
    try
    {
      payload = std::make_shared<std::vector<uint8_t>>(size);
    }
    catch (...)
    {
      return false;
    }
    if (!Encode(payload->data(), payload->size(), &value))
      return false;
    return QueueNL(peer, delivery, messageType, flags, componentEpoch,
      requestID, frameID, std::move(payload));
  }

  bool QueueConfigNL(PeerState& peer, uint64_t requestID = 0)
  {
    if (m_failed)
    {
      peer.configured = false;
      return QueueStatusNL(
        peer, LG_NET_VIDEO_STATE_ERROR, 6, requestID);
    }
    if (!PeerCanStreamNL(peer))
    {
      peer.configured = false;
      return QueueStatusNL(
        peer, LG_NET_VIDEO_STATE_ERROR, 1, requestID);
    }
    LGNetVideoStreamConfig config = StreamConfigNL();
    config.maxFrameLength    = (std::min)(
      config.maxFrameLength, peer.subscription.maxFrameLength);
    config.maxFragmentLength = (std::min)(
      config.maxFragmentLength, config.maxFrameLength);
    config.maxFrameLatencyMs = (std::min)(
      config.maxFrameLatencyMs, peer.subscription.maxFrameLatencyMs);
    if ((peer.subscription.flags &
          LG_NET_VIDEO_SUBSCRIBE_ALLOW_DATAGRAMS) &&
        DatagramFragmentsSupported(peer.info))
      config.flags |= LG_NET_VIDEO_STREAM_DATAGRAMS;
    if (!lgNetVideoStreamConfigValid(&config) ||
        !QueueFixedNL<LGNetVideoStreamConfig,
          lgNetVideoStreamConfigEncode>(peer.peer,
            NetworkVideoDelivery::RELIABLE,
            LG_NET_VIDEO_MESSAGE_STREAM_CONFIG,
            LG_NET_ENVELOPE_FINAL | (requestID ?
              LG_NET_ENVELOPE_RESPONSE : 0),
            m_configEpoch, requestID, 0, config,
            LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE))
      return false;
    peer.componentEpoch    = m_configEpoch;
    peer.configured        = true;
    peer.lastCaptureTimeNs = 0;
    peer.keyframePending   = true;
    peer.sendFailed        = false;
    ResetPeerReferenceNL(peer);
    return QueueStatusNL(peer, LG_NET_VIDEO_STATE_CONFIGURED, 0);
  }

  bool QueueStatusNL(PeerState& peer, LGNetVideoState state,
    uint32_t detail, uint64_t requestID = 0)
  {
    LGNetVideoStatus status = {};
    status.streamID       = STREAM_ID;
    status.state          = state;
    status.configEpoch    = peer.componentEpoch;
    status.lastFrameID    = peer.lastFrameID;
    status.statusSequence = Next64(peer.statusSequence);
    if (peer.keyframePending)
      status.flags |= LG_NET_VIDEO_STATUS_KEYFRAME_PENDING;
    if (peer.congested)
      status.flags |= LG_NET_VIDEO_STATUS_CONGESTED;
    if (m_changing)
      status.flags |= LG_NET_VIDEO_STATUS_RECONFIGURING;
    status.detail         = detail;

    if (!requestID && peer.statusSent &&
        peer.statusEpoch == status.configEpoch &&
        peer.lastStatusState == status.state &&
        peer.lastStatusFlags == status.flags &&
        peer.lastStatusDetail == status.detail)
      return true;

    if (!QueueFixedNL<LGNetVideoStatus, lgNetVideoStatusEncode>(
      peer.peer, NetworkVideoDelivery::RELIABLE,
      LG_NET_VIDEO_MESSAGE_STATUS,
      LG_NET_ENVELOPE_FINAL | (requestID ?
        LG_NET_ENVELOPE_RESPONSE : 0),
      peer.componentEpoch, requestID, 0, status,
      LG_NET_VIDEO_STATUS_WIRE_SIZE))
      return false;

    peer.statusSequence   = status.statusSequence;
    peer.statusEpoch      = status.configEpoch;
    peer.lastStatusState  = status.state;
    peer.lastStatusFlags  = status.flags;
    peer.lastStatusDetail = status.detail;
    peer.statusSent       = true;
    return true;
  }

  void FailNL(uint32_t detail)
  {
    m_failed             = true;
    m_latestSourceSerial = 0;
    m_frames.clear();
    m_canonicalFrame.reset();
    m_latestFrame.reset();
    for (PeerState& peer : m_peers)
    {
      ResetPeerReferenceNL(peer);
      if (peer.subscribed)
        QueueStatusNL(peer, LG_NET_VIDEO_STATE_ERROR, detail);
    }
  }

  void DropPeerFramesNL(PeerState& peer)
  {
    // Finish a frame once any of its fragments have been accepted. Replacing
    // it mid-flight prevents a continuously changing desktop from ever
    // delivering a complete frame on a constrained link.
    uint64_t activeFrame = 0;
    for (const SendItem& item : m_sends)
      if (item.frameID && item.started &&
          FragmentMessage(item.messageType) &&
          SamePeer(item.peer, peer.peer))
      {
        activeFrame = item.frameID;
        break;
      }

    for (auto item = m_sends.begin(); item != m_sends.end();)
      if (item->frameID && item->frameID != activeFrame &&
          SamePeer(item->peer, peer.peer))
      {
        RemoveReferenceFrameNL(peer, item->frameID);
        ReleaseSendAccountingNL(*item);
        item = m_sends.erase(item);
      }
      else
        ++item;
  }

  void DropPeerSendsNL(PeerState& peer)
  {
    for (auto item = m_sends.begin(); item != m_sends.end();)
      if (SamePeer(item->peer, peer.peer))
      {
        RemoveReferenceFrameNL(peer, item->frameID);
        ReleaseSendAccountingNL(*item);
        item = m_sends.erase(item);
      }
      else
        ++item;
    peer.statusSent = false;
  }

  bool QueueScheduleNL(PeerState& peer, const EncodedFrame& frame,
    uint64_t frameID, uint64_t presentationBaseNs)
  {
    const uint32_t deadlineMs     = (std::min)(
      frame.deadlineMs, peer.subscription.maxFrameLatencyMs);
    const uint64_t presentationNs = presentationBaseNs +
      static_cast<uint64_t>(deadlineMs) * UINT64_C(1000000);

    LGNetVideoSchedule schedule = {};
    schedule.streamID                = STREAM_ID;
    schedule.flags                   = LG_NET_VIDEO_SCHEDULE_PRESENT |
      LG_NET_VIDEO_SCHEDULE_DROP_IF_LATE;
    schedule.configEpoch             = frame.configEpoch;
    schedule.frameID                 = frameID;
    schedule.captureTimestampNs      = frame.captureTimeNs;
    schedule.presentationTimestampNs = presentationNs;
    schedule.deadlineTimestampNs     = presentationNs;
    return QueueFixedNL<LGNetVideoSchedule,
      lgNetVideoScheduleEncode>(peer.peer,
        NetworkVideoDelivery::RELIABLE, LG_NET_VIDEO_MESSAGE_SCHEDULE,
        LG_NET_ENVELOPE_FINAL, frame.configEpoch, 0, frameID,
        schedule, LG_NET_VIDEO_SCHEDULE_WIRE_SIZE);
  }

  static std::shared_ptr<const std::vector<uint8_t>> BuildWholeFramePayload(
    const EncodedFrame& encoded, uint32_t deadlineMs,
    uint64_t frameID, uint64_t presentationBaseNs)
  {
    LGNetVideoFrame frame = {};
    frame.streamID                = STREAM_ID;
    frame.codec                   = LG_NET_VIDEO_CODEC_PYROWAVE;
    frame.flags                   = LG_NET_VIDEO_FRAME_KEYFRAME;
    frame.configEpoch             = encoded.configEpoch;
    frame.frameID                 = frameID;
    frame.captureTimestampNs      = encoded.captureTimeNs;
    frame.presentationTimestampNs = presentationBaseNs +
      static_cast<uint64_t>(deadlineMs) * UINT64_C(1000000);
    frame.encodedLength           = static_cast<uint32_t>(encoded.data.size());
    frame.blockCount              = static_cast<uint32_t>(encoded.blocks.size());
    frame.fragmentCount           = 1;
    frame.deadlineMs              = deadlineMs;
    frame.data                    = encoded.data.data();

    const size_t size = lgNetVideoFrameSize(&frame);
    if (!size || size > UINT32_MAX)
      return {};

    std::shared_ptr<std::vector<uint8_t>> payload;
    try
    {
      payload = std::make_shared<std::vector<uint8_t>>(size);
    }
    catch (...)
    {
      return {};
    }
    if (!lgNetVideoFrameEncode(
          payload->data(), payload->size(), &frame))
      return {};
    return payload;
  }

  static std::shared_ptr<const std::vector<uint8_t>> BuildUpdatePayload(
    const EncodedFrame& encoded, uint64_t baseFrameID,
    uint32_t deadlineMs, uint64_t frameID,
    uint64_t presentationBaseNs)
  {
    LGNetVideoFrameUpdate update = {};
    update.streamID                = STREAM_ID;
    update.codec                   = LG_NET_VIDEO_CODEC_PYROWAVE;
    update.configEpoch             = encoded.configEpoch;
    update.frameID                 = frameID;
    update.baseFrameID             = baseFrameID;
    update.captureTimestampNs      = encoded.captureTimeNs;
    update.presentationTimestampNs = presentationBaseNs +
      static_cast<uint64_t>(deadlineMs) * UINT64_C(1000000);
    update.encodedLength           =
      static_cast<uint32_t>(encoded.data.size());
    update.blockCount              =
      static_cast<uint32_t>(encoded.blocks.size());
    update.fragmentCount           = 1;
    update.deadlineMs              = deadlineMs;
    update.data                    = encoded.data.data();

    const size_t size = lgNetVideoFrameUpdateSize(&update);
    if (!size || size > UINT32_MAX)
      return {};

    std::shared_ptr<std::vector<uint8_t>> payload;
    try
    {
      payload = std::make_shared<std::vector<uint8_t>>(size);
    }
    catch (...)
    {
      return {};
    }
    if (!lgNetVideoFrameUpdateEncode(
          payload->data(), payload->size(), &update))
      return {};
    return payload;
  }

  bool QueueWholeFrameNL(PeerState& peer, const EncodedFrame& encoded,
    NetworkVideoDelivery delivery, uint64_t frameID,
    uint64_t presentationBaseNs)
  {
    const uint32_t deadlineMs = (std::min)(
      encoded.deadlineMs, peer.subscription.maxFrameLatencyMs);
    std::shared_ptr<const std::vector<uint8_t>> payload =
      deadlineMs == encoded.deadlineMs && frameID == encoded.frameID &&
        presentationBaseNs == encoded.captureTimeNs ?
          encoded.wholePayload : nullptr;
    if (!payload)
      payload = BuildWholeFramePayload(
        encoded, deadlineMs, frameID, presentationBaseNs);
    if (!payload)
      return false;
    return QueueNL(peer.peer, delivery, LG_NET_VIDEO_MESSAGE_FRAME,
      LG_NET_ENVELOPE_FINAL, encoded.configEpoch, 0, frameID,
      std::move(payload));
  }

  bool QueueWholeUpdateNL(PeerState& peer, const EncodedFrame& encoded,
    NetworkVideoDelivery delivery, uint64_t baseFrameID, uint64_t frameID,
    uint64_t presentationBaseNs)
  {
    const uint32_t deadlineMs = (std::min)(
      encoded.deadlineMs, peer.subscription.maxFrameLatencyMs);
    std::shared_ptr<const std::vector<uint8_t>> payload =
      BuildUpdatePayload(encoded, baseFrameID,
        deadlineMs, frameID, presentationBaseNs);
    if (!payload)
      return false;
    return QueueNL(peer.peer, delivery,
      LG_NET_VIDEO_MESSAGE_FRAME_UPDATE, LG_NET_ENVELOPE_FINAL,
      encoded.configEpoch, 0, frameID, std::move(payload));
  }

  static bool FragmentPlan(const EncodedFrame& encoded,
    uint32_t maxPayload, uint32_t firstBlockIndex,
    uint32_t firstFragmentIndex,
    uint32_t& payloadLimit, uint32_t& fragmentCount,
    uint32_t headerSize = LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
  {
    if (maxPayload <= headerSize ||
        firstBlockIndex > encoded.blocks.size())
      return false;
    payloadLimit = (std::min)(
      maxPayload - headerSize,
      static_cast<uint32_t>(LG_NET_VIDEO_MAX_FRAGMENT_LENGTH));
    if (!payloadLimit)
      return false;

    uint64_t fragmentCount64 = firstFragmentIndex;
    for (size_t blockIndex = firstBlockIndex;
         blockIndex < encoded.blocks.size(); ++blockIndex)
    {
      const EncodedBlock& block = encoded.blocks[blockIndex];
      if (!block.length)
        return false;
      fragmentCount64 +=
        (static_cast<uint64_t>(block.length) + payloadLimit - 1U) /
        payloadLimit;
    }
    if (!fragmentCount64 || fragmentCount64 > LG_NET_VIDEO_MAX_FRAGMENTS ||
        fragmentCount64 > UINT32_MAX)
      return false;
    fragmentCount = static_cast<uint32_t>(fragmentCount64);
    return true;
  }

  static bool CriticalFragmentPlan(const EncodedFrame& encoded,
    uint32_t maxPayload, uint32_t& payloadLimit,
    uint32_t& fragmentCount)
  {
    if (encoded.blocks.empty() ||
        maxPayload <= LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
      return false;
    payloadLimit = (std::min)(
      maxPayload - LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE,
      static_cast<uint32_t>(LG_NET_VIDEO_MAX_FRAGMENT_LENGTH));
    const uint32_t length = encoded.blocks.front().length;
    if (!payloadLimit || !length)
      return false;
    const uint64_t count =
      (static_cast<uint64_t>(length) + payloadLimit - 1U) / payloadLimit;
    if (!count || count > LG_NET_VIDEO_MAX_FRAGMENTS || count > UINT32_MAX)
      return false;
    fragmentCount = static_cast<uint32_t>(count);
    return true;
  }

  bool QueueFragmentsNL(PeerState& peer,
    std::shared_ptr<const EncodedFrame> encoded,
    NetworkVideoDelivery delivery, uint64_t frameID, uint32_t maxPayload,
    uint32_t firstBlockIndex    = 0,
    uint32_t firstFragmentIndex = 0,
    uint16_t messageType        = LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT,
    uint64_t baseFrameID        = 0)
  {
    const bool update =
      messageType == LG_NET_VIDEO_MESSAGE_FRAME_UPDATE_FRAGMENT;
    if (!encoded || (update != (baseFrameID != 0)))
      return false;
    uint32_t payloadLimit;
    uint32_t fragmentCount;
    if (!FragmentPlan(*encoded, maxPayload, firstBlockIndex,
          firstFragmentIndex,
          payloadLimit, fragmentCount,
          update ? LG_NET_VIDEO_FRAME_UPDATE_FRAGMENT_HEADER_WIRE_SIZE :
            LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE))
      return false;
    if (firstBlockIndex == encoded->blocks.size())
      return true;

    const bool newFrame = !EncodedQueuedNL(encoded.get());
    size_t     bytes    = 0;
    if ((newFrame && !EncodedQueueBytes(*encoded, bytes)) ||
        m_sends.size() >= MAX_SEND_ITEMS ||
        bytes > MAX_SEND_BYTES - (std::min)(m_sendBytes, MAX_SEND_BYTES))
      return false;
    try
    {
      SendItem item;
      item.serial         = Next64(m_nextSendSerial);
      item.peer           = peer.peer;
      item.delivery       = delivery;
      item.messageType    = messageType;
      item.flags          = LG_NET_ENVELOPE_FINAL;
      item.componentEpoch = encoded->configEpoch;
      item.frameID        = frameID;
      item.baseFrameID    = baseFrameID;
      item.encoded        = std::move(encoded);
      item.fragmentIndex  = firstFragmentIndex;
      item.fragmentCount  = fragmentCount;
      item.blockIndex     = firstBlockIndex;
      item.payloadLimit   = payloadLimit;
      const uint64_t serial = item.serial;
      m_sends.push_back(std::move(item));
      m_nextSendSerial = serial;
      m_sendBytes      += bytes;
    }
    catch (...)
    {
      return false;
    }
    return true;
  }

  bool QueueReliableFirstBlockNL(PeerState& peer,
    const std::shared_ptr<const EncodedFrame>& encoded,
    uint64_t frameID, uint32_t payloadLimit,
    uint32_t criticalFragmentCount, uint32_t fragmentCount)
  {
    if (!encoded || encoded->blocks.empty() || !payloadLimit ||
        !criticalFragmentCount || fragmentCount < criticalFragmentCount ||
        !DeliverySupported(peer.info, NetworkVideoDelivery::RELIABLE) ||
        peer.info.maxReliablePayload <=
          LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
      return false;
    const EncodedBlock& block = encoded->blocks.front();
    if (!block.length || payloadLimit > peer.info.maxReliablePayload -
          LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
      return false;

    uint32_t blockOffset = 0;
    for (uint32_t fragmentIndex = 0;
         fragmentIndex < criticalFragmentCount; ++fragmentIndex)
    {
      SendItem item;
      item.peer           = peer.peer;
      item.delivery       = NetworkVideoDelivery::RELIABLE;
      item.messageType    = LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT;
      item.flags          = LG_NET_ENVELOPE_FINAL;
      item.componentEpoch = encoded->configEpoch;
      item.frameID        = frameID;
      item.encoded        = encoded;
      item.fragmentIndex  = fragmentIndex;
      item.fragmentCount  = fragmentCount;
      item.blockOffset    = blockOffset;
      item.payloadLimit   = payloadLimit;

      std::shared_ptr<std::vector<uint8_t>> payload;
      try
      {
        payload = std::make_shared<std::vector<uint8_t>>();
      }
      catch (...)
      {
        return false;
      }
      if (!BuildFragmentPayload(item, *payload) ||
          payload->size() > peer.info.maxReliablePayload ||
          !QueueNL(peer.peer, NetworkVideoDelivery::RELIABLE,
            LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT, LG_NET_ENVELOPE_FINAL,
            encoded->configEpoch, 0, frameID, std::move(payload)))
        return false;
      blockOffset += (std::min)(payloadLimit, block.length - blockOffset);
    }
    return blockOffset == block.length;
  }

  bool BuildFragmentPayload(
    const SendItem& item, std::vector<uint8_t>& payload) const
  {
    if (!item.encoded || !item.payloadLimit ||
        item.blockIndex >= item.encoded->blocks.size() ||
        item.fragmentIndex >= item.fragmentCount)
      return false;

    const EncodedBlock& block = item.encoded->blocks[item.blockIndex];
    if (item.blockOffset >= block.length)
      return false;
    const uint32_t length = (std::min)(
      item.payloadLimit, block.length - item.blockOffset);

    if (item.messageType == LG_NET_VIDEO_MESSAGE_FRAME_UPDATE_FRAGMENT)
    {
      LGNetVideoFrameUpdateFragment fragment = {};
      fragment.streamID      = STREAM_ID;
      fragment.configEpoch   = item.encoded->configEpoch;
      fragment.frameID       = item.frameID;
      fragment.baseFrameID   = item.baseFrameID;
      fragment.frameOffset   = block.offset + item.blockOffset;
      fragment.frameLength   =
        static_cast<uint32_t>(item.encoded->data.size());
      fragment.blockIndex    = item.blockIndex;
      fragment.blockOffset   = item.blockOffset;
      fragment.blockLength   = block.length;
      fragment.blockCount    =
        static_cast<uint32_t>(item.encoded->blocks.size());
      fragment.fragmentIndex = item.fragmentIndex;
      fragment.fragmentCount = item.fragmentCount;
      fragment.payloadLength = length;
      fragment.payload       =
        item.encoded->data.data() + fragment.frameOffset;
      if (!item.blockOffset)
        fragment.flags |= LG_NET_VIDEO_FRAGMENT_BLOCK_START;
      if (item.blockOffset + length == block.length)
        fragment.flags |= LG_NET_VIDEO_FRAGMENT_BLOCK_END;
      if (static_cast<size_t>(fragment.frameOffset) + length ==
          item.encoded->data.size())
        fragment.flags |= LG_NET_VIDEO_FRAGMENT_FRAME_END;

      const size_t size = lgNetVideoFrameUpdateFragmentSize(&fragment);
      if (!size)
        return false;
      try
      {
        payload.resize(size);
      }
      catch (...)
      {
        return false;
      }
      return lgNetVideoFrameUpdateFragmentEncode(
        payload.data(), payload.size(), &fragment);
    }
    if (item.messageType != LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT)
      return false;

    LGNetVideoFragment fragment = {};
    fragment.streamID      = STREAM_ID;
    fragment.configEpoch   = item.encoded->configEpoch;
    fragment.frameID       = item.frameID;
    fragment.frameOffset   = block.offset + item.blockOffset;
    fragment.frameLength   =
      static_cast<uint32_t>(item.encoded->data.size());
    fragment.blockIndex    = item.blockIndex;
    fragment.blockOffset   = item.blockOffset;
    fragment.blockLength   = block.length;
    fragment.blockCount    =
      static_cast<uint32_t>(item.encoded->blocks.size());
    fragment.fragmentIndex = item.fragmentIndex;
    fragment.fragmentCount = item.fragmentCount;
    fragment.payloadLength = length;
    fragment.payload       =
      item.encoded->data.data() + fragment.frameOffset;
    if (!item.blockOffset)
      fragment.flags |= LG_NET_VIDEO_FRAGMENT_BLOCK_START;
    if (item.blockOffset + length == block.length)
      fragment.flags |= LG_NET_VIDEO_FRAGMENT_BLOCK_END;
    if (static_cast<size_t>(fragment.frameOffset) + length ==
        item.encoded->data.size())
      fragment.flags |= LG_NET_VIDEO_FRAGMENT_FRAME_END;

    const size_t size = lgNetVideoFragmentSize(&fragment);
    if (!size)
      return false;
    try
    {
      payload.resize(size);
    }
    catch (...)
    {
      return false;
    }
    return lgNetVideoFragmentEncode(
      payload.data(), payload.size(), &fragment);
  }

  bool AdvanceFragmentNL(SendItem& item) const
  {
    if (!item.encoded || item.blockIndex >= item.encoded->blocks.size())
      return false;
    const EncodedBlock& block  = item.encoded->blocks[item.blockIndex];
    const uint32_t      length = (std::min)(
      item.payloadLimit, block.length - item.blockOffset);
    item.blockOffset += length;
    ++item.fragmentIndex;
    if (item.blockOffset == block.length)
    {
      ++item.blockIndex;
      item.blockOffset = 0;
    }
    return item.fragmentIndex == item.fragmentCount;
  }

  bool QueueFrameNL(PeerState& peer,
    const std::shared_ptr<const EncodedFrame>& frame,
    uint64_t frameID, uint64_t presentationBaseNs,
    bool establishReference = false)
  {
    if (!frame || !frameID || !presentationBaseNs || !peer.configured ||
        peer.componentEpoch != frame->configEpoch ||
        frame->data.empty() || frame->data.size() >
          peer.subscription.maxFrameLength)
      return false;

    std::vector<uint8_t> cleanBlocks;
    if (establishReference)
      try
      {
        cleanBlocks.assign(frame->blockCapacity, 0);
      }
      catch (...)
      {
        return false;
      }

    DropPeerFramesNL(peer);
    const bool     datagrams = !establishReference &&
      (peer.subscription.flags & LG_NET_VIDEO_SUBSCRIBE_ALLOW_DATAGRAMS) &&
      DatagramFragmentsSupported(peer.info);
    const bool     bulk      = BulkFragmentsSupported(peer.info);
    const uint32_t fullSize  = static_cast<uint32_t>(frame->data.size()) +
      LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE;

    NetworkVideoDelivery delivery = NetworkVideoDelivery::RELIABLE;
    bool                 result;
    if (datagrams)
    {
      delivery = NetworkVideoDelivery::DATAGRAM;
      uint32_t   criticalPayloadLimit  = 0;
      uint32_t   criticalFragmentCount = 0;
      uint32_t   payloadLimit          = 0;
      uint32_t   fragmentCount         = 0;
      const bool plan                  =
        CriticalFragmentPlan(*frame, peer.info.maxReliablePayload,
          criticalPayloadLimit, criticalFragmentCount) &&
        FragmentPlan(*frame, DeliveryPayload(peer.info, delivery), 1U,
          criticalFragmentCount, payloadLimit, fragmentCount);
      result = plan && QueueScheduleNL(
          peer, *frame, frameID, presentationBaseNs) &&
        QueueReliableFirstBlockNL(peer, frame, frameID,
          criticalPayloadLimit, criticalFragmentCount, fragmentCount) &&
        QueueFragmentsNL(peer, frame, delivery, frameID,
          payloadLimit + LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE, 1U,
          criticalFragmentCount);
    }
    else if (bulk)
    {
      delivery = NetworkVideoDelivery::BULK;
      result   = fullSize <= peer.info.maxBulkPayload ?
        QueueWholeFrameNL(
          peer, *frame, delivery, frameID, presentationBaseNs) :
        QueueScheduleNL(peer, *frame, frameID, presentationBaseNs) &&
          QueueFragmentsNL(
            peer, frame, delivery, frameID,
            DeliveryPayload(peer.info, delivery));
    }
    else if (!datagrams && fullSize <= peer.info.maxReliablePayload)
      result = QueueWholeFrameNL(
        peer, *frame, NetworkVideoDelivery::RELIABLE,
        frameID, presentationBaseNs);
    else
    {
      const uint32_t maxPayload = DeliveryPayload(peer.info, delivery);
      result = DeliverySupported(peer.info, delivery) &&
        QueueScheduleNL(peer, *frame, frameID, presentationBaseNs) &&
        QueueFragmentsNL(peer, frame, delivery, frameID, maxPayload);
    }

    if (!result)
    {
      DropPeerFramesNL(peer);
      ++peer.droppedFrames;
      peer.congested = true;
      return false;
    }
    peer.lastFrameID       = frameID;
    peer.lastCaptureTimeNs = frame->captureTimeNs;
    if (establishReference)
    {
      peer.referenceHistory.clear();
      peer.referenceFrame   = frame;
      peer.referenceFrameID = frameID;
      peer.referencePending = true;
      peer.updateBlocks     = std::move(cleanBlocks);
      RecordReferenceFrameNL(peer, frameID, frame);
    }
    peer.keyframePending = establishReference;
    return true;
  }

  bool QueueUpdateNL(PeerState& peer,
    const std::shared_ptr<const EncodedFrame>& update,
    uint64_t baseFrameID, uint64_t frameID,
    uint64_t presentationBaseNs)
  {
    if (!update || !baseFrameID || baseFrameID >= frameID ||
        !presentationBaseNs || !peer.configured ||
        peer.referencePending || peer.referenceFrameID != baseFrameID ||
        peer.componentEpoch != update->configEpoch ||
        update->data.empty() || update->data.size() >
          peer.subscription.maxFrameLength)
      return false;

    DropPeerFramesNL(peer);
    const bool            bulk       = BulkFragmentsSupported(peer.info);
    NetworkVideoDelivery  delivery   = bulk ?
      NetworkVideoDelivery::BULK : NetworkVideoDelivery::RELIABLE;
    const uint32_t        fullSize   =
      static_cast<uint32_t>(update->data.size()) +
      LG_NET_VIDEO_FRAME_UPDATE_HEADER_WIRE_SIZE;
    const uint32_t        maxPayload = DeliveryPayload(peer.info, delivery);
    bool                  result;
    if (fullSize <= maxPayload)
      result = QueueWholeUpdateNL(peer, *update, delivery,
        baseFrameID, frameID, presentationBaseNs);
    else
      result = QueueScheduleNL(peer, *update, frameID,
          presentationBaseNs) &&
        QueueFragmentsNL(peer, update, delivery, frameID, maxPayload,
          0, 0, LG_NET_VIDEO_MESSAGE_FRAME_UPDATE_FRAGMENT, baseFrameID);

    if (!result)
    {
      DropPeerFramesNL(peer);
      ++peer.droppedFrames;
      peer.congested = true;
      return false;
    }
    peer.lastFrameID       = frameID;
    peer.lastCaptureTimeNs = update->captureTimeNs;
    return true;
  }

  bool QueueCurrentFrameNL(PeerState& peer,
    const std::shared_ptr<const EncodedFrame>& current,
    uint64_t frameID, uint64_t presentationBaseNs)
  {
    if (!current)
      return false;
    if (peer.referencePending)
      return true;
    if (PeerNeedsReferenceNL(peer, *current))
      return QueueFrameNL(
        peer, current, frameID, presentationBaseNs, true);
    DropPeerFramesNL(peer);
    std::vector<uint8_t>                      updateBlocks;
    const std::shared_ptr<const EncodedFrame> update = BuildUpdateFrame(
      *peer.referenceFrame, *current, peer.updateBlocks,
      updateBlocks);
    const bool compared = updateBlocks.size() == current->blockCapacity;
    const bool changed  = std::any_of(updateBlocks.begin(),
      updateBlocks.end(), [](uint8_t value) { return value != 0; });
    if (!update)
    {
      if (compared && !changed)
      {
        peer.lastCaptureTimeNs = current->captureTimeNs;
        return true;
      }
      return QueueFrameNL(
        peer, current, frameID, presentationBaseNs, true);
    }

    const uint64_t fullWireSize   = current->data.size() +
      LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE;
    const uint64_t updateWireSize = update->data.size() +
      LG_NET_VIDEO_FRAME_UPDATE_HEADER_WIRE_SIZE;
    if (updateWireSize >= fullWireSize)
      return QueueFrameNL(
        peer, current, frameID, presentationBaseNs, true);

    if (!QueueUpdateNL(peer, update, peer.referenceFrameID,
          frameID, presentationBaseNs))
      return false;
    peer.updateBlocks = std::move(updateBlocks);
    RecordReferenceFrameNL(peer, frameID, current);
    return true;
  }

  bool ReplayLatestFrameNL(PeerState& peer)
  {
    if (peer.referencePending)
      return true;
    if (!m_latestFrame || !m_latestSourceSerial ||
        m_latestFrame->configEpoch != m_configEpoch ||
        m_latestFrame->sourceSerial != m_latestSourceSerial ||
        m_latestFrame->data.size() > peer.subscription.maxFrameLength)
      return false;

    const uint64_t frameID = Next64(m_nextFrameID);
    if (!QueueCurrentFrameNL(peer, m_latestFrame, frameID,
          CFrameScheduler::Nanotime()))
      return false;

    m_nextFrameID = frameID;
    QueueStatusNL(peer, LG_NET_VIDEO_STATE_STREAMING, 0);
    return true;
  }

  bool RetryFrameReplays()
  {
    bool queued = false;
    CSRWExclusiveLock lock(m_lock);
    for (PeerState& peer : m_peers)
      if (peer.subscribed && peer.configured &&
          (peer.keyframePending ||
            (!peer.referencePending && peer.referenceFrame && m_latestFrame &&
              m_latestFrame->captureTimeNs > peer.lastCaptureTimeNs)))
        queued |= ReplayLatestFrameNL(peer);
    return queued;
  }

  static bool PeerFrameDueNL(
    const PeerState& peer, const EncodedFrame& frame)
  {
    if (!peer.lastCaptureTimeNs ||
        frame.captureTimeNs <= peer.lastCaptureTimeNs)
      return true;
    const uint64_t period = UINT64_C(1000000000) /
      peer.subscription.maxFrameRate;
    return !period ||
      frame.captureTimeNs - peer.lastCaptureTimeNs >= period;
  }

  static bool PeerUsesWholeFrameNL(
    const PeerState& peer, const EncodedFrame& frame)
  {
    const uint64_t fullSize = static_cast<uint64_t>(frame.data.size()) +
      LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE;
    const bool datagrams =
      (peer.subscription.flags & LG_NET_VIDEO_SUBSCRIBE_ALLOW_DATAGRAMS) &&
      DatagramFragmentsSupported(peer.info);
    if (datagrams)
      return false;
    if (BulkFragmentsSupported(peer.info))
      return fullSize <= peer.info.maxBulkPayload;
    return fullSize <= peer.info.maxReliablePayload;
  }

  void PublishFrame(EncodedFrame&& frame)
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_started || m_stopped || m_failed || !m_active ||
        frame.configEpoch != m_configEpoch)
      return;
    std::shared_ptr<EncodedFrame> canonical = BuildCanonicalFrame(
      m_canonicalFrame, std::move(frame));
    if (!canonical)
    {
      FailNL(4);
      return;
    }

    uint64_t frameID = canonical->frameID;
    if (frameID != m_nextFrameID)
    {
      frameID            = Next64(m_nextFrameID);
      m_nextFrameID      = frameID;
      canonical->frameID = frameID;
    }
    bool buildWholePayload = false;
    for (const PeerState& peer : m_peers)
      if (peer.subscribed && peer.configured &&
          !peer.referencePending &&
          PeerNeedsReferenceNL(peer, *canonical) &&
          PeerFrameDueNL(peer, *canonical) &&
          PeerUsesWholeFrameNL(peer, *canonical))
      {
        buildWholePayload = true;
        break;
      }
    if (buildWholePayload)
    {
      canonical->wholePayload = BuildWholeFramePayload(*canonical,
        canonical->deadlineMs, frameID, canonical->captureTimeNs);
      if (!canonical->wholePayload)
      {
        FailNL(4);
        return;
      }
    }

    const std::shared_ptr<const EncodedFrame> encoded = canonical;
    m_canonicalFrame = encoded;
    // An older in-flight encode must not replace a newer retained source.
    if (encoded->sourceSerial &&
        encoded->sourceSerial == m_latestSourceSerial)
      m_latestFrame = encoded;
    for (PeerState& peer : m_peers)
      if (peer.subscribed && peer.configured &&
          PeerFrameDueNL(peer, *encoded))
      {
        if (!QueueCurrentFrameNL(peer, encoded,
              frameID, encoded->captureTimeNs))
          QueueStatusNL(peer, LG_NET_VIDEO_STATE_STALLED, 2);
        else
          QueueStatusNL(peer, LG_NET_VIDEO_STATE_STREAMING, 0);
      }
  }

  bool SubmitPending()
  {
    PendingFrame                  pending;
    LGNetVideoChromaSubsampling   chroma      = CHROMA_444;
    uint32_t                      hardLimit   = MAX_FRAME_LENGTH;
    uint32_t                      updateLimit = MAX_FRAME_LENGTH;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_encoder.Busy() || m_frames.empty() || !m_started ||
          m_stopped || m_failed || !m_active)
        return false;
      bool configured = false;
      for (const PeerState& peer : m_peers)
        if (peer.subscribed && peer.configured)
        {
          hardLimit  = (std::min)(
            hardLimit, peer.subscription.maxFrameLength);
          configured = true;
        }
      if (!configured)
        return false;
      pending = std::move(m_frames.front());
      m_frames.pop_front();
      if (pending.configEpoch != m_configEpoch ||
          !SameCfg(pending.cfg, m_activeCfg))
        return true;
      updateLimit = (std::min)(
        hardLimit, FrameLimitNL(pending.lease->time.postStart));
      chroma      = m_videoChroma;
    }

    if (m_encoder.Submit(std::move(pending.frame),
          std::move(pending.lease), pending.cfg,
          chroma,
          hardLimit, updateLimit))
      return true;

    CSRWExclusiveLock lock(m_lock);
    FailNL(3);
    return false;
  }

  void Flush(size_t limit = MAX_SEND_ITEMS)
  {
    CSRWSharedLock callbackLock(m_senderCallbackLock);
    {
      CSRWSharedLock lock(m_lock);
      if (m_sends.empty())
        return;
    }
    size_t skipped      = 0;
    bool   madeProgress = false;
    for (size_t count = 0; count < limit; ++count)
    {
      SendItem              item;
      INetworkVideoSender * sender          = nullptr;
      uint64_t              readyGeneration = 0;
      {
        CSRWExclusiveLock lock(m_lock);
        if (m_sends.empty() || !m_sender)
          return;
        PeerState * peer = FindPeerNL(m_sends.front().peer);
        if (!peer)
        {
          ReleaseSendAccountingNL(m_sends.front());
          m_sends.pop_front();
          madeProgress = true;
          continue;
        }
        if (peer->blockedDeliveries &
            DeliveryMask(m_sends.front().delivery))
        {
          m_sends.push_back(std::move(m_sends.front()));
          m_sends.pop_front();
          if (++skipped >= m_sends.size())
            return;
          continue;
        }
        item            = m_sends.front();
        sender          = m_sender;
        readyGeneration = peer->sendReadyGeneration[
          DeliveryIndex(item.delivery)];
      }
      skipped = 0;

      std::shared_ptr<const std::vector<uint8_t>> payload = item.payload;
      if (!payload)
      {
        std::shared_ptr<std::vector<uint8_t>> fragmentPayload;
        try
        {
          fragmentPayload = std::make_shared<std::vector<uint8_t>>();
        }
        catch (...)
        {
        }
        if (fragmentPayload &&
            BuildFragmentPayload(item, *fragmentPayload))
          payload = std::move(fragmentPayload);
      }
      const NetworkVideoSendResult result = payload ?
        sender->SendVideo(item.peer, item.delivery, item.messageType,
          item.flags, item.componentEpoch, item.requestID,
          std::move(payload)) :
        NetworkVideoSendResult::FAILED;

      CSRWExclusiveLock lock(m_lock);
      if (m_sends.empty() || m_sends.front().serial != item.serial)
        continue;
      PeerState * peer = FindPeerNL(item.peer);
      if (result == NetworkVideoSendResult::BLOCKED)
      {
        if (peer && peer->sendReadyGeneration[
              DeliveryIndex(item.delivery)] == readyGeneration)
        {
          peer->congested = true;
          peer->blockedDeliveries |= DeliveryMask(item.delivery);
          m_sends.push_back(std::move(m_sends.front()));
          m_sends.pop_front();
        }
        continue;
      }
      if (result == NetworkVideoSendResult::FAILED)
      {
        if (peer)
        {
          const bool notify = !peer->sendFailed;

          peer->configured = false;
          peer->congested  = true;
          peer->sendFailed = true;
          DropPeerSendsNL(*peer);
          if (notify)
            QueueStatusNL(*peer, LG_NET_VIDEO_STATE_ERROR, 7);
        }
        else
        {
          ReleaseSendAccountingNL(m_sends.front());
          m_sends.pop_front();
        }
        return;
      }
      if (FragmentMessage(item.messageType) && item.frameID)
        for (SendItem& queued : m_sends)
          if (queued.frameID == item.frameID &&
              FragmentMessage(queued.messageType) &&
              SamePeer(queued.peer, item.peer))
            queued.started = true;
      madeProgress = true;
      const bool complete = !m_sends.front().encoded ||
        AdvanceFragmentNL(m_sends.front());
      if (complete)
      {
        ReleaseSendAccountingNL(m_sends.front());
        m_sends.pop_front();
      }
      if (peer && item.frameID)
        peer->congested = false;
    }

    CSRWSharedLock lock(m_lock);
    if (madeProgress && !m_sends.empty())
      Wake();
  }

  void DriveEncoder()
  {
    CSRWExclusiveLock processLock(m_processLock);

    m_encoder.ReleaseCompletedLeases();

    {
      CSRWSharedLock lock(m_lock);
      if (m_failed)
      {
        lock.Unlock();
        Flush(32);
        return;
      }
    }

    if (m_encoder.Failed())
    {
      m_encoder.Reset();
      CSRWExclusiveLock lock(m_lock);
      FailNL(5);
      return;
    }

    while (m_encoder.HasInFlight() &&
           m_encoder.State() == D12SyncState::READY)
    {
      uint64_t configEpoch;
      uint64_t frameID;
      {
        CSRWExclusiveLock lock(m_lock);
        configEpoch = m_configEpoch;
        frameID     = Next64(m_nextFrameID);

        m_nextFrameID = frameID;
      }
      EncodedFrame frame;
      if (m_encoder.Complete(configEpoch, frameID, frame))
        PublishFrame(std::move(frame));
      else
      {
        CSRWExclusiveLock lock(m_lock);
        FailNL(4);
        return;
      }
    }

    while (!m_encoder.Busy() && SubmitPending())
    {
    }
    Flush();
    if (RetryFrameReplays())
      Flush();
  }
};

CNetworkVideo::CNetworkVideo() :
  m_impl(new (std::nothrow) Impl())
{
}

CNetworkVideo::~CNetworkVideo()
{
  if (!m_impl)
    return;
  Stop();
  DetachSender();
}

bool CNetworkVideo::AttachSender(INetworkVideoSender& sender)
{
  if (!m_impl)
    return false;
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_sender)
    return m_impl->m_sender == &sender;
  m_impl->m_sender = &sender;
  return true;
}

void CNetworkVideo::DetachSender()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
    CSRWExclusiveLock lock(m_impl->m_lock);
    m_impl->m_sender             = nullptr;
    m_impl->m_latestSourceSerial = 0;
    m_impl->m_sendBytes          = 0;
    m_impl->m_frames.clear();
    m_impl->m_sends.clear();
    m_impl->m_peers.clear();
    m_impl->m_canonicalFrame.reset();
    m_impl->m_latestFrame.reset();
  }
  m_impl->m_encoder.Reset();
}

bool CNetworkVideo::Start(uint32_t videoBitrateMbps,
  LGNetVideoChromaSubsampling videoChroma)
{
  if (!m_impl || !m_impl->WorkerReady() ||
      !videoBitrateMbps ||
      videoBitrateMbps > NETWORK_MAX_VIDEO_BITRATE_MBPS ||
      (videoChroma != CHROMA_420 && videoChroma != CHROMA_444))
    return false;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (m_impl->m_stopped || m_impl->m_failed)
      return false;
    m_impl->m_lastBudgetCaptureTimeNs = 0;
    m_impl->m_videoBitrateMbps        = videoBitrateMbps;
    m_impl->m_videoChroma             = videoChroma;
    m_impl->m_started                 = true;
  }
  m_impl->Wake();
  return true;
}

void CNetworkVideo::Stop()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (m_impl->m_stopped)
      return;
    m_impl->m_stopped                 = true;
    m_impl->m_started                 = false;
    m_impl->m_active                  = false;
    m_impl->m_pending                 = false;
    m_impl->m_changing                = false;
    m_impl->m_sendBytes               = 0;
    m_impl->m_lastBudgetCaptureTimeNs = 0;
    m_impl->m_latestSourceSerial      = 0;
    m_impl->m_frames.clear();
    m_impl->m_sends.clear();
    m_impl->m_canonicalFrame.reset();
    m_impl->m_latestFrame.reset();
    for (Impl::PeerState& peer : m_impl->m_peers)
    {
      peer.configured     = false;
      peer.componentEpoch = 0;
      m_impl->ResetPeerReferenceNL(peer);
    }
  }
  m_impl->m_encoder.Reset();
}

bool CNetworkVideo::PeerConnected(const NetworkVideoPeer& key)
{
  if (!m_impl || !key.client || !key.generation)
    return false;

  NetworkVideoPeerInfo info;
  {
    CSRWSharedLock callbackLock(m_impl->m_senderCallbackLock);
    INetworkVideoSender * sender;
    {
      CSRWSharedLock lock(m_impl->m_lock);
      sender = m_impl->m_sender;
    }
    if (!sender || !sender->VideoPeerInfo(key, info))
      return false;
  }

  if (info.serviceVersion < LG_NET_VIDEO_VERSION_MIN ||
      info.serviceVersion > LG_NET_VIDEO_VERSION_CURRENT ||
      !(info.capabilityFlags & LG_NET_CAPABILITY_RELIABLE) ||
      info.maxReliablePayload < LG_NET_VIDEO_STATUS_WIRE_SIZE ||
      info.maxReliablePayload < LG_NET_VIDEO_STREAM_CONFIG_WIRE_SIZE ||
      info.maxReliablePayload < PYROWAVE_FRAGMENT_MIN)
    return false;

  CSRWExclusiveLock lock(m_impl->m_lock);
  Impl::PeerState * existing = m_impl->FindPeerNL(key);
  if (existing)
  {
    existing->info = info;
    return true;
  }
  if (m_impl->m_peers.size() >= NETWORK_MAX_CLIENTS || m_impl->m_stopped)
    return false;
  try
  {
    Impl::PeerState peer;
    peer.peer   = key;
    peer.info   = info;
    peer.serial = Next64(m_impl->m_nextPeerSerial);

    m_impl->m_nextPeerSerial = peer.serial;
    m_impl->m_peers.push_back(peer);
  }
  catch (...)
  {
    return false;
  }
  return true;
}

void CNetworkVideo::PeerDisconnected(const NetworkVideoPeer& key)
{
  if (!m_impl)
    return;
  CSRWExclusiveLock lock(m_impl->m_lock);
  for (auto item = m_impl->m_sends.begin(); item != m_impl->m_sends.end();)
    if (SamePeer(item->peer, key))
    {
      m_impl->ReleaseSendAccountingNL(*item);
      item = m_impl->m_sends.erase(item);
    }
    else
      ++item;
  const auto peer = std::find_if(m_impl->m_peers.begin(),
    m_impl->m_peers.end(), [&key](const Impl::PeerState& current)
    {
      return SamePeer(current.peer, key);
    });
  if (peer != m_impl->m_peers.end())
    m_impl->m_peers.erase(peer);
  if (!m_impl->HasSubscribersNL())
    m_impl->m_lastBudgetCaptureTimeNs = 0;
}

NetworkVideoPacketResult CNetworkVideo::HandlePacket(
  const NetworkVideoPeer& key, const LGNetPacketView& packet,
  NetworkVideoDelivery delivery)
{
  if (!m_impl || packet.envelope.service != LG_NET_SERVICE_VIDEO ||
      packet.envelope.serviceVersion < LG_NET_VIDEO_VERSION_MIN ||
      packet.envelope.serviceVersion > LG_NET_VIDEO_VERSION_CURRENT ||
      packet.payloadLength != packet.envelope.payloadLength)
    return NetworkVideoPacketResult::INVALID;

  CSRWExclusiveLock lock(m_impl->m_lock);
  Impl::PeerState * peer = m_impl->FindPeerNL(key);
  if (!peer || peer->info.serviceVersion != packet.envelope.serviceVersion)
    return NetworkVideoPacketResult::IGNORED;
  if (!DeliverySupported(peer->info, delivery))
    return NetworkVideoPacketResult::INVALID;

  /* A configuration update crosses client traffic already in flight. Decode
   * and validate stale messages, but do not make their epoch mismatch fatal. */
  switch (packet.envelope.messageType)
  {
    case LG_NET_VIDEO_MESSAGE_SUBSCRIBE:
    {
      if (delivery != NetworkVideoDelivery::RELIABLE ||
          packet.envelope.componentEpoch || !packet.envelope.requestID ||
          !DeliveryFlags(packet.envelope, delivery,
            LG_NET_ENVELOPE_ACK_REQUIRED | LG_NET_ENVELOPE_FINAL))
        return NetworkVideoPacketResult::INVALID;
      LGNetVideoSubscribe subscribe = {};
      if (lgNetVideoSubscribeDecode(&subscribe, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK ||
          subscribe.maxWidth > MAX_WIDTH ||
          subscribe.maxHeight > MAX_HEIGHT ||
          subscribe.maxFrameLength < PYROWAVE_MIN_FRAME_BYTES ||
          subscribe.maxFrameLength > MAX_FRAME_LENGTH ||
          (subscribe.preferredCodec &&
            subscribe.preferredCodec != LG_NET_VIDEO_CODEC_PYROWAVE))
        return NetworkVideoPacketResult::INVALID;

      const bool hadSubscribers = m_impl->HasSubscribersNL();
      m_impl->DropPeerSendsNL(*peer);
      m_impl->ResetPeerReferenceNL(*peer);
      peer->subscription    = subscribe;
      peer->subscribed      = true;
      peer->configured      = false;
      peer->componentEpoch  = m_impl->m_active ?
        m_impl->m_configEpoch : 0;
      peer->congested       = false;
      peer->keyframePending = true;
      peer->sendFailed      = false;
      if (!hadSubscribers)
        m_impl->m_lastBudgetCaptureTimeNs = 0;
      if (m_impl->m_active)
      {
        if (!m_impl->QueueConfigNL(
              *peer, packet.envelope.requestID))
          return NetworkVideoPacketResult::INVALID;
        m_impl->ReplayLatestFrameNL(*peer);
      }
      else if (!m_impl->QueueStatusNL(
          *peer, LG_NET_VIDEO_STATE_SUBSCRIBED, 0,
          packet.envelope.requestID))
        return NetworkVideoPacketResult::INVALID;
      break;
    }

    case LG_NET_VIDEO_MESSAGE_UNSUBSCRIBE:
    {
      if (delivery != NetworkVideoDelivery::RELIABLE ||
          !DeliveryFlags(packet.envelope, delivery,
            LG_NET_ENVELOPE_FINAL))
        return NetworkVideoPacketResult::INVALID;
      LGNetVideoControl control = {};
      if (lgNetVideoControlDecode(&control, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK ||
          control.streamID != STREAM_ID)
        return NetworkVideoPacketResult::INVALID;
      if ((control.configEpoch &&
            control.configEpoch != peer->componentEpoch) ||
          (packet.envelope.componentEpoch &&
            packet.envelope.componentEpoch != peer->componentEpoch))
        return NetworkVideoPacketResult::IGNORED;
      m_impl->DropPeerSendsNL(*peer);
      peer->subscribed      = false;
      peer->configured      = false;
      peer->componentEpoch  = 0;
      peer->keyframePending = false;
      m_impl->ResetPeerReferenceNL(*peer);
      if (!m_impl->HasSubscribersNL())
        m_impl->m_lastBudgetCaptureTimeNs = 0;
      break;
    }

    case LG_NET_VIDEO_MESSAGE_KEYFRAME_REQUEST:
    {
      if (delivery == NetworkVideoDelivery::BULK ||
          !DeliveryFlags(packet.envelope, delivery,
            LG_NET_ENVELOPE_FINAL))
        return NetworkVideoPacketResult::INVALID;
      LGNetVideoControl control = {};
      if (lgNetVideoControlDecode(&control, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK ||
          control.streamID != STREAM_ID)
        return NetworkVideoPacketResult::INVALID;
      if ((control.configEpoch &&
            control.configEpoch != peer->componentEpoch) ||
          (packet.envelope.componentEpoch &&
            packet.envelope.componentEpoch != peer->componentEpoch))
        return NetworkVideoPacketResult::IGNORED;
      if (!peer->subscribed)
        return NetworkVideoPacketResult::INVALID;
      m_impl->DropPeerSendsNL(*peer);
      m_impl->ResetPeerReferenceNL(*peer);
      peer->keyframePending = true;
      m_impl->ReplayLatestFrameNL(*peer);
      break;
    }

    case LG_NET_VIDEO_MESSAGE_FEEDBACK:
    {
      if (delivery == NetworkVideoDelivery::BULK ||
          !DeliveryFlags(packet.envelope, delivery,
            LG_NET_ENVELOPE_FINAL))
        return NetworkVideoPacketResult::INVALID;
      LGNetVideoFeedback feedback = {};
      if (lgNetVideoFeedbackDecode(&feedback, packet.payload,
            packet.payloadLength) != LG_NET_PARSE_OK ||
          feedback.streamID != STREAM_ID ||
          !packet.envelope.componentEpoch ||
          feedback.highestSeenFrameID > peer->lastFrameID)
        return NetworkVideoPacketResult::INVALID;
      if (packet.envelope.componentEpoch != peer->componentEpoch)
        return NetworkVideoPacketResult::IGNORED;
      if (!peer->subscribed)
        return NetworkVideoPacketResult::INVALID;
      peer->feedback       = feedback;
      peer->lastCompleteID = feedback.lastCompleteFrameID;
      peer->congested      =
        feedback.flags & LG_NET_VIDEO_FEEDBACK_CONGESTED;
      const bool referenceAccepted =
        m_impl->AdvancePeerReferenceNL(*peer,
          feedback.lastCompleteFrameID, feedback.lastDecodedFrameID);
      if (feedback.flags & (LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME |
            LG_NET_VIDEO_FEEDBACK_DECODER_STALLED))
      {
        m_impl->DropPeerSendsNL(*peer);
        m_impl->ResetPeerReferenceNL(*peer);
        peer->keyframePending = true;
        m_impl->ReplayLatestFrameNL(*peer);
      }
      else if (referenceAccepted)
        m_impl->QueueStatusNL(*peer, LG_NET_VIDEO_STATE_STREAMING, 0);
      break;
    }

    default:
      return NetworkVideoPacketResult::INVALID;
  }

  lock.Unlock();
  m_impl->Wake();
  return NetworkVideoPacketResult::HANDLED;
}

void CNetworkVideo::Process(uint64_t nowMs)
{
  (void)nowMs;
  if (!m_impl)
    return;
  m_impl->Wake();
}

void CNetworkVideo::SendReady(
  const NetworkVideoPeer& key, NetworkVideoDelivery delivery)
{
  if (!m_impl)
    return;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    Impl::PeerState * peer = m_impl->FindPeerNL(key);
    if (!peer || !DeliverySupported(peer->info, delivery))
      return;
    const unsigned index = DeliveryIndex(delivery);
    peer->congested                  = false;
    peer->blockedDeliveries         &=
      static_cast<uint8_t>(~DeliveryMask(delivery));
    peer->sendReadyGeneration[index] =
      Next64(peer->sendReadyGeneration[index]);
  }
  m_impl->Wake();
}

const FrameProfile * CNetworkVideo::Profiles(unsigned& count) const
{
  count = ARRAYSIZE(VIDEO_PROFILES);
  return VIDEO_PROFILES;
}

std::shared_ptr<const FrameCaps> CNetworkVideo::GetFrameCaps() const
{
  if (!m_impl)
    return std::shared_ptr<const FrameCaps>();
  LGNetVideoChromaSubsampling chroma;
  {
    CSRWSharedLock lock(m_impl->m_lock);
    chroma = m_impl->m_videoChroma;
  }
  return std::shared_ptr<const FrameCaps>(
    new (std::nothrow) NetworkVideoFrameCaps(chroma));
}

CfgResult CNetworkVideo::Probe(const FrameCfg& cfg) const
{
  if (!m_impl)
    return CfgResult::FAILED;
  CSRWSharedLock lock(m_impl->m_lock);
  if (m_impl->m_stopped)
    return CfgResult::FAILED;
  if (!Frame::Valid(cfg.mode) || !cfg.width || !cfg.height ||
      cfg.width > MAX_WIDTH || cfg.height > MAX_HEIGHT ||
      !SupportedProfile(cfg.profile))
    return CfgResult::NEXT;
  const LGNetVideoChromaSubsampling chroma = m_impl->m_videoChroma;
  if (!PlaneElementsFit(cfg.width, cfg.height, chroma))
    return CfgResult::NEXT;
  return CfgResult::ACCEPTED;
}

CfgResult CNetworkVideo::Prepare(const FrameCfg& cfg)
{
  const CfgResult result = Probe(cfg);
  if (result != CfgResult::ACCEPTED)
    return result;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_changing || m_impl->m_stopped)
    return CfgResult::RETRY;
  m_impl->m_pendingCfg = cfg;
  m_impl->m_pending    = true;
  m_impl->m_changing   = true;
  return CfgResult::ACCEPTED;
}

void CNetworkVideo::Commit()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (!m_impl->m_pending || !m_impl->m_changing || m_impl->m_stopped)
      return;
    m_impl->m_activeCfg               = m_impl->m_pendingCfg;
    m_impl->m_pendingCfg              = {};
    m_impl->m_active                  = true;
    m_impl->m_pending                 = false;
    m_impl->m_failed                  = false;
    m_impl->m_changing                = false;
    m_impl->m_configEpoch             = Next64(m_impl->m_configEpoch);
    m_impl->m_lastBudgetCaptureTimeNs = 0;
    m_impl->m_latestSourceSerial      = 0;
    m_impl->m_frames.clear();
    m_impl->m_canonicalFrame.reset();
    m_impl->m_latestFrame.reset();
    for (Impl::PeerState& peer : m_impl->m_peers)
      if (peer.subscribed)
      {
        peer.configured      = false;
        peer.componentEpoch  = m_impl->m_configEpoch;
        peer.keyframePending = true;
        m_impl->DropPeerSendsNL(peer);
        m_impl->QueueConfigNL(peer);
      }
  }
  m_impl->m_encoder.Reset();
  m_impl->Wake();
}

void CNetworkVideo::Abort()
{
  if (!m_impl)
    return;
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (!m_impl->m_pending || !m_impl->m_changing)
    return;
  m_impl->m_pendingCfg = {};
  m_impl->m_pending    = false;
  m_impl->m_changing   = false;
}

PushResult CNetworkVideo::Push(FrameIn frame, TexLease lease) noexcept
{
  if (!m_impl || !lease || !lease.Get())
    return PushResult::REJECTED;
  CSRWExclusiveLock lock = CSRWExclusiveLock::Try(m_impl->m_lock);
  if (!lock)
    return PushResult::BUSY;
  if (!m_impl->m_started || m_impl->m_stopped || m_impl->m_failed ||
      !m_impl->m_active || m_impl->m_changing)
    return PushResult::BUSY;

  const CFrameTex          * texture = lease.Get();
  const D3D12_RESOURCE_DESC  desc    = texture->Get()->GetDesc();
  if (!frame.desc.frame.content || !frame.desc.frame.content->serial ||
      frame.graph != texture->graph || frame.desc.node != texture->node ||
      !Frame::Same(frame.desc.profile, m_impl->m_activeCfg.profile) ||
      !Frame::Same(texture->profile, m_impl->m_activeCfg.profile) ||
      desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width != m_impl->m_activeCfg.width ||
      desc.Height != m_impl->m_activeCfg.height ||
      desc.Format != ProfileFormat(m_impl->m_activeCfg.profile.pixel) ||
      texture->State() != D3D12_RESOURCE_STATE_COMMON)
    return PushResult::STALE;
  const uint64_t sourceSerial = frame.desc.frame.content->serial;

  try
  {
    // Keep the newest source available until a subscriber can encode it.
    if (m_impl->m_frames.size() >= MAX_PENDING_FRAMES)
    {
      MergeFrameDamage(frame.desc.frame,
        m_impl->m_frames.front().frame.desc.frame);
      m_impl->m_frames.pop_front();
    }
    Impl::PendingFrame pending;
    pending.frame       = std::move(frame);
    pending.lease       = std::move(lease);
    pending.cfg         = m_impl->m_activeCfg;
    pending.configEpoch = m_impl->m_configEpoch;
    m_impl->m_frames.push_back(std::move(pending));
    m_impl->m_latestSourceSerial = sourceSerial;
  }
  catch (...)
  {
    return PushResult::FAILED;
  }
  lock.Unlock();
  m_impl->Wake();
  return PushResult::ACCEPTED;
}

PushResult CNetworkVideo::Push(FrameIn frame, D11Lease lease) noexcept
{
  (void)frame;
  (void)lease;
  return PushResult::REJECTED;
}
