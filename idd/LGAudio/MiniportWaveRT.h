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

#include "Adapter.h"
#include "Common.h"
#include "Formats.h"

class CMiniportWaveRT final : public IMiniportWaveRT
{
public:
  CMiniportWaveRT(
    _In_ CAudioAdapter *adapter,
    _In_ EndpointKind kind);
  ~CMiniportWaveRT();

  STDMETHODIMP_(NTSTATUS) QueryInterface(
    _In_ REFIID interfaceId,
    _COM_Outptr_ void **object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP_(NTSTATUS) GetDescription(
    _Out_ PPCFILTER_DESCRIPTOR *description) override;
  STDMETHODIMP_(NTSTATUS) DataRangeIntersection(
    _In_ ULONG pinId,
    _In_ PKSDATARANGE dataRange,
    _In_ PKSDATARANGE matchingDataRange,
    _In_ ULONG outputBufferLength,
    _Out_writes_bytes_to_opt_(outputBufferLength, *resultantFormatLength)
      void *resultantFormat,
    _Out_ ULONG *resultantFormatLength) override;
  STDMETHODIMP_(NTSTATUS) Init(
    _In_ PUNKNOWN unknownAdapter,
    _In_ PRESOURCELIST resourceList,
    _In_ PPORTWAVERT port) override;
  STDMETHODIMP_(NTSTATUS) NewStream(
    _Out_ PMINIPORTWAVERTSTREAM *stream,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ ULONG pin,
    _In_ BOOLEAN capture,
    _In_ PKSDATAFORMAT dataFormat) override;
  STDMETHODIMP_(NTSTATUS) GetDeviceDescription(
    _Out_ PDEVICE_DESCRIPTION deviceDescription) override;

  void StreamClosed();

private:
  volatile LONG m_refCount;
  volatile LONG m_streamOpen;
  CAudioAdapter *m_adapter;
  EndpointKind m_kind;
};
