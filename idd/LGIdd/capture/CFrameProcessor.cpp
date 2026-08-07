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

#include "capture/CFrameProcessor.h"
#include "capture/CFrameProcessorUtil.h"
#include "capture/CHardwareFrameProcessor.h"
#include "capture/CSoftwareFrameProcessor.h"

#include <cstring>
#include <new>
#include <utility>

CFrameProcessor::CFrameProcessor(IFrameTransport * transport,
    std::shared_ptr<CD3D12Device> dx12,
    CPostProcessor postProcessors[TRANSPORT_FRAME_QUEUE_LENGTH],
    SRWLOCK * pipelineLock, HANDLE terminateEvent) :
  m_transport(transport),
  m_dx12(std::move(dx12)),
  m_postProcessors(postProcessors),
  m_pipelineLock(pipelineLock),
  m_terminateEvent(terminateEvent)
{
  m_readyEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
  m_frameBuffers.Init(m_transport, m_dx12.get());
}

bool CFrameProcessor::IsValid() const
{
  return m_readyEvent.Get() != nullptr;
}

void CFrameProcessor::Reset()
{
  m_frameBuffers.Reset();
}

void CFrameProcessor::Invalidate()
{
  AcquireSRWLockExclusive(&m_damageLock);
  m_previousDamageCount = 0;
  m_hasPendingDamage    = true;
  m_pendingDamageCount  = 0;
  SetFullDamageLocked();
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CFrameProcessor::ResetPipeline()
{
  Invalidate();
}

void CFrameProcessor::AccumulateDamage(
  const RECT dirtyRects[], unsigned count)
{
  AcquireSRWLockExclusive(&m_damageLock);
  CFrameProcessorUtil::AccumulateDamage(
    m_pendingDamage, &m_pendingDamageCount, &m_hasPendingDamage,
    dirtyRects, count);
  AccumulateDamageLocked(dirtyRects, count);
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CFrameProcessor::SetFullDamage()
{
  AcquireSRWLockExclusive(&m_damageLock);
  m_hasPendingDamage   = true;
  m_pendingDamageCount = 0;
  SetFullDamageLocked();
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CFrameProcessor::AccumulateDamageLocked(
  const RECT[], unsigned)
{
}

void CFrameProcessor::SetFullDamageLocked()
{
}

bool CFrameProcessor::HasPendingDamage() const
{
  AcquireSRWLockShared(&m_damageLock);
  const bool result = m_hasPendingDamage;
  ReleaseSRWLockShared(&m_damageLock);
  return result;
}

bool CFrameProcessor::TakePendingDamage(
  RECT dirtyRects[], unsigned * count)
{
  AcquireSRWLockExclusive(&m_damageLock);
  const bool hasDamage = m_hasPendingDamage;
  *count = hasDamage ? m_pendingDamageCount : 0;
  if (*count)
    memcpy(dirtyRects, m_pendingDamage,
      *count * sizeof(*dirtyRects));
  m_hasPendingDamage   = false;
  m_pendingDamageCount = 0;
  ReleaseSRWLockExclusive(&m_damageLock);
  return hasDamage;
}

void CFrameProcessor::RestorePendingDamage(
  const RECT dirtyRects[], unsigned count, bool hasDamage)
{
  if (!hasDamage)
    return;

  AcquireSRWLockExclusive(&m_damageLock);
  CFrameProcessorUtil::AccumulateDamage(
    m_pendingDamage, &m_pendingDamageCount, &m_hasPendingDamage,
    dirtyRects, count);
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CFrameProcessor::CommitDamage(
  const RECT dirtyRects[], unsigned count)
{
  AcquireSRWLockExclusive(&m_damageLock);
  m_previousDamageCount = count;
  if (count)
    memcpy(m_previousDamage, dirtyRects,
      count * sizeof(*m_previousDamage));
  ReleaseSRWLockExclusive(&m_damageLock);
}

void CFrameProcessor::GetPreviousDamage(
  RECT dirtyRects[], unsigned * count) const
{
  AcquireSRWLockShared(&m_damageLock);
  *count = m_previousDamageCount;
  if (*count)
    memcpy(dirtyRects, m_previousDamage,
      *count * sizeof(*dirtyRects));
  ReleaseSRWLockShared(&m_damageLock);
}

std::unique_ptr<CFrameProcessor> CreateFrameProcessor(
  bool software, IFrameTransport * transport,
  std::shared_ptr<CD3D12Device> dx12,
  CPostProcessor postProcessors[TRANSPORT_FRAME_QUEUE_LENGTH],
  SRWLOCK * pipelineLock, HANDLE terminateEvent)
{
  std::unique_ptr<CFrameProcessor> processor;
  if (software)
    processor.reset(new (std::nothrow) CSoftwareFrameProcessor(
      transport, std::move(dx12), postProcessors,
      pipelineLock, terminateEvent));
  else
    processor.reset(new (std::nothrow) CHardwareFrameProcessor(
      transport, std::move(dx12), postProcessors,
      pipelineLock, terminateEvent));

  if (!processor || !processor->IsValid())
    return nullptr;
  return processor;
}
