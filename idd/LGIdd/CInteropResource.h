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

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <memory>

#include "CD3D11Device.h"
#include "CD3D12Device.h"
#include "CD3D12CommandQueue.h"

using namespace Microsoft::WRL;

#define LG_MAX_DIRTY_RECTS 256

class CInteropResource
{
  private:
    std::shared_ptr<CD3D11Device> m_dx11Device;
    std::shared_ptr<CD3D12Device> m_dx12Device;

    /* this value is likely released, it is only used to check if the texture supplied
    is different, do not rely on it pointing to valid memory */
    void * m_srcTex;

    ComPtr<ID3D12Resource > m_d12Res;
    D3D11_TEXTURE2D_DESC    m_format;
    ComPtr<ID3D11Fence    > m_d11Fence;
    ComPtr<ID3D12Fence    > m_d12Fence;
    UINT64                  m_fenceValue;
    bool                    m_ready;

    RECT     m_dirtyRects[LG_MAX_DIRTY_RECTS];
    unsigned m_nbDirtyRects;

  public:
    bool Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device, ComPtr<ID3D11Texture2D> srcTex);
    void Reset();

    bool IsReady() { return m_ready; }
    bool Compare(const ComPtr<ID3D11Texture2D>& srcTex);
    void Signal();
    void Sync(CD3D12CommandQueue& queue);
    void SetFullDamage();
    void SetDirtyRects(const RECT * dirtyRects, unsigned nbDirtyRects);

    const ComPtr<ID3D12Resource>& GetRes() { return m_d12Res; }
    const D3D11_TEXTURE2D_DESC& GetFormat() { return m_format; }
    const RECT * GetDirtyRects() { return m_dirtyRects; }
    unsigned GetDirtyRectCount() { return m_nbDirtyRects; }
};
