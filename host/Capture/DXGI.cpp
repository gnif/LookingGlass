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
#include "TraceUtil.h"

#include <mfapi.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <mferror.h>
#include <evr.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#if __MINGW32__

EXTERN_GUID(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);
EXTERN_GUID(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 0xf81da2c, 0xb537, 0x4672, 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x7, 0xa3);
EXTERN_GUID(MF_SA_D3D11_AWARE, 0x206b4fc8, 0xfcf9, 0x4c51, 0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0);

#define METransformUnknown 600
#define METransformNeedInput 601
#define METransformHaveOutput 602
#define METransformDrainComplete 603
#define METransformMarker 604
#endif

template <class T> void SafeRelease(T **ppT)
{
  if (*ppT)
  {
    (*ppT)->Release();
    *ppT = NULL;
  }
}

DXGI::DXGI() :
  m_cRef(1),
  m_options(NULL),
  m_initialized(false),
  m_dxgiFactory(),
  m_device(),
  m_deviceContext(),
  m_dup(),
  m_texture(),
  m_pointer(NULL)
{
  MFStartup(MF_VERSION);
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
  HRESULT status;

  MFT_REGISTER_TYPE_INFO typeInfo;
  IMFActivate  **activationPointers;
  UINT32         activationPointerCount;

  ID3D10MultithreadPtr mt(m_device);
  mt->SetMultithreadProtected(TRUE);
  SafeRelease(&mt);

  m_encodeEvent   = CreateEvent(NULL, TRUE , FALSE, NULL);
  m_shutdownEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  InitializeCriticalSection(&m_encodeCS);

  typeInfo.guidMajorType = MFMediaType_Video;
  typeInfo.guidSubtype   = MFVideoFormat_H264;

  status = MFTEnumEx(
    MFT_CATEGORY_VIDEO_ENCODER,
    MFT_ENUM_FLAG_HARDWARE,
    NULL,
    &typeInfo,
    &activationPointers,
    &activationPointerCount
  );
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to enumerate encoder MFTs", status);
    return false;
  }

  if (activationPointerCount == 0)
  {
    DEBUG_WINERROR("Hardware H264 MFT not available", status);
    return false;
  }

  {
    UINT32 nameLen = 0;
    activationPointers[0]->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &nameLen);
    wchar_t * name = new wchar_t[nameLen+1];
    activationPointers[0]->GetString(MFT_FRIENDLY_NAME_Attribute, name, nameLen + 1, NULL);
    DEBUG_INFO("Using Encoder: %S", name);
    delete[] name;
  }

  m_mfActivation = activationPointers[0];
  CoTaskMemFree(activationPointers);

  status = m_mfActivation->ActivateObject(IID_PPV_ARGS(&m_mfTransform));
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create H264 encoder MFT", status);
    return false;
  }

  IMFAttributesPtr attribs;
  m_mfTransform->GetAttributes(&attribs);
  attribs->SetUINT32 (MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS          , TRUE);
  attribs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING         , TRUE);
  attribs->SetUINT32 (MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
  attribs->SetUINT32 (MF_LOW_LATENCY                                   , TRUE);

  UINT32 d3d11Aware = 0;
  UINT32 async = 0;
  attribs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
  attribs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
  if (async)
    attribs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  SafeRelease(&attribs);

  status = m_mfTransform.QueryInterface(IID_PPV_ARGS(&m_mediaEventGen));
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to obtain th emedia event generator interface", status);
    return false;
  }

  status = m_mediaEventGen->BeginGetEvent(this, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the begin get event", status);
    return false;
  }

  if (d3d11Aware)
  {    
    MFCreateDXGIDeviceManager(&m_resetToken, &m_mfDeviceManager);
    status = m_mfDeviceManager->ResetDevice(m_device, m_resetToken);
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to call reset device", status);
      return false;
    }

    status = m_mfTransform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(m_mfDeviceManager.GetInterfacePtr()));
    if (FAILED(status))
    {
      DEBUG_WINERROR("Failed to set the D3D manager", status);
      return false;
    }
  }

  IMFMediaTypePtr outType;
  MFCreateMediaType(&outType);

  outType->SetGUID  (MF_MT_MAJOR_TYPE             , MFMediaType_Video);
  outType->SetGUID  (MF_MT_SUBTYPE                , MFVideoFormat_H264);
  outType->SetUINT32(MF_MT_AVG_BITRATE            , 384*1000);
  outType->SetUINT32(MF_MT_INTERLACE_MODE         , MFVideoInterlace_Progressive);
  outType->SetUINT32(MF_MT_MPEG2_PROFILE          , eAVEncH264VProfile_High);
  outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

  MFSetAttributeSize (outType, MF_MT_FRAME_SIZE        , m_width, m_height);
  MFSetAttributeRatio(outType, MF_MT_FRAME_RATE        , 30, 1);
  MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  status = m_mfTransform->SetOutputType(0, outType, 0);
  SafeRelease(&outType);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the output media type on the H264 encoder MFT", status);
    return false;
  }

  IMFMediaTypePtr inType;
  MFCreateMediaType(&inType);

  inType->SetGUID  (MF_MT_MAJOR_TYPE             , MFMediaType_Video );
  inType->SetGUID  (MF_MT_SUBTYPE                , MFVideoFormat_NV12);
  inType->SetUINT32(MF_MT_INTERLACE_MODE         , MFVideoInterlace_Progressive);
  inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

  MFSetAttributeSize (inType, MF_MT_FRAME_SIZE        , m_width, m_height);
  MFSetAttributeRatio(inType, MF_MT_FRAME_RATE        , 30, 1);
  MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1 , 1);

  status = m_mfTransform->SetInputType(0, inType, 0);
  SafeRelease(&inType);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the input media type on the H264 encoder MFT", status);
    return false;
  }

  m_mfTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH         , 0);
  m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

#if 0  
  status = MFTRegisterLocalByCLSID(
    __uuidof(CColorConvertDMO),
    MFT_CATEGORY_VIDEO_PROCESSOR,
    L"",
    MFT_ENUM_FLAG_SYNCMFT,
    0,
    NULL,
    0,
    NULL
  );
  if (FAILED(status))
  {
    DEBUG_ERROR("Failed to register color converter DSP");
    return false;
  }
#endif

  return true;
}

void DXGI::DeInitialize()
{
  if (m_mediaEventGen)
  {
    m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_mfTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    while (WaitForSingleObject(m_shutdownEvent, INFINITE) != WAIT_OBJECT_0) {}
    m_mfTransform->DeleteInputStream(0);
  }

  if (m_releaseFrame)
  {
    m_releaseFrame = false;
    m_dup->ReleaseFrame();
  }

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

  SafeRelease(&m_mediaEventGen);
  SafeRelease(&m_mfTransform);
  SafeRelease(&m_mfDeviceManager);

  SafeRelease(&m_texture);
  SafeRelease(&m_dup);
  SafeRelease(&m_output);
  SafeRelease(&m_deviceContext);
  SafeRelease(&m_device);
  SafeRelease(&m_dxgiFactory);

  if (m_encodeEvent)
  {
    CloseHandle(m_encodeEvent  );
    CloseHandle(m_shutdownEvent);
    m_encodeEvent   = NULL;
    m_shutdownEvent = NULL;
    DeleteCriticalSection(&m_encodeCS);
  }

  if (m_mfActivation)
  {
    m_mfActivation->ShutdownObject();
    SafeRelease(&m_mfActivation);
  }

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

STDMETHODIMP Capture::DXGI::Invoke(IMFAsyncResult * pAsyncResult)
{
  HRESULT status, evtStatus;
  MediaEventType meType = MEUnknown;
  IMFMediaEvent *pEvent = NULL;

  status = m_mediaEventGen->EndGetEvent(pAsyncResult, &pEvent);
  if (FAILED(status))
  {
    DEBUG_WINERROR("EndGetEvent", status);
    return status;
  }

  status = pEvent->GetStatus(&evtStatus);
  if (FAILED(status))
  {
    SafeRelease(&pEvent);
    DEBUG_WINERROR("GetStatus", status);
    return status;
  }

  if (FAILED(evtStatus))
  {
    SafeRelease(&pEvent);
    DEBUG_WINERROR("evtStatus", evtStatus);
    return evtStatus;
  }

  status = pEvent->GetType(&meType);
  if (FAILED(status))
  {
    SafeRelease(&pEvent);
    DEBUG_WINERROR("GetType", status);
    return status;
  }
  SafeRelease(&pEvent);

  switch (meType)
  {
    case METransformNeedInput:
      EnterCriticalSection(&m_encodeCS);
      m_encodeNeedsData = true;
      SetEvent(m_encodeEvent);
      LeaveCriticalSection(&m_encodeCS);
      break;

    case METransformHaveOutput:
      EnterCriticalSection(&m_encodeCS);
      m_encodeHasData = true;
      SetEvent(m_encodeEvent);
      LeaveCriticalSection(&m_encodeCS);
      break;

    case METransformDrainComplete:
    {
      status = m_mfTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
      if (FAILED(status))
      {
        DEBUG_WINERROR("MFT_MESSAGE_COMMAND_FLUSH", status);
        return status;
      }

      SetEvent(m_shutdownEvent);
      return S_OK;
    }

    case MEError:
      DEBUG_INFO("err");
      break;

    default:
      DEBUG_INFO("unk");
      break;
  }

  status = m_mediaEventGen->BeginGetEvent(this, NULL);
  if (FAILED(status))
  {
    DEBUG_WINERROR("BeginGetEvent", status);
    return status;
  }

  return status;
}

void DXGI::WaitForDesktop()
{
  HDESK desktop;
  do
  {
    desktop = OpenInputDesktop(0, TRUE, GENERIC_READ);
    if (desktop)
      break;
    Sleep(100);
  }
  while (!desktop);
  CloseDesktop(desktop);
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
      if (m_releaseFrame)
      {
        m_releaseFrame = false;
        status = m_dup->ReleaseFrame();

        switch (status)
        {
        case S_OK:
          break;

        case DXGI_ERROR_INVALID_CALL:
          DEBUG_ERROR("Frame was already released");
          return GRAB_STATUS_ERROR;

        case DXGI_ERROR_ACCESS_LOST:
          WaitForDesktop();
          return GRAB_STATUS_REINIT;
        }
      }

      status = m_dup->AcquireNextFrame(1000, &frameInfo, &res);
      if (status == DXGI_ERROR_WAIT_TIMEOUT)
      {
        timeout = true;
        return GRAB_STATUS_OK;
      }

      if (!SUCCEEDED(status))
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
        if (!SUCCEEDED(status))
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

      // if we also have frame data
      if (frameInfo.LastPresentTime.QuadPart != 0)
        break;

      SafeRelease(&res);

      if (cursor.updated)
        return GRAB_STATUS_CURSOR;
    }

    if (SUCCEEDED(status))
      break;

    switch (status)
    {
      // desktop switch, mode change, switch DWM on or off or Secure Desktop
    case DXGI_ERROR_ACCESS_LOST:
    case WAIT_ABANDONED:
      WaitForDesktop();
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
    DEBUG_ERROR("Failed to get src ID3D11Texture2D");
    return GRAB_STATUS_ERROR;
  }

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::GrabFrameRaw(FrameInfo & frame, struct CursorInfo & cursor)
{
  GrabStatus result;
  ID3D11Texture2DPtr src;
  bool timeout;

  while(true)
  {
    TRACE_START("GrabFrame");
    result = GrabFrameTexture(frame, cursor, src, timeout);
    TRACE_END;
    if (result != GRAB_STATUS_OK)
      return result;

    if (timeout)
    {
      if (!m_surfaceMapped)
        continue;
      m_memcpy.Wake();

      // send the last frame again if we timeout to prevent the client stalling on restart
      frame.pitch  = m_mapping.RowPitch;
      frame.stride = m_mapping.RowPitch >> 2;

      unsigned int size = m_height * m_mapping.RowPitch;
      m_memcpy.Copy(frame.buffer, m_mapping.pData, LG_MIN(size, frame.bufferSize));
      return GRAB_STATUS_OK;
    }

    break;
  }

  m_deviceContext->CopyResource(m_texture, src);
  SafeRelease(&src);

  if (m_surfaceMapped)
  {
    m_deviceContext->Unmap(m_texture, 0);
    m_surfaceMapped = false;
  }

  HRESULT status;
  status = m_deviceContext->Map(m_texture, 0, D3D11_MAP_READ, 0, &m_mapping);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to map the texture", status);
    DeInitialize();
    return GRAB_STATUS_ERROR;
  }
  m_surfaceMapped = true;

  TRACE_START("DXGI Memory Copy");
  // wake up the copy threads
  m_memcpy.Wake();

  frame.pitch  = m_mapping.RowPitch;
  frame.stride = m_mapping.RowPitch >> 2;

  const unsigned int size = m_height * m_mapping.RowPitch;
  m_memcpy.Copy(frame.buffer, m_mapping.pData, LG_MIN(size, frame.bufferSize));
  TRACE_END;

  return GRAB_STATUS_OK;
}

GrabStatus Capture::DXGI::GrabFrameH264(struct FrameInfo & frame, struct CursorInfo & cursor)
{
  while(true)
  {
    // only reset the event if there isn't work pending
    EnterCriticalSection(&m_encodeCS);
    if (!m_encodeHasData && !m_encodeNeedsData)
      ResetEvent(m_encodeEvent);
    LeaveCriticalSection(&m_encodeCS);

    switch (WaitForSingleObject(m_encodeEvent, 1000))
    {
      case WAIT_FAILED:
        DEBUG_WINERROR("Wait for encode event failed", GetLastError());
        return GRAB_STATUS_ERROR;

      case WAIT_ABANDONED:
        DEBUG_ERROR("Wait abandoned");
        return GRAB_STATUS_ERROR;

      case WAIT_TIMEOUT:
        continue;

      case WAIT_OBJECT_0:
        break;
    }

    EnterCriticalSection(&m_encodeCS);

    HRESULT status;
    if (m_encodeNeedsData)
    {
      LeaveCriticalSection(&m_encodeCS);
      GrabStatus result;
      ID3D11Texture2DPtr src;
      bool timeout;

      while(true)
      {
        result = GrabFrameTexture(frame, cursor, src, timeout);
        if (result != GRAB_STATUS_OK)
        {
          return result;
        }

        //FIXME: we should send the last frame again
        if (!timeout)
          break;
      }

      // cursor data may be returned, only turn off the flag if we have a frame
      EnterCriticalSection(&m_encodeCS);
      m_encodeNeedsData = false;
      LeaveCriticalSection(&m_encodeCS);

      IMFMediaBufferPtr buffer;
      status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), src, 0, FALSE, &buffer);
      SafeRelease(&src);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to create DXGI surface buffer from texture", status);
        return GRAB_STATUS_ERROR;
      }

      IMF2DBufferPtr imfBuffer(buffer);
      DWORD length;
      imfBuffer->GetContiguousLength(&length);
      buffer->SetCurrentLength(length);
      SafeRelease(&imfBuffer);

      IMFSamplePtr sample;
      MFCreateSample(&sample);
      sample->AddBuffer(buffer);

      status = m_mfTransform->ProcessInput(0, sample, 0);
      if (FAILED(status))
      {
        DEBUG_WINERROR("Failed to process the input", status);
        return GRAB_STATUS_ERROR;
      }

      SafeRelease(&src   );
      SafeRelease(&sample);
      SafeRelease(&buffer);

      EnterCriticalSection(&m_encodeCS);
    }

    if (m_encodeHasData)
    {
      m_encodeHasData = false;
      LeaveCriticalSection(&m_encodeCS);

      // wake up the copy threads
      TRACE_START("copy");
      m_memcpy.Wake();

      MFT_OUTPUT_STREAM_INFO streamInfo;
      status = m_mfTransform->GetOutputStreamInfo(0, &streamInfo);
      if (FAILED(status))
      {
        DEBUG_WINERROR("GetOutputStreamInfo", status);
        return GRAB_STATUS_ERROR;
      }

      DWORD outStatus;
      MFT_OUTPUT_DATA_BUFFER outDataBuffer;
      outDataBuffer.dwStreamID = 0;
      outDataBuffer.dwStatus   = 0;
      outDataBuffer.pEvents    = NULL;
      outDataBuffer.pSample    = NULL;

      status = m_mfTransform->ProcessOutput(0, 1, &outDataBuffer, &outStatus);
      if (FAILED(status))
      {
        DEBUG_WINERROR("ProcessOutput", status);
        return GRAB_STATUS_ERROR;
      }

      IMFMediaBufferPtr buffer;
      MFCreateAlignedMemoryBuffer((DWORD)frame.bufferSize, MF_128_BYTE_ALIGNMENT, &buffer);
      outDataBuffer.pSample->CopyToBuffer(buffer);
      SafeRelease(&outDataBuffer.pEvents);
      SafeRelease(&outDataBuffer.pSample);

      BYTE *pixels;
      DWORD maxLen, curLen;
      buffer->Lock(&pixels, &maxLen, &curLen);      
      m_memcpy.Copy(frame.buffer, pixels, curLen);
      buffer->Unlock();
      SafeRelease(&buffer);

      frame.stride = 0;
      frame.pitch  = curLen;

      TRACE_END;
      return GRAB_STATUS_OK;
    }

    LeaveCriticalSection(&m_encodeCS);
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