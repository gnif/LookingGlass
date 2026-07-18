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

#include <Windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <memory>
#include <vector>

struct CD3D12Device;

extern "C" {
  #include "common/KVMFR.h"
  #include "common/types.h"
}

using namespace Microsoft::WRL;

enum class PostProcessStatus
{
  SUCCESS,
  BYPASS_EFFECT,
  FAILED
};

struct D12FrameFormat
{
  D3D12_RESOURCE_DESC desc = {};
  unsigned            width = 0;
  unsigned            height = 0;
  FrameType           format = FRAME_TYPE_INVALID;
  bool                hdr = false;
  bool                hdrPQ = false;
  bool                hdrMetadata = false;
  uint32_t            sdrWhiteLevel = KVMFR_SDR_WHITE_LEVEL_DEFAULT;

  // HDR static metadata (SMPTE ST 2086)
  // Display color primaries in 0.00002 units (xy coordinates)
  uint16_t displayPrimary[3][2];
  // White point in 0.00002 units
  uint16_t whitePoint[2];
  // Max mastering display luminance in whole cd/m²
  uint32_t maxDisplayLuminance;
  // Min mastering display luminance in 0.0001 cd/m² units
  uint32_t minDisplayLuminance;
  // MaxCLL and MaxFALL in cd/m²
  uint32_t maxContentLightLevel;
  uint32_t maxFrameAverageLightLevel;
};

class CPostProcessEffect
{
public:
  virtual ~CPostProcessEffect() {}
  virtual const char * GetName() const = 0;
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
  std::shared_ptr<CD3D12Device> m_dx12Device;
  ComPtr<ID3D12Device3> m_device;
  std::vector<std::unique_ptr<CPostProcessEffect>> m_effects;
  D12FrameFormat m_srcFormat = {};
  D12FrameFormat m_dstFormat = {};
  bool m_effectsActive = false;

public:
  bool Init(std::shared_ptr<CD3D12Device> dx12Device);
  void Reset();

  bool Configure(const D12FrameFormat& srcFormat, bool * formatChanged);
  void AdjustFrameDamage(RECT dirtyRects[], unsigned * nbDirtyRects);
  ComPtr<ID3D12Resource> Run(
    const ComPtr<ID3D12GraphicsCommandList>& commandList,
    const ComPtr<ID3D12Resource>& src, RECT dirtyRects[],
    unsigned * nbDirtyRects);

  const D12FrameFormat& GetOutputFormat() const { return m_dstFormat; }
  bool HasActiveEffects() const { return m_effectsActive; }
};
