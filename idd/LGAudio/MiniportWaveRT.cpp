/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "MiniportWaveRT.h"
#include "MiniportWaveRTStream.h"

namespace
{
KSDATARANGE g_BridgeRange =
{
  sizeof(KSDATARANGE),
  0,
  0,
  0,
  STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
  STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
  STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

PKSDATARANGE g_BridgeRanges[] =
{
  &g_BridgeRange
};

PCPIN_DESCRIPTOR g_RenderWavePins[] =
{
  {
    1, 1, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      kRenderFormatCount, g_renderDataRangePointers,
      KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_SINK,
      &KSCATEGORY_AUDIO,
      nullptr,
      0
    }
  },
  {
    0, 0, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      ARRAYSIZE(g_BridgeRanges), g_BridgeRanges,
      KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_NONE,
      &KSCATEGORY_AUDIO,
      nullptr,
      0
    }
  }
};

PCCONNECTION_DESCRIPTOR g_RenderWaveConnections[] =
{
  {
    PCFILTER_NODE,
    kRenderWaveHostPin,
    PCFILTER_NODE,
    kRenderWaveBridgePin
  }
};

PCFILTER_DESCRIPTOR g_RenderWaveDescriptor =
{
  0,
  nullptr,
  sizeof(PCPIN_DESCRIPTOR),
  ARRAYSIZE(g_RenderWavePins),
  g_RenderWavePins,
  sizeof(PCNODE_DESCRIPTOR),
  0,
  nullptr,
  ARRAYSIZE(g_RenderWaveConnections),
  g_RenderWaveConnections,
  0,
  nullptr
};

PCPIN_DESCRIPTOR g_CaptureWavePins[] =
{
  {
    0, 0, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      ARRAYSIZE(g_BridgeRanges), g_BridgeRanges,
      KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_NONE,
      &KSCATEGORY_AUDIO,
      nullptr,
      0
    }
  },
  {
    1, 1, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      kCaptureFormatCount, g_captureDataRangePointers,
      KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_SINK,
      &KSCATEGORY_AUDIO,
      &KSAUDFNAME_RECORDING_CONTROL,
      0
    }
  }
};

PCNODE_DESCRIPTOR g_CaptureWaveNodes[] =
{
  {
    0,
    nullptr,
    &KSNODETYPE_ADC,
    nullptr
  }
};

PCCONNECTION_DESCRIPTOR g_CaptureWaveConnections[] =
{
  {
    PCFILTER_NODE,
    kCaptureWaveBridgePin,
    0,
    1
  },
  {
    0,
    0,
    PCFILTER_NODE,
    kCaptureWaveHostPin
  }
};

PCFILTER_DESCRIPTOR g_CaptureWaveDescriptor =
{
  0,
  nullptr,
  sizeof(PCPIN_DESCRIPTOR),
  ARRAYSIZE(g_CaptureWavePins),
  g_CaptureWavePins,
  sizeof(PCNODE_DESCRIPTOR),
  ARRAYSIZE(g_CaptureWaveNodes),
  g_CaptureWaveNodes,
  ARRAYSIZE(g_CaptureWaveConnections),
  g_CaptureWaveConnections,
  0,
  nullptr
};
}

CMiniportWaveRT::CMiniportWaveRT(
  _In_ CAudioAdapter *adapter,
  _In_ EndpointKind kind) :
  m_refCount(1),
  m_streamOpen(0),
  m_adapter(adapter),
  m_kind(kind)
{
  m_adapter->AddRef();
}

CMiniportWaveRT::~CMiniportWaveRT()
{
  m_adapter->Release();
}

NTSTATUS CMiniportWaveRT::QueryInterface(
  _In_ REFIID interfaceId,
  _COM_Outptr_ void **object)
{
  if (!object)
    return STATUS_INVALID_PARAMETER;

  *object = nullptr;
  if (GuidEqual(interfaceId, IID_IUnknown) ||
      GuidEqual(interfaceId, IID_IMiniport) ||
      GuidEqual(interfaceId, IID_IMiniportWaveRT))
  {
    *object = static_cast<IMiniportWaveRT *>(this);
    AddRef();
    return STATUS_SUCCESS;
  }

  return STATUS_NOINTERFACE;
}

ULONG CMiniportWaveRT::AddRef()
{
  return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG CMiniportWaveRT::Release()
{
  const LONG count = InterlockedDecrement(&m_refCount);
  if (!count)
    delete this;
  return static_cast<ULONG>(count);
}

NTSTATUS CMiniportWaveRT::GetDescription(
  _Out_ PPCFILTER_DESCRIPTOR *description)
{
  if (!description)
    return STATUS_INVALID_PARAMETER;

  *description = m_kind == EndpointKind::Render ?
    &g_RenderWaveDescriptor : &g_CaptureWaveDescriptor;
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRT::DataRangeIntersection(
  _In_ ULONG pinId,
  _In_ PKSDATARANGE dataRange,
  _In_ PKSDATARANGE matchingDataRange,
  _In_ ULONG outputBufferLength,
  _Out_writes_bytes_to_opt_(outputBufferLength, *resultantFormatLength)
    void *resultantFormat,
  _Out_ ULONG *resultantFormatLength)
{
  const ULONG hostPin = m_kind == EndpointKind::Render ?
    kRenderWaveHostPin : kCaptureWaveHostPin;
  if (pinId != hostPin)
    return STATUS_NOT_IMPLEMENTED;

  return SelectFormatIntersection(m_kind, dataRange, matchingDataRange,
    outputBufferLength, resultantFormat, resultantFormatLength);
}

NTSTATUS CMiniportWaveRT::Init(
  _In_ PUNKNOWN unknownAdapter,
  _In_ PRESOURCELIST resourceList,
  _In_ PPORTWAVERT port)
{
  UNREFERENCED_PARAMETER(unknownAdapter);
  UNREFERENCED_PARAMETER(resourceList);
  UNREFERENCED_PARAMETER(port);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRT::NewStream(
  _Out_ PMINIPORTWAVERTSTREAM *stream,
  _In_ PPORTWAVERTSTREAM portStream,
  _In_ ULONG pin,
  _In_ BOOLEAN capture,
  _In_ PKSDATAFORMAT dataFormat)
{
  if (!stream || !portStream || !dataFormat)
    return STATUS_INVALID_PARAMETER;
  *stream = nullptr;

  const bool isCapture = m_kind == EndpointKind::Capture;
  const ULONG hostPin = isCapture ?
    kCaptureWaveHostPin : kRenderWaveHostPin;
  if (pin != hostPin || (!!capture != isCapture))
    return STATUS_INVALID_PARAMETER;

  const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *matchedFormat = nullptr;
  NTSTATUS status = ValidateFormat(m_kind, dataFormat, &matchedFormat);
  if (!NT_SUCCESS(status))
    return status;
  if (!matchedFormat)
    return STATUS_NO_MATCH;

  if (InterlockedCompareExchange(&m_streamOpen, 1, 0) != 0)
    return STATUS_DEVICE_BUSY;

  CMiniportWaveRTStream *newStream =
    new(NonPagedPoolNx, LGAUDIO_POOL_TAG) CMiniportWaveRTStream(
      this, m_adapter, portStream, m_kind);
  if (!newStream)
  {
    InterlockedExchange(&m_streamOpen, 0);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = newStream->Initialize(matchedFormat);
  if (!NT_SUCCESS(status))
  {
    newStream->Release();
    return status;
  }

  *stream = static_cast<IMiniportWaveRTStream *>(newStream);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRT::GetDeviceDescription(
  _Out_ PDEVICE_DESCRIPTION deviceDescription)
{
  if (!deviceDescription)
    return STATUS_INVALID_PARAMETER;

  RtlZeroMemory(deviceDescription, sizeof(*deviceDescription));
  deviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
  deviceDescription->Master = TRUE;
  deviceDescription->ScatterGather = TRUE;
  deviceDescription->Dma32BitAddresses = TRUE;
  deviceDescription->InterfaceType = PCIBus;
  deviceDescription->MaximumLength = MAXULONG;
  return STATUS_SUCCESS;
}

void CMiniportWaveRT::StreamClosed()
{
  InterlockedExchange(&m_streamOpen, 0);
}
