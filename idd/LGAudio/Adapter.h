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

#pragma once

#include "AudioClock.h"
#include "Common.h"
#include "RingBuffer.h"

struct AudioStreamSnapshot
{
  KSSTATE state;
  bool formatValid;
  UCHAR reserved[3];
  ULONGLONG generation;
  FormatFingerprint format;
};

class CAudioAdapter final : public IUnknown
{
public:
  CAudioAdapter();
  ~CAudioAdapter();

  NTSTATUS Initialize();

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS ConfigureStream(
    _In_ EndpointKind kind,
    _In_ const FormatFingerprint &format);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void SetStreamState(
    _In_ EndpointKind kind,
    _In_ KSSTATE state);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void GetStreamSnapshot(
    _In_ EndpointKind kind,
    _Out_ AudioStreamSnapshot *snapshot);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void SetBrokerActive(_In_ bool active);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void ResetCapture();

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void WriteRenderFrames(
    _In_reads_bytes_(bytes) const BYTE *data,
    _In_ ULONG bytes);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void ReadCaptureFrames(
    _In_ const FormatFingerprint &format,
    _Out_writes_bytes_(bytes) BYTE *data,
    _In_ ULONG bytes);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void DiscardCaptureFrames(
    _In_ const FormatFingerprint &format,
    _In_ ULONGLONG bytes);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS ReadRenderFrames(
    _In_ const FormatFingerprint &format,
    _In_ ULONGLONG streamGeneration,
    _Out_writes_bytes_(capacity) BYTE *data,
    _In_ ULONG capacity,
    _Out_ PULONG bytesRead,
    _Out_ bool *discontinuity);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS WriteCaptureFrames(
    _In_ const FormatFingerprint &format,
    _In_ ULONGLONG streamGeneration,
    _In_ bool discontinuity,
    _In_reads_bytes_(bytes) const BYTE *data,
    _In_ ULONG bytes,
    _Out_ PULONG droppedBytes);

  _IRQL_requires_max_(DISPATCH_LEVEL)
  AudioClockSnapshot GetClockSnapshot() const;

  STDMETHODIMP_(NTSTATUS) QueryInterface(
    _In_ REFIID interfaceId,
    _COM_Outptr_ void **object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

private:
  volatile LONG m_refCount;
  volatile LONG m_brokerActive;
  volatile LONG m_renderDiscontinuity;
  KSPIN_LOCK m_transportLock;
  KSPIN_LOCK m_streamStateLock;
  AudioStreamSnapshot m_renderState;
  AudioStreamSnapshot m_captureState;
  CAudioClock m_clock;
  CAudioRingBuffer m_renderRing;
  CAudioRingBuffer m_captureRing;
};

NTSTATUS StartAudioDevice(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ PIRP irp,
  _In_ PRESOURCELIST resourceList);
