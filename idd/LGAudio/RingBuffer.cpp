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

#include "RingBuffer.h"

namespace LGAudio
{
  namespace
  {
    constexpr ULONG kRingPoolTag = 'rAGL';

    ULONG MinUlong(_In_ ULONG left, _In_ ULONG right)
    {
      return left < right ? left : right;
    }
  }

  CAudioRingBuffer::CAudioRingBuffer() :
    m_buffer(nullptr),
    m_capacity(0),
    m_readOffset(0),
    m_writeOffset(0),
    m_available(0),
    m_format{},
    m_configured(false)
  {
    KeInitializeSpinLock(&m_lock);
  }

  CAudioRingBuffer::~CAudioRingBuffer()
  {
    Destroy();
  }

  _IRQL_requires_max_(PASSIVE_LEVEL)
  NTSTATUS CAudioRingBuffer::Init()
  {
    if (m_buffer)
      return STATUS_SUCCESS;

    constexpr ULONG capacity =
      (kMaximumBytesPerSecond + kMaximumBlockAlign - 1) /
      kMaximumBlockAlign * kMaximumBlockAlign;

#pragma warning(push)
#pragma warning(disable:4996) // Required for pre-2004 Windows 10 releases.
    BYTE * buffer = static_cast<BYTE *>(ExAllocatePoolWithTag(
      NonPagedPoolNx, capacity, kRingPoolTag));
#pragma warning(pop)
    if (!buffer)
      return STATUS_INSUFFICIENT_RESOURCES;

    RtlZeroMemory(buffer, capacity);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (!m_buffer)
    {
      m_buffer = buffer;
      m_capacity = capacity;
      buffer = nullptr;
    }
    KeReleaseSpinLock(&m_lock, oldIrql);

    if (buffer)
      ExFreePoolWithTag(buffer, kRingPoolTag);

    return STATUS_SUCCESS;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void CAudioRingBuffer::Destroy()
  {
    BYTE * buffer;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    buffer = m_buffer;
    m_buffer = nullptr;
    m_capacity = 0;
    m_readOffset = 0;
    m_writeOffset = 0;
    m_available = 0;
    m_format = {};
    m_configured = false;
    KeReleaseSpinLock(&m_lock, oldIrql);

    if (buffer)
      ExFreePoolWithTag(buffer, kRingPoolTag);
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  NTSTATUS CAudioRingBuffer::Configure(
    _In_ const FormatFingerprint & format)
  {
    if (format.sampleRate == 0 ||
        format.channels == 0 ||
        format.containerBits == 0 ||
        (format.containerBits % 8) != 0 ||
        format.validBits == 0 ||
        format.validBits > format.containerBits ||
        format.blockAlign !=
          format.channels * (format.containerBits / 8))
      return STATUS_INVALID_PARAMETER;

    const ULONGLONG bytesPerSecond =
      static_cast<ULONGLONG>(format.sampleRate) * format.blockAlign;
    if (bytesPerSecond > kMaximumBytesPerSecond)
      return STATUS_NOT_SUPPORTED;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (!m_buffer || (m_capacity % format.blockAlign) != 0)
    {
      KeReleaseSpinLock(&m_lock, oldIrql);
      return STATUS_DEVICE_NOT_READY;
    }

    m_format = format;
    m_readOffset = 0;
    m_writeOffset = 0;
    m_available = 0;
    m_configured = true;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return STATUS_SUCCESS;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void CAudioRingBuffer::Reset()
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    m_readOffset = 0;
    m_writeOffset = 0;
    m_available = 0;
    KeReleaseSpinLock(&m_lock, oldIrql);
  }

  bool CAudioRingBuffer::IsTransferValid(_In_ ULONG bytes) const
  {
    return
      m_buffer && m_configured && m_format.blockAlign != 0 &&
      (bytes % m_format.blockAlign) == 0;
  }

  void CAudioRingBuffer::CopyIntoRing(
    _In_reads_bytes_(bytes) const BYTE * data,
    _In_ ULONG bytes)
  {
    const ULONG first = MinUlong(bytes, m_capacity - m_writeOffset);
    RtlCopyMemory(m_buffer + m_writeOffset, data, first);

    const ULONG second = bytes - first;
    if (second)
      RtlCopyMemory(m_buffer, data + first, second);

    m_writeOffset = (m_writeOffset + bytes) % m_capacity;
  }

  void CAudioRingBuffer::CopyFromRing(
    _Out_writes_bytes_(bytes) BYTE * data,
    _In_ ULONG bytes)
  {
    const ULONG first = MinUlong(bytes, m_capacity - m_readOffset);
    RtlCopyMemory(data, m_buffer + m_readOffset, first);

    const ULONG second = bytes - first;
    if (second)
      RtlCopyMemory(data + first, m_buffer, second);

    m_readOffset = (m_readOffset + bytes) % m_capacity;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  ULONG CAudioRingBuffer::Push(
    _In_reads_bytes_(bytes) const BYTE * data,
    _In_ ULONG bytes,
    _Out_opt_ PULONG droppedBytes)
  {
    const ULONG inputBytes = bytes;

    if (droppedBytes)
      *droppedBytes = 0;
    if (bytes == 0)
      return 0;
    if (!data)
      return 0;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (!IsTransferValid(bytes))
    {
      KeReleaseSpinLock(&m_lock, oldIrql);
      return 0;
    }

    ULONG dropped = 0;
    if (bytes >= m_capacity)
    {
      dropped = m_available + (bytes - m_capacity);
      data += bytes - m_capacity;
      bytes = m_capacity;
      m_readOffset = 0;
      m_writeOffset = 0;
      m_available = 0;
    }
    else if (bytes > m_capacity - m_available)
    {
      dropped = bytes - (m_capacity - m_available);
      m_readOffset = (m_readOffset + dropped) % m_capacity;
      m_available -= dropped;
    }

    CopyIntoRing(data, bytes);
    m_available += bytes;
    KeReleaseSpinLock(&m_lock, oldIrql);

    if (droppedBytes)
      *droppedBytes = dropped;

    // Every source byte was consumed. If input exceeded capacity, the oldest
    // part of that input is included in the reported dropped byte count.
    return inputBytes;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  ULONG CAudioRingBuffer::Pop(
    _In_ const FormatFingerprint &format,
    _Out_writes_bytes_(bytes) BYTE *data,
    _In_ ULONG bytes)
  {
    if (bytes == 0)
      return 0;
    if (!data)
      return 0;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (!IsTransferValid(bytes) ||
        !EqualFormatFingerprint(m_format, format))
    {
      KeReleaseSpinLock(&m_lock, oldIrql);
      RtlZeroMemory(data, bytes);
      return 0;
    }

    const ULONG copied = MinUlong(bytes, m_available);
    if (copied)
    {
      CopyFromRing(data, copied);
      m_available -= copied;
    }
    KeReleaseSpinLock(&m_lock, oldIrql);

    if (copied < bytes)
      RtlZeroMemory(data + copied, bytes - copied);
    return copied;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  ULONG CAudioRingBuffer::Discard(
    _In_ const FormatFingerprint &format,
    _In_ ULONGLONG bytes)
  {
    if (!bytes)
      return 0;

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    if (!m_buffer || !m_configured || !m_format.blockAlign ||
        (bytes % m_format.blockAlign) != 0 ||
        !EqualFormatFingerprint(m_format, format))
    {
      KeReleaseSpinLock(&m_lock, oldIrql);
      return 0;
    }

    const ULONG discarded = bytes < m_available ?
      static_cast<ULONG>(bytes) : m_available;
    m_readOffset = (m_readOffset + discarded) % m_capacity;
    m_available -= discarded;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return discarded;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  ULONG CAudioRingBuffer::Available()
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    const ULONG available = m_available;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return available;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  ULONG CAudioRingBuffer::Capacity()
  {
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    const ULONG capacity = m_capacity;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return capacity;
  }

}
