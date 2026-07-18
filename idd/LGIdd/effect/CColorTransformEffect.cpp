/**
 * Looking Glass
 * Copyright Â© 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include "CColorTransformEffect.h"

#include "CDebug.h"

#include <cstring>

using namespace PostProcessUtil;

namespace
{
  enum TransferFunction : UINT
  {
    TRANSFER_LINEAR,
    TRANSFER_SRGB,
    TRANSFER_PQ,
  };

  bool CreateUploadBuffer(const ComPtr<ID3D12Device3>& device, size_t size,
    ComPtr<ID3D12Resource>& resource)
  {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    const HRESULT hr = device->CreateCommittedResource(&heapProps,
      D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr, IID_PPV_ARGS(&resource));
    return SUCCEEDED(hr);
  }

  bool Upload(const ComPtr<ID3D12Resource>& resource,
    const void * data, size_t size)
  {
    void * dst = nullptr;
    const D3D12_RANGE readRange = { 0, 0 };
    if (FAILED(resource->Map(0, &readRange, &dst)))
      return false;
    std::memcpy(dst, data, size);
    resource->Unmap(0, nullptr);
    return true;
  }
}

bool CColorTransformEffect::Init(const ComPtr<ID3D12Device3>& device)
{
  D3D12_DESCRIPTOR_RANGE ranges[4] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
  ranges[0].NumDescriptors = 1;
  ranges[0].BaseShaderRegister = 0;
  ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[1].NumDescriptors = 1;
  ranges[1].BaseShaderRegister = 0;
  ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[2].NumDescriptors = 1;
  ranges[2].BaseShaderRegister = 1;
  ranges[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
  ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  ranges[3].NumDescriptors = 1;
  ranges[3].BaseShaderRegister = 0;
  ranges[3].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  const char * shader =
    "cbuffer Constants : register(b0)\n"
    "{\n"
    "  float4 ColorMatrix[3];\n"
    "  float Scalar;\n"
    "  uint MatrixEnabled;\n"
    "  uint LutEnabled;\n"
    "  uint InputTransfer;\n"
    "  uint OutputTransfer;\n"
    "};\n"
    "Texture2D<float4> src : register(t0);\n"
    "Buffer<float4> lut : register(t1);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "static const float PQ_m1 = 0.1593017578125;\n"
    "static const float PQ_m2 = 78.84375;\n"
    "static const float PQ_c1 = 0.8359375;\n"
    "static const float PQ_c2 = 18.8515625;\n"
    "static const float PQ_c3 = 18.6875;\n"
    "float3 decode(float3 value)\n"
    "{\n"
    "  if (InputTransfer == 1)\n"
    "    return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4),\n"
    "      step(0.04045, value));\n"
    "  if (InputTransfer == 2)\n"
    "  {\n"
    "    float3 p = pow(max(value, 0.0), 1.0 / PQ_m2);\n"
    "    return pow(max(p - PQ_c1, 0.0) / max(PQ_c2 - PQ_c3 * p, 1e-6),\n"
    "      1.0 / PQ_m1);\n"
    "  }\n"
    "  return value;\n"
    "}\n"
    "float3 encode(float3 value)\n"
    "{\n"
    "  if (OutputTransfer == 1)\n"
    "    return lerp(value * 12.92, 1.055 * pow(max(value, 0.0), 1.0 / 2.4) - 0.055,\n"
    "      step(0.0031308, value));\n"
    "  if (OutputTransfer == 2)\n"
    "  {\n"
    "    float3 p = pow(max(value, 0.0), PQ_m1);\n"
    "    return pow((PQ_c1 + PQ_c2 * p) / (1.0 + PQ_c3 * p), PQ_m2);\n"
    "  }\n"
    "  return value;\n"
    "}\n"
    "float3 rgbToXYZ(float3 rgb)\n"
    "{\n"
    "  if (InputTransfer == 2)\n"
    "    return float3(\n"
    "      dot(rgb, float3(0.6369580, 0.1446169, 0.1688810)),\n"
    "      dot(rgb, float3(0.2627002, 0.6779981, 0.0593017)),\n"
    "      dot(rgb, float3(0.0000000, 0.0280727, 1.0609851)));\n"
    "  return float3(\n"
    "    dot(rgb, float3(0.4123908, 0.3575843, 0.1804808)),\n"
    "    dot(rgb, float3(0.2126390, 0.7151687, 0.0721923)),\n"
    "    dot(rgb, float3(0.0193308, 0.1191948, 0.9505322)));\n"
    "}\n"
    "float3 xyzToRGB(float3 xyz)\n"
    "{\n"
    "  if (OutputTransfer == 2)\n"
    "    return float3(\n"
    "      dot(xyz, float3( 1.7166512, -0.3556708, -0.2533663)),\n"
    "      dot(xyz, float3(-0.6666844,  1.6164812,  0.0157685)),\n"
    "      dot(xyz, float3( 0.0176399, -0.0427706,  0.9421031)));\n"
    "  return float3(\n"
    "    dot(xyz, float3( 3.2409699, -1.5373832, -0.4986108)),\n"
    "    dot(xyz, float3(-0.9692436,  1.8759675,  0.0415551)),\n"
    "    dot(xyz, float3( 0.0556301, -0.2039770,  1.0569715)));\n"
    "}\n"
    "float3 applyLut(float3 value)\n"
    "{\n"
    "  float3 pos = saturate(value) * 4095.0;\n"
    "  uint3 lo = (uint3)floor(pos);\n"
    "  uint3 hi = min(lo + 1, 4095);\n"
    "  float3 f = frac(pos);\n"
    "  return float3(\n"
    "    lerp(lut[lo.r].r, lut[hi.r].r, f.r),\n"
    "    lerp(lut[lo.g].g, lut[hi.g].g, f.g),\n"
    "    lerp(lut[lo.b].b, lut[hi.b].b, f.b));\n"
    "}\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", " POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  float4 pixel = src[dt.xy];\n"
    "  float3 value = decode(pixel.rgb);\n"
    "  if (MatrixEnabled != 0 || InputTransfer != OutputTransfer)\n"
    "  {\n"
    "    float3 xyz = rgbToXYZ(value);\n"
    "    if (MatrixEnabled != 0)\n"
    "      xyz = float3(dot(float4(xyz, 1.0), ColorMatrix[0]),\n"
    "                   dot(float4(xyz, 1.0), ColorMatrix[1]),\n"
    "                   dot(float4(xyz, 1.0), ColorMatrix[2])) * Scalar;\n"
    "    value = xyzToRGB(xyz);\n"
    "  }\n"
    "  if (InputTransfer == 0 && OutputTransfer == 2)\n"
    "    value *= 80.0 / 10000.0;\n"
    "  value = encode(value);\n"
    "  if (LutEnabled != 0)\n"
    "    value = applyLut(value);\n"
    "  dst[dt.xy] = float4(value, pixel.a);\n"
    "}\n";

  if (!InitCompute(device, ranges, ARRAYSIZE(ranges), nullptr, 0, shader))
    return false;

  const size_t constSize = AlignTo(sizeof(m_consts),
    (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  if (!CreateUploadBuffer(device, constSize, m_constBuffer) ||
      !CreateUploadBuffer(device, sizeof(float) * 4096 * 4, m_lutBuffer))
  {
    DEBUG_ERROR("Failed to create color transform buffers");
    return false;
  }

  return true;
}

PostProcessStatus CColorTransformEffect::SetFormat(
  const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  if (!src.colorTransform ||
      (!src.colorTransform->matrixEnabled && !src.colorTransform->lutEnabled))
    return PostProcessStatus::BYPASS_EFFECT;

  DXGI_FORMAT dstFormat;
  FrameType frameType;
  switch (src.desc.Format)
  {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
      dstFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
      frameType = FRAME_TYPE_RGBA;
      break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      dstFormat = src.desc.Format;
      frameType = src.format;
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      // The client wire format is HDR10. Perform the XYZ adjustment before
      // the BT.2020 rotation, and its LUT after PQ encoding, in one pass.
      dstFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
      frameType = FRAME_TYPE_RGBA10;
      break;
    default:
      DEBUG_ERROR("Unsupported color transform source format %u", src.desc.Format);
      return PostProcessStatus::FAILED;
  }

  D3D12_RESOURCE_DESC desc = src.desc;
  desc.Format = dstFormat;
  desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (!m_dst || m_dst->GetDesc().Width  != desc.Width  ||
                m_dst->GetDesc().Height != desc.Height ||
                m_dst->GetDesc().Format != desc.Format)
  {
    if (!CreateDefaultTexture(device, desc, m_dst))
      return PostProcessStatus::FAILED;
  }

  std::memcpy(m_consts.matrix, src.colorTransform->matrix,
    sizeof(m_consts.matrix));
  m_consts.scalar        = src.colorTransform->scalar;
  m_consts.matrixEnabled = src.colorTransform->matrixEnabled;
  m_consts.lutEnabled    = src.colorTransform->lutEnabled;
  m_consts.inputTransfer = src.hdrPQ ? TRANSFER_PQ :
    (src.hdr ? TRANSFER_LINEAR : TRANSFER_SRGB);
  m_consts.outputTransfer = src.hdr ? TRANSFER_PQ : TRANSFER_SRGB;

  std::memcpy(m_lut, src.colorTransform->lut, sizeof(m_lut));
  m_uploadPending = true;

  m_srcFormat = src.desc.Format;
  m_dstFormat = dstFormat;
  m_threadsX = ((unsigned)desc.Width  + (Threads - 1)) / Threads;
  m_threadsY = ((unsigned)desc.Height + (Threads - 1)) / Threads;

  dst.desc   = desc;
  dst.format = frameType;
  if (src.hdr)
    dst.hdrPQ = true;
  return PostProcessStatus::SUCCESS;
}

ComPtr<ID3D12Resource> CColorTransformEffect::Run(
  const ComPtr<ID3D12Device3>& device,
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  UNREFERENCED_PARAMETER(dirtyRects);
  UNREFERENCED_PARAMETER(nbDirtyRects);

  // GetComputeQueue waits for the previous submission before Run is called,
  // so this is the first point where the shared upload buffers are guaranteed
  // not to be in use by the GPU.
  if (m_uploadPending)
  {
    if (!Upload(m_constBuffer, &m_consts, sizeof(m_consts)) ||
        !Upload(m_lutBuffer, m_lut, sizeof(m_lut)))
      DEBUG_ERROR("Failed to upload display color transform");
    else
      m_uploadPending = false;
  }

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
  srvDesc.Format = m_srcFormat;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src.Get(), &srvDesc, handle);
  handle.ptr += inc;

  D3D12_SHADER_RESOURCE_VIEW_DESC lutDesc = {};
  lutDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  lutDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  lutDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  lutDesc.Buffer.NumElements = 4096;
  device->CreateShaderResourceView(m_lutBuffer.Get(), &lutDesc, handle);
  handle.ptr += inc;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = m_dstFormat;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uavDesc, handle);

  Bind(commandList);
  commandList->Dispatch(m_threadsX, m_threadsY, 1);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COPY_SOURCE);
  return m_dst;
}
