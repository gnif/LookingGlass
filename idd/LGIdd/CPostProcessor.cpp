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

#include "CPostProcessor.h"

#include "CD3D12Device.h"
#include "CDebug.h"
#include "effect/CDownsampleEffect.h"
#include "effect/CHDR16to10Effect.h"
#include "effect/CRGB24Effect.h"

#include <utility>

bool CPostProcessor::Init(std::shared_ptr<CD3D12Device> dx12Device)
{
  m_dx12Device = dx12Device;
  m_device = dx12Device->GetDevice();
  m_effects.clear();

  std::unique_ptr<CDownsampleEffect> downsample(new CDownsampleEffect());
  if (downsample->Init(m_device))
  {
    DEBUG_INFO("Created post-processing effect: %s", downsample->GetName());
    m_effects.push_back(std::move(downsample));
  }

  std::unique_ptr<CHDR16to10Effect> hdr16to10(new CHDR16to10Effect());
  if (hdr16to10->Init(m_device))
  {
    DEBUG_INFO("Created post-processing effect: %s", hdr16to10->GetName());
    m_effects.push_back(std::move(hdr16to10));
  }
  else
    return false;

  std::unique_ptr<CRGB24Effect> rgb24(new CRGB24Effect());
  if (rgb24->Init(m_device))
  {
    DEBUG_INFO("Created post-processing effect: %s", rgb24->GetName());
    m_effects.push_back(std::move(rgb24));
  }

  return true;
}

void CPostProcessor::Reset()
{
  m_effects.clear();
  m_dx12Device.reset();
  m_device.Reset();
  m_srcFormat = {};
  m_dstFormat = {};
  m_effectsActive = false;
}

bool CPostProcessor::Configure(const D12FrameFormat& srcFormat, bool * formatChanged)
{
  if (formatChanged)
    *formatChanged = false;

  if (srcFormat.desc.Width  == m_srcFormat.desc.Width  &&
      srcFormat.desc.Height == m_srcFormat.desc.Height &&
      srcFormat.desc.Format == m_srcFormat.desc.Format &&
      srcFormat.format      == m_srcFormat.format      &&
      srcFormat.width       == m_srcFormat.width       &&
      srcFormat.height      == m_srcFormat.height      &&
      srcFormat.hdr         == m_srcFormat.hdr         &&
      srcFormat.hdrPQ       == m_srcFormat.hdrPQ)
    return true;

  D12FrameFormat oldDst = m_dstFormat;
  D12FrameFormat cur = srcFormat;
  m_srcFormat = srcFormat;
  m_effectsActive = false;

  for (const auto& effect : m_effects)
  {
    D12FrameFormat dst = cur;
    switch (effect->SetFormat(m_device, cur, dst))
    {
    case PostProcessStatus::SUCCESS:
      effect->Enabled = true;
      m_effectsActive = true;
      cur = dst;
      DEBUG_INFO("Post-processing effect active: %s", effect->GetName());
      break;

    case PostProcessStatus::BYPASS_EFFECT:
      effect->Enabled = false;
      break;

    case PostProcessStatus::FAILED:
      DEBUG_ERROR("Failed to configure post-processing effect: %s", effect->GetName());
      return false;
    }
  }

  m_dstFormat = cur;
  if (formatChanged)
    *formatChanged = oldDst.desc.Width  != m_dstFormat.desc.Width  ||
                     oldDst.desc.Height != m_dstFormat.desc.Height ||
                     oldDst.desc.Format != m_dstFormat.desc.Format ||
                     oldDst.format      != m_dstFormat.format      ||
                     oldDst.width       != m_dstFormat.width       ||
                     oldDst.height      != m_dstFormat.height      ||
                     oldDst.hdr         != m_dstFormat.hdr         ||
                     oldDst.hdrPQ       != m_dstFormat.hdrPQ;
  return true;
}

void CPostProcessor::AdjustFrameDamage(RECT dirtyRects[], unsigned * nbDirtyRects)
{
  for (const auto& effect : m_effects)
    if (effect->Enabled)
      effect->AdjustDamage(dirtyRects, nbDirtyRects);
}

ComPtr<ID3D12Resource> CPostProcessor::Run(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
  unsigned * nbDirtyRects)
{
  ComPtr<ID3D12Resource> next = src;
  for (const auto& effect : m_effects)
  {
    if (!effect->Enabled)
      continue;

    //DEBUG_TRACE("Run post-processing effect: %s", effect->GetName());
    effect->AdjustDamage(dirtyRects, nbDirtyRects);
    next = effect->Run(m_device, commandList, next, dirtyRects, nbDirtyRects);
  }

  return next;
}
