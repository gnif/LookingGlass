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

static const char * DXGI_FORMAT_STR[] = {
  "DXGI_FORMAT_UNKNOWN",
  "DXGI_FORMAT_R32G32B32A32_TYPELESS",
  "DXGI_FORMAT_R32G32B32A32_FLOAT",
  "DXGI_FORMAT_R32G32B32A32_UINT",
  "DXGI_FORMAT_R32G32B32A32_SINT",
  "DXGI_FORMAT_R32G32B32_TYPELESS",
  "DXGI_FORMAT_R32G32B32_FLOAT",
  "DXGI_FORMAT_R32G32B32_UINT",
  "DXGI_FORMAT_R32G32B32_SINT",
  "DXGI_FORMAT_R16G16B16A16_TYPELESS",
  "DXGI_FORMAT_R16G16B16A16_FLOAT",
  "DXGI_FORMAT_R16G16B16A16_UNORM",
  "DXGI_FORMAT_R16G16B16A16_UINT",
  "DXGI_FORMAT_R16G16B16A16_SNORM",
  "DXGI_FORMAT_R16G16B16A16_SINT",
  "DXGI_FORMAT_R32G32_TYPELESS",
  "DXGI_FORMAT_R32G32_FLOAT",
  "DXGI_FORMAT_R32G32_UINT",
  "DXGI_FORMAT_R32G32_SINT",
  "DXGI_FORMAT_R32G8X24_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT_S8X24_UINT",
  "DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS",
  "DXGI_FORMAT_X32_TYPELESS_G8X24_UINT",
  "DXGI_FORMAT_R10G10B10A2_TYPELESS",
  "DXGI_FORMAT_R10G10B10A2_UNORM",
  "DXGI_FORMAT_R10G10B10A2_UINT",
  "DXGI_FORMAT_R11G11B10_FLOAT",
  "DXGI_FORMAT_R8G8B8A8_TYPELESS",
  "DXGI_FORMAT_R8G8B8A8_UNORM",
  "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
  "DXGI_FORMAT_R8G8B8A8_UINT",
  "DXGI_FORMAT_R8G8B8A8_SNORM",
  "DXGI_FORMAT_R8G8B8A8_SINT",
  "DXGI_FORMAT_R16G16_TYPELESS",
  "DXGI_FORMAT_R16G16_FLOAT",
  "DXGI_FORMAT_R16G16_UNORM",
  "DXGI_FORMAT_R16G16_UINT",
  "DXGI_FORMAT_R16G16_SNORM",
  "DXGI_FORMAT_R16G16_SINT",
  "DXGI_FORMAT_R32_TYPELESS",
  "DXGI_FORMAT_D32_FLOAT",
  "DXGI_FORMAT_R32_FLOAT",
  "DXGI_FORMAT_R32_UINT",
  "DXGI_FORMAT_R32_SINT",
  "DXGI_FORMAT_R24G8_TYPELESS",
  "DXGI_FORMAT_D24_UNORM_S8_UINT",
  "DXGI_FORMAT_R24_UNORM_X8_TYPELESS",
  "DXGI_FORMAT_X24_TYPELESS_G8_UINT",
  "DXGI_FORMAT_R8G8_TYPELESS",
  "DXGI_FORMAT_R8G8_UNORM",
  "DXGI_FORMAT_R8G8_UINT",
  "DXGI_FORMAT_R8G8_SNORM",
  "DXGI_FORMAT_R8G8_SINT",
  "DXGI_FORMAT_R16_TYPELESS",
  "DXGI_FORMAT_R16_FLOAT",
  "DXGI_FORMAT_D16_UNORM",
  "DXGI_FORMAT_R16_UNORM",
  "DXGI_FORMAT_R16_UINT",
  "DXGI_FORMAT_R16_SNORM",
  "DXGI_FORMAT_R16_SINT",
  "DXGI_FORMAT_R8_TYPELESS",
  "DXGI_FORMAT_R8_UNORM",
  "DXGI_FORMAT_R8_UINT",
  "DXGI_FORMAT_R8_SNORM",
  "DXGI_FORMAT_R8_SINT",
  "DXGI_FORMAT_A8_UNORM",
  "DXGI_FORMAT_R1_UNORM",
  "DXGI_FORMAT_R9G9B9E5_SHAREDEXP",
  "DXGI_FORMAT_R8G8_B8G8_UNORM",
  "DXGI_FORMAT_G8R8_G8B8_UNORM",
  "DXGI_FORMAT_BC1_TYPELESS",
  "DXGI_FORMAT_BC1_UNORM",
  "DXGI_FORMAT_BC1_UNORM_SRGB",
  "DXGI_FORMAT_BC2_TYPELESS",
  "DXGI_FORMAT_BC2_UNORM",
  "DXGI_FORMAT_BC2_UNORM_SRGB",
  "DXGI_FORMAT_BC3_TYPELESS",
  "DXGI_FORMAT_BC3_UNORM",
  "DXGI_FORMAT_BC3_UNORM_SRGB",
  "DXGI_FORMAT_BC4_TYPELESS",
  "DXGI_FORMAT_BC4_UNORM",
  "DXGI_FORMAT_BC4_SNORM",
  "DXGI_FORMAT_BC5_TYPELESS",
  "DXGI_FORMAT_BC5_UNORM",
  "DXGI_FORMAT_BC5_SNORM",
  "DXGI_FORMAT_B5G6R5_UNORM",
  "DXGI_FORMAT_B5G5R5A1_UNORM",
  "DXGI_FORMAT_B8G8R8A8_UNORM",
  "DXGI_FORMAT_B8G8R8X8_UNORM",
  "DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM",
  "DXGI_FORMAT_B8G8R8A8_TYPELESS",
  "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
  "DXGI_FORMAT_B8G8R8X8_TYPELESS",
  "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
  "DXGI_FORMAT_BC6H_TYPELESS",
  "DXGI_FORMAT_BC6H_UF16",
  "DXGI_FORMAT_BC6H_SF16",
  "DXGI_FORMAT_BC7_TYPELESS",
  "DXGI_FORMAT_BC7_UNORM",
  "DXGI_FORMAT_BC7_UNORM_SRGB",
  "DXGI_FORMAT_AYUV",
  "DXGI_FORMAT_Y410",
  "DXGI_FORMAT_Y416",
  "DXGI_FORMAT_NV12",
  "DXGI_FORMAT_P010",
  "DXGI_FORMAT_P016",
  "DXGI_FORMAT_420_OPAQUE",
  "DXGI_FORMAT_YUY2",
  "DXGI_FORMAT_Y210",
  "DXGI_FORMAT_Y216",
  "DXGI_FORMAT_NV11",
  "DXGI_FORMAT_AI44",
  "DXGI_FORMAT_IA44",
  "DXGI_FORMAT_P8",
  "DXGI_FORMAT_A8P8",
  "DXGI_FORMAT_B4G4R4A4_UNORM",

  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,

  "DXGI_FORMAT_P208",
  "DXGI_FORMAT_V208",
  "DXGI_FORMAT_V408"
};

const char * GetDXGIFormatStr(DXGI_FORMAT format)
{
  if (format > _countof(DXGI_FORMAT_STR))
    return DXGI_FORMAT_STR[0];
  return DXGI_FORMAT_STR[format];
}

DXGI::DXGI() :
  m_options(NULL),
  m_initialized(false),
  m_dxgiFactory(),
  m_device(),
  m_deviceContext(),
  m_dup()
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

  m_cursorRPos = 0;
  m_cursorWPos = 0;
  for (int i = 0; i < _countof(m_cursorRing); ++i)
  {
    CursorInfo & cursor = m_cursorRing[i];
    cursor.visible  = false;
    cursor.hasPos   = false;
    cursor.hasShape = false;
  }

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
        output = NULL;
        continue;
      }

      m_output = output;
      if (!m_output)
      {
        DEBUG_ERROR("Failed to get IDXGIOutput1");
        DeInitialize();
        return false;
      }

      DXGI_ADAPTER_DESC1 adapterDesc;
      adapter->GetDesc1(&adapterDesc);
      DEBUG_INFO("Device Descripion: %ls"    , adapterDesc.Description);
      DEBUG_INFO("Device Vendor ID : 0x%x"   , adapterDesc.VendorId);
      DEBUG_INFO("Device Device ID : 0x%x"   , adapterDesc.DeviceId);
      DEBUG_INFO("Device Video Mem : %lld MB", adapterDesc.DedicatedVideoMemory  / 1048576);
      DEBUG_INFO("Device Sys Mem   : %lld MB", adapterDesc.DedicatedSystemMemory / 1048576);
      DEBUG_INFO("Shared Sys Mem   : %lld MB", adapterDesc.SharedSystemMemory    / 1048576);

      m_width  = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
      m_height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
      DEBUG_INFO("Capture Size     : %u x %u", m_width, m_height);

      done = true;
      break;
    }

    if (done)
      break;

    adapter = NULL;
  }

  if (!done)
  {
    DEBUG_ERROR("Failed to locate a valid output device");
    DeInitialize();
    return false;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] = {
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1
  };

  #ifdef _DEBUG
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
  #undef CREATE_FLAGS  

  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create D3D11 device", status);
    DeInitialize();
    return false;
  }

  DEBUG_INFO("Feature Level    : 0x%x", m_featureLevel);

  IDXGIDevicePtr dxgi;
  status = m_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to obtain the IDXGIDevice interface from the D3D11 device", status);
    DeInitialize();
    return false;
  }

  dxgi->SetGPUThreadPriority(7);

  // we try this twice in case we still get an error on re-initialization
  for (int i = 0; i < 2; ++i)
  {
    const DXGI_FORMAT supportedFormats[] = {
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R10G10B10A2_UNORM
    };

    status = m_output->DuplicateOutput1(m_device, 0, _countof(supportedFormats), supportedFormats, &m_dup);
    if (SUCCEEDED(status))
      break;
    Sleep(200);
  }

  if (FAILED(status))
  {
    DEBUG_WINERROR("DuplicateOutput1 Failed", status);
    DeInitialize();
    return false;
  }

  DXGI_OUTDUPL_DESC dupDesc;
  m_dup->GetDesc(&dupDesc);
  DEBUG_INFO("Source Format    : %s", GetDXGIFormatStr(dupDesc.ModeDesc.Format));

  m_started     = false;
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
  texDesc.Format             = m_pixelFormat;
  texDesc.BindFlags          = 0;
  texDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
  texDesc.MiscFlags          = 0;

  HRESULT status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture[0]);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    return false;
  }

  return true;
}

bool DXGI::InitYUV420Capture()
{
  HRESULT status;
  D3D11_TEXTURE2D_DESC texDesc;

  ZeroMemory(&texDesc, sizeof(texDesc));
  texDesc.Width              = m_width;
  texDesc.Height             = m_height;
  texDesc.MipLevels          = 1;
  texDesc.ArraySize          = 1;
  texDesc.SampleDesc.Count   = 1;
  texDesc.SampleDesc.Quality = 0;
  texDesc.Usage              = D3D11_USAGE_STAGING;
  texDesc.Format             = DXGI_FORMAT_R8_UNORM;
  texDesc.BindFlags          = 0;
  texDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
  texDesc.MiscFlags          = 0;

  status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture[0]);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    return false;
  }

  texDesc.Width  /= 2;
  texDesc.Height /= 2;

  status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture[1]);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    return false;
  }

  status = m_device->CreateTexture2D(&texDesc, NULL, &m_texture[2]);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create texture", status);
    return false;
  }

  m_textureConverter = new TextureConverter();
  if (!m_textureConverter->Initialize(m_deviceContext, m_device, m_width, m_height, FRAME_TYPE_YUV420))
    return false;

  return true;
}

void DXGI::DeInitialize()
{
  if (m_h264)
  {
    delete m_h264;
    m_h264 = NULL;
  }

  if (m_textureConverter)
  {
    delete m_textureConverter;
    m_textureConverter = NULL;
  }

  ReleaseFrame();

  for(int i = 0; i < _countof(m_cursorRing); ++i)
  {
    if (m_cursorRing[i].shape.buffer)
      delete[] m_cursorRing[i].shape.buffer;
    m_cursorRing[i].shape.buffer     = NULL;
    m_cursorRing[i].shape.bufferSize = 0;
  }

  for(int i = 0; i < _countof(m_texture); ++i)
    m_texture[i] = NULL;

  m_dup           = NULL;
  m_output        = NULL;
  m_deviceContext = NULL;
  m_device        = NULL;
  m_dxgiFactory   = NULL;

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

unsigned int Capture::DXGI::Capture()
{
  if (!m_initialized)
    return GRAB_STATUS_ERROR;

  CursorInfo & cursor = m_cursorRing[m_cursorWPos];
  DXGI_OUTDUPL_FRAME_INFO frameInfo;
  IDXGIResourcePtr res;
  unsigned int ret;

  HRESULT status;
  for (int retryCount = 0; retryCount < 2; ++retryCount)
  {
    ret = ReleaseFrame();
    if (ret != GRAB_STATUS_OK)
      return ret;

    status = m_dup->AcquireNextFrame(1000, &frameInfo, &res);
    switch (status)
    {
      case S_OK:
        m_releaseFrame = true;
        break;

      case DXGI_ERROR_WAIT_TIMEOUT:
        return GRAB_STATUS_TIMEOUT;

      // desktop switch, mode change, switch DWM on or off or Secure Desktop
      case DXGI_ERROR_ACCESS_LOST:
      case WAIT_ABANDONED:
        return GRAB_STATUS_REINIT;

      default:
        // unknown failure
        DEBUG_WINERROR("AcquireNextFrame failed", status);
        return GRAB_STATUS_ERROR;
    }

    // if the pointer shape has changed
    if (frameInfo.PointerShapeBufferSize > 0)
    {
      // resize the buffer if required
      if (cursor.shape.bufferSize < frameInfo.PointerShapeBufferSize)
      {
        delete[] cursor.shape.buffer;
        cursor.shape.buffer     = new char[frameInfo.PointerShapeBufferSize];
        cursor.shape.bufferSize = frameInfo.PointerShapeBufferSize;
      }

      cursor.shape.pointerSize = 0;
      ret                     |= GRAB_STATUS_CURSOR;

      DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
      status = m_dup->GetFramePointerShape(cursor.shape.bufferSize, cursor.shape.buffer, &cursor.shape.pointerSize, &shapeInfo);
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
      cursor.w        = shapeInfo.Width;
      cursor.h        = shapeInfo.Height;
      cursor.pitch    = shapeInfo.Pitch;
      m_hotSpot.x     = shapeInfo.HotSpot.x;
      m_hotSpot.y     = shapeInfo.HotSpot.y;
    }

    // if we have a mouse update
    if (frameInfo.LastMouseUpdateTime.QuadPart)
    {
      if (
        m_lastCursorX != frameInfo.PointerPosition.Position.x ||
        m_lastCursorY != frameInfo.PointerPosition.Position.y
      ) {
        ret |= GRAB_STATUS_CURSOR;
        cursor.hasPos = true;
        cursor.x      = m_lastCursorX = frameInfo.PointerPosition.Position.x;
        cursor.y      = m_lastCursorY = frameInfo.PointerPosition.Position.y;
      }
    }
    else
    {
      // always report the mouse position to prevent the guest losing sync (ie: dragging windows)
      POINT curPos;
      if (GetCursorPos(&curPos))
      {
        curPos.x -= m_hotSpot.x;
        curPos.y -= m_hotSpot.y;

        if (curPos.x != m_lastCursorX || curPos.y != m_lastCursorY)
        {
          ret |= GRAB_STATUS_CURSOR;
          cursor.hasPos  = true;
          cursor.x       = m_lastCursorX = curPos.x;
          cursor.y       = m_lastCursorY = curPos.y;
        }
      }
    }

    if (m_lastMouseVis != frameInfo.PointerPosition.Visible)
      m_lastMouseVis = frameInfo.PointerPosition.Visible;
    cursor.visible = m_lastMouseVis == TRUE;

    if (ret & GRAB_STATUS_CURSOR && m_cursorWPos == m_cursorRPos)
    {
      // atomic advance so we don't have to worry about locking
      m_cursorWPos = (m_cursorWPos + 1 == DXGI_CURSOR_RING_SIZE) ? 0 : m_cursorWPos + 1;
    }

    // if we don't have frame data
    if (frameInfo.LastPresentTime.QuadPart == 0)
    {
      // if there is nothing to update, just start again
      if (!ret)
      {
        --retryCount;
        continue;
      }

      res = NULL;
      ret |= GRAB_STATUS_OK;
      return ret;
    }

    // success, break out of the retry loop
    break;
  }

  ret |= GRAB_STATUS_FRAME;

  // ensure we have a frame
  if (!m_releaseFrame)
  {
    DEBUG_WINERROR("Failed to acquire next frame", status);
    return GRAB_STATUS_ERROR;
  }

  // get the texture
  res.QueryInterface(IID_PPV_ARGS(&m_ftexture));
  res = NULL;
  if (!m_ftexture)
  {
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return GRAB_STATUS_ERROR;
  }

  if (!m_started)
  {
    m_started = true;

    // determine the native pixel format
    D3D11_TEXTURE2D_DESC dupDesc;
    ZeroMemory(&dupDesc, sizeof(dupDesc));
    m_ftexture->GetDesc(&dupDesc);
    m_pixelFormat = dupDesc.Format;

    switch(m_pixelFormat)
    {
      case DXGI_FORMAT_R8G8B8A8_UNORM:
        m_frameType = FRAME_TYPE_RGBA;
        break;

      case DXGI_FORMAT_B8G8R8A8_UNORM:
        m_frameType = FRAME_TYPE_BGRA;
        break;

      case DXGI_FORMAT_R10G10B10A2_UNORM:
        m_frameType = FRAME_TYPE_RGBA10;
        break;

      default:
        DEBUG_WARN("Unsupported pixel format %s, enabling conversions", GetDXGIFormatStr(m_pixelFormat));
        return GRAB_STATUS_ERROR;
    }

    DEBUG_INFO("Pixel Format     : %s", GetDXGIFormatStr(m_pixelFormat));

    for(CaptureOptions::const_iterator it = m_options->cbegin(); it != m_options->cend(); ++it)
    {
      if (_stricmp(*it, "yuv420") == 0) m_frameType = FRAME_TYPE_YUV420;
    }

    bool ok = false;
    switch (m_frameType)
    {
      case FRAME_TYPE_BGRA  :
      case FRAME_TYPE_RGBA  :
      case FRAME_TYPE_RGBA10: ok = InitRawCapture   (); break;
      case FRAME_TYPE_YUV420: ok = InitYUV420Capture(); break;
    }

    if (!ok)
      return GRAB_STATUS_ERROR;
  }

  // initiate the texture copy as early as possible
  if (m_frameType == FRAME_TYPE_YUV420)
  {
    TextureList planes;
    if (!m_textureConverter->Convert(m_ftexture, planes))
      return GRAB_STATUS_ERROR;

    for (int i = 0; i < 3; ++i)
    {
      ID3D11Texture2DPtr t = planes.at(i);
      m_deviceContext->CopyResource(m_texture[i], t);
    }
  }
  else
    m_deviceContext->CopyResource(m_texture[0], m_ftexture);

  ret |= GRAB_STATUS_OK;
  return ret;
}

GrabStatus Capture::DXGI::ReleaseFrame()
{
  if (!m_releaseFrame)
    return GRAB_STATUS_OK;

  m_releaseFrame = false;
  m_ftexture     = NULL;
  
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

GrabStatus Capture::DXGI::DiscardFrame()
{
  return ReleaseFrame();
}

GrabStatus Capture::DXGI::GrabFrameRaw(FrameInfo & frame)
{
  GrabStatus               result;
  D3D11_MAPPED_SUBRESOURCE mapping;

  HRESULT status;
  status = m_deviceContext->Map(m_texture[0], 0, D3D11_MAP_READ, 0, &mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    DeInitialize();
    return GRAB_STATUS_ERROR;
  }
  
  frame.pitch  = m_width * 4;
  frame.stride = m_width;

  if (frame.pitch == mapping.RowPitch)
    memcpySSE(frame.buffer, mapping.pData, frame.pitch * m_height);
  else
    for(unsigned int y = 0; y < m_height; ++y)
      memcpySSE(
        (uint8_t *)frame.buffer  + (frame.pitch      * y),
        (uint8_t *)mapping.pData + (mapping.RowPitch * y),
        frame.pitch
      );

  m_deviceContext->Unmap(m_texture[0], 0);

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::GrabFrameYUV420(struct FrameInfo & frame)
{
  GrabStatus  result;

  uint8_t * data   = (uint8_t *)frame.buffer;
  size_t    remain = frame.bufferSize;
  for(int i = 0; i < 3; ++i)
  {
    HRESULT                  status;
    D3D11_MAPPED_SUBRESOURCE mapping;
    D3D11_TEXTURE2D_DESC     desc;

    m_texture[i]->GetDesc(&desc);
    status = m_deviceContext->Map(m_texture[i], 0, D3D11_MAP_READ, 0, &mapping);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to map the texture", status);
      DeInitialize();
      return GRAB_STATUS_ERROR;
    }

    const unsigned int size = desc.Height * desc.Width;
    if (size > remain)
    {
      m_deviceContext->Unmap(m_texture[i], 0);
      DEBUG_ERROR("Too much data to fit in buffer");
      return GRAB_STATUS_ERROR;
    }

    const uint8_t * src = (uint8_t *)mapping.pData;
    for(unsigned int y = 0; y < desc.Height; ++y)
    {
      memcpySSE(data, src, desc.Width);
      data += desc.Width;
      src  += mapping.RowPitch;
    }
    m_deviceContext->Unmap(m_texture[i], 0);
    remain -= size;
  }

  frame.pitch  = m_width;
  frame.stride = m_width;
  return GRAB_STATUS_OK;
}

GrabStatus DXGI::GetFrame(struct FrameInfo & frame)
{
  if (!m_ftexture)
  {
    DEBUG_ERROR("A frame has not been captured");
    return GRAB_STATUS_ERROR;
  }

  frame.width  = m_width;
  frame.height = m_height;

  if (m_frameType == FRAME_TYPE_YUV420)
    return GrabFrameYUV420(frame);

  return GrabFrameRaw(frame);
}

bool DXGI::GetCursor(CursorInfo & cursor)
{
  if (m_cursorRPos == m_cursorWPos)
    return false;

  cursor = m_cursorRing[m_cursorRPos];
  return true;
}

void DXGI::FreeCursor()
{
  assert(m_cursorRPos != m_cursorWPos);

  CursorInfo & cursor = m_cursorRing[m_cursorRPos];
  cursor.visible  = false;
  cursor.hasPos   = false;
  cursor.hasShape = false;

  m_cursorRPos = (m_cursorRPos + 1 == DXGI_CURSOR_RING_SIZE) ? 0 : m_cursorRPos + 1;
}