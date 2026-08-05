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

static const uint64_t MIN_PERIOD_NS   = 2000000ULL;
static const uint64_t MAX_PERIOD_NS   = 1000000000ULL;
static const uint32_t MIN_LEASE_MS    = 100;
static const uint32_t MAX_LEASE_MS    = 5000;
static const uint64_t MIN_SAFETY_NS   = 250000ULL;
static const uint64_t LOG_INTERVAL_NS = 5000000000ULL;
static const uint64_t CADENCE_BREAK   = 4;

CFrameScheduler::CFrameScheduler()
{
  m_wakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

CFrameScheduler::~CFrameScheduler()
{
  if (m_wakeEvent)
    CloseHandle(m_wakeEvent);
}

void CFrameScheduler::WakePublisher() const
{
  if (m_wakeEvent)
    SetEvent(m_wakeEvent);
}

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

bool CFrameScheduler::ElectOwner(uint64_t now)
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
      continue;
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
  const uint32_t oldEpoch      = m_schedule.epoch;
  const uint64_t oldPeriod     = m_schedule.period;
  const uint64_t oldSlack      = m_schedule.targetSlack;
  if (!fastest)
  {
    m_schedule   = {};
    m_scheduling = false;
  }
  else
  {
    m_schedule.clientID    = fastest->clientID;
    m_schedule.generation  = fastest->generation;
    m_schedule.epoch       = oldEpoch;
    m_schedule.period      = fastest->period;
    m_schedule.targetSlack = fastest->targetSlack;
    m_scheduling           = true;
  }

  const bool ownerChanged = oldClientID != m_schedule.clientID;
  if (ownerChanged || oldGeneration != m_schedule.generation)
  {
    if (m_scheduling)
    {
      if (!++m_epoch)
        ++m_epoch;
      m_schedule.epoch = m_epoch;
    }
    m_nextDeadline  = m_scheduling ? now + m_schedule.period : 0;
    m_forceNext     = m_scheduling;
    m_republishNext = m_scheduling;

    if (fastest)
      fastest->lastFeedbackFrameSerial = 0;

    m_lastPublishedFrameSerial = 0;
    m_lastPhaseError            = 0;
    m_lastLog                   = now;
    m_lastLogAcquired           = m_acquiredFrames;
    m_lastLogSkipped            = m_skippedFrames;
    m_lastLogPublished          = m_publishedFrames;

    if (ownerChanged && m_scheduling)
      DEBUG_INFO("Frame timing owner %u generation %u epoch %u at %.3f Hz",
        m_schedule.clientID, m_schedule.generation, m_schedule.epoch,
        1000000000.0 / m_schedule.period);
    else if (ownerChanged && oldClientID)
      DEBUG_INFO("Frame timing owner released; using push delivery");
  }

  return ownerChanged || oldGeneration != m_schedule.generation ||
    oldPeriod != m_schedule.period || oldSlack != m_schedule.targetSlack;
}

void CFrameScheduler::Reset()
{
  AcquireSRWLockExclusive(&m_lock);
  for (Client& client : m_clients)
    client = {};
  m_schedule      = {};
  m_scheduling    = false;
  m_forceNext     = false;
  m_republishNext = false;
  m_epoch         = 0;

  m_lastArrival    = 0;
  m_guestPeriod    = 0;
  m_workEstimate   = 0;
  m_nextDeadline   = 0;

  m_lastPublishedFrameSerial = 0;

  m_lastPhaseError   = 0;
  m_acquiredFrames   = 0;
  m_skippedFrames    = 0;
  m_publishedFrames  = 0;
  m_lastLog          = 0;
  m_lastLogAcquired  = 0;
  m_lastLogSkipped   = 0;
  m_lastLogPublished = 0;
  ReleaseSRWLockExclusive(&m_lock);
  WakePublisher();
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
    {
      client->subscribed       = true;
      client->subscriptionSeen = true;
    }
  }

  for (Client& client : m_clients)
    if (client.clientID && !client.subscribed &&
        (client.subscriptionSeen || !client.active || client.expiry <= now))
      client = {};

  const bool changed = ElectOwner(now);
  ReleaseSRWLockExclusive(&m_lock);
  if (changed)
    WakePublisher();
}

bool CFrameScheduler::UpdateSchedule(uint32_t sourceClientID,
  const KVMFRFrameSchedule& schedule, uint64_t now)
{
  static const KVMFRFrameScheduleFlags validFlags =
    KVMFR_FRAME_SCHEDULE_ACTIVE    |
    KVMFR_FRAME_SCHEDULE_RELEASE   |
    KVMFR_FRAME_SCHEDULE_RESET     |
    KVMFR_FRAME_SCHEDULE_IMMEDIATE;

  if (!sourceClientID || schedule.clientID != sourceClientID ||
      schedule.flags & ~validFlags)
    return false;

  if (schedule.flags & KVMFR_FRAME_SCHEDULE_RELEASE)
  {
    AcquireSRWLockExclusive(&m_lock);
    Client * client = FindClient(schedule.clientID);
    bool wake = false;
    if (client)
    {
      client->active    = false;
      client->expiry    = 0;
      client->immediate = false;
      wake = ElectOwner(now);
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (wake)
      WakePublisher();
    return true;
  }

  if (!(schedule.flags & KVMFR_FRAME_SCHEDULE_ACTIVE) ||
      schedule.period < MIN_PERIOD_NS ||
      schedule.period > MAX_PERIOD_NS ||
      schedule.targetSlack >= schedule.period ||
      schedule.phaseError > static_cast<int64_t>(schedule.period) ||
      schedule.phaseError < -static_cast<int64_t>(schedule.period) ||
      schedule.lease < MIN_LEASE_MS || schedule.lease > MAX_LEASE_MS)
    return false;

  AcquireSRWLockExclusive(&m_lock);
  Client * client = FindClient(schedule.clientID);
  if (!client)
    for (Client& candidate : m_clients)
      if (!candidate.clientID)
      {
        candidate.clientID = schedule.clientID;
        client = &candidate;
        break;
      }

  if (!client)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return false;
  }

  bool wake = false;

  if (client->generation != schedule.generation)
  {
    client->lastFeedbackFrameSerial = 0;
    client->immediate               = false;
  }
  client->generation  = schedule.generation;
  client->period      = schedule.period;
  client->targetSlack = schedule.targetSlack;
  client->expiry      = now + static_cast<uint64_t>(schedule.lease) * 1000000;
  client->active      = true;
  if (schedule.flags & KVMFR_FRAME_SCHEDULE_IMMEDIATE)
    client->immediate = true;
  wake |= ElectOwner(now);
  if (m_scheduling && client->clientID == m_schedule.clientID &&
      client->generation == m_schedule.generation && client->immediate)
  {
    m_forceNext     = true;
    m_republishNext = true;
    wake            = true;
  }
  wake |= ApplyFeedback(*client, schedule);
  ReleaseSRWLockExclusive(&m_lock);
  if (wake)
    WakePublisher();
  return true;
}

bool CFrameScheduler::ApplyFeedback(Client& client,
  const KVMFRFrameSchedule& schedule)
{
  if (!m_scheduling || client.clientID != m_schedule.clientID ||
      schedule.generation != m_schedule.generation ||
      schedule.feedbackScheduleEpoch != m_schedule.epoch ||
      !schedule.feedbackFrameSerial ||
      (client.lastFeedbackFrameSerial &&
        static_cast<int32_t>(schedule.feedbackFrameSerial -
          client.lastFeedbackFrameSerial) <= 0) ||
      !m_lastPublishedFrameSerial ||
      static_cast<int32_t>(schedule.feedbackFrameSerial -
        m_lastPublishedFrameSerial) > 0)
    return false;

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
  m_lastPhaseError = schedule.phaseError;
  client.lastFeedbackFrameSerial = schedule.feedbackFrameSerial;
  return correction != 0;
}

void CFrameScheduler::AdvanceDeadline(uint64_t now)
{
  if (m_nextDeadline > now)
    return;

  const uint64_t periods =
    (now - m_nextDeadline) / m_schedule.period + 1;
  m_nextDeadline += periods * m_schedule.period;
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
  ++m_acquiredFrames;
  if (m_lastArrival && now > m_lastArrival)
  {
    const uint64_t interval = now - m_lastArrival;
    if (m_guestPeriod && interval > m_guestPeriod * CADENCE_BREAK)
    {
      m_guestPeriod = 0;
      m_forceNext   = true;
    }
    else if (interval >= MIN_PERIOD_NS && interval <= MAX_PERIOD_NS)
    {
      if (!m_guestPeriod)
        m_guestPeriod = interval;
      else
        m_guestPeriod = (m_guestPeriod * 7 + interval) / 8;
    }
  }
  m_lastArrival = now;
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::ForceFrame()
{
  AcquireSRWLockExclusive(&m_lock);
  m_forceNext = true;
  ReleaseSRWLockExclusive(&m_lock);
  WakePublisher();
}

bool CFrameScheduler::GetPublishTarget(uint64_t now, uint64_t& target,
  Schedule& schedule, bool& periodic, bool& republish)
{
  target    = now;
  schedule  = {};
  periodic  = false;
  republish = false;
  AcquireSRWLockExclusive(&m_lock);
  if (!m_scheduling)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  schedule  = m_schedule;
  republish = m_republishNext;

  if (m_forceNext)
  {
    periodic = m_nextDeadline <= now;
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  periodic = true;
  if (m_nextDeadline <= now)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  const uint64_t safety =
    max(MIN_SAFETY_NS, m_workEstimate / 8);
  const uint64_t lead =
    m_schedule.targetSlack + m_workEstimate + safety;
  target = m_nextDeadline > lead ? m_nextDeadline - lead : now;
  if (target < now)
    target = now;
  ReleaseSRWLockExclusive(&m_lock);
  return true;
}

void CFrameScheduler::FrameSuperseded()
{
  AcquireSRWLockExclusive(&m_lock);
  ++m_skippedFrames;
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::FramePublished(const Schedule& schedule,
  uint32_t frameSerial, uint64_t now, bool periodic)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch)
  {
    m_forceNext     = false;
    m_republishNext = false;

    Client * client = FindClient(m_schedule.clientID);
    if (client)
      client->immediate = false;
    m_lastPublishedFrameSerial = frameSerial;
    ++m_publishedFrames;
    if (periodic)
    {
      m_nextDeadline += m_schedule.period;
      AdvanceDeadline(now);
    }
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::FrameRepublished(const Schedule& schedule,
  uint32_t frameSerial)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch)
  {
    m_republishNext = false;

    Client * client = FindClient(m_schedule.clientID);
    if (client)
      client->immediate = false;
    m_lastPublishedFrameSerial = frameSerial;
    ++m_publishedFrames;
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::FrameDelivered(const uint32_t * clientIDs,
  unsigned count)
{
  AcquireSRWLockExclusive(&m_lock);
  for (unsigned i = 0; i < count; ++i)
  {
    Client * client = FindClient(clientIDs[i]);
    if (client)
      client->immediate = false;
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::LogStatistics(uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);
  if (!m_scheduling || now - m_lastLog < LOG_INTERVAL_NS)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return;
  }

  const uint64_t acquired  = m_acquiredFrames  - m_lastLogAcquired;
  const uint64_t skipped   = m_skippedFrames   - m_lastLogSkipped;
  const uint64_t published = m_publishedFrames - m_lastLogPublished;
  DEBUG_TRACE("Frame schedule owner %u: %.3f Hz client, %.3f Hz guest, "
    "%.3f ms work, %.3f ms phase; %llu acquired, %llu skipped, "
    "%llu published",
    m_schedule.clientID,
    1000000000.0 / m_schedule.period,
    m_guestPeriod ? 1000000000.0 / m_guestPeriod : 0.0,
    m_workEstimate / 1000000.0,
    m_lastPhaseError / 1000000.0,
    static_cast<unsigned long long>(acquired),
    static_cast<unsigned long long>(skipped),
    static_cast<unsigned long long>(published));

  m_lastLog          = now;
  m_lastLogAcquired  = m_acquiredFrames;
  m_lastLogSkipped   = m_skippedFrames;
  m_lastLogPublished = m_publishedFrames;
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

  ReleaseSRWLockExclusive(&m_lock);
}
