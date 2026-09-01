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

class CAudioAdapter final : public IUnknown
{
public:
  CAudioAdapter();
  ~CAudioAdapter();

  NTSTATUS Initialize();

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS ConfigureRender(
    _In_ const FormatFingerprint &format);

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
  AudioClockSnapshot GetClockSnapshot() const;

  STDMETHODIMP_(NTSTATUS) QueryInterface(
    _In_ REFIID interfaceId,
    _COM_Outptr_ void **object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

private:
  volatile LONG m_refCount;
  CAudioClock m_clock;
  CAudioRingBuffer m_ring;
};

NTSTATUS StartAudioDevice(
  _In_ PDEVICE_OBJECT deviceObject,
  _In_ PIRP irp,
  _In_ PRESOURCELIST resourceList);
