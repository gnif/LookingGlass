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

#include "Capture/DXGI.h"
using namespace Capture;

#include "common/debug.h"
#include "common/memcpySSE.h"

DXGI::DXGI() :
  m_options(NULL),
  m_initialized(false),
  m_dxgiFactory(),
  m_device(),
  m_deviceContext(),
  m_dup(),
  m_texture(),
  m_pointer(NULL)
{
}

DXGI::~DXGI()
{
}

bool DXGI::CanInitialize()
{
  HDESK desktop = OpenInputDesktop(0, TRUE, GENERIC_READ);
  if (!desktop)
    return false;

  CloseDesktop(desktop);
  return true;
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
    DEBUG_ERROR("Failed to create DXGIFactory: %08x", (int)status);
    return false;
  }

  bool done = false;
  IDXGIAdapter1Ptr adapter;
  for (int i = 0; m_dxgiFactory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++)
  {
    IDXGIOutputPtr output;
    for (int i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; i++)
    {
      DXGI_OUTPUT_DESC outputDesc;
      output->GetDesc(&outputDesc);
      if (!outputDesc.AttachedToDesktop)
      {
        SafeRelease(&output);
        continue;
      }

      m_output = output;
      if (!m_output)
      {
        SafeRelease(&output);
        SafeRelease(&adapter);
        DEBUG_ERROR("Failed to get IDXGIOutput1");
        DeInitialize();
        return false;
      }

      m_width  = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
      m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

      SafeRelease(&output);
      done = true;
      break;
    }

    if (done)
      break;

    SafeRelease(&adapter);
  }

  if (!done)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    DeInitialize();
    return false;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
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
    CREATE_FLAGS | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
    featureLevels, ARRAYSIZE(featureLevels),
    D3D11_SDK_VERSION,
    &m_device,
    &m_featureLevel,
    &m_deviceContext
  );
  SafeRelease(&adapter);
  #undef CREATE_FLAGS  

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    DeInitialize();
    return false;
  }

  bool h264 = false;
  for(CaptureOptions::const_iterator it = m_options->cbegin(); it != m_options->cend(); ++it)
  {
    if (_stricmp(*it, "h264") == 0) h264 = true;
  }

  if (h264)
  {
    DEBUG_WARN("Enabling experimental H.264 compression");
    m_frameType = FRAME_TYPE_H264;
    if (!InitH264Capture())
    {
      DeInitialize();
      return false;
    }
  }
  else
  {
    m_frameType = FRAME_TYPE_ARGB;
    if (!InitRawCapture())
    {
      DeInitialize();
      return false;
    }
  }

  IDXGIDevicePtr dxgi;
  status = m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to obtain the IDXGIDevice interface from the D3D11 device", status);
    DeInitialize();
    return false;
  }

  dxgi->SetGPUThreadPriority(7);

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
    DEBUG_WINERROR("DuplicateOutput Failed", status);
    DeInitialize();
    return false;
  }

  m_initialized = true;
  return true;
}

bool DXGI::InitRawCapture()
{
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

  HRESULT status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    return false;
  }

  return true;
}

bool DXGI::InitH264Capture()
{
  m_h264 = new MFT::H264();
  if (!m_h264->Initialize(m_device, m_width, m_height))
  {
    delete m_h264;
    m_h264 = NULL;
    return false;
  }
  
  return true;
}

void DXGI::DeInitialize()
{
  ReleaseFrame();

  if (m_pointer)
  {
    delete[] m_pointer;
    m_pointer = NULL;
    m_pointerBufSize = 0;
  }

  if (m_surfaceMapped)
  {
    m_deviceContext->Unmap(m_texture, 0);
    m_surfaceMapped = false;
  }

  SafeRelease(&m_texture);
  SafeRelease(&m_dup);
  SafeRelease(&m_output);
  SafeRelease(&m_deviceContext);
  SafeRelease(&m_device);
  SafeRelease(&m_dxgiFactory);

  m_initialized = false;
}

FrameType DXGI::GetFrameType()
{
  if (!m_initialized)
    return FRAME_TYPE_INVALID;
  return m_frameType;
}

size_t DXGI::GetMaxFrameSize()
{
  if (!m_initialized)
    return 0;

  return (m_width * m_height * 4);
}

GrabStatus Capture::DXGI::GrabFrameTexture(struct FrameInfo & frame, struct CursorInfo & cursor, ID3D11Texture2DPtr & texture, bool & timeout)
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

  timeout = false;
  DXGI_OUTDUPL_FRAME_INFO frameInfo;
  IDXGIResourcePtr res;

  HRESULT status;
  for (int i = 0; i < 2; ++i)
  {
    while (true)
    {
      status = m_dup->AcquireNextFrame(1000, &frameInfo, &res);
      if (status == DXGI_ERROR_WAIT_TIMEOUT)
      {
        timeout = true;
        return GRAB_STATUS_OK;
      }

      if (FAILED(status))
        break;

      m_releaseFrame = true;

      // if we have a mouse update
      if (frameInfo.LastMouseUpdateTime.QuadPart)
      {
        if (
          m_lastMousePos.x != frameInfo.PointerPosition.Position.x ||
          m_lastMousePos.y != frameInfo.PointerPosition.Position.y
        ) {
          cursor.updated = true;
          cursor.hasPos  = true;
          cursor.x = frameInfo.PointerPosition.Position.x;
          cursor.y = frameInfo.PointerPosition.Position.y;
          m_lastMousePos.x = frameInfo.PointerPosition.Position.x;
          m_lastMousePos.y = frameInfo.PointerPosition.Position.y;
        }

        if (m_lastMouseVis != frameInfo.PointerPosition.Visible)
        {
          cursor.updated = true;
          m_lastMouseVis = frameInfo.PointerPosition.Visible;
        }

        cursor.visible = m_lastMouseVis == TRUE;
      }

      // if the pointer shape has changed
      if (frameInfo.PointerShapeBufferSize > 0)
      {
        cursor.updated = true;
        if (m_pointerBufSize < frameInfo.PointerShapeBufferSize)
        {
          if (m_pointer)
            delete[] m_pointer;
          m_pointer = new BYTE[frameInfo.PointerShapeBufferSize];
          m_pointerBufSize = frameInfo.PointerShapeBufferSize;
        }

        DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
        status = m_dup->GetFramePointerShape(m_pointerBufSize, m_pointer, &m_pointerSize, &shapeInfo);
        if (FAILED(status))
        {
          DEBUG_WINERROR("Failed to get the new pointer shape", status);
          return GRAB_STATUS_ERROR;
        }

        switch (shapeInfo.Type)
        {
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR       : cursor.type = CURSOR_TYPE_COLOR;        break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR: cursor.type = CURSOR_TYPE_MASKED_COLOR; break;
        case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME  : cursor.type = CURSOR_TYPE_MONOCHROME;   break;
        default:
          DEBUG_ERROR("Invalid cursor type");
          return GRAB_STATUS_ERROR;
        }

        cursor.hasShape = true;
        cursor.shape    = m_pointer;
        cursor.w        = shapeInfo.Width;
        cursor.h        = shapeInfo.Height;
        cursor.pitch    = shapeInfo.Pitch;
        cursor.dataSize = m_pointerSize;
      }

      // if we also have frame data, break out to process it
      if (frameInfo.LastPresentTime.QuadPart != 0)
        break;

      // no frame data, clean up
      SafeRelease(&res);
      ReleaseFrame();

      // if the cursor has been updated
      if (cursor.updated)
        return GRAB_STATUS_CURSOR;

      // otherwise just try again
    }

    if (SUCCEEDED(status))
      break;

    switch (status)
    {
      // desktop switch, mode change, switch DWM on or off or Secure Desktop
    case DXGI_ERROR_ACCESS_LOST:
    case WAIT_ABANDONED:
      return GRAB_STATUS_REINIT;

    default:
      // unknown failure
      DEBUG_WINERROR("AcquireNextFrame failed", status);
      return GRAB_STATUS_ERROR;
    }
  }

  // retry count exceeded
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to acquire next frame", status);
    return GRAB_STATUS_ERROR;
  }
  
  res.QueryInterface(IID_PPV_ARGS(&texture));
  SafeRelease(&res);

  if (!texture)
  {
    ReleaseFrame();
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return GRAB_STATUS_ERROR;
  }

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::ReleaseFrame()
{
  if (!m_releaseFrame)
    return GRAB_STATUS_OK;

  m_releaseFrame = false;
  switch (m_dup->ReleaseFrame())
  {
  case S_OK:
    break;

  case DXGI_ERROR_INVALID_CALL:
    DEBUG_ERROR("Frame was already released");
    return GRAB_STATUS_ERROR;

  case WAIT_ABANDONED:
  case DXGI_ERROR_ACCESS_LOST:
    return GRAB_STATUS_REINIT;
  }

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::GrabFrameRaw(FrameInfo & frame, struct CursorInfo & cursor)
{
  GrabStatus result;
  ID3D11Texture2DPtr src;
  bool timeout;

  result = GrabFrameTexture(frame, cursor, src, timeout);
  if (timeout)
    return GRAB_STATUS_TIMEOUT;

  if (result != GRAB_STATUS_OK)
    return result;

  if (m_surfaceMapped)
  {
    m_deviceContext->Unmap(m_texture, 0);
    m_surfaceMapped = false;
  }

  m_deviceContext->CopyResource(m_texture, src);
  SafeRelease(&src);

  result = ReleaseFrame();
  if (result != GRAB_STATUS_OK)
    return result;

  HRESULT status;
  status = m_deviceContext->Map(m_texture, 0, D3D11_MAP_READ, 0, &m_mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    DeInitialize();
    return GRAB_STATUS_ERROR;
  }
  m_surfaceMapped = true;

  frame.pitch  = m_mapping.RowPitch;
  frame.stride = m_mapping.RowPitch >> 2;

  const unsigned int size = m_height * m_mapping.RowPitch;
  memcpySSE(frame.buffer, m_mapping.pData, LG_MIN(size, frame.bufferSize));

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::GrabFrameH264(struct FrameInfo & frame, struct CursorInfo & cursor)
{
  while(true)
  {
    unsigned int events = m_h264->Process();
    if (events & MFT::H264_EVENT_ERROR)
      return GRAB_STATUS_ERROR;

    if (events & MFT::H264_EVENT_NEEDS_DATA)
    {
      GrabStatus result;
      ID3D11Texture2DPtr texture;
      bool timeout;

      result = GrabFrameTexture(frame, cursor, texture, timeout);
      if (timeout)
      {
        // FIXME: this is wrong, we need to encode the last frame again
        return GRAB_STATUS_TIMEOUT;
      }

      if (result != GRAB_STATUS_OK)
      {
        ReleaseFrame();
        return result;
      }

      if (!m_h264->ProvideFrame(texture))
        return GRAB_STATUS_ERROR;

      SafeRelease(&texture);
      ReleaseFrame();
    }

    if (events & MFT::H264_EVENT_HAS_DATA)
    {
      if (!m_h264->GetFrame(frame.buffer, frame.bufferSize, frame.pitch))
        return GRAB_STATUS_ERROR;

      return GRAB_STATUS_OK;
    }
  }
}

GrabStatus DXGI::GrabFrame(struct FrameInfo & frame, struct CursorInfo & cursor)
{
  frame.width  = m_width;
  frame.height = m_height;

  if (m_frameType == FRAME_TYPE_H264)
    return GrabFrameH264(frame, cursor);
  else
    return GrabFrameRaw(frame, cursor);
}