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

static const uint64_t MIN_PERIOD_NS = 2000000ULL;
static const uint64_t MAX_PERIOD_NS = 1000000000ULL;
static const uint32_t MIN_LEASE_MS  = 100;
static const uint32_t MAX_LEASE_MS  = 5000;

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
      schedule.lease < MIN_LEASE_MS || schedule.lease > MAX_LEASE_MS)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return false;
  }

  client->generation  = schedule.generation;
  client->period      = schedule.period;
  client->targetSlack = schedule.targetSlack;
  client->expiry      = now + static_cast<uint64_t>(schedule.lease) * 1000000;
  client->active      = true;
  ElectOwner(now);
  ReleaseSRWLockExclusive(&m_lock);
  return true;
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
