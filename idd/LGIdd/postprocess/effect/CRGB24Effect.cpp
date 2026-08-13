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

#include "CRGB24Effect.h"

#include "CDebug.h"
#include "CSRWLock.h"
#include "config/CSettings.h"
#include "capture/FramePipeline.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>

using namespace PostProcessUtil;

static_assert(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 256,
  "RGB24 shader row alignment must match D3D12");

struct CRGB24Effect::State
{
  enum class Phase
  {
    DISABLED,
    NATIVE_WARMUP,
    NATIVE_SAMPLE,
    PACKED_WARMUP,
    PACKED_SAMPLE,
    LOCKED_NATIVE,
    LOCKED_PACKED,
  };

  struct FormatKey
  {
    D3D12_RESOURCE_DIMENSION                  resourceDimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    UINT64                                    resourceWidth     = 0;
    UINT                                      resourceHeight    = 0;
    DXGI_FORMAT                               resourceFormat    = DXGI_FORMAT_UNKNOWN;
    unsigned                                  width             = 0;
    unsigned                                  height            = 0;
    FrameType                                 format            = FRAME_TYPE_INVALID;
    bool                                      hdr               = false;
    bool                                      hdrPQ             = false;
    std::shared_ptr<const D12ColorTransform>  colorTransform;
  };

  static const unsigned WarmupCount = CAPTURE_PIPELINE_SLOTS;
  static const unsigned SampleCount = 64;
  static const unsigned TrimCount   = SampleCount / 8;

  CSRWLock   lock;
  Phase      phase                = Phase::DISABLED;
  FormatKey  format               = {};
  bool       formatValid          = false;
  uint64_t   generation           = 0;
  unsigned   warmups              = 0;
  unsigned   sampleCount          = 0;
  uint64_t   nativeMean           = 0;
  uint64_t   samples[SampleCount] = {};

  static bool IsEligible(const D12FrameFormat& format)
  {
    if (format.hdr ||
        format.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        format.desc.Format    != DXGI_FORMAT_B8G8R8A8_UNORM)
      return false;

    return !D12::Transform(format.colorTransform);
  }

  bool WantsPackedLocked() const
  {
    return phase == Phase::PACKED_WARMUP ||
           phase == Phase::PACKED_SAMPLE ||
           phase == Phase::LOCKED_PACKED;
  }

  bool IsBenchmarkingLocked() const
  {
    return phase == Phase::NATIVE_WARMUP ||
           phase == Phase::NATIVE_SAMPLE ||
           phase == Phase::PACKED_WARMUP ||
           phase == Phase::PACKED_SAMPLE;
  }

  void ResetStageLocked()
  {
    warmups     = 0;
    sampleCount = 0;
    std::memset(samples, 0, sizeof(samples));
  }

  uint64_t TrimmedMeanLocked()
  {
    std::sort(samples, samples + SampleCount);

    uint64_t total = 0;
    for (unsigned i = TrimCount; i < SampleCount - TrimCount; ++i)
      total += samples[i];

    return total / (SampleCount - TrimCount * 2);
  }

  void Update(const D12FrameFormat& next)
  {
    CSRWExclusiveLock guard(lock);

    const bool formatChanged = !formatValid ||
      format.resourceDimension != next.desc.Dimension        ||
      format.resourceWidth     != next.desc.Width            ||
      format.resourceHeight    != next.desc.Height           ||
      format.resourceFormat    != next.desc.Format           ||
      format.width             != next.width                 ||
      format.height            != next.height                ||
      format.format            != next.format                ||
      format.hdr               != next.hdr                   ||
      format.hdrPQ             != next.hdrPQ                 ||
      format.colorTransform    != next.colorTransform;

    if (formatChanged)
    {
      format.resourceDimension = next.desc.Dimension;
      format.resourceWidth     = next.desc.Width;
      format.resourceHeight    = next.desc.Height;
      format.resourceFormat    = next.desc.Format;
      format.width             = next.width;
      format.height            = next.height;
      format.format            = next.format;
      format.hdr               = next.hdr;
      format.hdrPQ             = next.hdrPQ;
      format.colorTransform    = next.colorTransform;
      formatValid              = true;

      phase      = IsEligible(next) ?
        Phase::NATIVE_WARMUP : Phase::DISABLED;
      nativeMean = 0;
      ++generation;
      ResetStageLocked();
    }

    switch (phase)
    {
      case Phase::NATIVE_WARMUP:
        if (warmups >= WarmupCount)
        {
          phase = Phase::NATIVE_SAMPLE;
          ++generation;
          ResetStageLocked();
        }
        break;

      case Phase::NATIVE_SAMPLE:
        if (sampleCount >= SampleCount)
        {
          nativeMean = TrimmedMeanLocked();
          phase      = Phase::PACKED_WARMUP;
          ++generation;
          ResetStageLocked();
        }
        break;

      case Phase::PACKED_WARMUP:
        if (warmups >= WarmupCount)
        {
          phase = Phase::PACKED_SAMPLE;
          ++generation;
          ResetStageLocked();
        }
        break;

      case Phase::PACKED_SAMPLE:
        if (sampleCount >= SampleCount)
        {
          const uint64_t packedMean        = TrimmedMeanLocked();
          const uint64_t relativeThreshold = nativeMean / 20;
          const uint64_t threshold         = relativeThreshold > 50000ULL ?
            relativeThreshold : 50000ULL;
          // Prefer the bandwidth saving unless native is meaningfully faster.
          const bool     usePacked         = packedMean <= nativeMean ||
            packedMean - nativeMean <= threshold;

          DEBUG_INFO(
            "RGB24 benchmark: native=%llu us, packed=%llu us, selected=%s",
            (unsigned long long)(nativeMean / 1000),
            (unsigned long long)(packedMean / 1000),
            usePacked ? "packed" : "native");

          phase = usePacked ?
            Phase::LOCKED_PACKED : Phase::LOCKED_NATIVE;
          ++generation;
          ResetStageLocked();
        }
        break;

      default:
        break;
    }

  }

  bool WantsPacked()
  {
    CSRWSharedLock guard(lock);
    const bool result = WantsPackedLocked();
    return result;
  }

  bool IsBenchmarking()
  {
    CSRWSharedLock guard(lock);
    const bool result = IsBenchmarkingLocked();
    return result;
  }

  uint64_t GetTimingToken(bool packed)
  {
    CSRWSharedLock guard(lock);

    const uint64_t result = IsBenchmarkingLocked() &&
      packed == WantsPackedLocked() ? generation : 0;

    return result;
  }

  void Reject()
  {
    CSRWExclusiveLock guard(lock);
    if (WantsPackedLocked())
    {
      phase = Phase::LOCKED_NATIVE;
      ++generation;
      ResetStageLocked();
    }
  }

  void RecordTiming(uint64_t token, bool fullCopy, uint64_t totalTime)
  {
    CSRWExclusiveLock guard(lock);

    if (token == generation && fullCopy)
      switch (phase)
      {
        case Phase::NATIVE_WARMUP:
        case Phase::PACKED_WARMUP:
          ++warmups;
          break;

        case Phase::NATIVE_SAMPLE:
        case Phase::PACKED_SAMPLE:
          if (sampleCount < SampleCount)
            samples[sampleCount++] = totalTime;
          break;

        default:
          break;
      }

  }
};

bool CRGB24Effect::Init(const ComPtr<ID3D12Device3>& device)
{
  if (!g_settings.ReadBoolValue(L"AllowRGB24", true))
    return false;

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors                    = 1;
  ranges[0].BaseShaderRegister                = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors                    = 1;
  ranges[1].BaseShaderRegister                = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  const char * shader =
    "Texture2D<float4> src : register(t0);\n"
    "RWByteAddressBuffer dst : register(u0);\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", " POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  uint width, height;\n"
    "  src.GetDimensions(width, height);\n"
    "  uint rowBytes = width * 3;\n"
    "  uint dataWidth = ((rowBytes + 255) & ~255u) / 4;\n"
    "  if (dt.x >= dataWidth || dt.y >= height)\n"
    "    return;\n"
    "  uint rowOffset = dt.x * 4;\n"
    "  if (rowOffset >= rowBytes)\n"
    "  {\n"
    "    dst.Store((dt.y * dataWidth + dt.x) * 4, 0);\n"
    "    return;\n"
    "  }\n"
    "  uint firstX = (dt.x * 4) / 3;\n"
    "  uint secondX = firstX + 1;\n"
    "  float4 color0 = src[uint2(firstX, dt.y)];\n"
    "  float4 color3 = secondX < width ?\n"
    "    src[uint2(secondX, dt.y)] : 0.0f;\n"
    "  uint xmod3 = dt.x % 3;\n"
    "  float4 color1 = xmod3 <= 1 ? color0 : color3;\n"
    "  float4 color2 = xmod3 == 0 ? color0 : color3;\n"
    "  float4 packed = float4(\n"
    "    color0.bgr[xmod3], color1.grb[xmod3],\n"
    "    color2.rbg[xmod3], color3.bgr[xmod3]);\n"
    "  uint4 bytes = (uint4)(saturate(packed) * 255.0f + 0.5f);\n"
    "  uint value = bytes.x | (bytes.y << 8) |\n"
    "    (bytes.z << 16) | (bytes.w << 24);\n"
    "  dst.Store((dt.y * dataWidth + dt.x) * 4, value);\n"
    "}\n";

  if (!InitCompute(device, ranges, ARRAYSIZE(ranges), nullptr, 0, shader))
    return false;

  m_state = std::make_shared<State>();
  return true;
}

void CRGB24Effect::ShareState(const CPostProcessEffect& other)
{
  m_state = static_cast<const CRGB24Effect&>(other).m_state;
}

void CRGB24Effect::Update(const D12FrameFormat& format)
{
  m_state->Update(format);
}

bool CRGB24Effect::NeedsReconfigure() const
{
  return m_state->WantsPacked() != Enabled;
}

bool CRGB24Effect::RequiresFullDamage() const
{
  return m_state->IsBenchmarking();
}

uint64_t CRGB24Effect::GetTimingToken() const
{
  return m_state->GetTimingToken(Enabled);
}

void CRGB24Effect::RecordTiming(
  uint64_t token, bool fullCopy, uint64_t totalTime)
{
  m_state->RecordTiming(token, fullCopy, totalTime);
}

bool CRGB24Effect::GetCopyLayout(
  unsigned * pitch, unsigned * dataHeight) const
{
  *pitch      = m_pitch;
  *dataHeight = m_height;
  return true;
}

static void GetCopySpan(const RECT& rect, unsigned width,
  unsigned pitch, UINT64 * left, UINT64 * right)
{
  // Damage stays in logical pixels until the packed copy is recorded.
  *left = ((UINT64)rect.left * 3) & ~3ULL;
  if (rect.left == 0 && rect.right == (LONG)width)
    *right = pitch;
  else
    *right = ((UINT64)rect.right * 3 + 3) & ~3ULL;
}

bool CRGB24Effect::ShouldCopyFully(
  const RECT dirtyRects[], unsigned nbDirtyRects) const
{
  static const unsigned commandLimit = 256;

  const uint64_t frameSize   = (uint64_t)m_pitch * m_height;
  uint64_t       copiedBytes = 0;
  unsigned       commands    = 0;
  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    UINT64 left;
    UINT64 right;
    GetCopySpan(*rect, m_width, m_pitch, &left, &right);

    const unsigned rows = (unsigned)(rect->bottom - rect->top);
    if (left == 0 && right == m_pitch)
    {
      ++commands;
      copiedBytes += (uint64_t)rows * m_pitch;
    }
    else
    {
      commands    += rows;
      copiedBytes += (right - left) * rows;
    }

    if (commands > commandLimit || copiedBytes >= frameSize)
      return true;
  }

  return false;
}

void CRGB24Effect::CopyFrame(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  ID3D12Resource * dst, ID3D12Resource * src,
  const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const
{
  if (fullCopy)
  {
    commandList->CopyBufferRegion(
      dst, 0, src, 0, (UINT64)m_pitch * m_height);
    return;
  }

  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    UINT64 left;
    UINT64 right;
    GetCopySpan(*rect, m_width, m_pitch, &left, &right);

    if (left == 0 && right == m_pitch)
    {
      const UINT64 offset = (UINT64)rect->top * m_pitch;
      const UINT64 size   =
        (UINT64)(rect->bottom - rect->top) * m_pitch;
      commandList->CopyBufferRegion(dst, offset, src, offset, size);
      continue;
    }

    for (LONG y = rect->top; y < rect->bottom; ++y)
    {
      const UINT64 offset = (UINT64)y * m_pitch + left;
      commandList->CopyBufferRegion(
        dst, offset, src, offset, right - left);
    }
  }
}

PostProcessStatus CRGB24Effect::SetFormat(const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  if (!m_state->WantsPacked())
  {
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  if (src.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      src.desc.Format    != DXGI_FORMAT_B8G8R8A8_UNORM         ||
      src.hdr)
  {
    DEBUG_WARN("RGB24 packing is unavailable for the current format");
    m_state->Reject();
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  if (src.desc.Width >
      (UINT64_MAX - (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)) / 3)
  {
    m_state->Reject();
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  const UINT64 packedPitch = AlignTo<UINT64>(
    src.desc.Width * 3, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  if (!src.desc.Height || packedPitch > LONG_MAX ||
      packedPitch > UINT64_MAX / src.desc.Height)
  {
    m_state->Reject();
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  const UINT64 bufferSize = packedPitch * src.desc.Height;
  const UINT64 maxUAVSize =
    (1ULL << D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP) *
      sizeof(uint32_t);
  if (bufferSize > UINT32_MAX || bufferSize > maxUAVSize)
  {
    m_state->Reject();
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  if (!CreateDefaultBuffer(device, bufferSize, m_dst))
  {
    m_state->Reject();
    m_dst.Reset();
    return PostProcessStatus::BYPASS_EFFECT;
  }

  const unsigned dataWidth = (unsigned)(packedPitch / 4);
  m_threadsX = (dataWidth       + (Threads - 1)) / Threads;
  m_threadsY = (src.desc.Height + (Threads - 1)) / Threads;
  m_width    = (unsigned)src.desc.Width;
  m_height   = src.desc.Height;
  m_pitch    = (unsigned)packedPitch;

  dst.desc       = m_dst->GetDesc();
  dst.dataWidth  = dataWidth;
  dst.dataHeight = src.desc.Height;
  dst.pitch      = m_pitch;
  dst.format     = FRAME_TYPE_BGR_32;
  return PostProcessStatus::SUCCESS;
}

ComPtr<ID3D12Resource> CRGB24Effect::Run(const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_CPU_DESCRIPTOR_HANDLE handle =
    m_descHeap->GetCPUDescriptorHandleForHeapStart();
  const UINT inc = device->GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
  srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels     = 1;
  device->CreateShaderResourceView(src.Get(), &srvDesc, handle);
  handle.ptr += inc;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format                      = DXGI_FORMAT_R32_TYPELESS;
  uavDesc.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
  uavDesc.Buffer.NumElements          = (UINT)(m_dst->GetDesc().Width / 4);
  uavDesc.Buffer.StructureByteStride  = 0;
  uavDesc.Buffer.CounterOffsetInBytes = 0;
  uavDesc.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uavDesc, handle);

  Bind(commandList);
  commandList->Dispatch(m_threadsX, m_threadsY, 1);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COMMON);

  return m_dst;
}
