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
#include "d3d/CD3D12CommandQueue.h"

#include <Windows.h>
#include <d3dcompiler.h>
#include <wrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <new>
#include <utility>
#include <vector>

using namespace Microsoft::WRL;

namespace
{
  constexpr uint32_t STREAM_ID              = 1;
  constexpr uint32_t MAX_WIDTH              = 8192;
  constexpr uint32_t MAX_HEIGHT             = 8192;
  constexpr uint32_t MAX_FRAME_LENGTH       = LG_NET_VIDEO_MAX_FRAME_LENGTH;
  constexpr uint32_t MAX_BLOCKS             = 262144;
  constexpr uint32_t MAX_PENDING_FRAMES     = 2;
  constexpr uint32_t MAX_SEND_ITEMS         = 8192;
  constexpr size_t   MAX_SEND_BYTES         = 96U * 1024U * 1024U;
  constexpr uint32_t MAX_FRAME_LATENCY_MS   = 50;
  constexpr uint32_t PYROWAVE_LEVELS        = 5;
  constexpr uint32_t PYROWAVE_ALIGNMENT     = 1U << PYROWAVE_LEVELS;
  constexpr uint32_t PYROWAVE_MIN_DIMENSION = 4U << PYROWAVE_LEVELS;
  constexpr uint8_t  PYROWAVE_QUANT_CODE    = 104;
  constexpr uint8_t  PYROWAVE_QSCALE_CODE   = 6;
  constexpr float    PYROWAVE_QUANT_SCALE   = 512.0f;

  constexpr LGNetEnvelopeFlags DELIVERY_MASK =
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

  class NetworkVideoFrameCaps final : public FrameCaps
  {
  public:
    bool CanUseMode(const FrameMode& mode,
      uint32_t * requiredSizeMiB = nullptr) const override
    {
      if (requiredSizeMiB)
        *requiredSizeMiB = 0;
      return mode.width && mode.height && mode.refresh100uHz &&
        mode.width <= MAX_WIDTH && mode.height <= MAX_HEIGHT;
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

  bool SupportedProfile(const FrameProfile& profile)
  {
    for (const FrameProfile& supported : VIDEO_PROFILES)
      if (Frame::Same(profile, supported))
        return true;
    return false;
  }

  uint32_t Align(uint32_t value, uint32_t alignment)
  {
    return (value + alignment - 1U) & ~(alignment - 1U);
  }

  uint64_t Next64(uint64_t value)
  {
    ++value;
    return value ? value : 1;
  }

  uint64_t MonotonicNs()
  {
    const uint64_t now = GetTickCount64() * UINT64_C(1000000);
    return now ? now : 1;
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

  void AppendLE16(std::vector<uint8_t>& data, uint16_t value)
  {
    data.push_back(static_cast<uint8_t>(value));
    data.push_back(static_cast<uint8_t>(value >> 8));
  }

  void AppendLE32(std::vector<uint8_t>& data, uint32_t value)
  {
    data.push_back(static_cast<uint8_t>(value));
    data.push_back(static_cast<uint8_t>(value >> 8));
    data.push_back(static_cast<uint8_t>(value >> 16));
    data.push_back(static_cast<uint8_t>(value >> 24));
  }

  void StoreLE32(uint8_t * data, uint32_t value)
  {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
  }

  unsigned MagnitudeBits(uint16_t value)
  {
    unsigned bits = 0;
    while (value)
    {
      ++bits;
      value >>= 1;
    }
    return bits;
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

  // The CDF 9/7 lifting constants and pass order below are derived from
  // PyroWave commit 89f7e47d4abbf650c91fae766728af866c5e32a0.
  // Copyright (c) 2025 Hans-Kristian Arntzen, provided under the MIT license.
  constexpr char ENCODER_SHADER[] = R"hlsl(
Texture2D<float4>              Source  : register(t0);
RWStructuredBuffer<float>      Wave    : register(u0);
RWStructuredBuffer<float>      Scratch : register(u1);
RWStructuredBuffer<uint>       Quant   : register(u2);

cbuffer Parameters : register(b0)
{
  uint4  Dim0;
  uint4  Op0;
  uint4  Op1;
  float4 Values;
};

uint mirrorCoord(int coord, uint length)
{
  if (coord < 0)
    return uint(-coord);
  if (uint(coord) >= length)
    return 2u * length - 2u - uint(coord);
  return uint(coord);
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

float3 loadYCbCr(uint2 coord, bool bt2020)
{
  coord.x = min(coord.x, Dim0.x - 1u);
  coord.y = min(coord.y, Dim0.y - 1u);
  return rgbToYCbCr(Source.Load(int3(coord, 0)).rgb, bt2020);
}

[numthreads(8, 8, 1)]
void ColorMain(uint3 tid : SV_DispatchThreadID)
{
  const uint component = Op0.z;
  if (tid.x >= Dim0.z || tid.y >= Dim0.w)
    return;

  const bool bt2020 = Op1.z != 0u;
  float3     yuv;
  if (Op0.w != 0u && component != 0u)
  {
    const uint2 src = tid.xy * 2u;
    yuv  = loadYCbCr(src,                       bt2020);
    yuv += loadYCbCr(src + uint2(1u, 0u),       bt2020);
    yuv += loadYCbCr(src + uint2(0u, 1u),       bt2020);
    yuv += loadYCbCr(src + uint2(1u, 1u),       bt2020);
    yuv *= 0.25;
  }
  else
    yuv = loadYCbCr(tid.xy, bt2020);

  float value;
  if (component == 0u)
    value = (16.0 / 255.0 + yuv.x * (219.0 / 255.0)) - 0.5;
  else
    value = yuv[component] * (224.0 / 255.0);
  Wave[tid.y * Dim0.z + tid.x] = value;
}

[numthreads(8, 8, 1)]
void LiftMain(uint3 tid : SV_DispatchThreadID)
{
  if (tid.x >= Op0.x || tid.y >= Op0.y)
    return;

  const bool vertical = Op0.z != 0u;
  const uint coord    = vertical ? tid.y : tid.x;
  if ((coord & 1u) != Op1.x)
    return;

  const uint length = vertical ? Op0.y : Op0.x;
  const uint before = mirrorCoord(int(coord) - 1, length);
  const uint after  = mirrorCoord(int(coord) + 1, length);

  const uint centerIndex = tid.y * Dim0.z + tid.x;
  const uint beforeIndex = vertical ? before * Dim0.z + tid.x :
    tid.y * Dim0.z + before;
  const uint afterIndex  = vertical ? after * Dim0.z + tid.x :
    tid.y * Dim0.z + after;
  if (vertical)
    Scratch[centerIndex] += Values.x *
      (Scratch[beforeIndex] + Scratch[afterIndex]);
  else
    Wave[centerIndex] += Values.x *
      (Wave[beforeIndex] + Wave[afterIndex]);
}

[numthreads(8, 8, 1)]
void HorizontalSplitMain(uint3 tid : SV_DispatchThreadID)
{
  if (tid.x >= Op0.x || tid.y >= Op0.y)
    return;
  const uint half = Op0.x >> 1;
  const bool high = tid.x >= half;
  const uint srcX = high ? 2u * (tid.x - half) + 1u : 2u * tid.x;
  Scratch[tid.y * Dim0.z + tid.x] =
    Wave[tid.y * Dim0.z + srcX] * (high ? Values.z : Values.y);
}

[numthreads(8, 8, 1)]
void VerticalSplitMain(uint3 tid : SV_DispatchThreadID)
{
  if (tid.x >= Op0.x || tid.y >= Op0.y)
    return;
  const uint half = Op0.y >> 1;
  const bool high = tid.y >= half;
  const uint srcY = high ? 2u * (tid.y - half) + 1u : 2u * tid.y;
  Wave[tid.y * Dim0.z + tid.x] =
    Scratch[srcY * Dim0.z + tid.x] * (high ? Values.z : Values.y);
}

int quantize(float value)
{
  const float magnitude = floor(abs(value) * Values.w);
  const int   result    = int(min(magnitude, 32767.0));
  return value < 0.0 ? -result : result;
}

[numthreads(64, 1, 1)]
void QuantMain(uint3 tid : SV_DispatchThreadID)
{
  const uint first = tid.x * 2u;
  if (first >= Op1.y)
    return;
  const int low  = quantize(Wave[first]);
  const int high = first + 1u < Op1.y ? quantize(Wave[first + 1u]) : 0;
  Quant[tid.x] = (uint(low) & 0xffffu) | ((uint(high) & 0xffffu) << 16u);
}
)hlsl";

  struct EncodedBlock
  {
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  struct EncodedFrame
  {
    uint64_t                  configEpoch    = 0;
    uint64_t                  frameID        = 0;
    uint64_t                  captureTimeNs  = 0;
    uint32_t                  deadlineMs     = 0;
    std::vector<uint8_t>      data;
    std::vector<EncodedBlock> blocks;
  };

  struct ShaderParameters
  {
    uint32_t dim0  [4] = {};
    uint32_t op0   [4] = {};
    uint32_t op1   [4] = {};
    float    values[4] = {};
  };

  static_assert(sizeof(ShaderParameters) == 16U * sizeof(uint32_t),
    "Unexpected shader parameter layout");

  class PyroWaveD3D12Encoder
  {
  private:
    enum Pipeline
    {
      PIPELINE_COLOR,
      PIPELINE_LIFT,
      PIPELINE_HORIZONTAL_SPLIT,
      PIPELINE_VERTICAL_SPLIT,
      PIPELINE_QUANT,
      PIPELINE_COUNT,
    };

    struct Plane
    {
      uint32_t width          = 0;
      uint32_t height         = 0;
      uint32_t levels         = 0;
      uint64_t readbackOffset = 0;
      uint64_t readbackBytes  = 0;
    };

    ComPtr<ID3D12Device3>          m_device;
    CD3D12CommandQueue             m_queue;
    ComPtr<ID3D12RootSignature>    m_rootSignature;
    ComPtr<ID3D12PipelineState>    m_pipelines[PIPELINE_COUNT];
    ComPtr<ID3D12DescriptorHeap>   m_descriptors;
    ComPtr<ID3D12Resource>         m_wave;
    ComPtr<ID3D12Resource>         m_scratch;
    ComPtr<ID3D12Resource>         m_quant;
    ComPtr<ID3D12Resource>         m_readback;
    D12Sync                        m_sync;
    TexLease                       m_lease;
    FrameIn                        m_frame;
    FrameCfg                       m_cfg;
    Plane                          m_planes[3]     = {};
    LGNetVideoChromaSubsampling    m_chroma        = 0;
    uint64_t                       m_readbackBytes = 0;
    uint32_t                       m_sequence      = 0;
    bool                           m_ready         = false;
    bool                           m_inFlight      = false;

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
      D3D12_DESCRIPTOR_RANGE ranges[4] = {};

      ranges[0].RangeType                         =
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      ranges[0].NumDescriptors                    = 1;
      ranges[0].BaseShaderRegister                = 0;
      ranges[0].OffsetInDescriptorsFromTableStart = 0;
      for (unsigned i = 1; i < 4; ++i)
      {
        ranges[i].RangeType                         =
          D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[i].NumDescriptors                    = 1;
        ranges[i].BaseShaderRegister                = i - 1;
        ranges[i].OffsetInDescriptorsFromTableStart = i;
      }

      D3D12_ROOT_PARAMETER parameters[2] = {};

      parameters[0].ParameterType                       =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameters[0].ShaderVisibility                    =
        D3D12_SHADER_VISIBILITY_ALL;
      parameters[0].DescriptorTable.NumDescriptorRanges = 4;
      parameters[0].DescriptorTable.pDescriptorRanges   = ranges;
      parameters[1].ParameterType                       =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      parameters[1].ShaderVisibility                    =
        D3D12_SHADER_VISIBILITY_ALL;
      parameters[1].Constants.ShaderRegister            = 0;
      parameters[1].Constants.RegisterSpace             = 0;
      parameters[1].Constants.Num32BitValues             =
        sizeof(ShaderParameters) / sizeof(uint32_t);

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
        "ColorMain",
        "LiftMain",
        "HorizontalSplitMain",
        "VerticalSplitMain",
        "QuantMain",
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

      D3D12_DESCRIPTOR_HEAP_DESC heap = {};

      heap.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
      heap.NumDescriptors = 4;
      heap.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
      hr = m_device->CreateDescriptorHeap(
        &heap, IID_PPV_ARGS(&m_descriptors));
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr,
          "Failed to create the network video descriptor heap");
        return false;
      }
      return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Descriptor(unsigned index) const
    {
      D3D12_CPU_DESCRIPTOR_HANDLE result =
        m_descriptors->GetCPUDescriptorHandleForHeapStart();
      result.ptr += static_cast<SIZE_T>(index) *
        m_device->GetDescriptorHandleIncrementSize(
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      return result;
    }

    void CreateDescriptors(ID3D12Resource * source)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};

      srv.Format                  = ProfileFormat(m_cfg.profile.pixel);
      srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
      srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srv.Texture2D.MipLevels     = 1;
      m_device->CreateShaderResourceView(source, &srv, Descriptor(0));

      ID3D12Resource * resources[] =
      {
        m_wave.Get(),
        m_scratch.Get(),
        m_quant.Get(),
      };
      const uint64_t elements[] =
      {
        m_wave->GetDesc().Width / sizeof(float),
        m_scratch->GetDesc().Width / sizeof(float),
        m_quant->GetDesc().Width / sizeof(uint32_t),
      };
      for (unsigned i = 0; i < 3; ++i)
      {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};

        uav.Format                      = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements          = static_cast<UINT>(elements[i]);
        uav.Buffer.StructureByteStride  = sizeof(uint32_t);
        m_device->CreateUnorderedAccessView(
          resources[i], nullptr, &uav, Descriptor(i + 1));
      }
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
      list->SetPipelineState(m_pipelines[pipeline].Get());
      list->SetComputeRoot32BitConstants(1,
        sizeof(parameters) / sizeof(uint32_t), &parameters, 0);
      list->Dispatch(x, y, 1);
    }

    bool Configure(const FrameCfg& cfg,
      LGNetVideoChromaSubsampling chroma, ID3D12Resource * source)
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

      Plane    planes[3]     = {};
      uint64_t readbackBytes = 0;
      uint64_t maxPixels     = 0;
      for (unsigned i = 0; i < 3; ++i)
      {
        const bool subsampled = chroma == LG_NET_VIDEO_CHROMA_420 && i;
        planes[i].width       = subsampled ? alignedWidth / 2U : alignedWidth;
        planes[i].height      = subsampled ? alignedHeight / 2U : alignedHeight;
        planes[i].levels      = subsampled ?
          PYROWAVE_LEVELS - 1U : PYROWAVE_LEVELS;
        const uint64_t pixels =
          static_cast<uint64_t>(planes[i].width) * planes[i].height;
        planes[i].readbackOffset = readbackBytes;
        planes[i].readbackBytes  = ((pixels + 1U) / 2U) * sizeof(uint32_t);
        if (readbackBytes > UINT64_MAX - planes[i].readbackBytes)
          return false;
        readbackBytes += planes[i].readbackBytes;
        maxPixels      = (std::max)(maxPixels, pixels);
      }
      if (!maxPixels || maxPixels > UINT_MAX || !readbackBytes)
        return false;

      Reset();
      m_device = device;
      m_cfg    = cfg;
      m_chroma = chroma;
      std::copy(std::begin(planes), std::end(planes), std::begin(m_planes));
      m_readbackBytes = readbackBytes;

      if (!m_queue.Init(m_device.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE,
            L"Network Video", CD3D12CommandSlot::FAST, 1) ||
          !CreatePipelines() ||
          !CreateBuffer(m_device.Get(), maxPixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_wave) ||
          !CreateBuffer(m_device.Get(), maxPixels * sizeof(float),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_scratch) ||
          !CreateBuffer(m_device.Get(),
            ((maxPixels + 1U) / 2U) * sizeof(uint32_t),
            D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_quant) ||
          !CreateBuffer(m_device.Get(), readbackBytes,
            D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_FLAG_NONE, m_readback))
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

    void RecordPlane(ID3D12GraphicsCommandList * list, unsigned component)
    {
      static constexpr float    LIFT [4] =
      {
        -1.586134342059924f,
        -0.052980118572961f,
         0.882911075530934f,
         0.443506852043971f,
      };
      static constexpr uint32_t PHASE[4] = { 1, 0, 1, 0 };
      static constexpr float    K        = 1.230174104914001f;
      static constexpr float    INV_K    = 1.0f / K;

      const Plane& plane = m_planes[component];

      ShaderParameters parameters = {};
      parameters.dim0[0]   = m_cfg.width;
      parameters.dim0[1]   = m_cfg.height;
      parameters.dim0[2]   = plane.width;
      parameters.dim0[3]   = plane.height;
      parameters.op0[2]    = component;
      parameters.op0[3]    = m_chroma == LG_NET_VIDEO_CHROMA_420;
      parameters.op1[2]    = m_cfg.profile.signal == FrameSignal::PQ_BT2020;
      parameters.values[1] = INV_K;
      parameters.values[2] = K;
      parameters.values[3] = PYROWAVE_QUANT_SCALE;
      Dispatch(list, PIPELINE_COLOR, parameters,
        (plane.width + 7U) / 8U, (plane.height + 7U) / 8U);
      UAVBarrier(list, m_wave.Get());

      uint32_t activeWidth  = plane.width;
      uint32_t activeHeight = plane.height;
      for (unsigned level = 0; level < plane.levels; ++level)
      {
        parameters.op0[0] = activeWidth;
        parameters.op0[1] = activeHeight;
        parameters.op0[2] = 0;
        for (unsigned pass = 0; pass < 4; ++pass)
        {
          parameters.op1[0]    = PHASE[pass];
          parameters.values[0] = LIFT[pass];
          Dispatch(list, PIPELINE_LIFT, parameters,
            (activeWidth + 7U) / 8U, (activeHeight + 7U) / 8U);
          UAVBarrier(list, m_wave.Get());
        }

        Dispatch(list, PIPELINE_HORIZONTAL_SPLIT, parameters,
          (activeWidth + 7U) / 8U, (activeHeight + 7U) / 8U);
        UAVBarrier(list, m_scratch.Get());

        parameters.op0[2] = 1;
        for (unsigned pass = 0; pass < 4; ++pass)
        {
          parameters.op1[0]    = PHASE[pass];
          parameters.values[0] = LIFT[pass];
          Dispatch(list, PIPELINE_LIFT, parameters,
            (activeWidth + 7U) / 8U, (activeHeight + 7U) / 8U);
          UAVBarrier(list, m_scratch.Get());
        }

        Dispatch(list, PIPELINE_VERTICAL_SPLIT, parameters,
          (activeWidth + 7U) / 8U, (activeHeight + 7U) / 8U);
        UAVBarrier(list, m_wave.Get());
        activeWidth  >>= 1;
        activeHeight >>= 1;
      }

      const uint32_t pixels = plane.width * plane.height;

      parameters.op1[1] = pixels;
      Dispatch(list, PIPELINE_QUANT, parameters,
        ((pixels + 1U) / 2U + 63U) / 64U, 1);
      UAVBarrier(list, m_quant.Get());
      Transition(list, m_quant.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
      list->CopyBufferRegion(m_readback.Get(), plane.readbackOffset,
        m_quant.Get(), 0, plane.readbackBytes);
      Transition(list, m_quant.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    int16_t Coefficient(
      const uint8_t * data, unsigned component, uint32_t x, uint32_t y) const
    {
      const Plane& plane       = m_planes[component];
      const uint64_t index     = static_cast<uint64_t>(y) * plane.width + x;
      const uint64_t wordIndex = index / 2U;
      const uint8_t * source   = data + plane.readbackOffset +
        wordIndex * sizeof(uint32_t);
      const uint32_t word      =
        static_cast<uint32_t>(source[0])       |
        static_cast<uint32_t>(source[1]) << 8  |
        static_cast<uint32_t>(source[2]) << 16 |
        static_cast<uint32_t>(source[3]) << 24;
      return static_cast<int16_t>(index & 1U ? word >> 16 : word);
    }

    static void CoefficientCoord(unsigned linear, uint32_t& x, uint32_t& y)
    {
      y = (linear & 1U) | (((linear >> 3U) & 3U) << 1U);
      x = ((linear >> 1U) & 3U) | (((linear >> 5U) & 1U) << 2U);
    }

    bool AppendBlock(const uint8_t * coefficients, unsigned component,
      uint32_t bandX, uint32_t bandY, uint32_t bandWidth,
      uint32_t bandHeight, uint32_t blockX, uint32_t blockY,
      uint32_t blockIndex, EncodedFrame& output)
    {
      std::array<std::array<int16_t, 64>, 16> values = {};
      uint16_t                                ballot = 0;
      for (unsigned block = 0; block < 16; ++block)
      {
        const uint32_t block8X = block & 3U;
        const uint32_t block8Y = block >> 2U;

        bool nonzero = false;
        for (unsigned linear = 0; linear < 64; ++linear)
        {
          uint32_t localX;
          uint32_t localY;
          CoefficientCoord(linear, localX, localY);
          const uint32_t x = blockX * 32U + block8X * 8U + localX;
          const uint32_t y = blockY * 32U + block8Y * 8U + localY;
          if (x < bandWidth && y < bandHeight)
            values[block][linear] = Coefficient(coefficients, component,
              bandX + x, bandY + y);
          nonzero |= values[block][linear] != 0;
        }
        if (nonzero)
          ballot |= static_cast<uint16_t>(1U << block);
      }
      if (!ballot)
        return true;

      std::vector<uint16_t> controls;
      std::vector<uint8_t>  scales;
      std::vector<uint8_t>  magnitude;
      std::vector<uint8_t>  signs;
      controls.reserve(16);
      scales.reserve(16);
      for (unsigned block = 0; block < 16; ++block)
      {
        if (!(ballot & (1U << block)))
          continue;
        unsigned maxPlanes = 0;
        unsigned planes[8] = {};
        for (unsigned subblock = 0; subblock < 8; ++subblock)
          for (unsigned pixel = 0; pixel < 8; ++pixel)
          {
            const size_t   valueIndex =
              static_cast<size_t>(subblock) * 8U + pixel;
            const int      value      = values[block][valueIndex];
            const uint16_t absolute   = static_cast<uint16_t>(
              value < 0 ? -value : value);
            planes[subblock] = (std::max)(
              planes[subblock], MagnitudeBits(absolute));
            maxPlanes        = (std::max)(maxPlanes, planes[subblock]);
          }

        const unsigned qualityPlanes = maxPlanes > 3U ? maxPlanes - 3U : 0;
        uint16_t       control       = 0;
        for (unsigned subblock = 0; subblock < 8; ++subblock)
        {
          const unsigned coded = planes[subblock] > qualityPlanes ?
            planes[subblock] - qualityPlanes : 0;
          control |= static_cast<uint16_t>((std::min)(coded, 3U) <<
            (subblock * 2U));
        }
        controls.push_back(control);
        scales.push_back(static_cast<uint8_t>(
          (PYROWAVE_QSCALE_CODE << 4U) | qualityPlanes));

        for (unsigned subblock = 0; subblock < 8; ++subblock)
        {
          const unsigned coded =
            ((control >> (subblock * 2U)) & 3U) + qualityPlanes;
          for (unsigned plane = 0; plane < coded; ++plane)
          {
            const unsigned shift = coded - plane - 1U;
            uint8_t        byte  = 0;
            for (unsigned pixel = 0; pixel < 8; ++pixel)
            {
              const size_t   valueIndex =
                static_cast<size_t>(subblock) * 8U + pixel;
              const int      value      = values[block][valueIndex];
              const uint16_t absolute   = static_cast<uint16_t>(
                value < 0 ? -value : value);
              byte |= static_cast<uint8_t>(
                ((absolute >> shift) & 1U) << pixel);
            }
            magnitude.push_back(byte);
          }
        }
      }

      uint8_t  signByte = 0;
      unsigned signBit  = 0;
      for (unsigned block = 0, compact = 0; block < 16; ++block)
      {
        if (!(ballot & (1U << block)))
          continue;
        const uint16_t control       = controls[compact];
        const unsigned qualityPlanes = scales[compact] & 0xfU;
        ++compact;
        for (unsigned subblock = 0; subblock < 8; ++subblock)
        {
          const unsigned coded =
            ((control >> (subblock * 2U)) & 3U) + qualityPlanes;
          if (!coded)
            continue;
          for (unsigned pixel = 0; pixel < 8; ++pixel)
          {
            const size_t valueIndex =
              static_cast<size_t>(subblock) * 8U + pixel;
            const int    value      = values[block][valueIndex];
            if (!value)
              continue;
            if (value < 0)
              signByte |= static_cast<uint8_t>(1U << signBit);
            if (++signBit == 8U)
            {
              signs.push_back(signByte);
              signByte = 0;
              signBit  = 0;
            }
          }
        }
      }
      if (signBit)
        signs.push_back(signByte);

      const size_t start = output.data.size();
      output.data.resize(start + 8U, 0);
      for (uint16_t control : controls)
        AppendLE16(output.data, control);
      output.data.insert(output.data.end(), scales.begin(), scales.end());
      output.data.insert(
        output.data.end(), magnitude.begin(), magnitude.end());
      output.data.insert(output.data.end(), signs.begin(), signs.end());
      while (output.data.size() & 3U)
        output.data.push_back(0);

      const size_t bytes = output.data.size() - start;
      const size_t words = bytes / sizeof(uint32_t);
      if (words > 0xfffU || blockIndex > 0xffffffU)
        return false;
      const uint32_t word0 = ballot |
        static_cast<uint32_t>(words << 16U) |
        (m_sequence << 28U);
      const uint32_t word1 = PYROWAVE_QUANT_CODE | (blockIndex << 8U);
      StoreLE32(output.data.data() + start, word0);
      StoreLE32(output.data.data() + start + 4U, word1);
      output.blocks.push_back(
        { static_cast<uint32_t>(start), static_cast<uint32_t>(bytes) });
      return output.data.size() <= MAX_FRAME_LENGTH &&
        output.blocks.size() <= MAX_BLOCKS;
    }

    bool Packetize(const uint8_t * coefficients, EncodedFrame& output)
    {
      output.data.assign(8, 0);
      output.blocks.clear();
      uint32_t blockIndex = 0;
      for (int level = static_cast<int>(PYROWAVE_LEVELS) - 1;
           level >= 0; --level)
        for (unsigned component = 0; component < 3; ++component)
        {
          if (m_chroma == LG_NET_VIDEO_CHROMA_420 && component && !level)
            continue;
          const uint32_t localLevel =
            m_chroma == LG_NET_VIDEO_CHROMA_420 && component ?
              static_cast<uint32_t>(level - 1) :
              static_cast<uint32_t>(level);
          const uint32_t bandWidth  =
            m_planes[component].width >> (localLevel + 1U);
          const uint32_t bandHeight =
            m_planes[component].height >> (localLevel + 1U);
          const unsigned firstBand  =
            level == static_cast<int>(PYROWAVE_LEVELS) - 1 ? 0 : 1;
          for (unsigned band = firstBand; band < 4; ++band)
          {
            const uint32_t bandX   = band & 1U ? bandWidth : 0;
            const uint32_t bandY   = band & 2U ? bandHeight : 0;
            const uint32_t blocksX = (bandWidth + 31U) / 32U;
            const uint32_t blocksY = (bandHeight + 31U) / 32U;
            for (uint32_t y = 0; y < blocksY; ++y)
              for (uint32_t x = 0; x < blocksX; ++x)
              {
                if (blockIndex >= MAX_BLOCKS ||
                    !AppendBlock(coefficients, component, bandX, bandY,
                      bandWidth, bandHeight, x, y, blockIndex, output))
                  return false;
                ++blockIndex;
              }
          }
        }

      if (blockIndex > MAX_BLOCKS || output.blocks.empty() ||
          output.data.size() > MAX_FRAME_LENGTH)
        return false;

      const uint32_t word0 = (m_cfg.width - 1U) |
        ((m_cfg.height - 1U) << 14U) | (m_sequence << 28U) |
        (1U << 31U);
      const bool     hdr   =
        m_cfg.profile.signal == FrameSignal::PQ_BT2020;
      const uint32_t word1 =
        static_cast<uint32_t>(output.blocks.size()) |
        ((m_chroma == LG_NET_VIDEO_CHROMA_444 ? 1U : 0U) << 26U) |
        (static_cast<uint32_t>(hdr) << 27U) |
        (static_cast<uint32_t>(hdr) << 28U) |
        (static_cast<uint32_t>(hdr) << 29U) |
        (1U << 30U);
      StoreLE32(output.data.data(), word0);
      StoreLE32(output.data.data() + 4U, word1);
      output.blocks[0].offset  = 0;
      output.blocks[0].length += 8U;

      return true;
    }

  public:
    ~PyroWaveD3D12Encoder()
    {
      Reset();
    }

    bool Busy() const
    {
      return m_inFlight;
    }

    bool Submit(FrameIn frame, TexLease lease, const FrameCfg& cfg,
      LGNetVideoChromaSubsampling chroma)
    {
      if (m_inFlight || !lease || !lease.Get() ||
          lease->State() != D3D12_RESOURCE_STATE_COMMON)
        return false;
      ID3D12Resource * source = lease->Get();
      if (!m_ready || !SameCfg(m_cfg, cfg) || m_chroma != chroma ||
          !SameDevice(source))
        if (!Configure(cfg, chroma, source))
          return false;

      CD3D12CommandSlot * command = m_queue.Try(0);
      if (!command)
        return false;
      if (!lease->Wait(*command))
      {
        command->Cancel();
        return false;
      }

      CreateDescriptors(source);
      ID3D12DescriptorHeap * heaps[] = { m_descriptors.Get() };

      ComPtr<ID3D12GraphicsCommandList> list = command->GetGfxList();
      list->SetDescriptorHeaps(1, heaps);
      list->SetComputeRootSignature(m_rootSignature.Get());
      list->SetComputeRootDescriptorTable(
        0, m_descriptors->GetGPUDescriptorHandleForHeapStart());
      for (unsigned component = 0; component < 3; ++component)
        RecordPlane(list.Get(), component);
      if (!command->Execute(&m_sync))
        return false;

      m_frame    = std::move(frame);
      m_lease    = std::move(lease);
      m_inFlight = true;
      return true;
    }

    D12SyncState State() const
    {
      return m_inFlight ? m_sync.State() : D12SyncState::FAILED;
    }

    bool Complete(uint64_t configEpoch, uint64_t frameID,
      EncodedFrame& output)
    {
      if (!m_inFlight || m_sync.State() != D12SyncState::READY)
        return false;

      const D3D12_RANGE readRange =
      {
        0,
        static_cast<SIZE_T>(m_readbackBytes),
      };
      void          * mapped = nullptr;
      const HRESULT   hr     = m_readback->Map(0, &readRange, &mapped);
      const uint8_t * data   = static_cast<const uint8_t *>(mapped);
      if (FAILED(hr) || !data)
      {
        m_inFlight = false;
        m_lease.Reset();
        return false;
      }

      output = {};

      const bool result = Packetize(data, output);

      const D3D12_RANGE writeRange = { 0, 0 };
      m_readback->Unmap(0, &writeRange);
      if (result)
      {
        output.configEpoch    = configEpoch;
        output.frameID        = frameID;
        output.captureTimeNs  = MonotonicNs();
        output.deadlineMs     = MAX_FRAME_LATENCY_MS;
      }
      m_sequence = (m_sequence + 1U) & 7U;
      m_inFlight = false;
      m_lease.Reset();
      m_sync = {};
      return result;
    }

    void Reset()
    {
      m_queue.WaitForIdle();
      m_inFlight = false;
      m_ready    = false;
      m_lease.Reset();
      m_sync = {};
      m_readback.Reset();
      m_quant.Reset();
      m_scratch.Reset();
      m_wave.Reset();
      m_descriptors.Reset();
      for (ComPtr<ID3D12PipelineState>& pipeline : m_pipelines)
        pipeline.Reset();
      m_rootSignature.Reset();
      m_queue.DeInit();
      m_device.Reset();
      m_cfg           = {};
      m_chroma        = 0;
      m_readbackBytes = 0;
      std::fill(std::begin(m_planes), std::end(m_planes), Plane {});
    }
  };
}

struct CNetworkVideo::Impl
{
  struct PeerState
  {
    NetworkVideoPeer     peer                   = {};
    NetworkVideoPeerInfo info                   = {};
    LGNetVideoSubscribe  subscription           = {};
    LGNetVideoFeedback   feedback               = {};
    uint64_t             serial                 = 0;
    uint64_t             componentEpoch         = 0;
    uint64_t             statusSequence         = 0;
    uint64_t             lastFrameID            = 0;
    uint64_t             lastCompleteID         = 0;
    uint64_t             lastCaptureTimeNs      = 0;
    uint64_t             sendReadyGeneration[3] = {};
    uint32_t             droppedFrames          = 0;
    uint8_t              blockedDeliveries      = 0;
    bool                 subscribed             = false;
    bool                 configured             = false;
    bool                 congested              = false;
    bool                 keyframePending        = false;
    bool                 sendFailed             = false;
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
    uint64_t                              serial         = 0;
    NetworkVideoPeer                      peer           = {};
    NetworkVideoDelivery                  delivery       =
      NetworkVideoDelivery::RELIABLE;
    uint16_t                              messageType    = 0;
    LGNetEnvelopeFlags                    flags          =
      LG_NET_ENVELOPE_FINAL;
    uint64_t                              componentEpoch = 0;
    uint64_t                              requestID      = 0;
    uint64_t                              frameID        = 0;
    std::shared_ptr<std::vector<uint8_t>> payload;
    std::shared_ptr<const EncodedFrame>    encoded;
    uint32_t                              fragmentIndex  = 0;
    uint32_t                              fragmentCount  = 0;
    uint32_t                              blockIndex     = 0;
    uint32_t                              blockOffset    = 0;
    uint32_t                              payloadLimit   = 0;
  };

  CSRWLock m_processLock;
  CSRWLock m_senderCallbackLock;
  CSRWLock m_lock;

  INetworkVideoSender * m_sender   = nullptr;
  bool                  m_started  = false;
  bool                  m_stopped  = false;
  bool                  m_failed   = false;
  bool                  m_changing = false;

  FrameCfg m_activeCfg  = {};
  FrameCfg m_pendingCfg = {};
  bool     m_active     = false;
  bool     m_pending    = false;

  uint64_t m_configEpoch    = 0;
  uint64_t m_nextFrameID    = 0;
  uint64_t m_nextPeerSerial = 0;
  uint64_t m_nextSendSerial = 0;

  std::vector<PeerState>   m_peers;
  std::deque<PendingFrame> m_frames;
  std::deque<SendItem>     m_sends;
  size_t                   m_sendBytes = 0;

  PyroWaveD3D12Encoder m_encoder;

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

  LGNetVideoChromaSubsampling ChromaNL() const
  {
    if (!m_active)
      return 0;
    if (m_activeCfg.profile.signal == FrameSignal::PQ_BT2020 ||
        (m_activeCfg.width & 1U) || (m_activeCfg.height & 1U))
      return LG_NET_VIDEO_CHROMA_444;
    return LG_NET_VIDEO_CHROMA_420;
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
      chroma == LG_NET_VIDEO_CHROMA_420 ?
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
      request.maxFrameLength >= 8U &&
      (!(request.flags & LG_NET_VIDEO_SUBSCRIBE_REQUIRE_HDR) || hdr);
  }

  bool QueueNL(const NetworkVideoPeer& peer,
    NetworkVideoDelivery delivery, uint16_t messageType,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    uint64_t frameID, std::shared_ptr<std::vector<uint8_t>> payload)
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
      m_sendBytes     += bytes;
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
    return Multiply(encoded.blocks.size(), sizeof(EncodedBlock), blockBytes) &&
      Add(encoded.data.size(), blockBytes, bytes);
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
        DeliverySupported(peer.info, NetworkVideoDelivery::DATAGRAM))
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
    peer.sendFailed        = false;
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

    peer.statusSequence = status.statusSequence;
    return QueueFixedNL<LGNetVideoStatus, lgNetVideoStatusEncode>(
      peer.peer, NetworkVideoDelivery::RELIABLE,
      LG_NET_VIDEO_MESSAGE_STATUS,
      LG_NET_ENVELOPE_FINAL | (requestID ?
        LG_NET_ENVELOPE_RESPONSE : 0),
      peer.componentEpoch, requestID, 0, status,
      LG_NET_VIDEO_STATUS_WIRE_SIZE);
  }

  void DropPeerFramesNL(PeerState& peer)
  {
    for (auto item = m_sends.begin(); item != m_sends.end();)
      if (item->frameID && SamePeer(item->peer, peer.peer))
      {
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
        ReleaseSendAccountingNL(*item);
        item = m_sends.erase(item);
      }
      else
        ++item;
  }

  bool QueueScheduleNL(PeerState& peer, const EncodedFrame& frame)
  {
    const uint32_t deadlineMs     = (std::min)(
      frame.deadlineMs, peer.subscription.maxFrameLatencyMs);
    const uint64_t presentationNs = frame.captureTimeNs +
      static_cast<uint64_t>(deadlineMs) * UINT64_C(1000000);

    LGNetVideoSchedule schedule = {};
    schedule.streamID                = STREAM_ID;
    schedule.flags                   = LG_NET_VIDEO_SCHEDULE_PRESENT |
      LG_NET_VIDEO_SCHEDULE_DROP_IF_LATE;
    schedule.configEpoch             = frame.configEpoch;
    schedule.frameID                 = frame.frameID;
    schedule.captureTimestampNs      = frame.captureTimeNs;
    schedule.presentationTimestampNs = presentationNs;
    schedule.deadlineTimestampNs     = presentationNs;
    return QueueFixedNL<LGNetVideoSchedule,
      lgNetVideoScheduleEncode>(peer.peer,
        NetworkVideoDelivery::RELIABLE, LG_NET_VIDEO_MESSAGE_SCHEDULE,
        LG_NET_ENVELOPE_FINAL, frame.configEpoch, 0, frame.frameID,
        schedule, LG_NET_VIDEO_SCHEDULE_WIRE_SIZE);
  }

  bool QueueWholeFrameNL(PeerState& peer, const EncodedFrame& encoded,
    NetworkVideoDelivery delivery)
  {
    const uint32_t deadlineMs = (std::min)(
      encoded.deadlineMs, peer.subscription.maxFrameLatencyMs);

    LGNetVideoFrame frame = {};
    frame.streamID                = STREAM_ID;
    frame.codec                   = LG_NET_VIDEO_CODEC_PYROWAVE;
    frame.flags                   = LG_NET_VIDEO_FRAME_KEYFRAME;
    frame.configEpoch             = encoded.configEpoch;
    frame.frameID                 = encoded.frameID;
    frame.captureTimestampNs      = encoded.captureTimeNs;
    frame.presentationTimestampNs = encoded.captureTimeNs +
      static_cast<uint64_t>(deadlineMs) * UINT64_C(1000000);
    frame.encodedLength           = static_cast<uint32_t>(encoded.data.size());
    frame.blockCount              = static_cast<uint32_t>(encoded.blocks.size());
    frame.fragmentCount           = 1;
    frame.deadlineMs              = deadlineMs;
    frame.data                    = encoded.data.data();

    const size_t size = lgNetVideoFrameSize(&frame);
    if (!size || size > UINT32_MAX)
      return false;

    std::shared_ptr<std::vector<uint8_t>> payload;
    try
    {
      payload = std::make_shared<std::vector<uint8_t>>(size);
    }
    catch (...)
    {
      return false;
    }
    if (!lgNetVideoFrameEncode(
          payload->data(), payload->size(), &frame))
      return false;
    return QueueNL(peer.peer, delivery, LG_NET_VIDEO_MESSAGE_FRAME,
      LG_NET_ENVELOPE_FINAL, encoded.configEpoch, 0, encoded.frameID,
      std::move(payload));
  }

  bool QueueFragmentsNL(PeerState& peer,
    std::shared_ptr<const EncodedFrame> encoded,
    NetworkVideoDelivery delivery, uint32_t maxPayload)
  {
    if (!encoded || maxPayload <= LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE)
      return false;
    const uint32_t payloadLimit = (std::min)(
      maxPayload - LG_NET_VIDEO_FRAGMENT_HEADER_WIRE_SIZE,
      static_cast<uint32_t>(LG_NET_VIDEO_MAX_FRAGMENT_LENGTH));
    if (!payloadLimit)
      return false;

    uint64_t fragmentCount64 = 0;
    for (const EncodedBlock& block : encoded->blocks)
      fragmentCount64 +=
        (static_cast<uint64_t>(block.length) + payloadLimit - 1U) /
        payloadLimit;
    if (!fragmentCount64 || fragmentCount64 > LG_NET_VIDEO_MAX_FRAGMENTS ||
        fragmentCount64 > UINT32_MAX)
      return false;
    const uint32_t fragmentCount = static_cast<uint32_t>(fragmentCount64);

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
      item.messageType    = LG_NET_VIDEO_MESSAGE_FRAME_FRAGMENT;
      item.flags          = LG_NET_ENVELOPE_FINAL;
      item.componentEpoch = encoded->configEpoch;
      item.frameID        = encoded->frameID;
      item.encoded        = std::move(encoded);
      item.fragmentCount  = fragmentCount;
      item.payloadLimit   = payloadLimit;
      const uint64_t serial = item.serial;
      m_sends.push_back(std::move(item));
      m_nextSendSerial = serial;
      m_sendBytes     += bytes;
    }
    catch (...)
    {
      return false;
    }
    return true;
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

    LGNetVideoFragment fragment = {};
    fragment.streamID      = STREAM_ID;
    fragment.configEpoch   = item.encoded->configEpoch;
    fragment.frameID       = item.encoded->frameID;
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
    const std::shared_ptr<const EncodedFrame>& frame)
  {
    if (!frame || !peer.configured ||
        peer.componentEpoch != frame->configEpoch ||
        frame->data.empty() || frame->data.size() >
          peer.subscription.maxFrameLength)
      return false;

    DropPeerFramesNL(peer);
    if (!QueueScheduleNL(peer, *frame))
      return false;

    const bool     datagrams =
      (peer.subscription.flags & LG_NET_VIDEO_SUBSCRIBE_ALLOW_DATAGRAMS) &&
      DeliverySupported(peer.info, NetworkVideoDelivery::DATAGRAM);
    const bool     bulk      =
      DeliverySupported(peer.info, NetworkVideoDelivery::BULK);
    const uint32_t fullSize  = static_cast<uint32_t>(frame->data.size()) +
      LG_NET_VIDEO_FRAME_HEADER_WIRE_SIZE;

    NetworkVideoDelivery delivery = NetworkVideoDelivery::RELIABLE;
    bool                 result;
    if (bulk)
    {
      delivery = NetworkVideoDelivery::BULK;
      result   = fullSize <= peer.info.maxBulkPayload ?
        QueueWholeFrameNL(peer, *frame, delivery) :
        QueueFragmentsNL(
          peer, frame, delivery, DeliveryPayload(peer.info, delivery));
    }
    else if (!datagrams && fullSize <= peer.info.maxReliablePayload)
      result = QueueWholeFrameNL(
        peer, *frame, NetworkVideoDelivery::RELIABLE);
    else
    {
      if (datagrams)
        delivery = NetworkVideoDelivery::DATAGRAM;
      const uint32_t maxPayload = DeliveryPayload(peer.info, delivery);
      result = DeliverySupported(peer.info, delivery) &&
        QueueFragmentsNL(peer, frame, delivery, maxPayload);
    }

    if (!result)
    {
      DropPeerFramesNL(peer);
      ++peer.droppedFrames;
      peer.congested = true;
      return false;
    }
    peer.lastFrameID       = frame->frameID;
    peer.lastCaptureTimeNs = frame->captureTimeNs;
    peer.keyframePending   = false;
    return true;
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

  void PublishFrame(EncodedFrame&& frame)
  {
    std::shared_ptr<const EncodedFrame> encoded;
    try
    {
      encoded = std::make_shared<EncodedFrame>(std::move(frame));
    }
    catch (...)
    {
      return;
    }

    CSRWExclusiveLock lock(m_lock);
    if (!m_started || m_stopped || !m_active ||
        encoded->configEpoch != m_configEpoch)
      return;
    for (PeerState& peer : m_peers)
      if (peer.subscribed && peer.configured &&
          PeerFrameDueNL(peer, *encoded))
      {
        if (!QueueFrameNL(peer, encoded))
          QueueStatusNL(peer, LG_NET_VIDEO_STATE_STALLED, 2);
        else
          QueueStatusNL(peer, LG_NET_VIDEO_STATE_STREAMING, 0);
      }
  }

  bool SubmitPending()
  {
    PendingFrame pending;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_encoder.Busy() || m_frames.empty() || !m_started ||
          m_stopped || !m_active || !HasSubscribersNL())
        return false;
      pending = std::move(m_frames.front());
      m_frames.pop_front();
      if (pending.configEpoch != m_configEpoch ||
          !SameCfg(pending.cfg, m_activeCfg))
        return true;
    }

    if (m_encoder.Submit(std::move(pending.frame),
          std::move(pending.lease), pending.cfg,
          static_cast<LGNetVideoChromaSubsampling>(
            pending.cfg.profile.signal == FrameSignal::PQ_BT2020 ||
              (pending.cfg.width & 1U) || (pending.cfg.height & 1U) ?
                LG_NET_VIDEO_CHROMA_444 : LG_NET_VIDEO_CHROMA_420)))
      return true;

    CSRWExclusiveLock lock(m_lock);
    m_failed = true;
    for (PeerState& peer : m_peers)
      if (peer.subscribed)
        QueueStatusNL(peer, LG_NET_VIDEO_STATE_ERROR, 3);
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
    size_t skipped = 0;
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

      std::vector<uint8_t>         fragmentPayload;
      const bool                   prepared    = item.payload ||
        BuildFragmentPayload(item, fragmentPayload);
      const void                 * payload     = item.payload ?
        item.payload->data() : fragmentPayload.data();
      const size_t                 payloadSize = item.payload ?
        item.payload->size() : fragmentPayload.size();
      const NetworkVideoSendResult result      = prepared ?
        sender->SendVideo(item.peer, item.delivery, item.messageType,
          item.flags, item.componentEpoch, item.requestID, payload,
          static_cast<uint32_t>(payloadSize)) :
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
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  m_impl->m_sender    = nullptr;
  m_impl->m_sendBytes = 0;
  m_impl->m_sends.clear();
  m_impl->m_peers.clear();
}

bool CNetworkVideo::Start()
{
  if (!m_impl)
    return false;
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_stopped || m_impl->m_failed)
    return false;
  m_impl->m_started = true;
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
    m_impl->m_stopped   = true;
    m_impl->m_started   = false;
    m_impl->m_active    = false;
    m_impl->m_pending   = false;
    m_impl->m_changing  = false;
    m_impl->m_sendBytes = 0;
    m_impl->m_frames.clear();
    m_impl->m_sends.clear();
    for (Impl::PeerState& peer : m_impl->m_peers)
    {
      peer.configured     = false;
      peer.componentEpoch = 0;
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
      info.maxReliablePayload < LG_NET_VIDEO_STATUS_WIRE_SIZE)
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
    m_impl->m_frames.clear();
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
          subscribe.maxFrameLength > MAX_FRAME_LENGTH ||
          (subscribe.preferredCodec &&
            subscribe.preferredCodec != LG_NET_VIDEO_CODEC_PYROWAVE))
        return NetworkVideoPacketResult::INVALID;

      m_impl->DropPeerSendsNL(*peer);
      peer->subscription    = subscribe;
      peer->subscribed      = true;
      peer->configured      = false;
      peer->componentEpoch  = m_impl->m_active ?
        m_impl->m_configEpoch : 0;
      peer->congested       = false;
      peer->keyframePending = true;
      peer->sendFailed      = false;
      if (m_impl->m_active)
      {
        if (!m_impl->QueueConfigNL(
              *peer, packet.envelope.requestID))
          return NetworkVideoPacketResult::INVALID;
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
          control.streamID != STREAM_ID ||
          (control.configEpoch &&
            control.configEpoch != peer->componentEpoch))
        return NetworkVideoPacketResult::INVALID;
      m_impl->DropPeerSendsNL(*peer);
      peer->subscribed      = false;
      peer->configured      = false;
      peer->componentEpoch  = 0;
      peer->keyframePending = false;
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
          control.streamID != STREAM_ID || !peer->subscribed ||
          (control.configEpoch &&
            control.configEpoch != peer->componentEpoch))
        return NetworkVideoPacketResult::INVALID;
      peer->keyframePending = true;
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
          feedback.streamID != STREAM_ID || !peer->subscribed ||
          packet.envelope.componentEpoch != peer->componentEpoch ||
          feedback.highestSeenFrameID > peer->lastFrameID)
        return NetworkVideoPacketResult::INVALID;
      peer->feedback       = feedback;
      peer->lastCompleteID = feedback.lastCompleteFrameID;
      peer->congested      =
        feedback.flags & LG_NET_VIDEO_FEEDBACK_CONGESTED;
      if (feedback.flags & (LG_NET_VIDEO_FEEDBACK_REQUEST_KEYFRAME |
            LG_NET_VIDEO_FEEDBACK_DECODER_STALLED))
        peer->keyframePending = true;
      break;
    }

    default:
      return NetworkVideoPacketResult::INVALID;
  }

  lock.Unlock();
  CSRWExclusiveLock processLock =
    CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (processLock)
    m_impl->Flush(32);
  return NetworkVideoPacketResult::HANDLED;
}

void CNetworkVideo::Process(uint64_t nowMs)
{
  (void)nowMs;
  if (!m_impl)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);

  if (m_impl->m_encoder.Busy())
  {
    const D12SyncState state = m_impl->m_encoder.State();
    if (state == D12SyncState::READY)
    {
      uint64_t configEpoch;
      uint64_t frameID;
      {
        CSRWExclusiveLock lock(m_impl->m_lock);
        configEpoch = m_impl->m_configEpoch;
        frameID     = Next64(m_impl->m_nextFrameID);

        m_impl->m_nextFrameID = frameID;
      }
      EncodedFrame frame;
      if (m_impl->m_encoder.Complete(configEpoch, frameID, frame))
        m_impl->PublishFrame(std::move(frame));
      else
      {
        CSRWExclusiveLock lock(m_impl->m_lock);
        m_impl->m_failed = true;
        for (Impl::PeerState& peer : m_impl->m_peers)
          if (peer.subscribed)
            m_impl->QueueStatusNL(
              peer, LG_NET_VIDEO_STATE_ERROR, 4);
      }
    }
    else if (state == D12SyncState::FAILED)
    {
      m_impl->m_encoder.Reset();
      CSRWExclusiveLock lock(m_impl->m_lock);
      m_impl->m_failed = true;
      for (Impl::PeerState& peer : m_impl->m_peers)
        if (peer.subscribed)
          m_impl->QueueStatusNL(peer, LG_NET_VIDEO_STATE_ERROR, 5);
    }
  }

  if (!m_impl->m_encoder.Busy())
    m_impl->SubmitPending();
  m_impl->Flush();
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
  CSRWExclusiveLock processLock =
    CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (processLock)
    m_impl->Flush();
}

const FrameProfile * CNetworkVideo::Profiles(unsigned& count) const
{
  count = ARRAYSIZE(VIDEO_PROFILES);
  return VIDEO_PROFILES;
}

std::shared_ptr<const FrameCaps> CNetworkVideo::GetFrameCaps() const
{
  static const std::shared_ptr<const FrameCaps> caps(
    new (std::nothrow) NetworkVideoFrameCaps());
  return caps;
}

CfgResult CNetworkVideo::Probe(const FrameCfg& cfg) const
{
  if (!m_impl)
    return CfgResult::FAILED;
  CSRWSharedLock lock(m_impl->m_lock);
  if (m_impl->m_stopped || m_impl->m_failed)
    return CfgResult::FAILED;
  if (cfg.mode != GpuMode::HARDWARE || !cfg.width || !cfg.height ||
      cfg.width > MAX_WIDTH || cfg.height > MAX_HEIGHT ||
      !SupportedProfile(cfg.profile))
    return CfgResult::NEXT;
  const uint32_t alignedWidth  = (std::max)(
    Align(cfg.width, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
  const uint32_t alignedHeight = (std::max)(
    Align(cfg.height, PYROWAVE_ALIGNMENT), PYROWAVE_MIN_DIMENSION);
  if (alignedWidth > MAX_WIDTH || alignedHeight > MAX_HEIGHT)
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
    m_impl->m_activeCfg   = m_impl->m_pendingCfg;
    m_impl->m_pendingCfg  = {};
    m_impl->m_active      = true;
    m_impl->m_pending     = false;
    m_impl->m_changing    = false;
    m_impl->m_configEpoch = Next64(m_impl->m_configEpoch);
    m_impl->m_frames.clear();
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
      !m_impl->m_active || m_impl->m_changing ||
      !m_impl->HasSubscribersNL())
    return PushResult::BUSY;

  const CFrameTex          * texture = lease.Get();
  const D3D12_RESOURCE_DESC  desc    = texture->Get()->GetDesc();
  if (frame.graph != texture->graph || frame.desc.node != texture->node ||
      !Frame::Same(frame.desc.profile, m_impl->m_activeCfg.profile) ||
      !Frame::Same(texture->profile, m_impl->m_activeCfg.profile) ||
      desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.Width != m_impl->m_activeCfg.width ||
      desc.Height != m_impl->m_activeCfg.height ||
      desc.Format != ProfileFormat(m_impl->m_activeCfg.profile.pixel) ||
      texture->State() != D3D12_RESOURCE_STATE_COMMON)
    return PushResult::STALE;

  try
  {
    if (m_impl->m_frames.size() >= MAX_PENDING_FRAMES)
      m_impl->m_frames.pop_front();
    Impl::PendingFrame pending;
    pending.frame       = std::move(frame);
    pending.lease       = std::move(lease);
    pending.cfg         = m_impl->m_activeCfg;
    pending.configEpoch = m_impl->m_configEpoch;
    m_impl->m_frames.push_back(std::move(pending));
  }
  catch (...)
  {
    return PushResult::FAILED;
  }
  return PushResult::ACCEPTED;
}

PushResult CNetworkVideo::Push(FrameIn frame, D11Lease lease) noexcept
{
  (void)frame;
  (void)lease;
  return PushResult::REJECTED;
}
