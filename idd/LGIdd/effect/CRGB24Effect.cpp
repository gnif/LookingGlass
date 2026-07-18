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

#include "../CSettings.h"

using namespace PostProcessUtil;

bool CRGB24Effect::Init(const ComPtr<ID3D12Device3>& device)
{
  if (!g_settings.ReadBoolValue(L"AllowRGB24", false))
    return false;

  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  const char * shader =
    "Texture2D  <float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", " POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  uint fstInputX = (dt.x * 4) / 3;\n"
    "  float4 color0 = src[uint2(fstInputX, dt.y)];\n"
    "  uint sndInputX = fstInputX + 1;\n"
    "  float4 color3 = src[uint2(sndInputX, dt.y)];\n"
    "  uint xmod3 = dt.x % 3;\n"
    "  float4 color1 = xmod3 <= 1 ? color0 : color3;\n"
    "  float4 color2 = xmod3 == 0 ? color0 : color3;\n"
    "  float b = color0.bgr[xmod3];\n"
    "  float g = color1.grb[xmod3];\n"
    "  float r = color2.rbg[xmod3];\n"
    "  float a = color3.bgr[xmod3];\n"
    "  dst[dt.xy] = float4(r, g, b, a);\n"
    "}\n";

  return InitCompute(device, ranges, ARRAYSIZE(ranges), nullptr, 0, shader);
}

PostProcessStatus CRGB24Effect::SetFormat(const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  if (src.desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    return PostProcessStatus::BYPASS_EFFECT;

  const unsigned packedPitch = AlignTo((unsigned)src.desc.Width * 3, 4u);
  D3D12_RESOURCE_DESC desc = src.desc;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.Width  = AlignTo(packedPitch / 4, 64u);
  desc.Height = ((unsigned)src.desc.Width * src.desc.Height) / (packedPitch / 3);
  desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (!CreateDefaultTexture(device, desc, m_dst))
    return PostProcessStatus::FAILED;

  m_threadsX = ((unsigned)desc.Width  + (Threads - 1)) / Threads;
  m_threadsY = ((unsigned)desc.Height + (Threads - 1)) / Threads;

  dst.desc         = desc;
  dst.format = FRAME_TYPE_BGR_32;
  return PostProcessStatus::SUCCESS;
}

ComPtr<ID3D12Resource> CRGB24Effect::Run(const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_CPU_DESCRIPTOR_HANDLE handle =
    m_descHeap->GetCPUDescriptorHandleForHeapStart();
  const UINT inc = device->GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src.Get(), &srvDesc, handle);
  handle.ptr += inc;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uavDesc, handle);

  Bind(commandList);
  commandList->Dispatch(m_threadsX, m_threadsY, 1);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COPY_SOURCE);

  for (RECT * rect = dirtyRects; rect < dirtyRects + *nbDirtyRects; ++rect)
  {
    const LONG left  = rect->left;
    const LONG right = rect->right;
    rect->left  = (left  * 3) / 4;
    rect->right = (right * 3 + 3) / 4;
  }

  return m_dst;
}
