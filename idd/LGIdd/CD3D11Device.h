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

#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <d3d11_4.h>

using namespace Microsoft::WRL;

struct CD3D11Device
{
private:
  LUID m_adapterLuid;
  ComPtr<IDXGIFactory5       > m_factory;
  ComPtr<IDXGIAdapter1       > m_adapter;
  ComPtr<ID3D11Device5       > m_device;
  ComPtr<ID3D11DeviceContext4> m_context;

public:
  CD3D11Device(LUID adapterLuid) :
    m_adapterLuid(adapterLuid) {};

  CD3D11Device()
  {
    m_adapterLuid = LUID{};
  }

  HRESULT Init();

  ComPtr<ID3D11Device5> GetDevice() { return m_device; }

  ComPtr<ID3D11DeviceContext4> GetContext() { return m_context; }
};