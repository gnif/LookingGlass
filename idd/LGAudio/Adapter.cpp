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

#include "Adapter.h"
#include "Formats.h"
#include "MiniportTopology.h"
#include "MiniportWaveRT.h"

namespace
{
NTSTATUS InstallTopologySubdevice(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ PIRP irp,
  _In_ PRESOURCELIST resourceList,
  _In_ CAudioAdapter *adapter,
  _In_ EndpointKind kind,
  _In_ const wchar_t *name,
  _Outptr_ PUNKNOWN *portUnknown)
{
  *portUnknown = nullptr;

  PPORT port = nullptr;
  NTSTATUS status = PcNewPort(&port, CLSID_PortTopology);
  if (!NT_SUCCESS(status))
    return status;

  CMiniportTopology *miniport = new(NonPagedPoolNx, LGAUDIO_POOL_TAG)
    CMiniportTopology(kind);
  if (!miniport)
  {
    port->Release();
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = port->Init(deviceObject, irp,
    static_cast<IMiniportTopology *>(miniport), adapter, resourceList);
  if (NT_SUCCESS(status))
    status = PcRegisterSubdevice(deviceObject,
      const_cast<PWSTR>(name), port);

  miniport->Release();

  if (!NT_SUCCESS(status))
  {
    port->Release();
    return status;
  }

  *portUnknown = port;
  return STATUS_SUCCESS;
}

NTSTATUS InstallWaveSubdevice(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ PIRP irp,
  _In_ PRESOURCELIST resourceList,
  _In_ CAudioAdapter *adapter,
  _In_ EndpointKind kind,
  _In_ const wchar_t *name,
  _Outptr_ PUNKNOWN *portUnknown)
{
  *portUnknown = nullptr;

  PPORT port = nullptr;
  NTSTATUS status = PcNewPort(&port, CLSID_PortWaveRT);
  if (!NT_SUCCESS(status))
    return status;

  CMiniportWaveRT *miniport = new(NonPagedPoolNx, LGAUDIO_POOL_TAG)
    CMiniportWaveRT(adapter, kind);
  if (!miniport)
  {
    port->Release();
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = port->Init(deviceObject, irp,
    static_cast<IMiniportWaveRT *>(miniport), adapter, resourceList);
  if (NT_SUCCESS(status))
    status = PcRegisterSubdevice(deviceObject,
      const_cast<PWSTR>(name), port);

  miniport->Release();

  if (!NT_SUCCESS(status))
  {
    port->Release();
    return status;
  }

  *portUnknown = port;
  return STATUS_SUCCESS;
}
}

CAudioAdapter::CAudioAdapter() :
  m_refCount(1)
{
}

CAudioAdapter::~CAudioAdapter()
{
  m_ring.Destroy();
}

NTSTATUS CAudioAdapter::Initialize()
{
  NTSTATUS status = m_clock.Initialize();
  if (NT_SUCCESS(status))
    status = m_ring.Init();
  return status;
}
_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS CAudioAdapter::ConfigureRender(
  _In_ const FormatFingerprint &format)
{
  return m_ring.Configure(format);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::ResetCapture()
{
  m_ring.Reset();
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::WriteRenderFrames(
  _In_reads_bytes_(bytes) const BYTE *data,
  _In_ ULONG bytes)
{
  m_ring.Push(data, bytes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::ReadCaptureFrames(
  _In_ const FormatFingerprint &format,
  _Out_writes_bytes_(bytes) BYTE *data,
  _In_ ULONG bytes)
{
  m_ring.Pop(format, data, bytes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::DiscardCaptureFrames(
  _In_ const FormatFingerprint &format,
  _In_ ULONGLONG bytes)
{
  m_ring.Discard(format, bytes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
AudioClockSnapshot CAudioAdapter::GetClockSnapshot() const
{
  return m_clock.GetSnapshot();
}

NTSTATUS CAudioAdapter::QueryInterface(
  _In_ REFIID interfaceId,
  _COM_Outptr_ void **object)
{
  if (!object)
    return STATUS_INVALID_PARAMETER;

  *object = nullptr;
  if (!GuidEqual(interfaceId, IID_IUnknown))
    return STATUS_NOINTERFACE;

  *object = static_cast<IUnknown *>(this);
  AddRef();
  return STATUS_SUCCESS;
}

ULONG CAudioAdapter::AddRef()
{
  return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG CAudioAdapter::Release()
{
  const LONG count = InterlockedDecrement(&m_refCount);
  if (!count)
    delete this;
  return static_cast<ULONG>(count);
}

NTSTATUS StartAudioDevice(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ PIRP irp,
  _In_ PRESOURCELIST resourceList)
{
  PUNKNOWN topologyRender = nullptr;
  PUNKNOWN waveRender = nullptr;
  PUNKNOWN topologyCapture = nullptr;
  PUNKNOWN waveCapture = nullptr;

  CAudioAdapter *adapter = new(NonPagedPoolNx, LGAUDIO_POOL_TAG)
    CAudioAdapter();
  if (!adapter)
    return STATUS_INSUFFICIENT_RESOURCES;

  NTSTATUS status = adapter->Initialize();
  if (NT_SUCCESS(status))
    status = InstallTopologySubdevice(deviceObject, irp, resourceList,
      adapter, EndpointKind::Render, kTopologyRenderName, &topologyRender);
  if (NT_SUCCESS(status))
    status = InstallWaveSubdevice(deviceObject, irp, resourceList,
      adapter, EndpointKind::Render, kWaveRenderName, &waveRender);
  if (NT_SUCCESS(status))
    status = InstallTopologySubdevice(deviceObject, irp, resourceList,
      adapter, EndpointKind::Capture, kTopologyCaptureName,
      &topologyCapture);
  if (NT_SUCCESS(status))
    status = InstallWaveSubdevice(deviceObject, irp, resourceList,
      adapter, EndpointKind::Capture, kWaveCaptureName, &waveCapture);

  if (NT_SUCCESS(status))
    status = PcRegisterPhysicalConnection(deviceObject, waveRender,
      kRenderWaveBridgePin, topologyRender, kRenderTopologyBridgePin);
  if (NT_SUCCESS(status))
    status = PcRegisterPhysicalConnection(deviceObject, topologyCapture,
      kCaptureTopologyBridgePin, waveCapture, kCaptureWaveBridgePin);

  SafeRelease(waveCapture);
  SafeRelease(topologyCapture);
  SafeRelease(waveRender);
  SafeRelease(topologyRender);
  adapter->Release();

  if (!NT_SUCCESS(status))
    LGAUDIO_TRACE_ERROR("device start failed: 0x%08X", status);
  else
    LGAUDIO_TRACE_INFO("render and capture endpoints registered (%s)",
      "Looking Glass Audio");

  return status;
}
