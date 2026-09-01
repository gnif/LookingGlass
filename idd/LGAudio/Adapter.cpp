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
#include "BrokerDevice.h"
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
  m_refCount(1),
  m_brokerActive(0),
  m_renderDiscontinuity(0),
  m_renderState{},
  m_captureState{}
{
  KeInitializeSpinLock(&m_transportLock);
  KeInitializeSpinLock(&m_streamStateLock);
  m_renderState.state = KSSTATE_STOP;
  m_captureState.state = KSSTATE_STOP;
}

CAudioAdapter::~CAudioAdapter()
{
  m_captureRing.Destroy();
  m_renderRing.Destroy();
}

NTSTATUS CAudioAdapter::Initialize()
{
  NTSTATUS status = m_clock.Initialize();
  if (NT_SUCCESS(status))
    status = m_renderRing.Init();
  if (NT_SUCCESS(status))
    status = m_captureRing.Init();
  return status;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS CAudioAdapter::ConfigureStream(
  _In_ EndpointKind kind,
  _In_ const FormatFingerprint &format)
{
  CAudioRingBuffer &ring = kind == EndpointKind::Render ?
    m_renderRing : m_captureRing;
  KIRQL transportIrql;
  KeAcquireSpinLock(&m_transportLock, &transportIrql);
  const NTSTATUS status = ring.Configure(format);
  if (!NT_SUCCESS(status))
  {
    KeReleaseSpinLock(&m_transportLock, transportIrql);
    return status;
  }

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_streamStateLock, &oldIrql);
  AudioStreamSnapshot &stream = kind == EndpointKind::Render ?
    m_renderState : m_captureState;
  stream.format = format;
  stream.formatValid = true;
  ++stream.generation;
  KeReleaseSpinLock(&m_streamStateLock, oldIrql);
  KeReleaseSpinLock(&m_transportLock, transportIrql);

  BrokerDeviceNotifyStreamChanged(this, kind);
  return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::SetStreamState(
  _In_ EndpointKind kind,
  _In_ KSSTATE state)
{
  bool changed;
  bool reset;

  KIRQL transportIrql;
  KeAcquireSpinLock(&m_transportLock, &transportIrql);
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_streamStateLock, &oldIrql);
  AudioStreamSnapshot &stream = kind == EndpointKind::Render ?
    m_renderState : m_captureState;
  changed = stream.state != state;
  reset = changed && state == KSSTATE_STOP;
  stream.state = state;
  if (reset)
    ++stream.generation;
  KeReleaseSpinLock(&m_streamStateLock, oldIrql);

  if (reset)
  {
    if (kind == EndpointKind::Render)
      m_renderRing.Reset();
    else
      m_captureRing.Reset();
  }
  KeReleaseSpinLock(&m_transportLock, transportIrql);
  if (changed)
    BrokerDeviceNotifyStreamChanged(this, kind);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::GetStreamSnapshot(
  _In_ EndpointKind kind,
  _Out_ AudioStreamSnapshot *snapshot)
{
  if (!snapshot)
    return;

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_streamStateLock, &oldIrql);
  *snapshot = kind == EndpointKind::Render ?
    m_renderState : m_captureState;
  KeReleaseSpinLock(&m_streamStateLock, oldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::SetBrokerActive(_In_ bool active)
{
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_transportLock, &oldIrql);
  const LONG value = active ? 1 : 0;
  if (InterlockedExchange(&m_brokerActive, value) == value)
  {
    KeReleaseSpinLock(&m_transportLock, oldIrql);
    return;
  }

  m_captureRing.Reset();
  m_renderRing.Reset();
  InterlockedExchange(&m_renderDiscontinuity, active ? 1 : 0);
  KeReleaseSpinLock(&m_transportLock, oldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::ResetCapture()
{
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_transportLock, &oldIrql);
  if (InterlockedCompareExchange(&m_brokerActive, 0, 0))
    m_captureRing.Reset();
  else
    m_renderRing.Reset();
  KeReleaseSpinLock(&m_transportLock, oldIrql);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::WriteRenderFrames(
  _In_reads_bytes_(bytes) const BYTE *data,
  _In_ ULONG bytes)
{
  ULONG droppedBytes = 0;
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_transportLock, &oldIrql);
  m_renderRing.Push(data, bytes, &droppedBytes);
  if (droppedBytes)
    InterlockedExchange(&m_renderDiscontinuity, 1);
  KeReleaseSpinLock(&m_transportLock, oldIrql);
  BrokerDeviceNotifyRenderAvailable(this, bytes, droppedBytes);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::ReadCaptureFrames(
  _In_ const FormatFingerprint &format,
  _Out_writes_bytes_(bytes) BYTE *data,
  _In_ ULONG bytes)
{
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_transportLock, &oldIrql);
  const bool brokerActive =
    InterlockedCompareExchange(&m_brokerActive, 0, 0) != 0;
  CAudioRingBuffer &ring = brokerActive ?
    m_captureRing : m_renderRing;
  const ULONG copied = ring.Pop(format, data, bytes);
  KeReleaseSpinLock(&m_transportLock, oldIrql);
  if (brokerActive && copied < bytes)
    BrokerDeviceRecordCaptureUnderrun(this, bytes - copied);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
void CAudioAdapter::DiscardCaptureFrames(
  _In_ const FormatFingerprint &format,
  _In_ ULONGLONG bytes)
{
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_transportLock, &oldIrql);
  const bool brokerActive =
    InterlockedCompareExchange(&m_brokerActive, 0, 0) != 0;
  CAudioRingBuffer &ring = brokerActive ?
    m_captureRing : m_renderRing;
  const ULONG discarded = ring.Discard(format, bytes);
  KeReleaseSpinLock(&m_transportLock, oldIrql);
  if (brokerActive && bytes > discarded)
    BrokerDeviceRecordCaptureUnderrun(this, bytes - discarded);
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS CAudioAdapter::ReadRenderFrames(
  _In_ const FormatFingerprint &format,
  _In_ ULONGLONG streamGeneration,
  _Out_writes_bytes_(capacity) BYTE *data,
  _In_ ULONG capacity,
  _Out_ PULONG bytesRead,
  _Out_ bool *discontinuity)
{
  if (!data || !bytesRead || !discontinuity || !format.blockAlign)
    return STATUS_INVALID_PARAMETER;

  *bytesRead = 0;
  *discontinuity = false;
  KIRQL transportIrql;
  KeAcquireSpinLock(&m_transportLock, &transportIrql);

  KIRQL stateIrql;
  KeAcquireSpinLock(&m_streamStateLock, &stateIrql);
  const bool active =
    InterlockedCompareExchange(&m_brokerActive, 0, 0) != 0 &&
    m_renderState.state == KSSTATE_RUN &&
    m_renderState.formatValid &&
    m_renderState.generation == streamGeneration &&
    EqualFormatFingerprint(m_renderState.format, format);
  KeReleaseSpinLock(&m_streamStateLock, stateIrql);
  if (!active)
  {
    KeReleaseSpinLock(&m_transportLock, transportIrql);
    return STATUS_DEVICE_NOT_READY;
  }

  ULONG bytes = m_renderRing.Available();
  if (bytes > capacity)
    bytes = capacity;
  bytes -= bytes % format.blockAlign;
  if (!bytes)
  {
    KeReleaseSpinLock(&m_transportLock, transportIrql);
    return STATUS_SUCCESS;
  }

  const ULONG copied = m_renderRing.Pop(format, data, bytes);
  if (copied)
  {
    *discontinuity = InterlockedExchange(
      &m_renderDiscontinuity, 0) != 0;
  }
  *bytesRead = copied;
  KeReleaseSpinLock(&m_transportLock, transportIrql);
  return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
NTSTATUS CAudioAdapter::WriteCaptureFrames(
  _In_ const FormatFingerprint &format,
  _In_ ULONGLONG streamGeneration,
  _In_ bool discontinuity,
  _In_reads_bytes_(bytes) const BYTE *data,
  _In_ ULONG bytes,
  _Out_ PULONG droppedBytes)
{
  if (!data || !droppedBytes)
    return STATUS_INVALID_PARAMETER;

  *droppedBytes = 0;
  KIRQL transportIrql;
  KeAcquireSpinLock(&m_transportLock, &transportIrql);

  KIRQL stateIrql;
  KeAcquireSpinLock(&m_streamStateLock, &stateIrql);
  const bool active =
    InterlockedCompareExchange(&m_brokerActive, 0, 0) != 0 &&
    m_captureState.state == KSSTATE_RUN &&
    m_captureState.formatValid &&
    m_captureState.generation == streamGeneration &&
    EqualFormatFingerprint(m_captureState.format, format);
  KeReleaseSpinLock(&m_streamStateLock, stateIrql);
  if (!active)
  {
    KeReleaseSpinLock(&m_transportLock, transportIrql);
    return STATUS_DEVICE_NOT_READY;
  }

  if (discontinuity)
    m_captureRing.Reset();
  const ULONG written = m_captureRing.Push(data, bytes, droppedBytes);
  KeReleaseSpinLock(&m_transportLock, transportIrql);
  return written == bytes ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
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

  if (NT_SUCCESS(status))
  {
    const NTSTATUS brokerStatus =
      BrokerDeviceAttachAdapter(deviceObject, adapter);
    if (!NT_SUCCESS(brokerStatus))
    {
      LGAUDIO_TRACE_ERROR("broker adapter unavailable: 0x%08X",
        brokerStatus);
    }
  }

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
