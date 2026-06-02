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

#include "CDownsampleEffect.h"

#include "CDebug.h"
#include "../CSettings.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstring>

using namespace PostProcessUtil;

bool CDownsampleEffect::ParseRules(const std::wstring& value)
{
  m_rules.clear();
  if (value.empty())
    return false;

  size_t pos = 0;
  while (pos < value.size())
  {
    size_t comma = value.find(L',', pos);
    std::wstring token = value.substr(pos,
      comma == std::wstring::npos ? std::wstring::npos : comma - pos);

    while (!token.empty() && std::iswspace(token.front()))
      token.erase(token.begin());
    while (!token.empty() && std::iswspace(token.back()))
      token.pop_back();

    if (!token.empty())
    {
      Rule rule = {};
      const wchar_t * start = token.c_str();
      if (*start == L'>')
      {
        rule.greater = true;
        ++start;
      }

      if (swscanf_s(start, L"%ux%u:%ux%u",
        &rule.x, &rule.y, &rule.targetX, &rule.targetY) != 4)
      {
        DEBUG_ERROR("Unable to parse IDD downsample rule");
        m_rules.clear();
        return false;
      }

      DEBUG_INFO("idd:downsample rule: %ux%u -> %ux%u%s",
        rule.x, rule.y, rule.targetX, rule.targetY,
        rule.greater ? " (greater-than)" : "");
      m_rules.push_back(rule);
    }

    if (comma == std::wstring::npos)
      break;
    pos = comma + 1;
  }

  return !m_rules.empty();
}

const CDownsampleEffect::Rule * CDownsampleEffect::MatchRule(
  unsigned width, unsigned height) const
{
  const Rule * match = nullptr;
  for (const auto& rule : m_rules)
    if (( rule.greater && (width  > rule.x || height  > rule.y)) ||
        (!rule.greater && (width == rule.x && height == rule.y)))
      match = &rule;

  return match;
}

bool CDownsampleEffect::Init(const ComPtr<ID3D12Device3>& device)
{
  if (!ParseRules(g_settings.ReadStringValue(L"Downsample")))
    return false;

  D3D12_STATIC_SAMPLER_DESC sampler = {};
  sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MaxLOD           = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister   = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

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
    "cbuffer Constants       : register(b0)\n"
    "{\n"
    "  float Width;\n"
    "  float Height;\n"
    "};\n"
    "Texture2D  <float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "SamplerState        ss  : register(s0);\n"
    "[numthreads(" POST_PROCESS_THREADS_STR ", " POST_PROCESS_THREADS_STR ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  dst[dt.xy] = src.SampleLevel(ss,\n"
    "    float2(\n"
    "      (float(dt.x) + 0.5f) / Width,\n"
    "      (float(dt.y) + 0.5f) / Height),\n"
    "    0);\n"
    "}\n";

  if (!InitCompute(device, ranges, ARRAYSIZE(ranges), &sampler, 1, shader))
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
    DEBUG_ERROR_HR(hr, "Failed to create Downsample constant buffer");
    return false;
  }

  return true;
}

PostProcessStatus CDownsampleEffect::SetFormat(
  const ComPtr<ID3D12Device3>& device,
  const D12FrameFormat& src, D12FrameFormat& dst)
{
  const Rule * rule = MatchRule((unsigned)src.desc.Width, src.desc.Height);
  if (!rule ||
      (rule->targetX == src.desc.Width && rule->targetY == src.desc.Height))
    return PostProcessStatus::BYPASS_EFFECT;

  D3D12_RESOURCE_DESC desc = src.desc;
  desc.Width  = rule->targetX;
  desc.Height = rule->targetY;
  desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  if (!CreateDefaultTexture(device, desc, m_dst))
    return PostProcessStatus::FAILED;

  m_consts.width  = (float)rule->targetX;
  m_consts.height = (float)rule->targetY;

  void * data = nullptr;
  D3D12_RANGE readRange = { 0, 0 };
  HRESULT hr = m_constBuffer->Map(0, &readRange, &data);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to map Downsample constant buffer");
    return PostProcessStatus::FAILED;
  }
  std::memcpy(data, &m_consts, sizeof(m_consts));
  m_constBuffer->Unmap(0, nullptr);

  m_threadsX = ((unsigned)desc.Width  + (Threads - 1)) / Threads;
  m_threadsY = ((unsigned)desc.Height + (Threads - 1)) / Threads;
  m_format   = src.desc.Format;
  m_scaleX   = (double)desc.Width  / src.desc.Width;
  m_scaleY   = (double)desc.Height / src.desc.Height;
  m_width    = (unsigned)desc.Width;
  m_height   = desc.Height;

  dst.desc        = desc;
  dst.width  = (unsigned)desc.Width;
  dst.height = desc.Height;
  return PostProcessStatus::SUCCESS;
}

void CDownsampleEffect::AdjustDamage(RECT dirtyRects[], unsigned * nbDirtyRects)
{
  for (RECT * rect = dirtyRects; rect < dirtyRects + *nbDirtyRects; ++rect)
  {
    unsigned width  = (unsigned)std::ceil((double)(rect->right  - rect->left) * m_scaleX);
    unsigned height = (unsigned)std::ceil((double)(rect->bottom - rect->top ) * m_scaleY);
    rect->left   = (LONG)max(0.0, std::floor((double)rect->left * m_scaleX));
    rect->right  = (LONG)min((double)m_width , (double)rect->left + width);
    rect->top    = (LONG)max(0.0, std::floor((double)rect->top * m_scaleY));
    rect->bottom = (LONG)min((double)m_height, (double)rect->top  + height);

    if (rect->left   > 0              ) rect->left   -= 1;
    if (rect->top    > 0              ) rect->top    -= 1;
    if (rect->right  < (LONG)m_width  ) rect->right  += 1;
    if (rect->bottom < (LONG)m_height ) rect->bottom += 1;
  }
}

ComPtr<ID3D12Resource> CDownsampleEffect::Run(
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
  srvDesc.Format = m_format;
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(src.Get(), &srvDesc, handle);
  handle.ptr += inc;

  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.Format = m_format;
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device->CreateUnorderedAccessView(m_dst.Get(), nullptr, &uavDesc, handle);

  Bind(commandList);
  commandList->Dispatch(m_threadsX, m_threadsY, 1);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COPY_SOURCE);
  return m_dst;
}
