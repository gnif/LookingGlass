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

#include "capture/CSwapChainProcessor.h"

#include "transport/CFrameTransport.h"

#include <avrt.h>
#include "CDebug.h"

static const uint64_t PUBLISH_RETRY_NS = 1000000ULL;

static bool ArmPublishTimer(HANDLE timer, uint64_t delay)
{
  if (!timer)
    return false;

  LARGE_INTEGER due = {};
  due.QuadPart = -static_cast<LONGLONG>((delay + 99) / 100);
  if (!due.QuadPart)
    due.QuadPart = -1;
  return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
}

DWORD CALLBACK CSwapChainProcessor::_PublisherThread(LPVOID arg)
{
  reinterpret_cast<CSwapChainProcessor *>(arg)->PublisherThread();
  return 0;
}

void CSwapChainProcessor::PublisherThread()
{
  DWORD  avTask       = 0;
  HANDLE avTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
  if (avTaskHandle &&
      !AvSetMmThreadPriority(avTaskHandle, AVRT_PRIORITY_HIGH))
    DEBUG_WARN("Failed to raise publisher MMCSS priority: %lu",
      GetLastError());

  const HANDLE scheduleEvent = m_transport.GetFrameScheduleEvent();
  HANDLE idleHandles[] =
  {
    m_terminateEvent.Get(),
    m_frameProcessor->GetReadyEvent(),
    scheduleEvent,
  };
  HANDLE timerHandles[] =
  {
    m_terminateEvent.Get(),
    m_frameProcessor->GetReadyEvent(),
    scheduleEvent,
    m_publishTimer.Get(),
  };
  const bool cadenceEnabled = m_frameProcessor->UsesCadence();

  for (;;)
  {
    const uint64_t            now = CFrameScheduler::Nanotime();
    uint64_t                  target;
    CFrameScheduler::Schedule schedule;
    bool                      periodic;
    bool                      republish;
    m_transport.GetPublishTarget(
      now, target, schedule, periodic, republish);

    const bool ready = m_frameProcessor->HasReadyFrame();
    if (!ready)
    {
      m_transport.ProcessFrameQueue();
      if (m_frameProcessor->HasReadyFrame())
        continue;

      uint64_t current       = CFrameScheduler::Nanotime();
      uint64_t cadenceTarget = 0;
      if (cadenceEnabled && schedule.deliveryDeadlineSerial && periodic)
      {
        if (schedule.deadline <= current)
        {
          m_transport.FrameMissed(schedule, current, periodic);
          continue;
        }
        cadenceTarget = schedule.deadline;
      }

      if (republish && m_transport.HasPublishedFrame())
      {
        if (m_transport.RepublishFrameBuffer(schedule))
          continue;

        current = CFrameScheduler::Nanotime();
        if (cadenceTarget && cadenceTarget <= current)
        {
          m_transport.FrameMissed(schedule, current, periodic);
          continue;
        }

        uint64_t retryTarget = current + PUBLISH_RETRY_NS;
        if (cadenceTarget)
          retryTarget = min(retryTarget, cadenceTarget);
        ArmPublishTimer(m_publishTimer.Get(), retryTarget - current);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }

      uint64_t replayTarget;
      if (m_transport.GetSharedFrameTarget(current, replayTarget))
      {
        bool retry = false;
        if (replayTarget <= current)
        {
          if (m_transport.ReplaySharedFrame(current, retry))
            continue;

          current = CFrameScheduler::Nanotime();
          if (cadenceTarget && cadenceTarget <= current)
          {
            m_transport.FrameMissed(schedule, current, periodic);
            continue;
          }

          if (retry)
            replayTarget = current + PUBLISH_RETRY_NS;
          else
          {
            if (cadenceTarget)
              replayTarget = cadenceTarget;
            else
            {
              if (m_publishTimer.Get())
                CancelWaitableTimer(m_publishTimer.Get());
              if (WaitForMultipleObjects(
                    ARRAYSIZE(idleHandles), idleHandles, FALSE, INFINITE) ==
                  WAIT_OBJECT_0)
                break;
              continue;
            }
          }
        }

        if (cadenceTarget)
          replayTarget = min(replayTarget, cadenceTarget);

        current = CFrameScheduler::Nanotime();
        if (cadenceTarget && cadenceTarget <= current)
        {
          m_transport.FrameMissed(schedule, current, periodic);
          continue;
        }
        if (replayTarget <= current)
          continue;

        ArmPublishTimer(m_publishTimer.Get(), replayTarget - current);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }

      if (cadenceTarget)
      {
        current = CFrameScheduler::Nanotime();
        if (cadenceTarget <= current)
        {
          m_transport.FrameMissed(schedule, current, periodic);
          continue;
        }

        ArmPublishTimer(m_publishTimer.Get(), cadenceTarget - current);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }

      if (m_publishTimer.Get())
        CancelWaitableTimer(m_publishTimer.Get());
      if (WaitForMultipleObjects(
            ARRAYSIZE(idleHandles), idleHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
      continue;
    }

    uint64_t current = CFrameScheduler::Nanotime();
    uint64_t replayTarget;
    if (m_transport.GetSharedFrameTarget(current, replayTarget) &&
        replayTarget < target)
    {
      if (replayTarget <= current)
      {
        m_transport.ProcessFrameQueue();
        current = CFrameScheduler::Nanotime();
        bool retry = false;
        if (m_transport.ReplaySharedFrame(current, retry))
          continue;

        current = CFrameScheduler::Nanotime();
        if (retry)
          replayTarget = current + PUBLISH_RETRY_NS;
        else
          replayTarget = target;
      }

      replayTarget = min(replayTarget, target);
      current      = CFrameScheduler::Nanotime();
      if (target > current)
      {
        if (replayTarget <= current)
          continue;

        ArmPublishTimer(m_publishTimer.Get(), replayTarget - current);
        if (WaitForMultipleObjects(
              ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
            WAIT_OBJECT_0)
          break;
        continue;
      }
    }

    current = CFrameScheduler::Nanotime();
    if (target > current)
    {
      ArmPublishTimer(m_publishTimer.Get(), target - current);
      if (WaitForMultipleObjects(
            ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
      continue;
    }

    const uint64_t publishStart = CFrameScheduler::Nanotime();
    m_transport.ProcessFrameQueue();
    if (!m_transport.FrameBufferAvailable(schedule) ||
        !m_frameProcessor->Publish(schedule, periodic, publishStart))
    {
      ArmPublishTimer(m_publishTimer.Get(), PUBLISH_RETRY_NS);
      if (WaitForMultipleObjects(
            ARRAYSIZE(timerHandles), timerHandles, FALSE, INFINITE) ==
          WAIT_OBJECT_0)
        break;
    }
  }

  if (avTaskHandle)
    AvRevertMmThreadCharacteristics(avTaskHandle);
}
