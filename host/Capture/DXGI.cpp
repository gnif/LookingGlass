/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

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

#include "Util.h"
#include "common\debug.h"
#include "common\memcpySSE.h"

DXGI::DXGI() :
  m_options(NULL),
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

bool DXGI::Initialize(CaptureOptions * options)
{
  if (m_initialized)
    DeInitialize();

  m_options = options;
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

  #if DEBUG
    #define CREATE_FLAGS (D3D11_CREATE_DEVICE_DEBUG)
  #else
    #define CREATE_FLAGS (0)
  #endif

  status = D3D11CreateDevice(
    adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    CREATE_FLAGS,
    featureLevels, ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    &m_device,
    &m_featureLevel,
    &m_deviceContext
  );

#undef CREATE_FLAGS

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
  
  m_initialized = true;
  return true;
}

void DXGI::DeInitialize()
{
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

FrameType DXGI::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;

  return FRAME_TYPE_ARGB;
}

size_t DXGI::GetMaxFrameSize()
{
  if (!m_initialized)
    return 0;

  return (m_width * m_height * 4);
}

GrabStatus DXGI::GrabFrame(FrameInfo & frame)
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

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
        return GRAB_STATUS_REINIT;

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
          return GRAB_STATUS_ERROR;
        }
        continue;
      }

      default:
        // unknown failure
        DEBUG_INFO("AcquireNextFrame failed: %08x", status);
        return GRAB_STATUS_ERROR;
    }
  }

  // retry count exceeded
  if (FAILED(status))
  {
    m_dup->ReleaseFrame();
    DEBUG_ERROR("Failed to acquire next frame");
    return GRAB_STATUS_ERROR;
  }

  CComQIPtr<ID3D11Texture2D> src = res;
  if (!src)
  {
    m_dup->ReleaseFrame();
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return GRAB_STATUS_ERROR;
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
      return GRAB_STATUS_ERROR;
    }
  }

  m_dup->ReleaseFrame();
  res.Release();
  src.Release();

  CComQIPtr<IDXGISurface1> surface = m_texture;
  if (!surface)
  {
    DEBUG_ERROR("Failed to get IDXGISurface1");
    return GRAB_STATUS_ERROR;
  }

  DXGI_MAPPED_RECT rect;
  status = surface->Map(&rect, DXGI_MAP_READ);
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to map surface: %08x", status);
    return GRAB_STATUS_ERROR;
  }

  m_width  = desc.Width;
  m_height = desc.Height;
  const int pitch = m_width * 4;

  frame.width   = desc.Width;
  frame.height  = desc.Height;
  frame.stride  = desc.Width;
  frame.outSize = min(frame.bufferSize, m_height * pitch);

  // if we have a mouse update
  if (frameInfo.LastMouseUpdateTime.QuadPart)
  {
    m_pointerVisible = frameInfo.PointerPosition.Visible;
    m_pointerPos     = frameInfo.PointerPosition.Position;

    frame.hasMousePos = true;
    frame.mouseX = m_pointerPos.x;
    frame.mouseY = m_pointerPos.y;
  }

  memcpySSE(frame.buffer, rect.pBits, frame.outSize);
  status = surface->Unmap();

  // if the pointer is to be drawn
  if (m_pointerVisible)
  {
    enum CursorType type;
    switch (m_shapeInfo.Type)
    {
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR       : type = CURSOR_TYPE_COLOR       ; break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: type = CURSOR_TYPE_MASKED_COLOR; break;
      case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME  : type = CURSOR_TYPE_MONOCHROME  ; break;
      default:
        DEBUG_ERROR("Invalid cursor type");
        return GRAB_STATUS_ERROR;
    }

    POINT cursorPos;
    POINT cursorRect;
    cursorPos.x  = m_pointerPos.x;
    cursorPos.y  = m_pointerPos.y;
    cursorRect.x = m_shapeInfo.Width;
    cursorRect.y = m_shapeInfo.Height;

    Util::DrawCursor(
      type,
      m_pointer,
      cursorRect,
      m_shapeInfo.Pitch,
      cursorPos,
      frame
    );
  }

  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to unmap surface: %08x", status);
    return GRAB_STATUS_ERROR;
  }

  return GRAB_STATUS_OK;
}