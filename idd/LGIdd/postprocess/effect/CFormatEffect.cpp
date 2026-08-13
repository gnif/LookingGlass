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

#include "CFormatEffect.h"

#include "CDebug.h"

using namespace PostProcessUtil;

namespace
{
  bool IsSDR8(const D12FrameFormat& format)
  {
    if (format.hdr || format.hdrPQ)
      return false;

    switch (format.format)
    {
      case FRAME_TYPE_BGRA:
        return format.desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
      case FRAME_TYPE_RGBA:
        return format.desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM;
      default:
        return false;
    }
  }
}

bool CFormatEffect::Init(const ComPtr<ID3D12Device3>& device)
{
  D3D12_DESCRIPTOR_RANGE ranges[] =
  {
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0),
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0),
  };

  const char * shader =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", "
      POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  dst[dt.xy] = src[dt.xy];\n"
    "}\n";

  return InitCompute(device, ranges, ARRAYSIZE(ranges), nullptr, 0, shader);
}

PostProcessStatus CFormatEffect::SetFormat(
  const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  return Cfg(device, src, dst);
}

PostProcessStatus CFormatEffect::Cfg(
  const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, const D12FrameFormat& dst)
{
  UNREFERENCED_PARAMETER(device);

  if (src.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      dst.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      !src.desc.Width                                          ||
      !src.desc.Height                                         ||
      src.desc.Width     != dst.desc.Width                     ||
      src.desc.Height    != dst.desc.Height                    ||
      !IsSDR8(src)                                             ||
      !IsSDR8(dst))
  {
    DEBUG_ERROR("Unsupported texture format conversion");
    return PostProcessStatus::FAILED;
  }

  if (src.desc.Format == dst.desc.Format)
    return PostProcessStatus::BYPASS_EFFECT;

  if (!(dst.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
  {
    DEBUG_ERROR("Format conversion destination does not allow UAV access");
    return PostProcessStatus::FAILED;
  }

  m_srcDesc   = src.desc;
  m_outDesc   = dst.desc;
  m_srcFormat = src.desc.Format;
  m_dstFormat = dst.desc.Format;
  m_threadsX  = Groups((unsigned)dst.desc.Width);
  m_threadsY  = Groups(dst.desc.Height);
  return PostProcessStatus::SUCCESS;
}

ComPtr<ID3D12Resource> CFormatEffect::Run(
  const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(device);
  UNREFERENCED_PARAMETER(commandList);
  UNREFERENCED_PARAMETER(src);
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);
  return nullptr;
}

bool CFormatEffect::IsSrc(ID3D12Resource * src) const
{
  if (!src)
    return false;

  return D12::Same(src->GetDesc(), m_srcDesc, D12::DescCmp::VIEW);
}

bool CFormatEffect::Run(
  const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, ID3D12Resource * dst,
  RECT dirtyRects[], unsigned * nbDirtyRects)
{
  if (!device || !commandList || !IsSrc(src.Get()) ||
      src.Get() == dst || !IsDst(dst))
    return false;
  return Draw(device, commandList, src, dst, dirtyRects, nbDirtyRects);
}

bool CFormatEffect::Draw(
  const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, ID3D12Resource * dst,
  RECT dirtyRects[], unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);

  TransitionDst(commandList, dst, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  SRV(device, 0, src.Get(), m_srcFormat);
  UAV(device, 1, dst, m_dstFormat);
  Dispatch(commandList);

  TransitionDst(commandList, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COMMON);
  return true;
}
