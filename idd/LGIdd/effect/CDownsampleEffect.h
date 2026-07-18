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

#include <vector>
#include <string>

class CDownsampleEffect : public CComputeEffect
{
private:
  struct Rule
  {
    bool greater = false;
    unsigned x = 0;
    unsigned y = 0;
    unsigned targetX = 0;
    unsigned targetY = 0;
  };

  struct Consts
  {
    float width;
    float height;
  } m_consts = {};

  std::vector<Rule> m_rules;
  ComPtr<ID3D12Resource> m_constBuffer;
  DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
  double m_scaleX = 1.0;
  double m_scaleY = 1.0;
  unsigned m_width = 0;
  unsigned m_height = 0;

  bool ParseRules(const std::wstring& value);
  const Rule * MatchRule(unsigned width, unsigned height) const;

public:
  const char * GetName() const override { return "Downsample"; }

  bool Init(const ComPtr<ID3D12Device3>& device);

  PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) override;

  void AdjustDamage(RECT dirtyRects[], unsigned * nbDirtyRects) override;

  ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) override;
};
