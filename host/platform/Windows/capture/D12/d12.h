/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#ifndef _H_D12_
#define _H_D12_

#include "com_ref.h"
#include "interface/capture.h"

#include <d3d12.h>

extern ComScope * d12_comScope;
#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(d12_comScope, dst, src)

// APIs for the backends to call

void d12_updatePointer(
  CapturePointer * pointer, void * shape, size_t shapeSize);

// Structures for backends and effects

typedef struct D12FrameDesc
{
  CaptureRotation       rotation;
  RECT                * dirtyRects;
  unsigned              nbDirtyRects;
  DXGI_COLOR_SPACE_TYPE colorSpace;
}
D12FrameDesc;

typedef struct D12FrameFormat
{
  D3D12_RESOURCE_DESC   desc;
  DXGI_COLOR_SPACE_TYPE colorSpace;
  unsigned              width, height;
  CaptureFormat         format;
}
D12FrameFormat;

// DirectX12 library functions

struct DX12
{
  PFN_D3D12_CREATE_DEVICE       D3D12CreateDevice;
  PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterface;
  PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE
    D3D12SerializeVersionedRootSignature;
};

extern struct DX12 DX12;

#ifdef ID3D12Heap_GetDesc
#undef ID3D12Heap_GetDesc
static inline D3D12_HEAP_DESC ID3D12Heap_GetDesc(ID3D12Heap* This)
{
  D3D12_HEAP_DESC __ret;
  return *This->lpVtbl->GetDesc(This, &__ret);
}
#endif

#ifdef ID3D12Resource_GetDesc
#undef ID3D12Resource_GetDesc
static inline D3D12_RESOURCE_DESC ID3D12Resource_GetDesc(ID3D12Resource* This) {
    D3D12_RESOURCE_DESC __ret;
    return *This->lpVtbl->GetDesc(This,&__ret);
}
#endif

#ifndef ID3DBlob_GetBufferPointer
#define ID3DBlob_GetBufferPointer ID3D10Blob_GetBufferPointer
#endif

#ifndef ID3DBlob_GetBufferSize
#define ID3DBlob_GetBufferSize ID3D10Blob_GetBufferSize
#endif

#ifdef ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart
#undef ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart
static inline D3D12_CPU_DESCRIPTOR_HANDLE
  ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
  ID3D12DescriptorHeap* This)
{
  D3D12_CPU_DESCRIPTOR_HANDLE __ret;
  return *This->lpVtbl->GetCPUDescriptorHandleForHeapStart(This,&__ret);
}
#endif

#ifdef ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart
#undef ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart
static inline D3D12_GPU_DESCRIPTOR_HANDLE
  ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(
  ID3D12DescriptorHeap* This)
{
  D3D12_GPU_DESCRIPTOR_HANDLE __ret;
  return *This->lpVtbl->GetGPUDescriptorHandleForHeapStart(This,&__ret);
}
#endif

#ifdef ID3D12Resource_GetDesc
#undef ID3D12Resource_GetDesc
static inline D3D12_RESOURCE_DESC ID3D12Resource_GetDesc(ID3D12Resource* This)
{
  D3D12_RESOURCE_DESC __ret;
  return *This->lpVtbl->GetDesc(This,&__ret);
}
#endif

#endif
