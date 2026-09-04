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

class CMiniportWaveRT;

class CMiniportWaveRTStream final :
  public IMiniportWaveRTStreamNotification,
  public IMiniportWaveRTInputStream,
  public IMiniportWaveRTOutputStream
{
public:
  CMiniportWaveRTStream(
    _In_ CMiniportWaveRT *miniport,
    _In_ CAudioAdapter *adapter,
    _In_ PPORTWAVERTSTREAM portStream,
    _In_ EndpointKind kind);
  ~CMiniportWaveRTStream();

  NTSTATUS Initialize(
    _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *format);

  STDMETHODIMP_(NTSTATUS) QueryInterface(
    _In_ REFIID interfaceId,
    _COM_Outptr_ void **object) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  STDMETHODIMP_(NTSTATUS) SetFormat(
    _In_ PKSDATAFORMAT dataFormat) override;
  STDMETHODIMP_(NTSTATUS) SetState(_In_ KSSTATE state) override;
  STDMETHODIMP_(NTSTATUS) GetPosition(
    _Out_ PKSAUDIO_POSITION position) override;
  STDMETHODIMP_(NTSTATUS) AllocateAudioBuffer(
    _In_ ULONG requestedSize,
    _Out_ PMDL *audioBufferMdl,
    _Out_ ULONG *actualSize,
    _Out_ ULONG *offsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *cacheType) override;
  STDMETHODIMP_(void) FreeAudioBuffer(
    _In_opt_ PMDL audioBufferMdl,
    _In_ ULONG bufferSize) override;
  STDMETHODIMP_(void) GetHWLatency(
    _Out_ PKSRTAUDIO_HWLATENCY latency) override;
  STDMETHODIMP_(NTSTATUS) GetPositionRegister(
    _Out_ PKSRTAUDIO_HWREGISTER positionRegister) override;
  STDMETHODIMP_(NTSTATUS) GetClockRegister(
    _Out_ PKSRTAUDIO_HWREGISTER clockRegister) override;

  STDMETHODIMP_(NTSTATUS) AllocateBufferWithNotification(
    _In_ ULONG notificationCount,
    _In_ ULONG requestedSize,
    _Out_ PMDL *audioBufferMdl,
    _Out_ ULONG *actualSize,
    _Out_ ULONG *offsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *cacheType) override;
  STDMETHODIMP_(void) FreeBufferWithNotification(
    _In_ PMDL audioBufferMdl,
    _In_ ULONG bufferSize) override;
  STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(
    _In_ PKEVENT notificationEvent) override;
  STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(
    _In_ PKEVENT notificationEvent) override;

  STDMETHODIMP_(NTSTATUS) GetReadPacket(
    _Out_ ULONG *packetNumber,
    _Out_ DWORD *flags,
    _Out_ ULONG64 *performanceCounterValue,
    _Out_ BOOL *moreData) override;

  STDMETHODIMP_(NTSTATUS) SetWritePacket(
    _In_ ULONG packetNumber,
    _In_ DWORD flags,
    _In_ ULONG eosPacketLength) override;
  STDMETHODIMP_(NTSTATUS) GetOutputStreamPresentationPosition(
    _Out_ KSAUDIO_PRESENTATION_POSITION *position) override;
  STDMETHODIMP_(NTSTATUS) GetPacketCount(
    _Out_ ULONG *packetCount) override;

private:
  static void TimerCallback(
    _In_ PEX_TIMER timer,
    _In_opt_ void *context);

  NTSTATUS AllocateBuffer(
    _In_ ULONG notificationCount,
    _In_ ULONG requestedSize,
    _Out_ PMDL *audioBufferMdl,
    _Out_ ULONG *actualSize,
    _Out_ ULONG *offsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *cacheType);
  void FreeBuffer(_In_opt_ PMDL audioBufferMdl);
  void StopTimer();
  void ArmTimerLocked();
  void AdvanceLocked(_In_ const AudioClockSnapshot &clock);
  void AdvanceEventPacketsLocked(
    _In_ const AudioClockSnapshot &clock);
  bool RenderPacketCommittedLocked(_In_ ULONGLONG position) const;
  void PublishRenderFramesLocked();
  void UpdatePresentationQpcLocked(
    _In_ const AudioClockSnapshot &clock);
  void TransferFramesLocked(
    _In_ ULONGLONG bytes,
    _In_ bool transferRender = true);
  void SignalNotificationsLocked();

  volatile LONG m_refCount;
  CMiniportWaveRT *m_miniport;
  CAudioAdapter *m_adapter;
  PPORTWAVERTSTREAM m_portStream;
  EndpointKind m_kind;

  KSDATAFORMAT_WAVEFORMATEXTENSIBLE m_format;
  FormatFingerprint m_fingerprint;
  KSSTATE m_state;

  KSPIN_LOCK m_positionLock;
  PEX_TIMER m_timer;
  bool m_hasRun;

  PMDL m_bufferMdl;
  BYTE *m_buffer;
  bool m_bufferAllocationPending;
  ULONG m_bufferSize;
  ULONG m_notificationCount;
  ULONG m_packetSize;
  ULONG m_renderPublicationSliceBytes;
  PKEVENT m_notificationEvents[8];
  ULONG m_notificationEventCount;

  ULONGLONG m_clockFrequency;
  ULONGLONG m_qpcFrequency;
  ULONGLONG m_clockRateQ32;
  ULONGLONG m_lastClockPosition;
  ULONGLONG m_lastQpcPosition;
  ULONGLONG m_lastPresentationQpc;
  ULONGLONG m_frameRemainder;
  ULONGLONG m_pendingBytes;
  ULONGLONG m_linearPosition;
  ULONG m_cyclicPosition;
  ULONGLONG m_renderPublishedPosition;
  ULONGLONG m_completedPackets;
  ULONGLONG m_lastReadPacket;
  ULONG m_lastWritePacket;
  bool m_eosReceived;
  bool m_eosNotificationSent;
  ULONGLONG m_eosLinearPosition;
};
