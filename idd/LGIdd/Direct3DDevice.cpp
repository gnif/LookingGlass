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