#include "effect.h"

#include "d12.h"
#include "command_group.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"
#include "common/option.h"

#include "downsample_parser.h"

#include <d3dcompiler.h>
#include <math.h>

typedef struct DownsampleInst
{
  D12Effect base;

  struct
  {
    float width;
    float height;
  }
  consts;

  ID3D12RootSignature  ** rootSignature;
  ID3D12PipelineState  ** pso;
  ID3D12DescriptorHeap ** resHeap;
  ID3D12Resource       ** constBuffer;

  unsigned threadsX, threadsY;
  DXGI_FORMAT format;
  double scaleX, scaleY;
  unsigned width, height;
  ID3D12Resource ** dst;
}
DownsampleInst;

#define THREADS 8

static Vector downsampleRules = {0};

static void d12_effect_downsampleInitOptions(void)
{
  struct Option options[] =
  {
    DOWNSAMPLE_PARSER("d12", &downsampleRules),
    {0}
  };

  option_register(options);
}

static D12EffectStatus d12_effect_downsampleCreate(D12Effect ** instance,
  ID3D12Device3 * device)
{
  DownsampleInst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return D12_EFFECT_STATUS_ERROR;
  }

  bool result = D12_EFFECT_STATUS_ERROR;
  HRESULT hr;
  comRef_scopePush(10);

  // samplers
  D3D12_STATIC_SAMPLER_DESC staticSamplers[1] =
  {
    {
      .Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR,
      .AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      .AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      .AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
      .ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER,
      .MaxLOD           = D3D12_FLOAT32_MAX,
      .ShaderRegister   = 0,
      .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL
    }
  };

  // shader resource view
  D3D12_DESCRIPTOR_RANGE resDescRanges[3] =
  {
    {
      .RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
      .NumDescriptors                    = 1,
      .BaseShaderRegister                = 0,
      .RegisterSpace                     = 0,
      .OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
    },
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
        .NumDescriptorRanges = ARRAY_LENGTH(resDescRanges),
        .pDescriptorRanges   = resDescRanges
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
      .NumStaticSamplers = ARRAY_LENGTH(staticSamplers),
      .pStaticSamplers   = staticSamplers,
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
  const char * computeShader =
    "cbuffer Constants       : register(b0)\n"
    "{\n"
    "  float Width;\n"
    "  float Height;\n"
    "};\n"
    "Texture2D  <float4> src : register(t0);\n"
    "RWTexture2D<float4> dst : register(u0);\n"
    "SamplerState        ss  : register(s0);\n"
    "\n"
    "[numthreads(" STR(THREADS) ", " STR(THREADS) ", 1)]\n"
    "void main(uint3 dt : SV_DispatchThreadID)\n"
    "{\n"
    "  dst[dt.xy] = src.SampleLevel(ss, \n"
    "    float2(\n"
    "      (float(dt.x) + 0.5f) / Width,\n"
    "      (float(dt.y) + 0.5f) / Height),\n"
    "    0);\n"
    "}\n";

  bool debug = false;
  hr = D3DCompile(
    computeShader, strlen(computeShader),
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

  // Create the resource descriptor heap
  D3D12_DESCRIPTOR_HEAP_DESC resHeapDesc =
  {
    .Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
    .NumDescriptors = ARRAY_LENGTH(resDescRanges),
    .Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,
    .NodeMask       = 0
  };

  comRef_defineLocal(ID3D12DescriptorHeap, resHeap);
  hr = ID3D12Device3_CreateDescriptorHeap(
    device, &resHeapDesc, &IID_ID3D12DescriptorHeap, (void **)resHeap);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the resource descriptor heap", hr);
    goto exit;
  }

  D3D12_HEAP_PROPERTIES constHeapProps =
  {
    .Type                 = D3D12_HEAP_TYPE_UPLOAD,
    .CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN
  };

  D3D12_RESOURCE_DESC constBufferDesc =
  {
    .Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER,
    .Width            = ALIGN_TO(sizeof(this->consts),
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT),
    .Height           = 1,
    .DepthOrArraySize = 1,
    .MipLevels        = 1,
    .Format           = DXGI_FORMAT_UNKNOWN,
    .SampleDesc.Count = 1,
    .Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
    .Flags            = D3D12_RESOURCE_FLAG_NONE
  };

  comRef_defineLocal(ID3D12Resource, constBuffer);
  hr = ID3D12Device3_CreateCommittedResource(
    device,
    &constHeapProps,
    D3D12_HEAP_FLAG_NONE,
    &constBufferDesc,
    D3D12_RESOURCE_STATE_GENERIC_READ,
    NULL,
    &IID_ID3D12Resource,
    (void **)constBuffer);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the constant buffer resource", hr);
    goto exit;
  }

  comRef_toGlobal(this->rootSignature, rootSignature);
  comRef_toGlobal(this->pso          , pso          );
  comRef_toGlobal(this->resHeap      , resHeap      );
  comRef_toGlobal(this->constBuffer  , constBuffer  );

  result = D12_EFFECT_STATUS_OK;
  *instance = &this->base;

exit:
  if (!*instance)
    free(this);

  comRef_scopePop();
  return result;
}

static void d12_effect_downsampleFree(D12Effect ** instance)
{
  DownsampleInst * this = UPCAST(DownsampleInst, *instance);

  free(this);
}

static D12EffectStatus d12_effect_downsampleSetFormat(D12Effect * effect,
  ID3D12Device3             * device,
  const D12FrameFormat * src,
        D12FrameFormat * dst)
{
  DownsampleInst * this = UPCAST(DownsampleInst, effect);
  comRef_scopePush(1);

  D12EffectStatus result = D12_EFFECT_STATUS_ERROR;
  HRESULT hr;

  DownsampleRule * rule = downsampleRule_match(
    &downsampleRules, src->desc.Width, src->desc.Height);

  if (!rule || (
    rule->targetX == src->desc.Width &&
    rule->targetY == src->desc.Height))
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
    .Format           = src->desc.Format,
    .Width            = rule->targetX,
    .Height           = rule->targetY,
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
    DEBUG_WINERROR("Failed to create the destination texture", hr);
    goto exit;
  }

  void * data;
  D3D12_RANGE readRange = { 0, 0 };
  hr = ID3D12Resource_Map(*this->constBuffer, 0, &readRange, &data);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to map the constants buffer", hr);
    goto exit;
  }

  this->consts.width  = rule->targetX;
  this->consts.height = rule->targetY;
  memcpy(data, &this->consts, sizeof(this->consts));
  ID3D12Resource_Unmap(*this->constBuffer, 0, NULL);

  comRef_toGlobal(this->dst, res);
  this->threadsX = (desc.Width  + (THREADS-1)) / THREADS;
  this->threadsY = (desc.Height + (THREADS-1)) / THREADS;
  this->format   = src->desc.Format;
  this->scaleX   = (double)desc.Width  / src->desc.Width;
  this->scaleY   = (double)desc.Height / src->desc.Height;
  this->width    = desc.Width;
  this->height   = desc.Height;

  dst->desc   = desc;
  dst->width  = desc.Width;
  dst->height = desc.Height;
  result      = D12_EFFECT_STATUS_OK;

exit:
  comRef_scopePop();
  return result;
}

static void d12_effect_downsampleAdjustDamage(D12Effect * effect,
  RECT dirtyRects[], unsigned * nbDirtyRects)
{
  DownsampleInst * this = UPCAST(DownsampleInst, effect);

  // scale the dirty rects
  for(RECT * rect = dirtyRects; rect < dirtyRects + *nbDirtyRects; ++rect)
  {
    unsigned width  = ceil((double)(rect->right  - rect->left) * this->scaleX);
    unsigned height = ceil((double)(rect->bottom - rect->top ) * this->scaleY);
    rect->left   = max(0, floor((double)rect->left * this->scaleX));
    rect->right  = min(this->width , rect->left + width);
    rect->top    = max(0, floor((double)rect->top * this->scaleY));
    rect->bottom = min(this->height, rect->top  + height);

    // enlarge the rect to avoid missing damage due to sampler rounding
    if (rect->left   > 0           ) rect->left   -= 1;
    if (rect->top    > 0           ) rect->top    -= 1;
    if (rect->right  < this->width ) rect->right  += 1;
    if (rect->bottom < this->height) rect->bottom += 1;
  }
}

static ID3D12Resource * d12_effect_downsampleRun(D12Effect * effect,
  ID3D12Device3 * device, ID3D12GraphicsCommandList * commandList,
  ID3D12Resource * src, RECT dirtyRects[], unsigned * nbDirtyRects)
{
  DownsampleInst * this = UPCAST(DownsampleInst, effect);

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

  // get the resource heap handle
  D3D12_CPU_DESCRIPTOR_HANDLE cpuResHeapHandle =
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(*this->resHeap);

  // descriptor for input CBV
  D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc =
  {
    .BufferLocation = ID3D12Resource_GetGPUVirtualAddress(*this->constBuffer),
    .SizeInBytes    = ALIGN_TO(sizeof(this->consts),
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)
  };
  ID3D12Device3_CreateConstantBufferView(device, &cbvDesc, cpuResHeapHandle);

  // move to the next slot
  cpuResHeapHandle.ptr += ID3D12Device3_GetDescriptorHandleIncrementSize(
    device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // descriptor for input SRV
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
  {
    .Format                  = this->format,
    .ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D,
    .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
    .Texture2D.MipLevels     = 1
  };
  ID3D12Device3_CreateShaderResourceView(
    device, src, &srvDesc, cpuResHeapHandle);

  // move to the next slot
  cpuResHeapHandle.ptr += ID3D12Device3_GetDescriptorHandleIncrementSize(
    device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // descriptor for the output UAV
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc =
  {
    .Format        = this->format,
    .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D
  };
  ID3D12Device3_CreateUnorderedAccessView(
    device, *this->dst, NULL, &uavDesc, cpuResHeapHandle);

  // bind the descriptor heaps to the pipeline
  ID3D12GraphicsCommandList_SetDescriptorHeaps(commandList, 1, this->resHeap);

  // set the pipeline state
  ID3D12GraphicsCommandList_SetPipelineState(commandList, *this->pso);

  // set the root signature on the command list
  ID3D12GraphicsCommandList_SetComputeRootSignature(
    commandList, *this->rootSignature);

  D3D12_GPU_DESCRIPTOR_HANDLE gpuResHeapHandle =
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(*this->resHeap);

  // bind the descriptor tables to the root signature
  ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(
    commandList, 0, gpuResHeapHandle);

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

const D12Effect D12Effect_Downsample =
{
  .name         = "Downsample",
  .initOptions  = d12_effect_downsampleInitOptions,
  .create       = d12_effect_downsampleCreate,
  .free         = d12_effect_downsampleFree,
  .setFormat    = d12_effect_downsampleSetFormat,
  .adjustDamage = d12_effect_downsampleAdjustDamage,
  .run          = d12_effect_downsampleRun
};
