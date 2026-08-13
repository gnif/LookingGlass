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

#include "CDownsampleEffect.h"

#include "CDebug.h"
#include "config/CSettings.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

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

  D3D12_DESCRIPTOR_RANGE ranges[] =
  {
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0),
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0),
    Range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0),
  };

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

  const size_t size = AlignTo(sizeof(m_consts),
    (size_t)D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  const HRESULT hr = CreateUploadBuffer(device, size, m_constBuffer);
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

  const HRESULT hr = Upload(m_constBuffer, &m_consts, sizeof(m_consts));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to map Downsample constant buffer");
    return PostProcessStatus::FAILED;
  }
  m_threadsX = Groups((unsigned)desc.Width);
  m_threadsY = Groups(desc.Height);
  m_format   = src.desc.Format;
  m_scaleX   = (double)desc.Width  / src.desc.Width;
  m_scaleY   = (double)desc.Height / src.desc.Height;
  m_width    = (unsigned)desc.Width;
  m_height   = desc.Height;

  dst.desc   = desc;
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

  TransitionDst(commandList, D3D12_RESOURCE_STATE_COMMON,
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  CBV(device, 0, m_constBuffer.Get(), sizeof(m_consts));
  SRV(device, 1, src.Get(), m_format);
  UAV(device, 2, m_dst.Get(), m_format);
  Dispatch(commandList);

  TransitionDst(commandList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
    D3D12_RESOURCE_STATE_COMMON);
  return m_dst;
}
