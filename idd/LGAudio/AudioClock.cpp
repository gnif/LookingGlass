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

#include <intrin.h>

namespace
{
  // Revert before the client activates local rate control after 100 ms.
  constexpr ULONGLONG kFeedbackTimeoutNumerator = 3;
  constexpr ULONGLONG kFeedbackTimeoutDivisor = 40;
  constexpr ULONGLONG kFractionMask = (1ULL << 32) - 1;

  bool Divide128(
    ULONGLONG high,
    ULONGLONG low,
    ULONGLONG divisor,
    ULONGLONG *quotient)
  {
    if (!divisor || !quotient || high >= divisor)
      return false;
    if (!high)
    {
      *quotient = low / divisor;
      return true;
    }

    ULONGLONG result = 0;
    ULONGLONG remainder = high;
    for (ULONG bit = 64; bit; --bit)
    {
      const ULONG index = bit - 1;
      const bool carry = (remainder & (1ULL << 63)) != 0;
      remainder = (remainder << 1) | ((low >> index) & 1);
      if (carry || remainder >= divisor)
      {
        remainder -= divisor;
        result |= 1ULL << index;
      }
    }

    *quotient = result;
    return true;
  }
}

namespace LGAudio
{
  CAudioClock::CAudioClock() :
    m_frequency(0),
    m_position(0),
    m_positionFractionQ32(0),
    m_lastQpcPosition(0),
    m_rateQ32(kAudioClockRateOneQ32),
    m_lastRateUpdateQpc(0),
    m_feedbackDeadlineQpc(0)
  {
    KeInitializeSpinLock(&m_lock);
  }

  _IRQL_requires_max_(PASSIVE_LEVEL)
  NTSTATUS CAudioClock::Initialize()
  {
    LARGE_INTEGER frequency{};
    KeQueryPerformanceCounter(&frequency);
    if (frequency.QuadPart <= 0)
      return STATUS_DEVICE_HARDWARE_ERROR;

    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    m_frequency = static_cast<ULONGLONG>(frequency.QuadPart);
    m_position = static_cast<ULONGLONG>(qpc.QuadPart);
    m_lastQpcPosition = m_position;
    m_lastRateUpdateQpc = m_position;
    return STATUS_SUCCESS;
  }

  _Requires_lock_held_(m_lock)
  void CAudioClock::AdvanceLocked(_In_ ULONGLONG qpcPosition)
  {
    if (qpcPosition <= m_lastQpcPosition)
      return;

    const ULONGLONG elapsed = qpcPosition - m_lastQpcPosition;
    ULONGLONG productHigh = 0;
    ULONGLONG productLow = _umul128(elapsed, m_rateQ32, &productHigh);
    const ULONGLONG oldLow = productLow;
    productLow += m_positionFractionQ32;
    if (productLow < oldLow)
      ++productHigh;

    const ULONGLONG positionDelta =
      (productHigh << 32) | (productLow >> 32);
    m_position += positionDelta;
    m_positionFractionQ32 = productLow & kFractionMask;
    m_lastQpcPosition = qpcPosition;
  }

  _Requires_lock_held_(m_lock)
  void CAudioClock::UpdateLocked(_In_ ULONGLONG qpcPosition)
  {
    if (m_feedbackDeadlineQpc &&
        m_feedbackDeadlineQpc > m_lastQpcPosition &&
        m_feedbackDeadlineQpc <= qpcPosition)
    {
      AdvanceLocked(m_feedbackDeadlineQpc);
      m_rateQ32 = kAudioClockRateOneQ32;
      m_lastRateUpdateQpc = m_feedbackDeadlineQpc;
      m_feedbackDeadlineQpc = 0;
    }
    else if (m_feedbackDeadlineQpc &&
        m_feedbackDeadlineQpc <= m_lastQpcPosition)
    {
      m_rateQ32 = kAudioClockRateOneQ32;
      m_lastRateUpdateQpc = m_lastQpcPosition;
      m_feedbackDeadlineQpc = 0;
    }

    AdvanceLocked(qpcPosition);
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  AudioClockSnapshot CAudioClock::GetSnapshot()
  {
    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG qpcPosition = static_cast<ULONGLONG>(qpc.QuadPart);

    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    UpdateLocked(qpcPosition);
    const AudioClockSnapshot result =
    {
      m_position,
      m_frequency,
      qpcPosition,
      m_frequency,
      m_rateQ32
    };
    KeReleaseSpinLock(&m_lock, oldIrql);
    return result;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  bool CAudioClock::SetRate(_In_ ULONGLONG rateQ32)
  {
    const ULONGLONG maximumDelta =
      kAudioClockRateOneQ32 * kAudioClockMaximumCorrectionPpm / 1000000;
    if (rateQ32 < kAudioClockRateOneQ32 - maximumDelta ||
        rateQ32 > kAudioClockRateOneQ32 + maximumDelta)
      return false;

    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG qpcPosition = static_cast<ULONGLONG>(qpc.QuadPart);
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    UpdateLocked(qpcPosition);
    if (!m_feedbackDeadlineQpc)
      m_lastRateUpdateQpc = qpcPosition;

    const ULONGLONG elapsed = qpcPosition > m_lastRateUpdateQpc ?
      qpcPosition - m_lastRateUpdateQpc : 0;
    const ULONGLONG slewQ32PerSecond =
      kAudioClockRateOneQ32 * kAudioClockMaximumSlewPpmPerSecond /
        1000000;
    ULONGLONG productHigh = 0;
    const ULONGLONG productLow =
      _umul128(elapsed, slewQ32PerSecond, &productHigh);
    ULONGLONG maximumStep = MAXULONGLONG;
    if (m_frequency && productHigh < m_frequency)
      Divide128(productHigh, productLow, m_frequency, &maximumStep);

    const ULONGLONG minimumRate = m_rateQ32 > maximumStep ?
      m_rateQ32 - maximumStep : 0;
    const ULONGLONG maximumRate =
      m_rateQ32 <= MAXULONGLONG - maximumStep ?
        m_rateQ32 + maximumStep : MAXULONGLONG;
    if (rateQ32 < minimumRate)
      m_rateQ32 = minimumRate;
    else if (rateQ32 > maximumRate)
      m_rateQ32 = maximumRate;
    else
      m_rateQ32 = rateQ32;
    m_lastRateUpdateQpc = qpcPosition;
    const ULONGLONG timeoutTicks =
      m_frequency * kFeedbackTimeoutNumerator /
        kFeedbackTimeoutDivisor;
    m_feedbackDeadlineQpc = qpcPosition <= MAXULONGLONG - timeoutTicks ?
      qpcPosition + timeoutTicks : MAXULONGLONG;
    KeReleaseSpinLock(&m_lock, oldIrql);
    return true;
  }

  _IRQL_requires_max_(DISPATCH_LEVEL)
  void CAudioClock::ResetRate()
  {
    const LARGE_INTEGER qpc = KeQueryPerformanceCounter(nullptr);
    const ULONGLONG qpcPosition = static_cast<ULONGLONG>(qpc.QuadPart);
    KIRQL oldIrql;
    KeAcquireSpinLock(&m_lock, &oldIrql);
    UpdateLocked(qpcPosition);
    m_rateQ32 = kAudioClockRateOneQ32;
    m_lastRateUpdateQpc = qpcPosition;
    m_feedbackDeadlineQpc = 0;
    KeReleaseSpinLock(&m_lock, oldIrql);
  }
}
