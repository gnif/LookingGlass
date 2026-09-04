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

#include "Common.h"

namespace LGAudio
{
  /**
   * A correlated sample of the audio time domain and the Windows QPC time
   * domain. Position drives audio progress; QpcPosition timestamps that
   * position for WaveRT clients.
   */
  struct AudioClockSnapshot
  {
    ULONGLONG Position;
    ULONGLONG Frequency;
    ULONGLONG QpcPosition;
    ULONGLONG QpcFrequency;
    // Audio-position ticks advanced per QPC tick in unsigned Q32.32.
    ULONGLONG RateQ32;
  };

  constexpr ULONGLONG kAudioClockRateOneQ32 = 1ULL << 32;
  constexpr ULONG kAudioClockMaximumCorrectionPpm = 5000;
  constexpr ULONG kAudioClockMaximumSlewPpmPerSecond = 5000;

  /**
   * Adapter-owned affine clock. Position is continuous and monotonic while
   * RateQ32 may be updated to discipline a render endpoint to a remote sink.
   * QpcPosition always remains in the local Windows QPC domain.
   */
  class CAudioClock final
  {
  public:
    CAudioClock();

    CAudioClock(const CAudioClock &) = delete;
    CAudioClock & operator=(const CAudioClock &) = delete;

    _IRQL_requires_max_(PASSIVE_LEVEL)
    NTSTATUS Initialize();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    AudioClockSnapshot GetSnapshot();

    _IRQL_requires_max_(DISPATCH_LEVEL)
    bool SetRate(_In_ ULONGLONG rateQ32);

    _IRQL_requires_max_(DISPATCH_LEVEL)
    void ResetRate();

  private:
    _Requires_lock_held_(m_lock)
    void AdvanceLocked(_In_ ULONGLONG qpcPosition);

    _Requires_lock_held_(m_lock)
    void UpdateLocked(_In_ ULONGLONG qpcPosition);

    KSPIN_LOCK m_lock;
    ULONGLONG m_frequency;
    ULONGLONG m_position;
    ULONGLONG m_positionFractionQ32;
    ULONGLONG m_lastQpcPosition;
    ULONGLONG m_rateQ32;
    ULONGLONG m_lastRateUpdateQpc;
    ULONGLONG m_feedbackDeadlineQpc;
  };
}

using LGAudio::AudioClockSnapshot;
using LGAudio::CAudioClock;
using LGAudio::kAudioClockMaximumCorrectionPpm;
using LGAudio::kAudioClockMaximumSlewPpmPerSecond;
using LGAudio::kAudioClockRateOneQ32;
