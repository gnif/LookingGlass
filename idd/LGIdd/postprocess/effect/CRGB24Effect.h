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

#include "CComputeEffect.h"

class CRGB24Effect : public CComputeEffect
{
private:
  struct State;
  std::shared_ptr<State> m_state;
  unsigned               m_width  = 0;
  unsigned               m_height = 0;
  unsigned               m_pitch  = 0;

public:
  const char * GetName() const override { return "RGB24"; }

  bool Init(const ComPtr<ID3D12Device3>& device);
  void ShareState(const CPostProcessEffect& other) override;
  void Update(const D12FrameFormat& format) override;
  bool NeedsReconfigure() const override;
  bool RequiresFullDamage() const override;
  uint64_t GetTimingToken() const override;
  void RecordTiming(
    uint64_t token, bool fullCopy, uint64_t totalTime) override;
  bool GetCopyLayout(
    unsigned * pitch, unsigned * dataHeight) const override;
  bool ShouldCopyFully(
    const RECT dirtyRects[], unsigned nbDirtyRects) const override;
  void CopyFrame(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, ID3D12Resource * src,
    const RECT dirtyRects[], unsigned nbDirtyRects,
    bool fullCopy) const override;

  PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) override;

  ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) override;
};
