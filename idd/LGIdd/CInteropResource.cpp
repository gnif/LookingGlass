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

#include "CInteropResource.h"
#include "CDebug.h"

bool CInteropResource::Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, ComPtr<ID3D11Texture2D> srcTex)
{
  HRESULT hr;

  D3D11_TEXTURE2D_DESC srcDesc;
  srcTex->GetDesc(&srcDesc);

  ComPtr<IDXGIResource1> rSrcTex;
  hr = srcTex.As(&rSrcTex);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to obtain the IDXGIResource1 interface");
    return false;
  }

  HANDLE h;
  Wrappers::HandleT<Wrappers::HandleTraits::HANDLENullTraits> sharedHandle;
  hr = rSrcTex->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ, NULL, &h);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the shared handle");
    return false;
  }
  sharedHandle.Attach(h);

  ComPtr<ID3D12Resource> dst;
  hr = dx12Device->GetDevice()->OpenSharedHandle(sharedHandle.Get(), IID_PPV_ARGS(&dst));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to open the D3D12Resource from the handle");
    return false;
  }

  sharedHandle.Close();

  ComPtr<ID3D11Fence> d11Fence;
  hr = dx11Device->GetDevice()->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d11Fence));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the d3d11 fence");
    return false;
  }

  hr = d11Fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &h);
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to create the d3d11 fence shared handle");
    return false;
  }
  sharedHandle.Attach(h);

  ComPtr<ID3D12Fence> d12Fence;
  hr = dx12Device->GetDevice()->OpenSharedHandle(sharedHandle.Get(), IID_PPV_ARGS(&d12Fence));
  if (FAILED(hr))
  {
    DEBUG_ERROR_HR(hr, "Failed to open the D3D12Fence from the handle");
    return false;
  }

  sharedHandle.Close();

  m_dx11Device = dx11Device;
  m_dx12Device = dx12Device;

  memcpy(&m_format, &srcDesc, sizeof(m_format));
  m_srcTex       = srcTex.Get();
  m_d12Res       = dst;
  m_d11Fence     = d11Fence;
  m_d12Fence     = d12Fence;
  m_fenceValue   = 0;
  m_nbDirtyRects = 0;
  m_ready        = true;

  return true;
}

void CInteropResource::Reset()
{
  m_ready         = false;
  m_fenceValue    = 0;
  m_d12Fence.Reset();
  m_d11Fence.Reset();
  m_d12Res  .Reset();
  m_srcTex       = NULL;
  m_nbDirtyRects = 0;
  memset(&m_format, 0, sizeof(m_format));

  m_dx12Device.reset();
  m_dx11Device.reset();
}

bool CInteropResource::Compare(const ComPtr<ID3D11Texture2D>& srcTex)
{
  if (srcTex.Get() != m_srcTex)
    return false;

  D3D11_TEXTURE2D_DESC format;
  srcTex->GetDesc(&format);
  
  return
    m_format.Width  == format.Width  &&
    m_format.Height == format.Height &&
    m_format.Format == format.Format;
}

void CInteropResource::Signal()
{
  ++m_fenceValue;
  m_dx11Device->GetContext()->Signal(m_d11Fence.Get(), m_fenceValue);
}

void CInteropResource::Sync(CD3D12CommandQueue& queue)
{
  if (m_d11Fence->GetCompletedValue() < m_fenceValue)
    queue.GetCmdQueue()->Wait(m_d12Fence.Get(), m_fenceValue);
}

void CInteropResource::SetFullDamage()
{
  m_dirtyRects[0].left   = 0;
  m_dirtyRects[0].top    = 0;
  m_dirtyRects[0].right  = m_format.Width;
  m_dirtyRects[0].bottom = m_format.Height;
  m_nbDirtyRects = 1;
}
void CInteropResource::SetDirtyRects(const RECT * dirtyRects, unsigned nbDirtyRects)
{
  if (nbDirtyRects > LG_MAX_DIRTY_RECTS)
  {
    SetFullDamage();
    return;
  }

  memcpy(m_dirtyRects, dirtyRects, nbDirtyRects * sizeof(*m_dirtyRects));
  m_nbDirtyRects = nbDirtyRects;
}
