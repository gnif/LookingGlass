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

#pragma once

#include "CComputeEffect.h"

class CHDR16to10Effect : public CComputeEffect
{
private:
  struct Consts
  {
    float ReferenceWhiteNits; // scRGB reference white in nits (BT.2408 recommends 203)
  } m_consts = { 203.0f };
  ComPtr<ID3D12Resource> m_constBuffer;

public:
  const char * GetName() const override { return "HDR16to10"; }

  bool Init(const ComPtr<ID3D12Device3>& device);

  PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) override;

  ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) override;
};
