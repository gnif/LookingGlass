#pragma once

#include <Windows.h>
#include <wdf.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <d3d11_4.h>

struct Direct3DDevice
{
  Direct3DDevice(LUID adapterLuid) :
    m_adapterLuid(adapterLuid) {};

  Direct3DDevice()
  {
    m_adapterLuid = LUID{};
  }

  HRESULT Init();

  LUID m_adapterLuid;
  Microsoft::WRL::ComPtr<IDXGIFactory5      > m_factory;
  Microsoft::WRL::ComPtr<IDXGIAdapter1      > m_adapter;
  Microsoft::WRL::ComPtr<ID3D11Device       > m_device;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
};