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

#include "postprocess/CPostProcessor.h"

#include <cstddef>

#define POST_PROCESS_THREADS_STR "8"

namespace PostProcessUtil
{
  static constexpr unsigned Threads = 8;

  template<typename T>
  static constexpr T AlignTo(T value, T align)
  {
    return (value + (align - 1)) & ~(align - 1);
  }

  bool CreateDefaultTexture(const ComPtr<ID3D12Device3>& device,
    const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& resource);
  bool CreateDefaultBuffer(const ComPtr<ID3D12Device3>& device,
    UINT64 size, ComPtr<ID3D12Resource>& resource);
  HRESULT CreateUploadBuffer(const ComPtr<ID3D12Device3>& device,
    size_t size, ComPtr<ID3D12Resource>& resource);
  HRESULT Upload(const ComPtr<ID3D12Resource>& resource,
    const void * data, size_t size);

  D3D12_DESCRIPTOR_RANGE Range(
    D3D12_DESCRIPTOR_RANGE_TYPE type, UINT shaderRegister);

  static constexpr unsigned Groups(unsigned value)
  {
    return (value + (Threads - 1)) / Threads;
  }
}

class CComputeEffect : public CPostProcessEffect
{
protected:
  ComPtr<ID3D12RootSignature>  m_rootSignature;
  ComPtr<ID3D12PipelineState>  m_pso;
  ComPtr<ID3D12DescriptorHeap> m_descHeap;
  ComPtr<ID3D12Resource>       m_dst;
  D3D12_RESOURCE_DESC          m_outDesc = {};
  unsigned                     m_threadsX = 0;
  unsigned                     m_threadsY = 0;

  bool InitCompute(const ComPtr<ID3D12Device3>& device,
    const D3D12_DESCRIPTOR_RANGE * ranges, UINT rangeCount,
    const D3D12_STATIC_SAMPLER_DESC * samplers, UINT samplerCount,
    const char * shader);

  void Bind(const ComPtr<ID3D12GraphicsCommandList>& commandList);
  void Dispatch(const ComPtr<ID3D12GraphicsCommandList>& commandList);

  D3D12_CPU_DESCRIPTOR_HANDLE Handle(
    const ComPtr<ID3D12Device3>& device, UINT index) const;
  void CBV(const ComPtr<ID3D12Device3>& device, UINT index,
    ID3D12Resource * resource, size_t size) const;
  void SRV(const ComPtr<ID3D12Device3>& device, UINT index,
    ID3D12Resource * resource, DXGI_FORMAT format) const;
  void UAV(const ComPtr<ID3D12Device3>& device, UINT index,
    ID3D12Resource * resource, DXGI_FORMAT format) const;

  bool IsDst(ID3D12Resource * dst) const;

  void TransitionDst(const ComPtr<ID3D12GraphicsCommandList>& commandList,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
  void TransitionDst(const ComPtr<ID3D12GraphicsCommandList>& commandList,
    ID3D12Resource * dst, D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after);
};
