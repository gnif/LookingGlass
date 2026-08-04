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

#include "CFrameScheduler.h"

#include "CDebug.h"

#include <algorithm>

static const uint64_t MIN_PERIOD_NS = 2000000ULL;
static const uint64_t MAX_PERIOD_NS = 1000000000ULL;
static const uint32_t MIN_LEASE_MS  = 100;
static const uint32_t MAX_LEASE_MS  = 5000;
static const uint64_t MIN_SAFETY_NS = 250000ULL;

uint64_t CFrameScheduler::Nanotime()
{
  static const uint64_t frequency = []()
  {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return static_cast<uint64_t>(value.QuadPart);
  }();

  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  const uint64_t ticks = static_cast<uint64_t>(counter.QuadPart);
  return ticks / frequency * 1000000000ULL +
    ticks % frequency * 1000000000ULL / frequency;
}

CFrameScheduler::Client * CFrameScheduler::FindClient(uint32_t clientID)
{
  if (!clientID)
    return nullptr;

  for (Client& client : m_clients)
    if (client.clientID == clientID)
      return &client;

  return nullptr;
}

void CFrameScheduler::ElectOwner(uint64_t now)
{
  Client * fastest     = nullptr;
  Client * incumbent   = FindClient(m_schedule.clientID);
  unsigned subscribers = 0;

  for (Client& client : m_clients)
  {
    if (!client.subscribed)
      continue;

    ++subscribers;
    if (!client.active || client.expiry <= now)
    {
      client.active = false;
      fastest       = nullptr;
      break;
    }

    if (!fastest || client.period < fastest->period)
      fastest = &client;
  }

  if (!subscribers)
    fastest = nullptr;

  if (fastest && incumbent && incumbent->subscribed && incumbent->active &&
      incumbent->expiry > now &&
      incumbent->period <= fastest->period + fastest->period / 200)
    fastest = incumbent;

  const uint32_t oldClientID   = m_schedule.clientID;
  const uint32_t oldGeneration = m_schedule.generation;
  if (!fastest)
  {
    m_schedule   = {};
    m_scheduling = false;
  }
  else
  {
    m_schedule.clientID    = fastest->clientID;
    m_schedule.generation  = fastest->generation;
    m_schedule.period      = fastest->period;
    m_schedule.targetSlack = fastest->targetSlack;
    m_scheduling = true;
  }

  if (oldClientID != m_schedule.clientID ||
      oldGeneration != m_schedule.generation)
  {
    m_nextDeadline = m_scheduling ? now + m_schedule.period : 0;
    m_forceNext    = m_scheduling;

    m_lastPublishedFrameSerial = 0;

    if (m_scheduling)
      DEBUG_INFO("Frame timing owner %u generation %u at %.3f Hz",
        m_schedule.clientID, m_schedule.generation,
        1000000000.0 / m_schedule.period);
    else if (oldClientID)
      DEBUG_INFO("Frame timing owner released; using push delivery");
  }
}

void CFrameScheduler::Reset()
{
  AcquireSRWLockExclusive(&m_lock);
  for (Client& client : m_clients)
    client = {};
  m_schedule   = {};
  m_scheduling = false;
  m_forceNext  = false;

  m_lastArrival    = 0;
  m_guestPeriod    = 0;
  m_guestJitter    = 0;
  m_workEstimate   = 0;
  m_nextDeadline   = 0;
  m_arrivalSamples = 0;
  m_timingSamples  = 0;

  m_lastPublishedFrameSerial = 0;
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::UpdateSubscribers(const uint32_t * clientIDs,
  unsigned count, uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);

  for (Client& client : m_clients)
    client.subscribed = false;

  for (unsigned i = 0; i < count; ++i)
  {
    Client * client = FindClient(clientIDs[i]);
    if (!client)
      for (Client& candidate : m_clients)
        if (!candidate.clientID)
        {
          candidate.clientID = clientIDs[i];
          client = &candidate;
          break;
        }

    if (client)
      client->subscribed = true;
  }

  for (Client& client : m_clients)
    if (client.clientID && !client.subscribed)
      client = {};

  ElectOwner(now);
  ReleaseSRWLockExclusive(&m_lock);
}

bool CFrameScheduler::UpdateSchedule(const KVMFRFrameSchedule& schedule,
  uint64_t now)
{
  static const KVMFRFrameScheduleFlags validFlags =
    KVMFR_FRAME_SCHEDULE_ACTIVE    |
    KVMFR_FRAME_SCHEDULE_RELEASE   |
    KVMFR_FRAME_SCHEDULE_RESET     |
    KVMFR_FRAME_SCHEDULE_IMMEDIATE;

  if (!schedule.clientID || schedule.flags & ~validFlags)
    return false;

  AcquireSRWLockExclusive(&m_lock);
  Client * client = FindClient(schedule.clientID);
  if (!client || !client->subscribed)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return false;
  }

  if (schedule.flags & KVMFR_FRAME_SCHEDULE_RELEASE)
  {
    client->active = false;
    client->expiry = 0;
    ElectOwner(now);
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  if (!(schedule.flags & KVMFR_FRAME_SCHEDULE_ACTIVE) ||
      schedule.period < MIN_PERIOD_NS ||
      schedule.period > MAX_PERIOD_NS ||
      schedule.targetSlack >= schedule.period ||
      schedule.phaseError > static_cast<int64_t>(schedule.period) ||
      schedule.phaseError < -static_cast<int64_t>(schedule.period) ||
      schedule.lease < MIN_LEASE_MS || schedule.lease > MAX_LEASE_MS)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return false;
  }

  if (client->generation != schedule.generation)
    client->lastFeedbackFrameSerial = 0;
  client->generation  = schedule.generation;
  client->period      = schedule.period;
  client->targetSlack = schedule.targetSlack;
  client->expiry      = now + static_cast<uint64_t>(schedule.lease) * 1000000;
  client->active      = true;
  if (schedule.flags & KVMFR_FRAME_SCHEDULE_IMMEDIATE)
    m_forceNext = true;
  ElectOwner(now);
  ApplyFeedback(*client, schedule);
  ReleaseSRWLockExclusive(&m_lock);
  return true;
}

void CFrameScheduler::ApplyFeedback(Client& client,
  const KVMFRFrameSchedule& schedule)
{
  if (!m_scheduling || client.clientID != m_schedule.clientID ||
      schedule.generation != m_schedule.generation ||
      !schedule.feedbackFrameSerial ||
      (client.lastFeedbackFrameSerial &&
        static_cast<int32_t>(schedule.feedbackFrameSerial -
          client.lastFeedbackFrameSerial) <= 0) ||
      !m_lastPublishedFrameSerial ||
      static_cast<int32_t>(schedule.feedbackFrameSerial -
        m_lastPublishedFrameSerial) > 0)
    return;

  int64_t correction = schedule.phaseError / 4;
  const int64_t limit = static_cast<int64_t>(m_schedule.period / 4);
  if (correction > limit)
    correction = limit;
  else if (correction < -limit)
    correction = -limit;

  if (correction >= 0)
    m_nextDeadline += static_cast<uint64_t>(correction);
  else
  {
    const uint64_t advance = static_cast<uint64_t>(-correction);
    m_nextDeadline = m_nextDeadline > advance ?
      m_nextDeadline - advance : 0;
  }
  client.lastFeedbackFrameSerial = schedule.feedbackFrameSerial;
}

bool CFrameScheduler::GetSchedule(Schedule& schedule) const
{
  AcquireSRWLockShared(&m_lock);
  const bool result = m_scheduling;
  if (result)
    schedule = m_schedule;
  ReleaseSRWLockShared(&m_lock);
  return result;
}

void CFrameScheduler::ObserveFrame(uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_lastArrival && now > m_lastArrival)
  {
    const uint64_t interval = now - m_lastArrival;
    if (interval >= MIN_PERIOD_NS && interval <= MAX_PERIOD_NS)
    {
      if (!m_guestPeriod)
        m_guestPeriod = interval;
      else
      {
        const uint64_t error = m_guestPeriod > interval ?
          m_guestPeriod - interval : interval - m_guestPeriod;
        m_guestPeriod = (m_guestPeriod * 7 + interval) / 8;
        m_guestJitter = (m_guestJitter * 7 + error) / 8;
      }

      if (m_arrivalSamples < 32)
        ++m_arrivalSamples;
    }
  }
  m_lastArrival = now;
  ReleaseSRWLockExclusive(&m_lock);
}

bool CFrameScheduler::SelectFrame(uint64_t now, bool force,
  uint32_t& generation)
{
  generation = 0;
  AcquireSRWLockExclusive(&m_lock);
  if (!m_scheduling)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  generation = m_schedule.generation;
  while (m_nextDeadline <= now)
    m_nextDeadline += m_schedule.period;

  if (force)
    m_forceNext = true;

  if (m_forceNext || m_arrivalSamples < 4 || m_timingSamples < 4)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  const uint64_t safety =
    std::max(MIN_SAFETY_NS,
      std::max(m_guestJitter * 2, m_workEstimate / 8));
  const uint64_t nextArrival = m_lastArrival + m_guestPeriod;
  const bool process = nextArrival <= now ||
    nextArrival + m_workEstimate + safety > m_nextDeadline;
  ReleaseSRWLockExclusive(&m_lock);
  return process;
}

void CFrameScheduler::FramePublished(uint32_t generation,
  uint32_t frameSerial, uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && generation == m_schedule.generation)
  {
    m_forceNext = false;
    m_lastPublishedFrameSerial = frameSerial;
    do
      m_nextDeadline += m_schedule.period;
    while (m_nextDeadline <= now);
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::RecordFrameTiming(uint64_t duration)
{
  if (!duration)
    return;

  AcquireSRWLockExclusive(&m_lock);
  if (!m_workEstimate || duration > m_workEstimate)
    m_workEstimate = duration;
  else
    m_workEstimate = (m_workEstimate * 31 + duration) / 32;

  if (m_timingSamples < 32)
    ++m_timingSamples;
  ReleaseSRWLockExclusive(&m_lock);
}
