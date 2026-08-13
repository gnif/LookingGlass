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

#include "postprocess/CPostProcessor.h"

#include "d3d/CD3D12Device.h"
#include "CDebug.h"
#include "postprocess/effect/CColorTransformEffect.h"
#include "postprocess/effect/CDownsampleEffect.h"
#include "postprocess/effect/CHDR16to10Effect.h"
#include "postprocess/effect/CRGB24Effect.h"

#include <limits>
#include <utility>

bool CPostProcessor::Init(std::shared_ptr<CD3D12Device> dx12Device,
  bool enableEffects, bool report)
{
  m_dx12Device = dx12Device;
  m_device      = dx12Device->GetDevice();
  m_effects.clear();

  if (!enableEffects)
    return true;

  std::unique_ptr<CColorTransformEffect> colorTransform(new CColorTransformEffect());
  if (colorTransform->Init(m_device))
    m_effects.push_back(std::move(colorTransform));
  else
  {
    DEBUG_ERROR("Failed to create post-processing effect: %s",
      colorTransform->GetName());
    return false;
  }

  std::unique_ptr<CDownsampleEffect> downsample(new CDownsampleEffect());
  if (downsample->Init(m_device, report))
    m_effects.push_back(std::move(downsample));

  std::unique_ptr<CHDR16to10Effect> hdr16to10(new CHDR16to10Effect());
  if (hdr16to10->Init(m_device))
    m_effects.push_back(std::move(hdr16to10));
  else
  {
    DEBUG_ERROR("Failed to create post-processing effect: %s",
      hdr16to10->GetName());
    return false;
  }

  std::unique_ptr<CRGB24Effect> rgb24(new CRGB24Effect());
  if (rgb24->Init(m_device))
    m_effects.push_back(std::move(rgb24));

  return true;
}

void CPostProcessor::LogEffects() const
{
  for (const std::unique_ptr<CPostProcessEffect>& effect : m_effects)
    DEBUG_INFO("Created post-processing effect: %s", effect->GetName());
}

void CPostProcessor::LogActiveEffects() const
{
  for (const std::unique_ptr<CPostProcessEffect>& effect : m_effects)
    if (effect->Enabled)
      DEBUG_INFO("Post-processing effect active: %s", effect->GetName());
}

void CPostProcessor::Reset()
{
  m_effects.clear();
  m_dx12Device.reset();
  m_device.Reset();
  m_srcFormat     = {};
  m_dstFormat     = {};
  m_texFormat     = {};
  m_copyLayout    = {};
  m_copyEffect    = nullptr;
  m_frameSize     = 0;
  m_pitch         = 0;
  m_effectsActive = false;
  m_configured    = false;
}

bool CPostProcessor::HasSameEffectChain(const CPostProcessor& other) const
{
  if (m_effects.size() != other.m_effects.size())
    return false;

  for (size_t i = 0; i < m_effects.size(); ++i)
    if (std::strcmp(m_effects[i]->GetName(),
                    other.m_effects[i]->GetName()) != 0)
      return false;

  return true;
}

bool CPostProcessor::ShareEffectState(const CPostProcessor& other)
{
  if (!HasSameEffectChain(other))
    return false;

  for (size_t i = 0; i < m_effects.size(); ++i)
    m_effects[i]->ShareState(*other.m_effects[i]);

  return true;
}

void CPostProcessor::Update(const D12FrameFormat& srcFormat)
{
  for (const auto& effect : m_effects)
    effect->Update(srcFormat);
}

bool CPostProcessor::NeedsReconfigure(const D12FrameFormat& srcFormat) const
{
  if (!m_configured ||
    srcFormat.desc.Width     != m_srcFormat.desc.Width  ||
    srcFormat.desc.Height    != m_srcFormat.desc.Height ||
    srcFormat.desc.Format    != m_srcFormat.desc.Format ||
    srcFormat.format         != m_srcFormat.format      ||
    srcFormat.width          != m_srcFormat.width       ||
    srcFormat.height         != m_srcFormat.height      ||
    srcFormat.hdr            != m_srcFormat.hdr         ||
    srcFormat.hdrPQ          != m_srcFormat.hdrPQ       ||
    srcFormat.colorTransform != m_srcFormat.colorTransform)
    return true;

  for (const auto& effect : m_effects)
    if (effect->NeedsReconfigure())
      return true;

  return false;
}

bool CPostProcessor::RequiresFullDamage() const
{
  for (const auto& effect : m_effects)
    if (effect->RequiresFullDamage())
      return true;

  return false;
}

bool CPostProcessor::Configure(const D12FrameFormat& srcFormat,
  bool * formatChanged, bool * configured)
{
  if (formatChanged)
    *formatChanged = false;
  if (configured)
    *configured = false;

  if (!NeedsReconfigure(srcFormat))
  {
    // Static HDR metadata may change independently of the resource format.
    // Propagate it without recreating resources or post-processing state.
    D12::CopyHdr(m_srcFormat, srcFormat);
    D12::CopyHdr(m_dstFormat, srcFormat);
    D12::CopyHdr(m_texFormat, srcFormat);
    return true;
  }

  if (configured)
    *configured = true;

  D12FrameFormat       oldDst        = m_dstFormat;
  D12FrameFormat       cur           = srcFormat;
  D12FrameFormat       tex           = srcFormat;
  CPostProcessEffect * outputEffect  = nullptr;
  bool                 effectsActive = false;

  for (const auto& effect : m_effects)
  {
    D12FrameFormat dst = cur;
    switch (effect->SetFormat(m_device, cur, dst))
    {
    case PostProcessStatus::SUCCESS:
      effect->Enabled = true;
      effectsActive   = true;
      cur             = dst;
      if (dst.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        tex = dst;
      outputEffect    = effect.get();
      break;

    case PostProcessStatus::BYPASS_EFFECT:
      effect->Enabled = false;
      break;

    case PostProcessStatus::FAILED:
      DEBUG_ERROR("Failed to configure post-processing effect: %s", effect->GetName());
      return false;
    }
  }

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT copyLayout = {};
  CPostProcessEffect * copyEffect               = nullptr;
  unsigned             pitch                    = 0;
  unsigned             dataHeight               = 0;
  if (outputEffect && outputEffect->GetCopyLayout(&pitch, &dataHeight))
    copyEffect = outputEffect;
  else
  {
    if (cur.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
      DEBUG_ERROR("Post-processing output has no copy implementation");
      return false;
    }

    m_device->GetCopyableFootprints(
      &cur.desc,
      0,
      1,
      0,
      &copyLayout,
      nullptr,
      nullptr,
      nullptr);
    pitch      = copyLayout.Footprint.RowPitch;
    dataHeight = cur.desc.Height;
  }

  if (!pitch || !dataHeight ||
      pitch > (std::numeric_limits<size_t>::max)() / dataHeight)
  {
    DEBUG_ERROR("Invalid post-processing output layout");
    return false;
  }

  const size_t frameSize = (size_t)pitch * dataHeight;
  if (copyEffect && cur.desc.Width < frameSize)
  {
    DEBUG_ERROR("Post-processing output buffer is too small");
    return false;
  }

  m_srcFormat     = srcFormat;
  m_dstFormat     = cur;
  m_texFormat     = tex;
  m_copyLayout    = copyLayout;
  m_copyEffect    = copyEffect;
  m_frameSize     = frameSize;
  m_pitch         = pitch;
  m_effectsActive = effectsActive;
  m_configured    = true;
  if (formatChanged)
    *formatChanged =
      oldDst.desc.Width     != m_dstFormat.desc.Width      ||
      oldDst.desc.Height    != m_dstFormat.desc.Height     ||
      oldDst.desc.Format    != m_dstFormat.desc.Format     ||
      oldDst.dataWidth      != m_dstFormat.dataWidth       ||
      oldDst.dataHeight     != m_dstFormat.dataHeight      ||
      oldDst.pitch          != m_dstFormat.pitch           ||
      oldDst.format         != m_dstFormat.format          ||
      oldDst.width          != m_dstFormat.width           ||
      oldDst.height         != m_dstFormat.height          ||
      oldDst.hdr            != m_dstFormat.hdr             ||
      oldDst.hdrPQ          != m_dstFormat.hdrPQ           ||
      oldDst.sdrWhiteLevel  != m_dstFormat.sdrWhiteLevel   ||
      oldDst.colorTransform != m_dstFormat.colorTransform;
  return true;
}

void CPostProcessor::GetTimingToken(
  unsigned * effectIndex, uint64_t * token) const
{
  if (effectIndex)
    *effectIndex = 0;
  if (token)
    *token = 0;

  for (size_t i = 0; i < m_effects.size(); ++i)
  {
    const uint64_t value = m_effects[i]->GetTimingToken();
    if (!value)
      continue;

    if (effectIndex)
      *effectIndex = (unsigned)i;
    if (token)
      *token = value;
    return;
  }
}

void CPostProcessor::RecordTiming(
  unsigned effectIndex, uint64_t token, bool fullCopy, uint64_t totalTime)
{
  if (!token || effectIndex >= m_effects.size())
    return;

  m_effects[effectIndex]->RecordTiming(token, fullCopy, totalTime);
}

bool CPostProcessor::ShouldCopyFully(
  const RECT dirtyRects[], unsigned nbDirtyRects) const
{
  return m_copyEffect &&
    m_copyEffect->ShouldCopyFully(dirtyRects, nbDirtyRects);
}

void CPostProcessor::CopyToFrameBuffer(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  ID3D12Resource * dst, ID3D12Resource * src,
  const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const
{
  if (m_copyEffect)
  {
    m_copyEffect->CopyFrame(
      commandList, dst, src, dirtyRects, nbDirtyRects, fullCopy);
    return;
  }

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource        = src;
  srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  srcLoc.SubresourceIndex = 0;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource = dst;
  if (dst->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
  {
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
  }
  else
  {
    dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = m_copyLayout;
  }

  if (fullCopy)
  {
    commandList->CopyTextureRegion(
      &dstLoc, 0, 0, 0, &srcLoc, nullptr);
    return;
  }

  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    D3D12_BOX box = {};
    box.left   = rect->left;
    box.top    = rect->top;
    box.front  = 0;
    box.right  = rect->right;
    box.bottom = rect->bottom;
    box.back   = 1;

    commandList->CopyTextureRegion(
      &dstLoc, box.left, box.top, 0, &srcLoc, &box);
  }
}

void CPostProcessor::CopyToCandidate(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  ID3D12Resource * dst, ID3D12Resource * src) const
{
  CopyToFrameBuffer(
    commandList, dst, src, nullptr, 0, true);
}

void CPostProcessor::CopyFromCandidate(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  ID3D12Resource * dst, ID3D12Resource * src,
  const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const
{
  if (m_copyEffect)
  {
    m_copyEffect->CopyFrame(
      commandList, dst, src, dirtyRects, nbDirtyRects, fullCopy);
    return;
  }

  D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
  srcLoc.pResource       = src;
  srcLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  srcLoc.PlacedFootprint = m_copyLayout;

  D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
  dstLoc.pResource       = dst;
  dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dstLoc.PlacedFootprint = m_copyLayout;

  if (fullCopy)
  {
    commandList->CopyBufferRegion(
      dst, 0, src, 0, m_frameSize);
    return;
  }

  for (const RECT * rect = dirtyRects;
       rect < dirtyRects + nbDirtyRects; ++rect)
  {
    D3D12_BOX box = {};
    box.left   = rect->left;
    box.top    = rect->top;
    box.front  = 0;
    box.right  = rect->right;
    box.bottom = rect->bottom;
    box.back   = 1;

    commandList->CopyTextureRegion(
      &dstLoc, box.left, box.top, 0, &srcLoc, &box);
  }
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
