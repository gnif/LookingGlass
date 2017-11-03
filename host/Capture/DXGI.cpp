/*
KVMGFX Client - A KVM Client for VGA Passthrough
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "DXGI.h"
using namespace Capture;

#include "common\debug.h"

DXGI::DXGI() :
  m_initialized(false),
  m_dxgiFactory(NULL),
  m_device(NULL),
  m_deviceContext(NULL),
  m_dup(NULL),
  m_texture(NULL)
{
}

DXGI::~DXGI()
{

}

bool DXGI::Initialize()
{
  if (m_initialized)
    DeInitialize();

  HRESULT status;

  status = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)(&m_dxgiFactory));
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to create DXGIFactory: %08x", status);
    return false;
  }

  bool done = false;
  CComPtr<IDXGIAdapter1> adapter;
  for (int i = 0; m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
  {
    CComPtr<IDXGIOutput> output;
    for (int i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++)
    {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      if (!outputDesc.AttachedToDesktop)
      {
        output.Release();
        continue;
      }

      m_output = output;
      if (!m_output)
      {
        output.Release();
        adapter.Release();
        DEBUG_ERROR("Failed to get IDXGIOutput1");
        DeInitialize();
        return false;
      }

      m_width  = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
      m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

      output.Release();
      done = true;
      break;
    }

    if (done)
      break;

    adapter.Release();
  }

  if (!done)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    DeInitialize();
    return false;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1
  };

  status = D3D11CreateDevice(
    adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    D3D11_CREATE_DEVICE_DEBUG,
    featureLevels, ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    &m_device,
    &m_featureLevel,
    &m_deviceContext
  );
  adapter.Release();

  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to create D3D11 device");
    DeInitialize();
    return false;
  }

  CComQIPtr<IDXGIDevice1> device1 = m_device;
  if (!device1)
  {
    DEBUG_ERROR("Failed to get IDXGIDevice1");
    DeInitialize();
    return false;
  }

  status = m_output->DuplicateOutput(m_device, &m_dup);
  if (FAILED(status))
  {
    DEBUG_ERROR("DuplicateOutput Failed: %08x", status);
    DeInitialize();
    return false;
  }

  D3D11_TEXTURE2D_DESC texDesc;
  ZeroMemory(&texDesc, sizeof(texDesc));
  texDesc.Width              = m_width;
  texDesc.Height             = m_height;
  texDesc.MipLevels          = 1;
  texDesc.ArraySize          = 1;
  texDesc.SampleDesc.Count   = 1;
  texDesc.SampleDesc.Quality = 0;
  texDesc.Usage              = D3D11_USAGE_STAGING;
  texDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
  texDesc.BindFlags          = 0;
  texDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
  texDesc.MiscFlags          = 0;

  status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture);
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to create texture: %08x", status);
    DeInitialize();
    return false;
  }

  if (!m_memcpy.Initialize())
  {
    DEBUG_ERROR("Failed to initialize memcpy");
    return false;
  }
  
  m_initialized = true;
  return true;
}

void DXGI::DeInitialize()
{
  m_memcpy.DeInitialize();

  if (m_texture)
    m_texture.Release();

  if (m_dup)
    m_dup.Release();

  if (m_output)
    m_output.Release();

  if (m_deviceContext)
    m_deviceContext.Release();

  if (m_device)
    m_device.Release();

  if (m_dxgiFactory)
    m_dxgiFactory.Release();

  m_initialized = false;
}

FrameType DXGI::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;

  return FRAME_TYPE_ARGB;
}

FrameComp DXGI::GetFrameCompression()
{
  if (!m_initialized)
    return FRAME_COMP_NONE;

  return FRAME_COMP_NONE;
}

size_t DXGI::GetMaxFrameSize()
{
  if (!m_initialized)
    return 0;

  return m_width * m_height * 4;
}

bool DXGI::GrabFrame(FrameInfo & frame)
{
  DXGI_OUTDUPL_FRAME_INFO frameInfo;
  CComPtr<IDXGIResource> res;

  HRESULT status;
  for(int i = 0; i < 2; ++i)
  {
    status = m_dup->AcquireNextFrame(INFINITE, &frameInfo, &res);
    if (SUCCEEDED(status))
      break;

    // desktop switch, mode change or switch DWM on or off
    if (status == DXGI_ERROR_ACCESS_LOST)
    {
      DeInitialize();
      if (!Initialize())
      {
        DEBUG_ERROR("Failed to re-initialize after access was lost");
        return false;
      }
      continue;
    }

    // unknown failure
    DEBUG_INFO("AcquireNextFrame failed: %08x", status);
    return false;
  }

  // retry count exceeded
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to acquire next frame");
    return false;
  }

  CComQIPtr<ID3D11Texture2D> src = res;
  if (!src)
  {
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return false;
  }

  D3D11_TEXTURE2D_DESC desc;
  src->GetDesc(&desc);

  m_deviceContext->CopyResource(m_texture, src);
  m_dup->ReleaseFrame();
  res.Release();
  src.Release();

  CComQIPtr<IDXGISurface1> surface = m_texture;
  if (!surface)
  {
    DEBUG_ERROR("Failed to get IDXGISurface1");
    return false;
  }

  DXGI_MAPPED_RECT rect;
  status = surface->Map(&rect, DXGI_MAP_READ);
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to map surface: %08x", status);
    return false;
  }

  m_width  = desc.Width;
  m_height = desc.Height;

  frame.width   = desc.Width;
  frame.height  = desc.Height;
  frame.stride  = rect.Pitch / 4;
  frame.outSize = min(frame.bufferSize, m_height * rect.Pitch);
  m_memcpy.Copy(frame.buffer, rect.pBits, frame.outSize);

  status = surface->Unmap();
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to unmap surface: %08x", status);
    return false;
  }

  return true;
}