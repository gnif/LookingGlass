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

#include "CComputeEffect.h"

#include "CDebug.h"

#include <d3dcompiler.h>
#include <cstring>

namespace PostProcessUtil
{
  bool CreateDefaultTexture(const ComPtr<ID3D12Device3>& device,
    const D3D12_RESOURCE_DESC& desc, ComPtr<ID3D12Resource>& resource)
  {
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                 = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask     = 1;
    heapProps.VisibleNodeMask      = 1;

    HRESULT hr = device->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_CREATE_NOT_ZEROED,
      &desc,
      D3D12_RESOURCE_STATE_COPY_SOURCE,
      nullptr,
      IID_PPV_ARGS(&resource));
    if (FAILED(hr))
    {
      DEBUG_ERROR_HR(hr, "Failed to create post-processing destination texture");
      return false;
    }

    return true;
  }
}

bool CComputeEffect::InitCompute(const ComPtr<ID3D12Device3>& device,
  const D3D12_DESCRIPTOR_RANGE * ranges, UINT rangeCount,
  const D3D12_STATIC_SAMPLER_DESC * samplers, UINT samplerCount,
  const char * shader)
{
  D3D12_ROOT_PARAMETER rootParam = {};
  rootParam.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  rootParam.DescriptorTable.NumDescriptorRanges = rangeCount;
  rootParam.DescriptorTable.pDescriptorRanges   = ranges;

  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
  rootSignatureDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1;
  rootSignatureDesc.Desc_1_0.NumParameters     = 1;
  rootSignatureDesc.Desc_1_0.pParameters       = &rootParam;
  rootSignatureDesc.Desc_1_0.NumStaticSamplers = samplerCount;
  rootSignatureDesc.Desc_1_0.pStaticSamplers   = samplers;
  rootSignatureDesc.Desc_1_0.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  ComPtr<ID3DBlob> blob;
  ComPtr<ID3DBlob> error;
  HRESULT hr = D3D12SerializeVersionedRootSignature(
    &rootSignatureDesc, &blob, &error);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to serialize post-processing root signature");
    if (error)
      DEBUG_ERROR("%s", (const char *)error->GetBufferPointer());
    return false;
  }

  hr = device->CreateRootSignature(
    0,
    blob->GetBufferPointer(),
    blob->GetBufferSize(),
    IID_PPV_ARGS(&m_rootSignature));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create post-processing root signature");
    return false;
  }

  blob.Reset();
  error.Reset();
  hr = D3DCompile(
    shader,
    std::strlen(shader),
    nullptr,
    nullptr,
    nullptr,
    "main",
    "cs_5_0",
    0,
    0,
    &blob,
    &error);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to compile post-processing shader");
    if (error)
      DEBUG_ERROR("%s", (const char *)error->GetBufferPointer());
    return false;
  }

  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
  psoDesc.pRootSignature = m_rootSignature.Get();
  psoDesc.CS.pShaderBytecode = blob->GetBufferPointer();
  psoDesc.CS.BytecodeLength  = blob->GetBufferSize();

  hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pso));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create post-processing PSO");
    return false;
  }

  UINT descriptorCount = 0;
  for (UINT i = 0; i < rangeCount; ++i)
    descriptorCount += ranges[i].NumDescriptors;

  D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
  heapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  heapDesc.NumDescriptors = descriptorCount;
  heapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

  hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_descHeap));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create post-processing descriptor heap");
    return false;
  }

  return true;
}

void CComputeEffect::Bind(const ComPtr<ID3D12GraphicsCommandList>& commandList)
{
  ID3D12DescriptorHeap * heaps[] = { m_descHeap.Get() };
  commandList->SetDescriptorHeaps(1, heaps);
  commandList->SetPipelineState(m_pso.Get());
  commandList->SetComputeRootSignature(m_rootSignature.Get());
  commandList->SetComputeRootDescriptorTable(
    0, m_descHeap->GetGPUDescriptorHandleForHeapStart());
}

void CComputeEffect::TransitionDst(
  const ComPtr<ID3D12GraphicsCommandList>& commandList,
  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource   = m_dst.Get();
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter  = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  commandList->ResourceBarrier(1, &barrier);
}
