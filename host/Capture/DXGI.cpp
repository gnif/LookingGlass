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
  m_texture(NULL),
  m_pointer(NULL)
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

  // we try this twice just incase we still get an error
  // on re-initialization
  for(int i = 0; i < 2; ++i)
  {
    status = m_output->DuplicateOutput(m_device, &m_dup);
    if (SUCCEEDED(status))
      break;
    Sleep(200);
  }

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

  if (m_pointer)
  {
    delete[] m_pointer;
    m_pointer = NULL;
    m_pointerBufSize = 0;
  }

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

bool DXGI::ReInitialize()
{
  DeInitialize();

  /*
    DXGI needs some time when mode switches occur, failing to do so causes
    failure to start and exceptions internal to DXGI
  */
  Sleep(200);

  return Initialize();
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
  if (!m_initialized)
    return false;

  DXGI_OUTDUPL_FRAME_INFO frameInfo;
  CComPtr<IDXGIResource> res;

  HRESULT status;
  for(int i = 0; i < 2; ++i)
  {
    status = m_dup->AcquireNextFrame(INFINITE, &frameInfo, &res);
    if (SUCCEEDED(status))
      break;
    
    switch (status)
    {      
      case DXGI_ERROR_ACCESS_LOST: // desktop switch, mode change or switch DWM on or off
      case WAIT_ABANDONED:         // this can happen also during desktop switches, not documented by MS though
      {
        // see if we can open the desktop, if not the secure desktop
        // is active so just wait for it instead of aborting out
        HDESK desktop = NULL;
        while(!desktop)
        {
          desktop = OpenInputDesktop(0, TRUE, GENERIC_READ);
          if (desktop)
            break;
          Sleep(100);
        }
        CloseDesktop(desktop);

        if (!ReInitialize())
        {
          DEBUG_ERROR("Failed to ReInitialize after lost access to desktop");
          return false;
        }
        break;
      }

      default:
        // unknown failure
        DEBUG_INFO("AcquireNextFrame failed: %08x", status);
        return false;
    }
  }

  // retry count exceeded
  if (FAILED(status))
  {
    m_dup->ReleaseFrame();
    DEBUG_ERROR("Failed to acquire next frame");
    return false;
  }

  CComQIPtr<ID3D11Texture2D> src = res;
  if (!src)
  {
    m_dup->ReleaseFrame();
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return false;
  }

  D3D11_TEXTURE2D_DESC desc;
  src->GetDesc(&desc);

  m_deviceContext->CopyResource(m_texture, src);

  // if the pointer shape has changed
  if (frameInfo.PointerShapeBufferSize > 0)
  {
    if (m_pointerBufSize < frameInfo.PointerShapeBufferSize)
    {
      if (m_pointer)
        delete[] m_pointer;
      m_pointer = new BYTE[frameInfo.PointerShapeBufferSize];
      m_pointerBufSize = frameInfo.PointerShapeBufferSize;
    }

    status = m_dup->GetFramePointerShape(m_pointerBufSize, m_pointer, &m_pointerSize, &m_shapeInfo);
    if (!SUCCEEDED(status))
    {
      m_dup->ReleaseFrame();
      DEBUG_ERROR("Failed to get the new pointer shape: %08x", status);
      return false;
    }
  }

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

  // if we have a mouse update
  if (frameInfo.LastMouseUpdateTime.QuadPart)
  {
    m_pointerVisible = frameInfo.PointerPosition.Visible;
    m_pointerPos     = frameInfo.PointerPosition.Position;
  }

  // if the pointer is to be drawn
  if (m_pointerVisible)
  {
    const int maxHeight = min(m_shapeInfo.Height, desc.Height - m_pointerPos.y);
    const int maxWidth  = min(m_shapeInfo.Width , desc.Width  - m_pointerPos.x);

    switch (m_shapeInfo.Type)
    {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      {
        for(int y = abs(min(0, m_pointerPos.y)); y < maxHeight; ++y)
          for (int x = abs(min(0, m_pointerPos.x)); x < maxWidth; ++x)
          {
            BYTE *src = (BYTE *)m_pointer + (m_shapeInfo.Pitch * y) + (x * 4);
            BYTE *dst = (BYTE *)frame.buffer + (rect.Pitch * (y + m_pointerPos.y)) + ((x + m_pointerPos.x) * 4);

            const unsigned int alpha = src[3] + 1;
            const unsigned int inv = 256 - alpha;
            dst[0] = (BYTE)((alpha * src[0] + inv * dst[0]) >> 8);
            dst[1] = (BYTE)((alpha * src[1] + inv * dst[1]) >> 8);
            dst[2] = (BYTE)((alpha * src[2] + inv * dst[2]) >> 8);
          }
        break;
      }

      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      {
        for (int y = abs(min(0, m_pointerPos.y)); y < maxHeight; ++y)
          for (int x = abs(min(0, m_pointerPos.x)); x < maxWidth; ++x)
          {
            UINT32 *src = (UINT32 *)m_pointer + ((m_shapeInfo.Pitch/4) * y) + x;
            UINT32 *dst = (UINT32 *)frame.buffer + (frame.stride * (y + m_pointerPos.y)) + (x + m_pointerPos.x);
            if (*src & 0xff000000)
                 *dst = 0xff000000 | (*dst ^ *src);
            else *dst = 0xff000000 | *src;
          }
        break;
      }

      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      {
        for (int y = abs(min(0, m_pointerPos.y)); y < maxHeight / 2; ++y)
          for (int x = abs(min(0, m_pointerPos.x)); x < maxWidth; ++x)
          {
            UINT8  *srcAnd = (UINT8  *)m_pointer + (m_shapeInfo.Pitch * y) + (x/8);
            UINT8  *srcXor = srcAnd + m_shapeInfo.Pitch * (m_shapeInfo.Height / 2);
            UINT32 *dst    = (UINT32 *)frame.buffer + (frame.stride * (y + m_pointerPos.y)) + (x + m_pointerPos.x);
            const BYTE mask = 0x80 >> (x % 8);
            const UINT32 andMask = (*srcAnd & mask) ? 0xFFFFFFFF : 0xFF000000;
            const UINT32 xorMask = (*srcXor & mask) ? 0x00FFFFFF : 0x00000000;
            *dst = (*dst & andMask) ^ xorMask;
          }
        break;
      }
    }
  }

  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to unmap surface: %08x", status);
    return false;
  }

  return true;
}