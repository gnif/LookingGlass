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

#include "AudioClock.h"

namespace LGAudio
{
  CAudioClock::CAudioClock() :
    m_frequency(0)
  {
  }

  _IRQL_requires_max_(PASSIVE_LEVEL)
  NTSTATUS CAudioClock::Initialize()
  {
    LARGE_INTEGER frequency{};
    KeQueryPerformanceCounter(&frequency);
    if (frequency.QuadPart <= 0)
      return STATUS_DEVICE_HARDWARE_ERROR;

    m_frequency = static_cast<ULONGLONG>(frequency.QuadPart);
    return STATUS_SUCCESS;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  AudioClockSnapshot CAudioClock::GetSnapshot() const
  {
    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG position = static_cast<ULONGLONG>(qpc.QuadPart);

    // The fallback domains are identical, making the correlation exact.
    return
    {
      position,
      m_frequency,
      position,
      m_frequency
    };
  }
}
