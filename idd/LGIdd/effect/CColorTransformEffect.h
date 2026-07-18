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

#pragma once

#include "CComputeEffect.h"

class CColorTransformEffect : public CComputeEffect
{
private:
  struct Consts
  {
    float matrix[3][4];
    float scalar;
    UINT  matrixEnabled;
    UINT  lutEnabled;
    UINT  inputTransfer;
    UINT  outputTransfer;
  } m_consts = {};
  float m_lut[4096][4] = {};
  bool  m_uploadPending = false;

  ComPtr<ID3D12Resource> m_constBuffer;
  ComPtr<ID3D12Resource> m_lutBuffer;
  DXGI_FORMAT            m_srcFormat = DXGI_FORMAT_UNKNOWN;
  DXGI_FORMAT            m_dstFormat = DXGI_FORMAT_UNKNOWN;

public:
  const char * GetName() const override { return "ColorTransform"; }

  bool Init(const ComPtr<ID3D12Device3>& device);

  PostProcessStatus SetFormat(const ComPtr<ID3D12Device3>& device,
    const D12FrameFormat& src, D12FrameFormat& dst) override;

  ComPtr<ID3D12Resource> Run(const ComPtr<ID3D12Device3>& device,
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects) override;
};
