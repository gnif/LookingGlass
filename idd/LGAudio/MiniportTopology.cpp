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

#include "MiniportTopology.h"

namespace
{
KSDATARANGE g_AnalogRange =
{
  sizeof(KSDATARANGE),
  0,
  0,
  0,
  STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
  STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
  STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};

PKSDATARANGE g_AnalogRanges[] =
{
  &g_AnalogRange
};

PCPIN_DESCRIPTOR g_RenderPins[] =
{
  {
    0, 0, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      ARRAYSIZE(g_AnalogRanges), g_AnalogRanges,
      KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_NONE,
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
      ARRAYSIZE(g_AnalogRanges), g_AnalogRanges,
      KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_NONE,
      &KSNODETYPE_SPEAKER,
      nullptr,
      0
    }
  }
};

PCCONNECTION_DESCRIPTOR g_RenderConnections[] =
{
  {
    PCFILTER_NODE,
    kRenderTopologyBridgePin,
    PCFILTER_NODE,
    kRenderTopologyEndpointPin
  }
};

PCFILTER_DESCRIPTOR g_RenderDescriptor =
{
  0,
  nullptr,
  sizeof(PCPIN_DESCRIPTOR),
  ARRAYSIZE(g_RenderPins),
  g_RenderPins,
  sizeof(PCNODE_DESCRIPTOR),
  0,
  nullptr,
  ARRAYSIZE(g_RenderConnections),
  g_RenderConnections,
  0,
  nullptr
};

PCPIN_DESCRIPTOR g_CapturePins[] =
{
  {
    0, 0, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      ARRAYSIZE(g_AnalogRanges), g_AnalogRanges,
      KSPIN_DATAFLOW_IN,
      KSPIN_COMMUNICATION_NONE,
      &KSNODETYPE_MICROPHONE,
      nullptr,
      0
    }
  },
  {
    0, 0, 0, nullptr,
    {
      0, nullptr,
      0, nullptr,
      ARRAYSIZE(g_AnalogRanges), g_AnalogRanges,
      KSPIN_DATAFLOW_OUT,
      KSPIN_COMMUNICATION_NONE,
      &KSCATEGORY_AUDIO,
      nullptr,
      0
    }
  }
};

PCCONNECTION_DESCRIPTOR g_CaptureConnections[] =
{
  {
    PCFILTER_NODE,
    kCaptureTopologyEndpointPin,
    PCFILTER_NODE,
    kCaptureTopologyBridgePin
  }
};

PCFILTER_DESCRIPTOR g_CaptureDescriptor =
{
  0,
  nullptr,
  sizeof(PCPIN_DESCRIPTOR),
  ARRAYSIZE(g_CapturePins),
  g_CapturePins,
  sizeof(PCNODE_DESCRIPTOR),
  0,
  nullptr,
  ARRAYSIZE(g_CaptureConnections),
  g_CaptureConnections,
  0,
  nullptr
};
}

CMiniportTopology::CMiniportTopology(EndpointKind kind) :
  m_refCount(1),
  m_kind(kind)
{
}

NTSTATUS CMiniportTopology::QueryInterface(
  _In_ REFIID interfaceId,
  _COM_Outptr_ void **object)
{
  if (!object)
    return STATUS_INVALID_PARAMETER;

  *object = nullptr;
  if (GuidEqual(interfaceId, IID_IUnknown) ||
      GuidEqual(interfaceId, IID_IMiniport) ||
      GuidEqual(interfaceId, IID_IMiniportTopology))
  {
    *object = static_cast<IMiniportTopology *>(this);
    AddRef();
    return STATUS_SUCCESS;
  }

  return STATUS_NOINTERFACE;
}

ULONG CMiniportTopology::AddRef()
{
  return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG CMiniportTopology::Release()
{
  const LONG count = InterlockedDecrement(&m_refCount);
  if (!count)
    delete this;
  return static_cast<ULONG>(count);
}

NTSTATUS CMiniportTopology::GetDescription(
  _Out_ PPCFILTER_DESCRIPTOR *description)
{
  if (!description)
    return STATUS_INVALID_PARAMETER;

  *description = m_kind == EndpointKind::Render ?
    &g_RenderDescriptor : &g_CaptureDescriptor;
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportTopology::DataRangeIntersection(
  _In_ ULONG pinId,
  _In_ PKSDATARANGE dataRange,
  _In_ PKSDATARANGE matchingDataRange,
  _In_ ULONG outputBufferLength,
  _Out_writes_bytes_to_opt_(outputBufferLength, *resultantFormatLength)
    void *resultantFormat,
  _Out_ ULONG *resultantFormatLength)
{
  UNREFERENCED_PARAMETER(pinId);
  UNREFERENCED_PARAMETER(dataRange);
  UNREFERENCED_PARAMETER(matchingDataRange);
  UNREFERENCED_PARAMETER(outputBufferLength);
  UNREFERENCED_PARAMETER(resultantFormat);
  UNREFERENCED_PARAMETER(resultantFormatLength);
  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS CMiniportTopology::Init(
  _In_ PUNKNOWN unknownAdapter,
  _In_ PRESOURCELIST resourceList,
  _In_ PPORTTOPOLOGY port)
{
  UNREFERENCED_PARAMETER(unknownAdapter);
  UNREFERENCED_PARAMETER(resourceList);
  UNREFERENCED_PARAMETER(port);
  return STATUS_SUCCESS;
}
