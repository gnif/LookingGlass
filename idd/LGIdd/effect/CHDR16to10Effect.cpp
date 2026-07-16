/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CHDR16to10Effect.h"

#include "CDebug.h"

#include <cstring>

using namespace PostProcessUtil;

bool CHDR16to10Effect::Init(const ComPtr<ID3D12Device3>& device)
{
  D3D12_DESCRIPTOR_RANGE ranges[3] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[2].NumDescriptors = 1;
  ranges[2].BaseShaderRegister = 0;
  ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

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

  D3D12_HEAP_PROPERTIES heapProps = {};
  heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = AlignTo(sizeof(m_consts),
    (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  HRESULT hr = device->CreateCommittedResource(&heapProps,
    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
    nullptr, IID_PPV_ARGS(&m_constBuffer));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create HDR16to10 constant buffer");
    return false;
  }

  void * data = nullptr;
  D3D12_RANGE readRange = { 0, 0 };
  hr = m_constBuffer->Map(0, &readRange, &data);
  if (FAILED(hr))
    return false;
  std::memcpy(data, &m_consts, sizeof(m_consts));
  m_constBuffer->Unmap(0, nullptr);

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

  m_threadsX = ((unsigned)desc.Width  + (Threads - 1)) / Threads;
  m_threadsY = ((unsigned)desc.Height + (Threads - 1)) / Threads;

  dst.desc   = desc;
  dst.format = FRAME_TYPE_RGBA10;
  dst.hdr    = true;
  dst.hdrPQ  = true;

  // Explicitly propagate HDR static metadata so it survives post-processing.
  // Do not rely on the caller's copy semantics in CPostProcessor::Configure().
  // The shader rotates the gamut from BT.709 to BT.2020, so advertise BT.2020
  // mastering primaries here rather than forwarding the source's BT.709 set
  // (in 0.00002 units) - otherwise consumers see PQ/BT.2020 content tagged
  // with BT.709 primaries.
  dst.displayPrimary[0][0] = 35400; // Rx
  dst.displayPrimary[0][1] = 14600; // Ry
  dst.displayPrimary[1][0] =  8500; // Gx
  dst.displayPrimary[1][1] = 39850; // Gy
  dst.displayPrimary[2][0] =  6550; // Bx
  dst.displayPrimary[2][1] =  2300; // By
  memcpy(dst.whitePoint    , src.whitePoint    , sizeof(dst.whitePoint    ));
  dst.maxDisplayLuminance       = src.maxDisplayLuminance;
  dst.minDisplayLuminance       = src.minDisplayLuminance;
  dst.maxContentLightLevel      = src.maxContentLightLevel;
  dst.maxFrameAverageLightLevel = src.maxFrameAverageLightLevel;

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

  TransitionDst(commandList, D3D12_RESOURCE_STATE_COPY_SOURCE,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  D3D12_CPU_DESCRIPTOR_HANDLE handle =
    m_descHeap->GetCPUDescriptorHandleForHeapStart();
  const UINT inc = device->GetDescriptorHandleIncrementSize(
    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
  cbvDesc.BufferLocation = m_constBuffer->GetGPUVirtualAddress();
  cbvDesc.SizeInBytes = (UINT)AlignTo(sizeof(m_consts),
    (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  device->CreateConstantBufferView(&cbvDesc, handle);
  handle.ptr += inc;

  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src.Get(), &srvDesc, handle);
  handle.ptr += inc;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uavDesc, handle);

  Bind(commandList);
  commandList->Dispatch(m_threadsX, m_threadsY, 1);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COPY_SOURCE);
  return m_dst;
}
