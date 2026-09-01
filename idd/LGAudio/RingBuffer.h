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

#include "Formats.h"

namespace LGAudio
{
  /**
   * Adapter-owned byte ring used to bridge a render stream to a capture
   * stream. The owner must stop its streams before destroying the ring.
   */
  class CAudioRingBuffer final
  {
  public:
    CAudioRingBuffer();
    ~CAudioRingBuffer();

    CAudioRingBuffer(const CAudioRingBuffer &) = delete;
    CAudioRingBuffer & operator=(const CAudioRingBuffer &) = delete;

    _IRQL_requires_max_(PASSIVE_LEVEL)
    NTSTATUS Init();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void Destroy();

    /** Configure a new byte layout and discard any queued audio. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    NTSTATUS Configure(_In_ const FormatFingerprint & format);

    /** Discard queued audio without changing the configured format. */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    void Reset();

    /**
     * Consume all input bytes. If necessary, the oldest queued bytes are
     * discarded. The return value is bytes consumed from data.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Push(
      _In_reads_bytes_(bytes) const BYTE * data,
      _In_ ULONG bytes,
      _Out_opt_ PULONG droppedBytes = nullptr);

    /**
     * Pop into data and zero-fill an underrun. The return value is the number
     * of bytes obtained from the ring, excluding zero-fill.
     */
    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Pop(
      _In_ const FormatFingerprint &format,
      _Out_writes_bytes_(bytes) BYTE *data,
      _In_ ULONG bytes);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Discard(
      _In_ const FormatFingerprint &format,
      _In_ ULONGLONG bytes);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Available();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    ULONG Capacity();

  private:
    bool IsTransferValid(_In_ ULONG bytes) const;
    void CopyIntoRing(_In_reads_bytes_(bytes) const BYTE * data,
      _In_ ULONG bytes);
    void CopyFromRing(_Out_writes_bytes_(bytes) BYTE * data,
      _In_ ULONG bytes);

    KSPIN_LOCK m_lock;
    BYTE * m_buffer;
    ULONG m_capacity;
    ULONG m_readOffset;
    ULONG m_writeOffset;
    ULONG m_available;
    FormatFingerprint m_format;
    bool m_configured;
  };
}

using CAudioRingBuffer = LGAudio::CAudioRingBuffer;
