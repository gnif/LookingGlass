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

#include "MiniportWaveRTStream.h"
#include "MiniportWaveRT.h"

namespace
{
ULONGLONG MinUlonglong(ULONGLONG left, ULONGLONG right)
{
  return left < right ? left : right;
}
}

CMiniportWaveRTStream::CMiniportWaveRTStream(
  _In_ CMiniportWaveRT *miniport,
  _In_ CAudioAdapter *adapter,
  _In_ PPORTWAVERTSTREAM portStream,
  _In_ EndpointKind kind) :
  m_refCount(1),
  m_miniport(miniport),
  m_adapter(adapter),
  m_portStream(portStream),
  m_kind(kind),
  m_format{},
  m_fingerprint{},
  m_state(KSSTATE_STOP),
  m_timer(nullptr),
  m_hasRun(false),
  m_bufferMdl(nullptr),
  m_buffer(nullptr),
  m_bufferAllocationPending(false),
  m_bufferSize(0),
  m_notificationCount(0),
  m_packetSize(0),
  m_notificationEvents{},
  m_notificationEventCount(0),
  m_clockFrequency(0),
  m_qpcFrequency(0),
  m_lastClockPosition(0),
  m_lastQpcPosition(0),
  m_lastPresentationQpc(0),
  m_frameRemainder(0),
  m_pendingBytes(0),
  m_linearPosition(0),
  m_cyclicPosition(0),
  m_completedPackets(0),
  m_lastReadPacket(~0ULL),
  m_lastWritePacket(MAXULONG),
  m_eosReceived(false),
  m_eosNotificationSent(false),
  m_eosLinearPosition(0)
{
  m_miniport->AddRef();
  m_adapter->AddRef();

  KeInitializeSpinLock(&m_positionLock);
}

CMiniportWaveRTStream::~CMiniportWaveRTStream()
{
  SetState(KSSTATE_STOP);

  if (m_timer)
  {
    NT_ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
    ExDeleteTimer(m_timer, TRUE, TRUE, nullptr);
    m_timer = nullptr;
  }

  FreeBuffer(m_bufferMdl);

  m_miniport->StreamClosed();
  m_adapter->Release();
  m_miniport->Release();
}

NTSTATUS CMiniportWaveRTStream::Initialize(
  _In_ const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *format)
{
  if (!format)
    return STATUS_INVALID_PARAMETER;

  if (!m_timer)
  {
    m_timer = ExAllocateTimer(TimerCallback, this,
      EX_TIMER_HIGH_RESOLUTION);
    if (!m_timer)
      return STATUS_INSUFFICIENT_RESOURCES;
  }

  FormatFingerprint fingerprint{};
  NTSTATUS status = GetFormatFingerprint(format, &fingerprint);
  if (!NT_SUCCESS(status))
    return status;

  m_format = *format;
  m_fingerprint = fingerprint;

  if (m_kind == EndpointKind::Render)
    status = m_adapter->ConfigureRender(m_fingerprint);
  return status;
}

NTSTATUS CMiniportWaveRTStream::QueryInterface(
  _In_ REFIID interfaceId,
  _COM_Outptr_ void **object)
{
  if (!object)
    return STATUS_INVALID_PARAMETER;

  *object = nullptr;
  if (GuidEqual(interfaceId, IID_IUnknown) ||
      GuidEqual(interfaceId, IID_IMiniportWaveRTStream) ||
      GuidEqual(interfaceId, IID_IMiniportWaveRTStreamNotification))
  {
    *object = static_cast<IMiniportWaveRTStreamNotification *>(this);
  }
  else if (m_kind == EndpointKind::Capture &&
           GuidEqual(interfaceId, IID_IMiniportWaveRTInputStream))
  {
    *object = static_cast<IMiniportWaveRTInputStream *>(this);
  }
  else if (m_kind == EndpointKind::Render &&
           GuidEqual(interfaceId, IID_IMiniportWaveRTOutputStream))
  {
    *object = static_cast<IMiniportWaveRTOutputStream *>(this);
  }
  else
  {
    return STATUS_NOINTERFACE;
  }

  AddRef();
  return STATUS_SUCCESS;
}

ULONG CMiniportWaveRTStream::AddRef()
{
  return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG CMiniportWaveRTStream::Release()
{
  const LONG count = InterlockedDecrement(&m_refCount);
  if (!count)
    delete this;
  return static_cast<ULONG>(count);
}

NTSTATUS CMiniportWaveRTStream::SetFormat(
  _In_ PKSDATAFORMAT dataFormat)
{
  if (!dataFormat)
    return STATUS_INVALID_PARAMETER;

  const KSDATAFORMAT_WAVEFORMATEXTENSIBLE *matched = nullptr;
  NTSTATUS status = ValidateFormat(m_kind, dataFormat, &matched);
  if (!NT_SUCCESS(status))
    return status;
  if (!matched)
    return STATUS_NO_MATCH;

  FormatFingerprint fingerprint{};
  status = GetFormatFingerprint(matched, &fingerprint);
  if (!NT_SUCCESS(status))
    return status;

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (m_state != KSSTATE_STOP || m_buffer)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INVALID_DEVICE_STATE;
  }

  m_format = *matched;
  m_fingerprint = fingerprint;
  KeReleaseSpinLock(&m_positionLock, oldIrql);

  if (m_kind == EndpointKind::Render)
    status = m_adapter->ConfigureRender(m_fingerprint);
  return status;
}

NTSTATUS CMiniportWaveRTStream::SetState(_In_ KSSTATE state)
{
  if (state < KSSTATE_STOP || state > KSSTATE_RUN)
    return STATUS_INVALID_PARAMETER;

  if (state == KSSTATE_RUN)
  {
    const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
    if (!clock.Frequency || !clock.QpcFrequency)
      return STATUS_DEVICE_HARDWARE_ERROR;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_positionLock, &oldIrql);
    if (!m_buffer || !m_bufferSize)
    {
      KeReleaseSpinLock(&m_positionLock, oldIrql);
      return STATUS_DEVICE_NOT_READY;
    }

    if (m_state == KSSTATE_RUN)
    {
      KeReleaseSpinLock(&m_positionLock, oldIrql);
      return STATUS_SUCCESS;
    }
    if (m_eosNotificationSent)
    {
      KeReleaseSpinLock(&m_positionLock, oldIrql);
      return STATUS_INVALID_DEVICE_STATE;
    }

    const bool resetCapture =
      m_kind == EndpointKind::Capture && !m_hasRun;
    m_clockFrequency = clock.Frequency;
    m_qpcFrequency = clock.QpcFrequency;
    m_lastClockPosition = clock.Position;
    m_lastQpcPosition = clock.QpcPosition;
    m_lastPresentationQpc = clock.QpcPosition;
    m_state = KSSTATE_RUN;
    m_hasRun = true;
    KeReleaseSpinLock(&m_positionLock, oldIrql);

    if (resetCapture)
      m_adapter->ResetCapture();

    KeAcquireSpinLock(&m_positionLock, &oldIrql);
    if (m_state == KSSTATE_RUN)
      ArmTimerLocked();
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_SUCCESS;
  }

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (m_state == KSSTATE_RUN)
    AdvanceLocked(clock);
  m_state = state;

  if (state == KSSTATE_STOP)
  {
    m_clockFrequency = 0;
    m_qpcFrequency = 0;
    m_lastClockPosition = 0;
    m_lastQpcPosition = 0;
    m_lastPresentationQpc = 0;
    m_frameRemainder = 0;
    m_pendingBytes = 0;
    m_linearPosition = 0;
    m_cyclicPosition = 0;
    m_completedPackets = 0;
    m_lastReadPacket = ~0ULL;
    m_lastWritePacket = MAXULONG;
    m_hasRun = false;
    m_eosReceived = false;
    m_eosNotificationSent = false;
    m_eosLinearPosition = 0;
  }
  KeReleaseSpinLock(&m_positionLock, oldIrql);

  StopTimer();
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::GetPosition(
  _Out_ PKSAUDIO_POSITION position)
{
  if (!position)
    return STATUS_INVALID_PARAMETER;

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  AdvanceLocked(clock);
  ULONG reportedPosition = m_cyclicPosition;
  if (m_notificationCount && m_bufferSize)
  {
    reportedPosition = static_cast<ULONG>(
      (reportedPosition + (m_pendingBytes % m_bufferSize)) %
        m_bufferSize);
  }
  position->PlayOffset = reportedPosition;
  position->WriteOffset = reportedPosition;
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::AllocateAudioBuffer(
  _In_ ULONG requestedSize,
  _Out_ PMDL *audioBufferMdl,
  _Out_ ULONG *actualSize,
  _Out_ ULONG *offsetFromFirstPage,
  _Out_ MEMORY_CACHING_TYPE *cacheType)
{
  return AllocateBuffer(0, requestedSize, audioBufferMdl, actualSize,
    offsetFromFirstPage, cacheType);
}

void CMiniportWaveRTStream::FreeAudioBuffer(
  _In_opt_ PMDL audioBufferMdl,
  _In_ ULONG bufferSize)
{
  UNREFERENCED_PARAMETER(bufferSize);
  FreeBuffer(audioBufferMdl);
}

void CMiniportWaveRTStream::GetHWLatency(
  _Out_ PKSRTAUDIO_HWLATENCY latency)
{
  if (latency)
    RtlZeroMemory(latency, sizeof(*latency));
}

NTSTATUS CMiniportWaveRTStream::GetPositionRegister(
  _Out_ PKSRTAUDIO_HWREGISTER positionRegister)
{
  if (positionRegister)
    RtlZeroMemory(positionRegister, sizeof(*positionRegister));
  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS CMiniportWaveRTStream::GetClockRegister(
  _Out_ PKSRTAUDIO_HWREGISTER clockRegister)
{
  if (clockRegister)
    RtlZeroMemory(clockRegister, sizeof(*clockRegister));
  return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS CMiniportWaveRTStream::AllocateBufferWithNotification(
  _In_ ULONG notificationCount,
  _In_ ULONG requestedSize,
  _Out_ PMDL *audioBufferMdl,
  _Out_ ULONG *actualSize,
  _Out_ ULONG *offsetFromFirstPage,
  _Out_ MEMORY_CACHING_TYPE *cacheType)
{
  if (notificationCount != 1 && notificationCount != 2)
    return STATUS_INVALID_PARAMETER;

  return AllocateBuffer(notificationCount, requestedSize, audioBufferMdl,
    actualSize, offsetFromFirstPage, cacheType);
}

void CMiniportWaveRTStream::FreeBufferWithNotification(
  _In_ PMDL audioBufferMdl,
  _In_ ULONG bufferSize)
{
  UNREFERENCED_PARAMETER(bufferSize);
  FreeBuffer(audioBufferMdl);
}

NTSTATUS CMiniportWaveRTStream::RegisterNotificationEvent(
  _In_ PKEVENT notificationEvent)
{
  if (!notificationEvent)
    return STATUS_INVALID_PARAMETER;

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  for (ULONG i = 0; i < m_notificationEventCount; ++i)
  {
    if (m_notificationEvents[i] == notificationEvent)
    {
      KeReleaseSpinLock(&m_positionLock, oldIrql);
      return STATUS_SUCCESS;
    }
  }

  if (m_notificationEventCount >= ARRAYSIZE(m_notificationEvents))
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  m_notificationEvents[m_notificationEventCount++] = notificationEvent;
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::UnregisterNotificationEvent(
  _In_ PKEVENT notificationEvent)
{
  if (!notificationEvent)
    return STATUS_INVALID_PARAMETER;

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  for (ULONG i = 0; i < m_notificationEventCount; ++i)
  {
    if (m_notificationEvents[i] != notificationEvent)
      continue;

    for (ULONG j = i + 1; j < m_notificationEventCount; ++j)
      m_notificationEvents[j - 1] = m_notificationEvents[j];
    m_notificationEvents[--m_notificationEventCount] = nullptr;
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_SUCCESS;
  }

  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_NOT_FOUND;
}

NTSTATUS CMiniportWaveRTStream::GetReadPacket(
  _Out_ ULONG *packetNumber,
  _Out_ DWORD *flags,
  _Out_ ULONG64 *performanceCounterValue,
  _Out_ BOOL *moreData)
{
  if (m_kind != EndpointKind::Capture || !packetNumber || !flags ||
      !performanceCounterValue || !moreData)
    return STATUS_INVALID_PARAMETER;

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (!m_notificationCount)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_NOT_SUPPORTED;
  }
  if (m_state < KSSTATE_PAUSE)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INVALID_DEVICE_STATE;
  }

  AdvanceLocked(clock);

  if (!m_completedPackets)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_DEVICE_NOT_READY;
  }

  const ULONGLONG availablePacket = m_completedPackets - 1;
  if (availablePacket == m_lastReadPacket)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_DEVICE_NOT_READY;
  }

  m_lastReadPacket = availablePacket;
  *packetNumber = static_cast<ULONG>(availablePacket);
  *flags = 0;

  const ULONGLONG blockAlign =
    m_format.WaveFormatExt.Format.nBlockAlign;
  const ULONGLONG sampleRate =
    m_format.WaveFormatExt.Format.nSamplesPerSec;
  const ULONGLONG packetStartFrame =
    availablePacket * (m_packetSize / blockAlign);
  const ULONGLONG currentFrame = m_linearPosition / blockAlign;
  const ULONGLONG elapsedFrames = currentFrame - packetStartFrame;
  const ULONGLONG elapsedTicks =
    elapsedFrames * m_qpcFrequency / sampleRate;
  *performanceCounterValue = elapsedTicks < m_lastPresentationQpc ?
    m_lastPresentationQpc - elapsedTicks : 0;
  *moreData = FALSE;
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::SetWritePacket(
  _In_ ULONG packetNumber,
  _In_ DWORD flags,
  _In_ ULONG eosPacketLength)
{
  if (m_kind != EndpointKind::Render)
    return STATUS_INVALID_DEVICE_REQUEST;
  constexpr DWORD allowedFlags =
    KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM;
  if (flags & ~allowedFlags)
    return STATUS_INVALID_PARAMETER;

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (!m_notificationCount)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_NOT_SUPPORTED;
  }
  if (m_eosReceived)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INVALID_DEVICE_STATE;
  }

  AdvanceLocked(clock);
  ULONGLONG expectedPacket64 = m_completedPackets;
  if (m_state == KSSTATE_RUN)
    ++expectedPacket64;
  const ULONG expectedPacket = static_cast<ULONG>(expectedPacket64);

  const LONG packetDelta = static_cast<LONG>(packetNumber - expectedPacket);
  if (packetDelta < 0)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_DATA_LATE_ERROR;
  }
  if (packetDelta > 0)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_DATA_OVERRUN;
  }

  if (flags & KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM)
  {
    const ULONG blockAlign =
      m_format.WaveFormatExt.Format.nBlockAlign;
    if (eosPacketLength > m_packetSize ||
        (eosPacketLength % blockAlign) != 0 ||
        expectedPacket64 >
          (MAXULONGLONG - eosPacketLength) / m_packetSize)
    {
      KeReleaseSpinLock(&m_positionLock, oldIrql);
      return STATUS_INVALID_PARAMETER;
    }

    m_eosReceived = true;
    m_eosNotificationSent = false;
    m_eosLinearPosition =
      expectedPacket64 * m_packetSize + eosPacketLength;
  }

  m_lastWritePacket = packetNumber;
  if (m_eosReceived && m_state == KSSTATE_RUN)
    ArmTimerLocked();
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::GetOutputStreamPresentationPosition(
  _Out_ KSAUDIO_PRESENTATION_POSITION *position)
{
  if (m_kind != EndpointKind::Render || !position)
    return STATUS_INVALID_PARAMETER;

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (!m_notificationCount)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_NOT_SUPPORTED;
  }

  AdvanceLocked(clock);
  const ULONGLONG reportedBytes =
    m_linearPosition + m_pendingBytes;
  position->u64PositionInBlocks =
    reportedBytes / m_format.WaveFormatExt.Format.nBlockAlign;

  const ULONG sampleRate =
    m_format.WaveFormatExt.Format.nSamplesPerSec;
  const ULONGLONG ticksSinceReportedFrame =
    sampleRate && clock.Frequency && clock.QpcFrequency ?
      (m_frameRemainder * clock.QpcFrequency / clock.Frequency) /
        sampleRate : 0;
  if (m_pendingBytes || m_state == KSSTATE_RUN)
  {
    position->u64QPCPosition =
      ticksSinceReportedFrame < m_lastQpcPosition ?
        m_lastQpcPosition - ticksSinceReportedFrame : 0;
  }
  else
    position->u64QPCPosition = m_lastPresentationQpc;
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::GetPacketCount(
  _Out_ ULONG *packetCount)
{
  if (m_kind != EndpointKind::Render || !packetCount)
    return STATUS_INVALID_PARAMETER;

  const AudioClockSnapshot clock = m_adapter->GetClockSnapshot();
  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (!m_notificationCount)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_NOT_SUPPORTED;
  }

  AdvanceLocked(clock);
  *packetCount = static_cast<ULONG>(m_completedPackets);
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  return STATUS_SUCCESS;
}

NTSTATUS CMiniportWaveRTStream::AllocateBuffer(
  _In_ ULONG notificationCount,
  _In_ ULONG requestedSize,
  _Out_ PMDL *audioBufferMdl,
  _Out_ ULONG *actualSize,
  _Out_ ULONG *offsetFromFirstPage,
  _Out_ MEMORY_CACHING_TYPE *cacheType)
{
  if (!audioBufferMdl || !actualSize || !offsetFromFirstPage || !cacheType)
    return STATUS_INVALID_PARAMETER;

  *audioBufferMdl = nullptr;
  *actualSize = 0;
  *offsetFromFirstPage = 0;
  *cacheType = MmCached;

  const ULONG blockAlign = m_format.WaveFormatExt.Format.nBlockAlign;
  if (!requestedSize || !blockAlign)
    return STATUS_INVALID_PARAMETER;

  const ULONG alignment = blockAlign *
    (notificationCount ? notificationCount : 1);
  const ULONG maximumSize =
    m_format.WaveFormatExt.Format.nAvgBytesPerSec * 2;
  if (requestedSize > maximumSize ||
      requestedSize > MAXULONG - (alignment - 1))
    return STATUS_INSUFFICIENT_RESOURCES;
  requestedSize =
    (requestedSize + alignment - 1) / alignment * alignment;
  if (!requestedSize || requestedSize > maximumSize)
    return STATUS_INVALID_PARAMETER;

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  const bool alreadyAllocated =
    m_buffer != nullptr || m_bufferAllocationPending;
  if (!alreadyAllocated)
    m_bufferAllocationPending = true;
  KeReleaseSpinLock(&m_positionLock, oldIrql);
  if (alreadyAllocated)
    return STATUS_DEVICE_BUSY;

  PHYSICAL_ADDRESS highAddress{};
  highAddress.QuadPart = MAXULONG;
  PMDL mdl = m_portStream->AllocatePagesForMdl(highAddress, requestedSize);
  if (!mdl)
  {
    KeAcquireSpinLock(&m_positionLock, &oldIrql);
    m_bufferAllocationPending = false;
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  BYTE *buffer = static_cast<BYTE *>(
    m_portStream->MapAllocatedPages(mdl, MmCached));
  if (!buffer)
  {
    m_portStream->FreePagesFromMdl(mdl);
    KeAcquireSpinLock(&m_positionLock, &oldIrql);
    m_bufferAllocationPending = false;
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  RtlZeroMemory(buffer, requestedSize);

  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  m_bufferMdl = mdl;
  m_buffer = buffer;
  m_bufferAllocationPending = false;
  m_bufferSize = requestedSize;
  m_notificationCount = notificationCount;
  m_packetSize = notificationCount ?
    requestedSize / notificationCount : 0;
  KeReleaseSpinLock(&m_positionLock, oldIrql);

  *audioBufferMdl = mdl;
  *actualSize = requestedSize;
  *offsetFromFirstPage = 0;
  *cacheType = MmCached;
  return STATUS_SUCCESS;
}

void CMiniportWaveRTStream::FreeBuffer(_In_opt_ PMDL audioBufferMdl)
{
  if (!audioBufferMdl)
    return;

  SetState(KSSTATE_STOP);
  StopTimer();

  KIRQL oldIrql;
  KeAcquireSpinLock(&m_positionLock, &oldIrql);
  if (audioBufferMdl != m_bufferMdl)
  {
    KeReleaseSpinLock(&m_positionLock, oldIrql);
    return;
  }

  BYTE *buffer = m_buffer;
  PMDL mdl = m_bufferMdl;
  m_buffer = nullptr;
  m_bufferMdl = nullptr;
  m_bufferSize = 0;
  m_notificationCount = 0;
  m_packetSize = 0;
  KeReleaseSpinLock(&m_positionLock, oldIrql);

  if (buffer)
    m_portStream->UnmapAllocatedPages(buffer, mdl);
  m_portStream->FreePagesFromMdl(mdl);
}

void CMiniportWaveRTStream::StopTimer()
{
  if (m_timer)
    ExCancelTimer(m_timer, nullptr);
}

void CMiniportWaveRTStream::ArmTimerLocked()
{
  if (!m_timer || m_state != KSSTATE_RUN || !m_notificationCount ||
      !m_packetSize || !m_clockFrequency)
    return;

  const ULONG blockAlign =
    m_format.WaveFormatExt.Format.nBlockAlign;
  const ULONG sampleRate =
    m_format.WaveFormatExt.Format.nSamplesPerSec;
  if (!blockAlign || !sampleRate)
    return;

  ULONGLONG targetBytes = m_packetSize;
  if (m_kind == EndpointKind::Render && m_eosReceived)
  {
    const ULONGLONG remaining = m_eosLinearPosition > m_linearPosition ?
      m_eosLinearPosition - m_linearPosition : 0;
    if (remaining < targetBytes)
      targetBytes = remaining;
  }

  const ULONGLONG bytesUntilTarget = targetBytes > m_pendingBytes ?
    targetBytes - m_pendingBytes : 0;
  const ULONGLONG framesUntilTarget =
    (bytesUntilTarget + blockAlign - 1) / blockAlign;
  const ULONGLONG frequency = m_clockFrequency;

  const ULONGLONG frameTicks = framesUntilTarget * frequency;
  const ULONGLONG ticksNumerator = frameTicks > m_frameRemainder ?
    frameTicks - m_frameRemainder : 0;
  ULONGLONG qpcTicks =
    (ticksNumerator + sampleRate - 1) / sampleRate;
  if (!qpcTicks)
    qpcTicks = 1;

  constexpr ULONGLONG kHundredNanosecondsPerSecond = 10000000;
  ULONGLONG dueTime =
    (qpcTicks * kHundredNanosecondsPerSecond + frequency - 1) /
      frequency;
  if (!dueTime)
    dueTime = 1;

  ExSetTimer(m_timer, -static_cast<LONGLONG>(dueTime), 0, nullptr);
}

void CMiniportWaveRTStream::AdvanceLocked(
  _In_ const AudioClockSnapshot &clock)
{
  if (m_state != KSSTATE_RUN || !m_buffer || !m_bufferSize ||
      !clock.Frequency || !clock.QpcFrequency)
    return;

  const bool clockDiscontinuity =
    !m_clockFrequency || clock.Frequency != m_clockFrequency ||
    clock.Position < m_lastClockPosition;
  if (clockDiscontinuity)
  {
    m_clockFrequency = clock.Frequency;
    m_qpcFrequency = clock.QpcFrequency;
    m_lastClockPosition = clock.Position;
    m_lastQpcPosition = clock.QpcPosition;
    m_lastPresentationQpc = clock.QpcPosition;
    m_frameRemainder = 0;
    return;
  }

  if (clock.Position == m_lastClockPosition)
  {
    m_qpcFrequency = clock.QpcFrequency;
    m_lastQpcPosition = clock.QpcPosition;
    return;
  }

  const ULONGLONG elapsedTicks =
    clock.Position - m_lastClockPosition;
  const ULONGLONG numerator = elapsedTicks *
    m_format.WaveFormatExt.Format.nSamplesPerSec + m_frameRemainder;
  const ULONGLONG frames = numerator / clock.Frequency;
  m_frameRemainder = numerator % clock.Frequency;
  m_clockFrequency = clock.Frequency;
  m_qpcFrequency = clock.QpcFrequency;
  m_lastClockPosition = clock.Position;
  m_lastQpcPosition = clock.QpcPosition;

  if (frames)
  {
    const ULONGLONG bytes = frames *
      m_format.WaveFormatExt.Format.nBlockAlign;
    if (m_notificationCount)
      m_pendingBytes += bytes;
    else
    {
      TransferFramesLocked(bytes);
      m_lastPresentationQpc = clock.QpcPosition;
    }
  }

  if (m_notificationCount)
    AdvanceEventPacketsLocked(clock);
}

void CMiniportWaveRTStream::AdvanceEventPacketsLocked(
  _In_ const AudioClockSnapshot &clock)
{
  if (!m_packetSize)
    return;

  // A WaveRT circular buffer cannot retain more than NotificationCount
  // packets. Bound catch-up after a long scheduling or power gap so the DPC
  // does not loop over audio that the client has already overwritten.
  if (!m_eosReceived)
  {
    const ULONGLONG readyPackets = m_pendingBytes / m_packetSize;
    if (readyPackets > m_notificationCount)
    {
      const ULONGLONG skippedPackets =
        readyPackets - m_notificationCount;
      const ULONGLONG skippedBytes = skippedPackets * m_packetSize;

      if (m_kind == EndpointKind::Capture)
        m_adapter->DiscardCaptureFrames(m_fingerprint, skippedBytes);

      m_pendingBytes -= skippedBytes;
      m_linearPosition += skippedBytes;
      m_cyclicPosition = static_cast<ULONG>(
        (m_cyclicPosition + (skippedBytes % m_bufferSize)) %
          m_bufferSize);
      m_completedPackets = m_linearPosition / m_packetSize;
    }
  }

  while (m_state == KSSTATE_RUN)
  {
    ULONGLONG transferBytes = m_packetSize;
    bool completesEos = false;

    if (m_kind == EndpointKind::Render && m_eosReceived)
    {
      const ULONGLONG remaining =
        m_eosLinearPosition > m_linearPosition ?
          m_eosLinearPosition - m_linearPosition : 0;
      if (remaining <= transferBytes)
      {
        transferBytes = remaining;
        completesEos = true;
      }
    }

    if (m_pendingBytes < transferBytes)
      break;

    const ULONGLONG oldPacketCount = m_completedPackets;
    if (transferBytes)
    {
      TransferFramesLocked(transferBytes);
      m_pendingBytes -= transferBytes;
    }

    UpdatePresentationQpcLocked(clock);
    m_completedPackets = m_linearPosition / m_packetSize;

    if (m_completedPackets != oldPacketCount ||
        (completesEos && !m_eosNotificationSent))
      SignalNotificationsLocked();

    if (completesEos)
    {
      m_eosNotificationSent = true;
      m_state = KSSTATE_PAUSE;
      m_pendingBytes = 0;
      m_frameRemainder = 0;
      break;
    }
  }
}

void CMiniportWaveRTStream::UpdatePresentationQpcLocked(
  _In_ const AudioClockSnapshot &clock)
{
  const ULONG blockAlign =
    m_format.WaveFormatExt.Format.nBlockAlign;
  const ULONG sampleRate =
    m_format.WaveFormatExt.Format.nSamplesPerSec;
  if (!clock.Frequency || !clock.QpcFrequency || !blockAlign ||
      !sampleRate)
  {
    m_lastPresentationQpc = clock.QpcPosition;
    return;
  }

  const ULONGLONG pendingFrames = m_pendingBytes / blockAlign;
  const ULONGLONG wholeQpcTicks =
    pendingFrames * clock.QpcFrequency / sampleRate;
  const ULONGLONG fractionalQpcTicks =
    (m_frameRemainder * clock.QpcFrequency / clock.Frequency) /
      sampleRate;
  const ULONGLONG ticksSinceBoundary =
    wholeQpcTicks + fractionalQpcTicks;
  m_lastPresentationQpc = ticksSinceBoundary < clock.QpcPosition ?
    clock.QpcPosition - ticksSinceBoundary : 0;
}

void CMiniportWaveRTStream::TransferFramesLocked(_In_ ULONGLONG bytes)
{
  if (!bytes || !m_bufferSize)
    return;

  if (m_kind == EndpointKind::Render && m_eosReceived)
  {
    if (m_linearPosition >= m_eosLinearPosition)
    {
      m_state = KSSTATE_PAUSE;
      return;
    }

    const ULONGLONG remaining =
      m_eosLinearPosition - m_linearPosition;
    if (bytes > remaining)
      bytes = remaining;
  }

  const ULONG finalPosition = static_cast<ULONG>(
    (m_cyclicPosition + (bytes % m_bufferSize)) % m_bufferSize);
  ULONG transferBytes = static_cast<ULONG>(MinUlonglong(bytes,
    m_bufferSize));
  ULONG offset = bytes > m_bufferSize ? finalPosition : m_cyclicPosition;
  const bool renderDataCommitted =
    !m_notificationCount ||
    static_cast<LONG>(m_lastWritePacket -
      static_cast<ULONG>(m_completedPackets)) >= 0;

  if (m_kind == EndpointKind::Capture && bytes > transferBytes)
    m_adapter->DiscardCaptureFrames(m_fingerprint,
      bytes - transferBytes);

  while (transferBytes)
  {
    const ULONG untilWrap = m_bufferSize - offset;
    const ULONG chunk = transferBytes < untilWrap ?
      transferBytes : untilWrap;

    if (m_kind == EndpointKind::Render)
    {
      // SetWritePacket is issued only after the client finishes writing the
      // slot. On an underrun, neither read nor modify a slot that the client
      // may still be updating. The capture side zero-fills the resulting
      // absence when the render-to-capture FIFO is exhausted.
      if (renderDataCommitted)
        m_adapter->WriteRenderFrames(m_buffer + offset, chunk);
    }
    else
    {
      m_adapter->ReadCaptureFrames(m_fingerprint,
        m_buffer + offset, chunk);
    }

    transferBytes -= chunk;
    offset = (offset + chunk) % m_bufferSize;
  }

  m_cyclicPosition = finalPosition;
  m_linearPosition += bytes;

  if (m_kind == EndpointKind::Render && m_eosReceived &&
      m_linearPosition >= m_eosLinearPosition)
    m_state = KSSTATE_PAUSE;
}

void CMiniportWaveRTStream::SignalNotificationsLocked()
{
  for (ULONG i = 0; i < m_notificationEventCount; ++i)
    KeSetEvent(m_notificationEvents[i], IO_NO_INCREMENT, FALSE);
}

void CMiniportWaveRTStream::TimerCallback(
  _In_ PEX_TIMER timer,
  _In_opt_ void *context)
{
  UNREFERENCED_PARAMETER(timer);

  auto *stream = static_cast<CMiniportWaveRTStream *>(context);
  if (!stream)
    return;

  const AudioClockSnapshot clock =
    stream->m_adapter->GetClockSnapshot();
  KeAcquireSpinLockAtDpcLevel(&stream->m_positionLock);
  stream->AdvanceLocked(clock);
  if (stream->m_state == KSSTATE_RUN)
    stream->ArmTimerLocked();
  KeReleaseSpinLockFromDpcLevel(&stream->m_positionLock);
}
