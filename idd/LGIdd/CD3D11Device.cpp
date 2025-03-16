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

#include "CD3D11Device.h"
#include "CDebug.h"

HRESULT CD3D11Device::Init()
{
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
  if (FAILED(hr))
    return hr;

  hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&m_adapter));
  if (FAILED(hr))
    return hr;
  
  // only 11.1 supports DX12 interoperabillity
  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_11_1
  };
  D3D_FEATURE_LEVEL featureLevel;

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  hr = D3D11CreateDevice(
    m_adapter.Get(),
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    featureLevels,
    ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    &device,
    &featureLevel,
    &context);
  if (FAILED(hr))
    return hr;

  DEBUG_INFO("Feature Level: 0x%x", featureLevel);

  hr = device.As(&m_device);
  if (FAILED(hr))
    return hr;

  hr = context.As(&m_context);
  if (FAILED(hr))
    return hr;

  return S_OK;
}