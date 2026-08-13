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

#include "CHDR16to10Effect.h"

#include "CDebug.h"

using namespace PostProcessUtil;

bool CHDR16to10Effect::Init(const ComPtr<ID3D12Device3>& device)
{
  D3D12_DESCRIPTOR_RANGE ranges[] =
  {
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0),
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0),
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0),
  };

  const char * shader =
    "cbuffer Constants : register(b0)\n"
    "{\n"
    "  float ReferenceWhiteNits;\n"
    "};\n"
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "static const float PQ_m1 = 0.1593017578125;\n"
    "static const float PQ_m2 = 78.84375;\n"
    "static const float PQ_c1 = 0.8359375;\n"
    "static const float PQ_c2 = 18.8515625;\n"
    "static const float PQ_c3 = 18.6875;\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", " POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  float3 linearValue = src[dt.xy].rgb * ReferenceWhiteNits;\n"
    "  // scRGB uses BT.709 primaries whereas HDR10/PQ output uses BT.2020.\n"
    "  // Rotate the gamut in linear light (BT.2087) BEFORE applying the PQ\n"
    "  // curve, otherwise the BT.709 values are later reinterpreted as BT.2020\n"
    "  // and saturated colours (most visibly red) are pushed outside their\n"
    "  // intended gamut.\n"
    "  float3 rec2020 = float3(\n"
    "    dot(linearValue, float3(0.6274039, 0.3292830, 0.0433131)),\n"
    "    dot(linearValue, float3(0.0690973, 0.9195404, 0.0113623)),\n"
    "    dot(linearValue, float3(0.0163914, 0.0880133, 0.8955953)));\n"
    "  // scRGB to PQ (ST.2084)\n"
    "  float3 Y = rec2020 / 10000.0;\n"
    "  float3 Ym1 = pow(max(Y, 0.0), PQ_m1);\n"
    "  float3 pq = pow((PQ_c1 + PQ_c2 * Ym1) / (1.0 + PQ_c3 * Ym1), PQ_m2);\n"
    "  dst[dt.xy] = float4(pq, src[dt.xy].a);\n"
    "}\n";

  if (!InitCompute(device, ranges, ARRAYSIZE(ranges), nullptr, 0, shader))
    return false;

  const size_t size = AlignTo(sizeof(m_consts),
    (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  HRESULT hr = CreateUploadBuffer(device, size, m_constBuffer);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create HDR16to10 constant buffer");
    return false;
  }

  hr = Upload(m_constBuffer, &m_consts, sizeof(m_consts));
  if (FAILED(hr))
    return false;

  return true;
}

PostProcessStatus CHDR16to10Effect::SetFormat(
  const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  if (src.desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || !src.hdr)
    return PostProcessStatus::BYPASS_EFFECT;

  D3D12_RESOURCE_DESC desc = src.desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (!CreateDefaultTexture(device, desc, m_dst))
    return PostProcessStatus::FAILED;

  m_threadsX = Groups((unsigned)desc.Width);
  m_threadsY = Groups(desc.Height);

  dst.desc   = desc;
  dst.format = FRAME_TYPE_RGBA10;
  dst.hdr    = true;
  dst.hdrPQ  = true;

  // Gamut conversion changes the signal's container primaries to BT.2020, but
  // does not change the mastering display chromaticities described by ST 2086.
  D12::CopyHdr(dst, src);

  return PostProcessStatus::SUCCESS;
}

ComPtr<ID3D12Resource> CHDR16to10Effect::Run(
  const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  CBV(device, 0, m_constBuffer.Get(), sizeof(m_consts));
  SRV(device, 1, src.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
  UAV(device, 2, m_dst.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
  Dispatch(commandList);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COMMON);
  return m_dst;
}
