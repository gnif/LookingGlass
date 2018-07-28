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

#include "MFT/H264.h"

#include "common/debug.h"
#include "common/memcpySSE.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <mferror.h>
#include <evr.h>

using namespace MFT;

#if __MINGW32__
EXTERN_GUID(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, 0xa634a91c, 0x822b, 0x41b9, 0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0);
EXTERN_GUID(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, 0xf81da2c, 0xb537, 0x4672, 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3);
EXTERN_GUID(MF_SA_D3D11_AWARE, 0x206b4fc8, 0xfcf9, 0x4c51, 0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0);

#define METransformUnknown       600
#define METransformNeedInput     601
#define METransformHaveOutput    602
#define METransformDrainComplete 603
#define METransformMarker        604
#endif

MFT::H264::H264() :
  m_cRef(1)
{
  MFStartup(MF_VERSION);
}

MFT::H264::~H264()
{
  DeInitialize();
}

bool MFT::H264::Initialize(ID3D11DevicePtr device, unsigned int width, unsigned int height)
{
  DeInitialize();
  HRESULT status;

  MFT_REGISTER_TYPE_INFO typeInfo;
  IMFActivate  **activationPointers;
  UINT32         activationPointerCount;

  m_device        = device;
  m_width         = width;
  m_height        = height;

  m_encodeEvent   = CreateEvent(NULL, TRUE , FALSE, NULL);
  m_shutdownEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  InitializeCriticalSection(&m_encodeCS);

  ID3D10MultithreadPtr mt(m_device);
  mt->SetMultithreadProtected(TRUE);
  mt = NULL;

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
    wchar_t * name = new wchar_t[nameLen + 1];
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
  attribs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
  attribs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  attribs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
  attribs->SetUINT32(MF_LOW_LATENCY, TRUE);

  UINT32 d3d11Aware = 0;
  UINT32 async = 0;
  attribs->GetUINT32(MF_TRANSFORM_ASYNC, &async);
  attribs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware);
  if (async)
    attribs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  attribs = NULL;

  status = m_mfTransform.QueryInterface(IID_PPV_ARGS(&m_mediaEventGen));
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to obtain the media event generator interface", status);
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

  outType->SetGUID  (MF_MT_MAJOR_TYPE             , MFMediaType_Video           );
  outType->SetGUID  (MF_MT_SUBTYPE                , MFVideoFormat_H264          );
  outType->SetUINT32(MF_MT_AVG_BITRATE            , 384 * 1000                  );
  outType->SetUINT32(MF_MT_INTERLACE_MODE         , MFVideoInterlace_Progressive);
  outType->SetUINT32(MF_MT_MPEG2_PROFILE          , eAVEncH264VProfile_High     );
  outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

  MFSetAttributeSize (outType, MF_MT_FRAME_SIZE        , m_width, m_height);
  MFSetAttributeRatio(outType, MF_MT_FRAME_RATE        , 60     , 1       );
  MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1      , 1       );

  status  = m_mfTransform->SetOutputType(0, outType, 0);
  outType = NULL;
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the output media type on the H264 encoder MFT", status);
    return false;
  }

  IMFMediaTypePtr inType;
  MFCreateMediaType(&inType);

  inType->SetGUID  (MF_MT_MAJOR_TYPE             , MFMediaType_Video           );
  inType->SetGUID  (MF_MT_SUBTYPE                , MFVideoFormat_NV12          );
  inType->SetUINT32(MF_MT_INTERLACE_MODE         , MFVideoInterlace_Progressive);
  inType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE                        );

  MFSetAttributeSize (inType, MF_MT_FRAME_SIZE        , m_width, m_height);
  MFSetAttributeRatio(inType, MF_MT_FRAME_RATE        , 60     , 1       );
  MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1      , 1       );

  status = m_mfTransform->SetInputType(0, inType, 0);
  inType = NULL;
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to set the input media type on the H264 encoder MFT", status);
    return false;
  }

  m_mfTransform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

  return true;
}

void MFT::H264::DeInitialize()
{
  if (m_mediaEventGen)
  {
    m_mfTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    m_mfTransform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    while (WaitForSingleObject(m_shutdownEvent, INFINITE) != WAIT_OBJECT_0) {}
    m_mfTransform->DeleteInputStream(0);
  }

  m_mediaEventGen   = NULL;
  m_mfTransform     = NULL;
  m_mfDeviceManager = NULL;

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
    m_mfActivation = NULL;
  }
}

unsigned int MFT::H264::Process()
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
        return H264_EVENT_ERROR;

      case WAIT_ABANDONED:
        DEBUG_ERROR("Wait abandoned");
        return H264_EVENT_ERROR;

      case WAIT_TIMEOUT:
        continue;

      case WAIT_OBJECT_0:
        break;
    }

    unsigned int events = 0;
    EnterCriticalSection(&m_encodeCS);
    if (m_encodeNeedsData) events |= H264_EVENT_NEEDS_DATA;
    if (m_encodeHasData  ) events |= H264_EVENT_HAS_DATA;
    LeaveCriticalSection(&m_encodeCS);

    return events;
  }

  return H264_EVENT_ERROR;
}

bool MFT::H264::ProvideFrame(ID3D11Texture2DPtr texture)
{
  EnterCriticalSection(&m_encodeCS);
  if (!m_encodeNeedsData)
  {
    LeaveCriticalSection(&m_encodeCS);
    return false;
  }
  m_encodeNeedsData = false;
  LeaveCriticalSection(&m_encodeCS);

  HRESULT status;
  IMFMediaBufferPtr buffer;
  status = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to create DXGI surface buffer from texture", status);
    return false;
  }

  IMF2DBufferPtr imfBuffer(buffer);
  DWORD length;
  imfBuffer->GetContiguousLength(&length);
  buffer->SetCurrentLength(length);

  IMFSamplePtr sample;
  MFCreateSample(&sample);
  sample->AddBuffer(buffer);

  status = m_mfTransform->ProcessInput(0, sample, 0);
  if (FAILED(status))
  {
    DEBUG_WINERROR("Failed to process the input", status);
    return false;
  }

  return true;
}

bool MFT::H264::GetFrame(void * buffer, const size_t bufferSize, unsigned int & dataLen)
{
  EnterCriticalSection(&m_encodeCS);
  if (!m_encodeHasData)
  {
    LeaveCriticalSection(&m_encodeCS);
    return false;
  }

  m_encodeHasData = false;
  LeaveCriticalSection(&m_encodeCS);

  HRESULT status;
  MFT_OUTPUT_STREAM_INFO streamInfo;
  status = m_mfTransform->GetOutputStreamInfo(0, &streamInfo);
  if (FAILED(status))
  {
    DEBUG_WINERROR("GetOutputStreamInfo", status);
    return false;
  }

  DWORD outStatus;
  MFT_OUTPUT_DATA_BUFFER outDataBuffer = { 0 };
  status = m_mfTransform->ProcessOutput(0, 1, &outDataBuffer, &outStatus);
  if (FAILED(status))
  {
    DEBUG_WINERROR("ProcessOutput", status);
    return false;
  }

  IMFMediaBufferPtr mb;
  outDataBuffer.pSample->ConvertToContiguousBuffer(&mb);

  BYTE *pixels;
  DWORD curLen;
  mb->Lock(&pixels, NULL, &curLen);
  memcpy(buffer, pixels, curLen);
  mb->Unlock();

  if (outDataBuffer.pSample)
    outDataBuffer.pSample->Release();

  if (outDataBuffer.pEvents)
    outDataBuffer.pEvents->Release();

  dataLen = curLen;
  return true;
}

STDMETHODIMP MFT::H264::Invoke(IMFAsyncResult * pAsyncResult)
{
  HRESULT status, evtStatus;
  MediaEventType meType = MEUnknown;
  IMFMediaEventPtr pEvent = NULL;

  status = m_mediaEventGen->EndGetEvent(pAsyncResult, &pEvent);
  if (FAILED(status))
  {
    DEBUG_WINERROR("EndGetEvent", status);
    return status;
  }

  status = pEvent->GetStatus(&evtStatus);
  if (FAILED(status))
  {
    DEBUG_WINERROR("GetStatus", status);
    return status;
  }

  if (FAILED(evtStatus))
  {
    DEBUG_WINERROR("evtStatus", evtStatus);
    return evtStatus;
  }

  status = pEvent->GetType(&meType);
  if (FAILED(status))
  {
    DEBUG_WINERROR("GetType", status);
    return status;
  }

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