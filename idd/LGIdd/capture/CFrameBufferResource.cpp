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

#include "capture/CFrameBufferResource.h"
#include "d3d/CD3D12Device.h"
#include "CDebug.h"

#include <cstring>

bool CFrameBufferResource::Init(CD3D12Device * dx12,
  const FrameToken& token, uint8_t * base, uint64_t heapOffset, size_t size,
  size_t maxFrameSize)
{
  if (size > maxFrameSize)
  {
    DEBUG_ERROR("Frame size of %llu is too large for transport memory",
      (unsigned long long)size);
    return false;
  }

  const bool indirect = dx12->IsIndirectCopy();

  D3D12_RESOURCE_DESC desc = {};
  desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width              = size;
  desc.Height             = 1;
  desc.DepthOrArraySize   = 1;
  desc.MipLevels          = 1;
  desc.Format             = DXGI_FORMAT_UNKNOWN;
  desc.SampleDesc.Count   = 1;
  desc.SampleDesc.Quality = 0;
  desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  desc.Flags              = D3D12_RESOURCE_FLAG_NONE;
  if (!indirect)
  {
    desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    desc.Flags     = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
  }

  // Nothing to do if the resource already represents this allocation.
  if (m_base == base && m_size >= size)
  {
    m_token     = token;
    m_frameSize = size;
    return true;
  }

  Reset();

  HRESULT       hr;
  const WCHAR * resName;

  if (indirect)
  {
    DEBUG_TRACE("Creating standard resource for %p", base);
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                 = D3D12_HEAP_TYPE_READBACK;
    heapProps.CPUPageProperty      = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask     = 1;
    heapProps.VisibleNodeMask      = 1;

    hr = dx12->GetDevice()->CreateCommittedResource(
      &heapProps,
      D3D12_HEAP_FLAG_NONE,
      &desc,
      D3D12_RESOURCE_STATE_COPY_DEST,
      NULL,
      IID_PPV_ARGS(&m_res)
    );
    resName = L"STAGING";

    if (SUCCEEDED(hr))
    {
      D3D12_RANGE range = {0, 0};
      hr = m_res->Map(0, &range, &m_map);
      if (FAILED(hr))
      {
        DEBUG_ERROR_HR(hr, "Failed to map the resource");
        return false;
      }
    }
  }
  else
  {
    const D3D12_RESOURCE_ALLOCATION_INFO allocation =
      dx12->GetDevice()->GetResourceAllocationInfo(0, 1, &desc);
    const D3D12_HEAP_DESC heapDesc =
      dx12->GetTransportHeap()->GetDesc();
    if (!allocation.Alignment ||
        heapOffset % allocation.Alignment ||
        allocation.SizeInBytes > maxFrameSize ||
        heapOffset > heapDesc.SizeInBytes ||
        allocation.SizeInBytes > heapDesc.SizeInBytes - heapOffset)
    {
      DEBUG_ERROR(
        "Transport resource does not fit its framebuffer allocation");
      return false;
    }

    DEBUG_TRACE("Creating transport buffer for %p", base);
    resName = L"Transport Buffer";

    hr = dx12->GetDevice()->CreatePlacedResource(
      dx12->GetTransportHeap().Get(),
      heapOffset,
      &desc,
      D3D12_RESOURCE_STATE_COMMON,
      NULL,
      IID_PPV_ARGS(&m_res)
    );
  }

  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the FrameBuffer ID3D12Resource");
    return false;
  }

  m_res->SetName(resName);

  m_token     = token;
  m_base      = base;
  m_size      = size;
  m_frameSize = size;
  return true;
}

void CFrameBufferResource::Reset()
{
  if (m_map)
  {
    m_res->Unmap(0, NULL);
    m_map = NULL;
  }

  m_token             = {};
  m_base              = nullptr;
  m_size              = 0;
  m_frameSize         = 0;
  m_fullCopy          = false;
  m_nbCopyDirtyRects  = 0;
  m_copyPitch         = 0;
  m_copyBytesPerPixel = 0;
  m_res.Reset();
}

void CFrameBufferResource::SetCopyDamage(const RECT dirtyRects[],
  unsigned nbDirtyRects, bool fullCopy, unsigned pitch,
  unsigned bytesPerPixel)
{
  m_fullCopy          = fullCopy;
  m_nbCopyDirtyRects  = fullCopy ? 0 : nbDirtyRects;
  m_copyPitch         = pitch;
  m_copyBytesPerPixel = bytesPerPixel;
  if (m_nbCopyDirtyRects)
    memcpy(m_copyDirtyRects, dirtyRects,
      m_nbCopyDirtyRects * sizeof(*m_copyDirtyRects));
}
