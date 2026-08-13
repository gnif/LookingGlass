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

class CFormatEffect : public CComputeEffect
{
private:
  D3D12_RESOURCE_DESC m_srcDesc   = {};
  DXGI_FORMAT         m_srcFormat = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT         m_dstFormat = DXGI_FORMAT_UNKNOWN;

  bool IsSrc(ID3D12Resource * src) const;
  bool Draw(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, ID3D12Resource * dst,
    RECT dirtyRects[], unsigned * nbDirtyRects);

public:
  const char * GetName() const override { return "Format"; }

  bool Init(const ComPtr<ID3D12Device3>& device);

  PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) override;
  PostProcessStatus Cfg(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, const D12FrameFormat& dst);

  ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) override;
  // dst must match Cfg's output and be in COMMON; Run restores COMMON.
  bool Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, ID3D12Resource * dst,
    RECT dirtyRects[], unsigned * nbDirtyRects);
};
