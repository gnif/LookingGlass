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
  };

  /**
   * Adapter-owned clock shared by all render and capture streams.
   *
   * The local fallback uses QPC for both domains. A transport-disciplined
   * implementation can later synthesize a continuous, monotonic Position
   * while preserving its correlation with the local QPC fields. Before a
   * non-unity clock rate is enabled, the stream deadline and timestamp
   * conversions must consume an explicit fixed-point affine mapping between
   * the two domains.
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
    AudioClockSnapshot GetSnapshot() const;

  private:
    ULONGLONG m_frequency;
  };
}

using LGAudio::AudioClockSnapshot;
using LGAudio::CAudioClock;
