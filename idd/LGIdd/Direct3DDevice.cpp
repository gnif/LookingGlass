/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
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

#include "Direct3DDevice.h"

HRESULT Direct3DDevice::Init()
{
  HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory));
  if (FAILED(hr))
    return hr;

  hr = m_factory->EnumAdapterByLuid(m_adapterLuid, IID_PPV_ARGS(&m_adapter));
  if (FAILED(hr))
    return hr;

  hr = D3D11CreateDevice(
    m_adapter.Get(),
    D3D_DRIVER_TYPE_UNKNOWN,
    nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
    nullptr,
    0,
    D3D11_SDK_VERSION,
    &m_device,
    nullptr,
    &m_context);
  if (FAILED(hr))
    return hr;

  return S_OK;
}