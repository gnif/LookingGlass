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

#include "effect.h"

#include "d12.h"
#include "command_group.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"
#include "common/option.h"

#include <d3dcompiler.h>

typedef struct HDR16to10Inst
{
  D12Effect base;

  ID3D12RootSignature  ** rootSignature;
  ID3D12PipelineState  ** pso;
  ID3D12DescriptorHeap ** descHeap;

  unsigned threadsX, threadsY;
  ID3D12Resource ** dst;
}
HDR16to10Inst;

#define THREADS 8

static void d12_effect_hdr16to10InitOptions(void)
{
  struct Option options[] =
  {
    {
      .module       = "d12",
      .name         = "HDR16to10",
      .description  =
        "Convert HDR16/8bpp to HDR10/4bpp (saves bandwidth)",
      .type         = OPTION_TYPE_BOOL,
      .value.x_bool = true
    },
    {0}
  };

  option_register(options);
}

static D12EffectStatus d12_effect_hdr16to10Create(D12Effect ** instance,
  ID3D12Device3 * device, const DISPLAYCONFIG_PATH_INFO * displayPathInfo)
{
  (void)displayPathInfo;

  if (!option_get_bool("d12", "HDR16to10"))
    return D12_EFFECT_STATUS_BYPASS;

  HDR16to10Inst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return D12_EFFECT_STATUS_ERROR;
  }

  bool result = D12_EFFECT_STATUS_ERROR;
  HRESULT hr;
  comRef_scopePush(10);

  // shader resource view
  D3D12_DESCRIPTOR_RANGE descriptorRanges[2] =
  {
    {
      .RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
      .NumDescriptors                    = 1,
      .BaseShaderRegister                = 0,
      .RegisterSpace                     = 0,
      .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
    },
    {
      .RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
      .NumDescriptors                    = 1,
      .BaseShaderRegister                = 0,
      .RegisterSpace                     = 0,
      .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
    }
  };

  // descriptor table
  D3D12_ROOT_PARAMETER rootParams[1] =
  {
    {
      .ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE,
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL,
      .DescriptorTable =
      {
        .NumDescriptorRanges = ARRAY_LENGTH(descriptorRanges),
        .pDescriptorRanges   = descriptorRanges
      }
    }
  };

  // root signature
  D3D12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc =
  {
    .Version  = D3D_ROOT_SIGNATURE_VERSION_1,
    .Desc_1_0 =
    {
      .NumParameters     = ARRAY_LENGTH(rootParams),
      .pParameters       = rootParams,
      .NumStaticSamplers = 0,
      .pStaticSamplers   = NULL,
      .Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE
    }
  };

  // Serialize the root signature
  comRef_defineLocal(ID3DBlob, blob );
  comRef_defineLocal(ID3DBlob, error);
  hr = DX12.D3D12SerializeVersionedRootSignature(
    &rootSignatureDesc, blob, error);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to serialize the root signature", hr);
    DEBUG_ERROR("%s", (const char *)ID3DBlob_GetBufferPointer(*error));
    goto exit;
  }

  // Create the root signature
  comRef_defineLocal(ID3D12RootSignature, rootSignature);
  hr = ID3D12Device_CreateRootSignature(
    device,
    0,
    ID3DBlob_GetBufferPointer(*blob),
    ID3DBlob_GetBufferSize(*blob),
    &IID_ID3D12RootSignature,
    (void **)rootSignature);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the root signature", hr);
    goto exit;
  }

  // Compile the shader
  const char * testCode =
    "Texture2D  <float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "static const float PQ_m1 = 0.1593017578125;\n"
    "static const float PQ_m2 = 78.84375;\n"
    "static const float PQ_c1 = 0.8359375;\n"
    "static const float PQ_c2 = 18.8515625;\n"
    "static const float PQ_c3 = 18.6875;\n"
    "\n"
    "[numthreads(" STR(THREADS) ", " STR(THREADS) ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  float4 color = src[dt.xy];\n"
    "  float3 linear709 = color.rgb * (80.0 / 10000.0);\n"
    "  float3 linear2020 = float3(\n"
    "    dot(linear709, float3(0.6274039, 0.3292830, 0.0433131)),\n"
    "    dot(linear709, float3(0.0690973, 0.9195404, 0.0113623)),\n"
    "    dot(linear709, float3(0.0163914, 0.0880133, 0.8955953)));\n"
    "  float3 p = pow(max(linear2020, 0.0), PQ_m1);\n"
    "  float3 pq = pow((PQ_c1 + PQ_c2 * p) /\n"
    "    (1.0 + PQ_c3 * p), PQ_m2);\n"
    "  dst[dt.xy] = float4(pq, color.a);\n"
    "}\n";

  bool debug = false;
  hr = D3DCompile(
    testCode, strlen(testCode),
    NULL, NULL, NULL, "main", "cs_5_0",
    debug ? (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION) : 0,
    0, blob, error);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to compile the shader");
    DEBUG_ERROR("%s", (const char *)ID3DBlob_GetBufferPointer(*error));
    goto exit;
  }

  // Create the PSO
  D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc =
  {
    .pRootSignature = *rootSignature,
    .CS             =
    {
      .pShaderBytecode = ID3DBlob_GetBufferPointer(*blob),
      .BytecodeLength  = ID3DBlob_GetBufferSize   (*blob)
    }
  };

  comRef_defineLocal(ID3D12PipelineState, pso);
  hr = ID3D12Device3_CreateComputePipelineState(
    device, &psoDesc, &IID_ID3D12PipelineState, (void **)pso);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the PSO", hr);
    goto exit;
  }

  // Create the descriptor heap
  D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc =
  {
    .Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    .NumDescriptors = ARRAY_LENGTH(descriptorRanges),
    .Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask       = 0
  };

  comRef_defineLocal(ID3D12DescriptorHeap, descHeap);
  hr = ID3D12Device3_CreateDescriptorHeap(
    device, &descHeapDesc, &IID_ID3D12DescriptorHeap, (void **)descHeap);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the parameter heap", hr);
    goto exit;
  }

  comRef_toGlobal(this->rootSignature, rootSignature);
  comRef_toGlobal(this->pso          , pso          );
  comRef_toGlobal(this->descHeap     , descHeap     );

  result = D12_EFFECT_STATUS_OK;
  *instance = &this->base;

exit:
  if (!*instance)
    free(this);

  comRef_scopePop();
  return result;
}

static void d12_effect_hdr16to10Free(D12Effect ** instance)
{
  HDR16to10Inst * this = UPCAST(HDR16to10Inst, *instance);

  free(this);
}

static D12EffectStatus d12_effect_hdr16to10SetFormat(D12Effect * effect,
  ID3D12Device3             * device,
  const D12FrameFormat * src,
        D12FrameFormat * dst)
{
  HDR16to10Inst * this = UPCAST(HDR16to10Inst, effect);
  comRef_scopePush(1);

  D12EffectStatus result = D12_EFFECT_STATUS_ERROR;
  HRESULT hr;

  if (src->desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
      !src->hdr || src->hdrPQ)
  {
    result = D12_EFFECT_STATUS_BYPASS;
    goto exit;
  }

  D3D12_HEAP_PROPERTIES heapProps =
  {
    .Type                 = D3D12_HEAP_TYPE_DEFAULT,
    .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
    .CreationNodeMask     = 1,
    .VisibleNodeMask      = 1
  };

  D3D12_RESOURCE_DESC desc =
  {
    .Format           = DXGI_FORMAT_R10G10B10A2_UNORM,
    .Width            = src->desc.Width,
    .Height           = src->desc.Height,
    .Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
    .Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
    .MipLevels        = 1,
    .DepthOrArraySize = 1,
    .SampleDesc.Count = 1
  };

  comRef_defineLocal(ID3D12Resource, res);
  hr = ID3D12Device3_CreateCommittedResource(
    device, &heapProps, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc,
    D3D12_RESOURCE_STATE_COPY_SOURCE, NULL, &IID_ID3D12Resource,
    (void **)res);

  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to create the destination texture");
    goto exit;
  }

  comRef_toGlobal(this->dst, res);
  this->threadsX = (desc.Width  + (THREADS-1)) / THREADS;
  this->threadsY = (desc.Height + (THREADS-1)) / THREADS;

  dst->desc   = desc;
  dst->format = CAPTURE_FMT_RGBA10;
  dst->hdr    = true;
  dst->hdrPQ  = true;
  result      = D12_EFFECT_STATUS_OK;

exit:
  comRef_scopePop();
  return result;
}

static ID3D12Resource * d12_effect_hdr16to10Run(D12Effect * effect,
  ID3D12Device3 * device, ID3D12GraphicsCommandList * commandList,
  ID3D12Resource * src, RECT dirtyRects[], unsigned * nbDirtyRects)
{
  HDR16to10Inst * this = UPCAST(HDR16to10Inst, effect);

  // transition the destination texture to unordered access so we can write to it
  {
    D3D12_RESOURCE_BARRIER barrier =
    {
      .Type  = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition =
      {
        .pResource   = *this->dst,
        .StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE,
        .StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
      }
    };
    ID3D12GraphicsCommandList_ResourceBarrier(commandList, 1, &barrier);
  }

  // get the heap handle
  D3D12_CPU_DESCRIPTOR_HANDLE cpuSrvUavHandle =
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(*this->descHeap);

  // descriptor for input SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
  {
    .Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT,
    .ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D,
    .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    .Texture2D.MipLevels     = 1
  };
  ID3D12Device3_CreateShaderResourceView(device, src, &srvDesc, cpuSrvUavHandle);

  // move to the next slot
  cpuSrvUavHandle.ptr += ID3D12Device3_GetDescriptorHandleIncrementSize(
    device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // descriptor for the output UAV
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc =
  {
    .Format        = DXGI_FORMAT_R10G10B10A2_UNORM,
    .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D
  };
  ID3D12Device3_CreateUnorderedAccessView(
    device, *this->dst, NULL, &uavDesc, cpuSrvUavHandle);

  // bind the descriptor heap to the pipeline
  ID3D12GraphicsCommandList_SetDescriptorHeaps(commandList, 1, this->descHeap);

  // set the pipeline state
  ID3D12GraphicsCommandList_SetPipelineState(commandList, *this->pso);

  // set the root signature on the command list
  ID3D12GraphicsCommandList_SetComputeRootSignature(
    commandList, *this->rootSignature);

  // get the GPU side handle for our heap
  D3D12_GPU_DESCRIPTOR_HANDLE gpuSrvUavHandle =
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(*this->descHeap);

  // bind the descriptor tables to the root signature
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
    commandList, 0, gpuSrvUavHandle);

  ID3D12GraphicsCommandList_Dispatch(
    commandList, this->threadsX, this->threadsY, 1);

  // transition the destination texture to a copy source for the next stage
  {
    D3D12_RESOURCE_BARRIER barrier =
    {
      .Type       = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Flags      = D3D12_RESOURCE_BARRIER_FLAG_NONE,
      .Transition =
      {
        .pResource   = *this->dst,
        .StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        .StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE,
        .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
      }
    };
    ID3D12GraphicsCommandList_ResourceBarrier(commandList, 1, &barrier);
  }

  // return the output buffer
  return *this->dst;
}

const D12Effect D12Effect_HDR16to10 =
{
  .name        = "HDR16to10",
  .initOptions = d12_effect_hdr16to10InitOptions,
  .create      = d12_effect_hdr16to10Create,
  .free        = d12_effect_hdr16to10Free,
  .setFormat   = d12_effect_hdr16to10SetFormat,
  .run         = d12_effect_hdr16to10Run
};
