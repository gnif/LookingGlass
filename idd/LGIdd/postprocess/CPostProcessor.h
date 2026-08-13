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

#pragma once

#include <Windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <memory>
#include <vector>

#include "postprocess/D12FrameFormat.h"

struct CD3D12Device;

using namespace Microsoft::WRL;

enum class PostProcessStatus
{
  SUCCESS,
  BYPASS_EFFECT,
  FAILED
};

class CPostProcessEffect
{
public:
  virtual ~CPostProcessEffect() {}
  virtual const char * GetName() const = 0;
  virtual void ShareState(const CPostProcessEffect& other)
  {
    UNREFERENCED_PARAMETER(other);
  }
  virtual void Update(const D12FrameFormat& format)
  {
    UNREFERENCED_PARAMETER(format);
  }
  virtual bool NeedsReconfigure() const { return false; }
  virtual bool RequiresFullDamage() const { return false; }
  virtual uint64_t GetTimingToken() const { return 0; }
  virtual void RecordTiming(
    uint64_t token, bool fullCopy, uint64_t totalTime)
  {
    UNREFERENCED_PARAMETER(token);
    UNREFERENCED_PARAMETER(fullCopy);
    UNREFERENCED_PARAMETER(totalTime);
  }
  // A final effect with a non-texture output owns its framebuffer copy.
  virtual bool GetCopyLayout(
    unsigned * pitch, unsigned * dataHeight) const
  {
    UNREFERENCED_PARAMETER(pitch);
    UNREFERENCED_PARAMETER(dataHeight);
    return false;
  }
  virtual bool ShouldCopyFully(
    const RECT dirtyRects[], unsigned nbDirtyRects) const
  {
    UNREFERENCED_PARAMETER(dirtyRects);
    UNREFERENCED_PARAMETER(nbDirtyRects);
    return false;
  }
  virtual void CopyFrame(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, ID3D12Resource * src,
    const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const
  {
    UNREFERENCED_PARAMETER(commandList);
    UNREFERENCED_PARAMETER(dst);
    UNREFERENCED_PARAMETER(src);
    UNREFERENCED_PARAMETER(dirtyRects);
    UNREFERENCED_PARAMETER(nbDirtyRects);
    UNREFERENCED_PARAMETER(fullCopy);
  }
  virtual PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) = 0;
  virtual void AdjustDamage(RECT dirtyRects[], unsigned * nbDirtyRects) { UNREFERENCED_PARAMETER(dirtyRects); UNREFERENCED_PARAMETER(nbDirtyRects); }
  virtual ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) = 0;
  bool Enabled = false;
};

class CPostProcessor
{
private:
  std::shared_ptr<CD3D12Device>                    m_dx12Device;
  ComPtr<ID3D12Device3>                            m_device;
  std::vector<std::unique_ptr<CPostProcessEffect>> m_effects;
  D12FrameFormat                                   m_srcFormat     = {};
  D12FrameFormat                                   m_dstFormat     = {};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT               m_copyLayout    = {};
  CPostProcessEffect                             * m_copyEffect    = nullptr;
  size_t                                           m_frameSize     = 0;
  unsigned                                         m_pitch         = 0;
  bool                                             m_effectsActive = false;
  bool                                             m_configured    = false;

public:
  bool Init(std::shared_ptr<CD3D12Device> dx12Device,
    bool enableEffects, bool report);
  void LogEffects() const;
  void LogActiveEffects() const;
  void Reset();

  bool HasSameEffectChain(const CPostProcessor& other) const;
  bool ShareEffectState(const CPostProcessor& other);
  void Update(const D12FrameFormat& srcFormat);
  bool NeedsReconfigure(const D12FrameFormat& srcFormat) const;
  bool RequiresFullDamage() const;
  bool Configure(const D12FrameFormat& srcFormat, bool * formatChanged,
    bool * configured);
  void AdjustFrameDamage(RECT dirtyRects[], unsigned * nbDirtyRects);
  ComPtr<ID3D12Resource> Run(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects);

  const D12FrameFormat& GetOutputFormat() const { return m_dstFormat; }
  bool HasActiveEffects() const { return m_effectsActive; }
  void GetTimingToken(unsigned * effectIndex, uint64_t * token) const;
  void RecordTiming(unsigned effectIndex, uint64_t token,
    bool fullCopy, uint64_t totalTime);
  unsigned GetOutputPitch() const { return m_pitch;     }
  size_t   GetOutputSize () const { return m_frameSize; }
  bool ShouldCopyFully(
    const RECT dirtyRects[], unsigned nbDirtyRects) const;
  void CopyToFrameBuffer(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, ID3D12Resource * src,
    const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const;
  void CopyToCandidate(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, ID3D12Resource * src) const;
  void CopyFromCandidate(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, ID3D12Resource * src,
    const RECT dirtyRects[], unsigned nbDirtyRects, bool fullCopy) const;
};
