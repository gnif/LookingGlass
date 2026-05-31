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
#include <d3d11_4.h>
#include "CInteropResource.h"

using namespace Microsoft::WRL;

#define POOL_SIZE 10

class CInteropResourcePool
{
  private:
    CInteropResource m_pool[POOL_SIZE];
    
    std::shared_ptr<CD3D11Device> m_dx11Device;
    std::shared_ptr<CD3D12Device> m_dx12Device;
   
  public:
    void Init(std::shared_ptr<CD3D11Device> dx11Device, std::shared_ptr<CD3D12Device> dx12Device);
    void Reset();

    CInteropResource* Get(ComPtr<ID3D11Texture2D> srcTex);
};