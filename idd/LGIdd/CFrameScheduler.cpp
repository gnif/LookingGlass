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

#include <string.h>

static const uint64_t MIN_SOURCE_PERIOD_NS   = 100000ULL;
static const uint64_t MIN_SCHEDULE_PERIOD_NS = 2000000ULL;
static const uint64_t MAX_PERIOD_NS          = 1000000000ULL;
static const uint32_t MIN_LEASE_MS           = 100;
static const uint32_t MAX_LEASE_MS           = 5000;
static const uint64_t MIN_SAFETY_NS          = 250000ULL;
static const uint64_t LOG_INTERVAL_NS        = 5000000000ULL;
static const uint64_t CADENCE_BREAK          = 4;

static uint64_t PublicationLead(uint64_t targetSlack, uint64_t workEstimate)
{
  const uint64_t safety = max(MIN_SAFETY_NS, workEstimate / 8);
  return targetSlack + workEstimate + safety;
}

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

CFrameScheduler::Client * CFrameScheduler::FindOrAllocateClient(
  uint32_t clientID)
{
  Client * client = FindClient(clientID);
  if (client || !clientID)
    return client;

  Client * replacement = nullptr;
  for (Client& candidate : m_clients)
  {
    if (!candidate.clientID)
    {
      replacement = &candidate;
      break;
    }

    // A schedule can arrive before its frame subscriptions are visible.
    // Such provisional entries must not prevent a real subscriber from
    // obtaining one of the bounded scheduler slots.
    if (candidate.subscribed || candidate.subscriptionSeen)
      continue;

    if (!replacement || candidate.expiry < replacement->expiry)
      replacement = &candidate;
  }

  if (replacement)
  {
    *replacement          = {};
    replacement->clientID = clientID;
  }
  return replacement;
}

CFrameScheduler::Publication * CFrameScheduler::FindPublication(
  const Schedule& schedule, uint32_t frameSerial)
{
  for (Publication& publication : m_publications)
    if (publication.generation     == schedule.generation     &&
        publication.epoch          == schedule.epoch          &&
        publication.deadlineSerial == schedule.deadlineSerial &&
        publication.frameSerial    == frameSerial              &&
        publication.deadline       == schedule.deadline)
      return &publication;

  return nullptr;
}

bool CFrameScheduler::ElectOwner(uint64_t now, uint32_t resetClientID)
{
  Client * fastest       = nullptr;
  Client * incumbent     = FindClient(m_schedule.clientID);
  unsigned subscribers   = 0;
  bool     clientExpired = false;

  for (Client& client : m_clients)
  {
    if (client.active && client.expiry <= now)
    {
      client.active = false;
      clientExpired = true;
    }

    if (!client.subscribed || !client.ownerCapable)
      continue;

    ++subscribers;
    if (!client.active)
      continue;

    if (!fastest || client.period < fastest->period)
      fastest = &client;
  }

  if (!subscribers)
    fastest = nullptr;

  if (fastest && incumbent && incumbent->subscribed &&
      incumbent->ownerCapable && incumbent->active &&
      incumbent->expiry > now &&
      incumbent->period <= fastest->period + fastest->period / 200)
    fastest = incumbent;

  const uint32_t oldClientID   = m_schedule.clientID;
  const uint32_t oldGeneration = m_schedule.generation;
  const uint32_t oldEpoch      = m_schedule.epoch;
  const uint64_t oldPeriod     = m_schedule.period;
  const uint64_t oldSlack      = m_schedule.targetSlack;
  if (incumbent && incumbent->clientID != resetClientID &&
      incumbent->active &&
      incumbent->generation == oldGeneration)
    incumbent->nextDelivery = m_nextDeadline;

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
  const bool ownerReset = resetClientID &&
    resetClientID == m_schedule.clientID;
  const bool identityChanged = ownerChanged || ownerReset ||
    oldGeneration != m_schedule.generation;
  if (identityChanged)
  {
    if (m_scheduling)
    {
      if (!++m_epoch)
        ++m_epoch;
      m_schedule.epoch = m_epoch;
    }
    m_nextDeadline  = m_scheduling ? fastest->nextDelivery : 0;
    if (m_scheduling && !m_nextDeadline)
      m_nextDeadline = now + m_schedule.period;
    m_deadlineSerial    = m_scheduling ? 1 : 0;
    m_pendingCorrection = 0;
    if (m_scheduling)
    {
      ++m_forceRequestTicket;
      ++m_republishRequestTicket;
    }
    else
    {
      m_forceAckTicket     = m_forceRequestTicket;
      m_republishAckTicket = m_republishRequestTicket;
    }

    if (fastest)
      fastest->lastFeedbackDeadlineSerial = 0;

    memset(m_publications, 0, sizeof(m_publications));
    m_publicationIndex          = 0;
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

  return identityChanged ||
    oldPeriod != m_schedule.period || oldSlack != m_schedule.targetSlack ||
    clientExpired;
}

void CFrameScheduler::Reset()
{
  AcquireSRWLockExclusive(&m_lock);
  for (Client& client : m_clients)
    client = {};
  m_schedule   = {};
  m_scheduling = false;
  m_epoch      = 0;

  m_forceAckTicket     = m_forceRequestTicket;
  m_republishAckTicket = m_republishRequestTicket;

  m_lastArrival       = 0;
  m_guestPeriod       = 0;
  m_workEstimate      = 0;
  memset(m_workTiming, 0, sizeof(m_workTiming));
  m_workTimingCount   = 0;
  m_workTimingIndex   = 0;
  m_nextDeadline      = 0;
  m_deadlineSerial    = 0;
  m_pendingCorrection = 0;

  memset(m_publications, 0, sizeof(m_publications));
  m_publicationIndex          = 0;

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
  unsigned count, const uint32_t * ownerClientIDs, unsigned ownerCount,
  uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);

  uint32_t oldClientIDs   [LGMP_MAX_CLIENTS] = {};
  bool     wasSubscribed  [LGMP_MAX_CLIENTS] = {};
  bool     wasOwnerCapable[LGMP_MAX_CLIENTS] = {};
  unsigned clientIndex                           = 0;
  for (const Client& client : m_clients)
  {
    oldClientIDs[clientIndex]    = client.clientID;
    wasSubscribed[clientIndex]   = client.subscribed;
    wasOwnerCapable[clientIndex] = client.ownerCapable;
    ++clientIndex;
  }

  bool subscribersChanged = false;
  for (Client& client : m_clients)
  {
    client.subscribed   = false;
    client.ownerCapable = false;
  }

  for (unsigned i = 0; i < count; ++i)
  {
    Client * client = FindOrAllocateClient(clientIDs[i]);

    if (client)
    {
      client->subscribed       = true;
      client->subscriptionSeen = true;
    }
  }

  for (unsigned i = 0; i < ownerCount; ++i)
  {
    Client * client = FindClient(ownerClientIDs[i]);
    if (client && client->subscribed)
      client->ownerCapable = true;
  }

  for (Client& client : m_clients)
    if (client.clientID && !client.subscribed &&
        (client.subscriptionSeen || !client.active || client.expiry <= now))
    {
      client = {};
    }

  clientIndex = 0;
  for (const Client& client : m_clients)
  {
    if (oldClientIDs[clientIndex] != client.clientID ||
        wasSubscribed[clientIndex] != client.subscribed ||
        wasOwnerCapable[clientIndex] != client.ownerCapable)
      subscribersChanged = true;
    ++clientIndex;
  }

  const bool changed = ElectOwner(now) || subscribersChanged;
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
      client->active       = false;
      client->expiry       = 0;
      client->immediate    = false;
      client->nextDelivery = 0;
      wake = true;
      wake |= ElectOwner(now);
    }
    ReleaseSRWLockExclusive(&m_lock);
    if (wake)
      WakePublisher();
    return true;
  }

  if (!(schedule.flags & KVMFR_FRAME_SCHEDULE_ACTIVE) ||
      schedule.period < MIN_SCHEDULE_PERIOD_NS ||
      schedule.period > MAX_PERIOD_NS ||
      schedule.targetSlack >= schedule.period ||
      schedule.phaseError > static_cast<int64_t>(schedule.period) ||
      schedule.phaseError < -static_cast<int64_t>(schedule.period) ||
      schedule.lease < MIN_LEASE_MS || schedule.lease > MAX_LEASE_MS)
    return false;

  AcquireSRWLockExclusive(&m_lock);
  Client * client = FindOrAllocateClient(schedule.clientID);

  if (!client)
  {
    ReleaseSRWLockExclusive(&m_lock);
    return false;
  }

  const bool explicitReset =
    (schedule.flags & KVMFR_FRAME_SCHEDULE_RESET) != 0;
  const bool reset = client->generation != schedule.generation ||
    explicitReset;
  bool wake = reset || !client->active ||
    client->period != schedule.period ||
    client->targetSlack != schedule.targetSlack;
  if (reset)
  {
    client->lastFeedbackDeadlineSerial = 0;
    client->immediate                  = false;
    client->nextDelivery               = now + schedule.period;
  }
  client->generation  = schedule.generation;
  client->period      = schedule.period;
  client->targetSlack = schedule.targetSlack;
  client->expiry      = now + static_cast<uint64_t>(schedule.lease) * 1000000;
  client->active      = true;
  if (schedule.flags & KVMFR_FRAME_SCHEDULE_IMMEDIATE)
  {
    client->immediate = true;
    wake              = true;
  }
  wake |= ElectOwner(now, explicitReset ? schedule.clientID : 0);
  if (m_scheduling && client->clientID == m_schedule.clientID &&
      client->generation == m_schedule.generation &&
      (schedule.flags & KVMFR_FRAME_SCHEDULE_IMMEDIATE))
  {
    ++m_forceRequestTicket;
    ++m_republishRequestTicket;
    wake = true;
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
      !schedule.feedbackDeadlineSerial ||
      (client.lastFeedbackDeadlineSerial &&
        static_cast<int32_t>(schedule.feedbackDeadlineSerial -
          client.lastFeedbackDeadlineSerial) <= 0))
    return false;

  Publication * publication = nullptr;
  for (Publication& candidate : m_publications)
    if (candidate.generation     == schedule.generation             &&
        candidate.epoch          == schedule.feedbackScheduleEpoch  &&
        candidate.deadlineSerial == schedule.feedbackDeadlineSerial &&
        candidate.frameSerial    == schedule.feedbackFrameSerial)
    {
      publication = &candidate;
      break;
    }

  if (!publication || !publication->committed ||
      !publication->completed || !publication->phaseValid ||
      publication->accepted)
    return false;

  publication->accepted = true;

  int64_t correction = schedule.phaseError / 4;
  const int64_t limit = static_cast<int64_t>(m_schedule.period / 4);
  if (correction > limit)
    correction = limit;
  else if (correction < -limit)
    correction = -limit;

  m_pendingCorrection += correction;
  m_lastPhaseError = schedule.phaseError;
  client.lastFeedbackDeadlineSerial = schedule.feedbackDeadlineSerial;
  return false;
}

void CFrameScheduler::AdvanceCurrentDeadline()
{
  m_nextDeadline += m_schedule.period;
  if (m_pendingCorrection >= 0)
    m_nextDeadline += static_cast<uint64_t>(m_pendingCorrection);
  else
  {
    const uint64_t correction =
      static_cast<uint64_t>(-m_pendingCorrection);
    m_nextDeadline = m_nextDeadline > correction ?
      m_nextDeadline - correction : 0;
  }
  m_pendingCorrection = 0;
  AdvanceDeadlineSerial(1);
}

void CFrameScheduler::AdvanceDeadlineSerial(uint64_t count)
{
  if (!count || !m_deadlineSerial)
    return;

  const uint64_t position =
    static_cast<uint64_t>(m_deadlineSerial - 1) + count;
  uint64_t epochAdvances = position / UINT32_MAX;
  m_deadlineSerial = static_cast<uint32_t>(position % UINT32_MAX) + 1;
  if (!epochAdvances)
    return;

  for (; epochAdvances; --epochAdvances)
    if (!++m_epoch)
      ++m_epoch;
  m_schedule.epoch = m_epoch;

  Client * client = FindClient(m_schedule.clientID);
  if (client)
    client->lastFeedbackDeadlineSerial = 0;
  memset(m_publications, 0, sizeof(m_publications));
  m_publicationIndex = 0;
}

void CFrameScheduler::AdvanceDeadline(uint64_t now)
{
  const uint64_t lead =
    PublicationLead(m_schedule.targetSlack, m_workEstimate);
  const uint64_t target = now + lead;
  if (m_nextDeadline > target)
    return;

  const uint64_t periods =
    (target - m_nextDeadline) / m_schedule.period + 1;
  m_nextDeadline += periods * m_schedule.period;
  AdvanceDeadlineSerial(periods);
}

void CFrameScheduler::AdvanceDelivery(Client& client, uint64_t now)
{
  if (!client.nextDelivery)
  {
    client.nextDelivery = now + client.period;
    return;
  }

  client.nextDelivery += client.period;
  if (client.nextDelivery <= now)
  {
    const uint64_t periods =
      (now - client.nextDelivery) / client.period + 1;
    client.nextDelivery += periods * client.period;
  }
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
  bool wake = false;
  AcquireSRWLockExclusive(&m_lock);
  ++m_acquiredFrames;
  if (m_lastArrival && now > m_lastArrival)
  {
    const uint64_t interval = now - m_lastArrival;
    if (m_guestPeriod && interval > m_guestPeriod * CADENCE_BREAK)
    {
      m_guestPeriod = 0;
      ++m_forceRequestTicket;
      wake = true;
    }
    else if (interval >= MIN_SOURCE_PERIOD_NS &&
             interval <= MAX_PERIOD_NS)
    {
      if (!m_guestPeriod)
        m_guestPeriod = interval;
      else
        m_guestPeriod = (m_guestPeriod * 7 + interval) / 8;
    }
  }
  m_lastArrival = now;
  ReleaseSRWLockExclusive(&m_lock);
  if (wake)
    WakePublisher();
}

void CFrameScheduler::ForceFrame()
{
  AcquireSRWLockExclusive(&m_lock);
  ++m_forceRequestTicket;
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

  schedule                 = m_schedule;
  schedule.forceTicket     = m_forceRequestTicket;
  schedule.republishTicket = m_republishRequestTicket;
  republish = schedule.republishTicket != m_republishAckTicket;

  schedule.deadline       = m_nextDeadline;
  schedule.deadlineSerial = m_deadlineSerial;

  const uint64_t lead =
    PublicationLead(m_schedule.targetSlack, m_workEstimate);
  const uint64_t periodicTarget = m_nextDeadline > lead ?
    m_nextDeadline - lead : now;

  if (schedule.forceTicket != m_forceAckTicket)
  {
    periodic = periodicTarget <= now;
    if (periodic)
      target = periodicTarget;
    schedule.deliveryDeadlineSerial = periodic ? m_deadlineSerial : 0;
    schedule.phaseEligible          = periodic;
    ReleaseSRWLockExclusive(&m_lock);
    return true;
  }

  periodic                        = true;
  schedule.deliveryDeadlineSerial = m_deadlineSerial;
  schedule.phaseEligible          = true;
  target                          = periodicTarget;
  ReleaseSRWLockExclusive(&m_lock);
  return true;
}

void CFrameScheduler::FrameMissed(const Schedule& schedule, uint64_t now,
  bool periodic)
{
  if (!periodic)
    return;

  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch &&
      schedule.deadlineSerial == m_deadlineSerial &&
      schedule.deadline == m_nextDeadline)
  {
    AdvanceCurrentDeadline();
    AdvanceDeadline(now);

    Client * client = FindClient(m_schedule.clientID);
    if (client)
      client->nextDelivery = m_nextDeadline;
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::FrameSuperseded()
{
  AcquireSRWLockExclusive(&m_lock);
  ++m_skippedFrames;
  ReleaseSRWLockExclusive(&m_lock);
}

bool CFrameScheduler::TryFrameSubmitted(const Schedule& schedule,
  uint32_t frameSerial)
{
  if (!schedule.phaseEligible || !schedule.deliveryDeadlineSerial ||
      schedule.deliveryDeadlineSerial != schedule.deadlineSerial)
    return false;

  if (!TryAcquireSRWLockExclusive(&m_lock))
    return false;

  bool registered = false;
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch &&
      schedule.deadlineSerial == m_deadlineSerial &&
      schedule.deadline == m_nextDeadline)
  {
    Publication& publication =
      m_publications[m_publicationIndex++ % PUBLICATION_HISTORY_SIZE];
    publication                = {};
    publication.generation     = schedule.generation;
    publication.epoch          = schedule.epoch;
    publication.deadlineSerial = schedule.deadlineSerial;
    publication.frameSerial    = frameSerial;
    publication.deadline       = schedule.deadline;
    publication.committed      = true;
    registered                 = true;
  }
  ReleaseSRWLockExclusive(&m_lock);
  return registered;
}

void CFrameScheduler::FramePublished(const Schedule& schedule,
  uint32_t frameSerial, uint64_t now, bool periodic)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch &&
      schedule.deadlineSerial == m_deadlineSerial &&
      schedule.deadline == m_nextDeadline)
  {
    Publication * publication = FindPublication(schedule, frameSerial);
    if (publication)
      publication->committed = true;

    if (schedule.forceTicket > m_forceAckTicket)
      m_forceAckTicket = schedule.forceTicket;
    if (schedule.republishTicket > m_republishAckTicket)
      m_republishAckTicket = schedule.republishTicket;

    Client * client = FindClient(m_schedule.clientID);
    if (client)
    {
      if (m_republishAckTicket == m_republishRequestTicket)
        client->immediate = false;
      client->lastDeliveredFrameSerial = frameSerial;
      client->deliveredFrameValid      = true;
    }
    ++m_publishedFrames;
    if (periodic)
    {
      AdvanceCurrentDeadline();
      AdvanceDeadline(now);
    }
    if (client)
      client->nextDelivery = m_nextDeadline;
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::FrameRetained(const Schedule& schedule,
  uint64_t now, bool periodic)
{
  bool wake = false;
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch &&
      schedule.deadlineSerial == m_deadlineSerial &&
      schedule.deadline == m_nextDeadline)
  {
    if (schedule.forceTicket > m_forceAckTicket)
      m_forceAckTicket = schedule.forceTicket;

    // The submission is retained locally, but the owner has not seen it.
    // Keep one republish request pending until an owner lane is released.
    if (m_republishRequestTicket == m_republishAckTicket)
    {
      ++m_republishRequestTicket;
      wake = true;
    }

    if (periodic)
    {
      AdvanceCurrentDeadline();
      AdvanceDeadline(now);
    }

    Client * client = FindClient(m_schedule.clientID);
    if (client)
      client->nextDelivery = m_nextDeadline;
  }
  ReleaseSRWLockExclusive(&m_lock);

  if (wake)
    WakePublisher();
}

bool CFrameScheduler::TryFrameCompleted(const Schedule& schedule,
  uint32_t frameSerial, uint64_t completedAt)
{
  if (!schedule.phaseEligible || !schedule.deliveryDeadlineSerial ||
      schedule.deliveryDeadlineSerial != schedule.deadlineSerial ||
      !schedule.deadline)
    return false;

  if (!TryAcquireSRWLockExclusive(&m_lock))
    return false;

  bool phaseValid = false;
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch)
  {
    Publication * publication = FindPublication(schedule, frameSerial);
    if (publication && publication->committed)
    {
      publication->completed  = true;
      publication->phaseValid = completedAt <= publication->deadline;
      phaseValid = publication->phaseValid;
    }
  }
  ReleaseSRWLockExclusive(&m_lock);
  return phaseValid;
}

void CFrameScheduler::FrameRepublished(const Schedule& schedule,
  uint32_t frameSerial)
{
  AcquireSRWLockExclusive(&m_lock);
  if (m_scheduling && schedule.clientID == m_schedule.clientID &&
      schedule.generation == m_schedule.generation &&
      schedule.epoch == m_schedule.epoch)
  {
    if (schedule.republishTicket > m_republishAckTicket)
      m_republishAckTicket = schedule.republishTicket;

    Client * client = FindClient(m_schedule.clientID);
    if (client)
    {
      if (m_republishAckTicket == m_republishRequestTicket)
        client->immediate = false;
      client->lastDeliveredFrameSerial = frameSerial;
      client->deliveredFrameValid      = true;
      client->nextDelivery             = m_nextDeadline;
    }
    ++m_publishedFrames;
  }
  ReleaseSRWLockExclusive(&m_lock);
}

void CFrameScheduler::RequestRepublish()
{
  bool wake = false;
  AcquireSRWLockExclusive(&m_lock);
  Client * client = FindClient(m_schedule.clientID);
  if (m_scheduling && client && !client->deliveredFrameValid &&
      m_republishRequestTicket == m_republishAckTicket)
  {
    ++m_republishRequestTicket;
    wake = true;
  }
  ReleaseSRWLockExclusive(&m_lock);

  if (wake)
    WakePublisher();
}

unsigned CFrameScheduler::GetSecondaryRecipients(
  const uint32_t * clientIDs, unsigned count, uint32_t frameSerial,
  uint64_t now, uint32_t * recipients) const
{
  AcquireSRWLockShared(&m_lock);
  unsigned recipientCount = 0;
  for (unsigned i = 0; i < count; ++i)
  {
    if (m_scheduling && clientIDs[i] == m_schedule.clientID)
      continue;

    const Client * selected = nullptr;
    for (const Client& client : m_clients)
      if (client.clientID == clientIDs[i])
      {
        selected = &client;
        break;
      }

    const bool active = selected && selected->active &&
      selected->expiry > now;
    const bool sameFrame = selected && selected->deliveredFrameValid &&
      selected->lastDeliveredFrameSerial == frameSerial;
    bool due = !sameFrame;
    if (active)
    {
      const uint64_t target = selected->nextDelivery >
          selected->targetSlack ?
        selected->nextDelivery - selected->targetSlack : 0;
      due = selected->immediate ||
        (!sameFrame && (!selected->nextDelivery || target <= now));
    }

    if (due)
      recipients[recipientCount++] = clientIDs[i];
  }
  ReleaseSRWLockShared(&m_lock);
  return recipientCount;
}

bool CFrameScheduler::GetSecondaryTarget(uint32_t frameSerial,
  uint64_t now, const uint32_t * blockedClientIDs,
  unsigned blockedCount, uint64_t& target) const
{
  bool found = false;
  target = now;

  AcquireSRWLockShared(&m_lock);
  for (const Client& client : m_clients)
  {
    if (!client.clientID || !client.subscribed ||
        (m_scheduling && client.clientID == m_schedule.clientID) ||
        (client.deliveredFrameValid &&
          client.lastDeliveredFrameSerial == frameSerial &&
          !client.immediate))
      continue;

    bool blocked = false;
    for (unsigned i = 0; i < blockedCount; ++i)
      if (blockedClientIDs[i] == client.clientID)
      {
        blocked = true;
        break;
      }
    if (blocked)
      continue;

    uint64_t candidate = now;
    if (client.active && client.expiry > now && !client.immediate &&
        client.nextDelivery > client.targetSlack)
      candidate = client.nextDelivery - client.targetSlack;
    if (candidate < now)
      candidate = now;

    if (!found || candidate < target)
    {
      target = candidate;
      found  = true;
    }
  }
  ReleaseSRWLockShared(&m_lock);
  return found;
}

void CFrameScheduler::FrameDelivered(const uint32_t * clientIDs,
  unsigned count, uint32_t frameSerial, uint64_t now)
{
  AcquireSRWLockExclusive(&m_lock);
  for (unsigned i = 0; i < count; ++i)
  {
    Client * client = FindClient(clientIDs[i]);
    if (!client)
      continue;

    const bool immediate = client->immediate;
    client->immediate                = false;
    client->lastDeliveredFrameSerial = frameSerial;
    client->deliveredFrameValid      = true;

    if (!client->active || client->expiry <= now)
    {
      client->nextDelivery = 0;
      continue;
    }

    if (m_scheduling && client->clientID == m_schedule.clientID)
    {
      client->nextDelivery = m_nextDeadline;
      continue;
    }

    const bool periodic = !client->nextDelivery ||
      client->nextDelivery <= now ||
      client->nextDelivery - now <= client->targetSlack;
    if (!immediate || periodic)
      AdvanceDelivery(*client, now);
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

  const uint32_t clientID     = m_schedule.clientID;
  const uint64_t period       = m_schedule.period;
  const uint64_t workEstimate = m_workEstimate;
  const int64_t  phaseError   = m_lastPhaseError;
  const uint64_t elapsed      = now - m_lastLog;
  const uint64_t acquired     = m_acquiredFrames - m_lastLogAcquired;
  const uint64_t skipped      = m_skippedFrames - m_lastLogSkipped;
  const uint64_t published    = m_publishedFrames - m_lastLogPublished;

  m_lastLog          = now;
  m_lastLogAcquired  = m_acquiredFrames;
  m_lastLogSkipped   = m_skippedFrames;
  m_lastLogPublished = m_publishedFrames;
  ReleaseSRWLockExclusive(&m_lock);

  const double acquiredRate =
    static_cast<double>(acquired) * 1000000000.0 / elapsed;
  DEBUG_TRACE("Frame schedule owner %u: %.3f Hz client, %.3f Hz acquired, "
    "%.3f ms work, %.3f ms phase; %llu acquired, %llu skipped, "
    "%llu published",
    clientID,
    1000000000.0 / period,
    acquiredRate,
    workEstimate / 1000000.0,
    phaseError / 1000000.0,
    static_cast<unsigned long long>(acquired),
    static_cast<unsigned long long>(skipped),
    static_cast<unsigned long long>(published));
}

void CFrameScheduler::TryRecordFrameTiming(uint64_t duration)
{
  if (!duration)
    return;

  if (!TryAcquireSRWLockExclusive(&m_lock))
    return;

  m_workTiming[m_workTimingIndex] = duration;
  m_workTimingIndex =
    (m_workTimingIndex + 1) % WORK_TIMING_HISTORY_SIZE;
  if (m_workTimingCount < WORK_TIMING_HISTORY_SIZE)
    ++m_workTimingCount;

  uint64_t sorted[WORK_TIMING_HISTORY_SIZE];
  memcpy(sorted, m_workTiming,
    m_workTimingCount * sizeof(*sorted));
  for (unsigned i = 1; i < m_workTimingCount; ++i)
  {
    const uint64_t sample = sorted[i];
    unsigned       j      = i;
    while (j && sorted[j - 1] > sample)
    {
      sorted[j] = sorted[j - 1];
      --j;
    }
    sorted[j] = sample;
  }

  if (m_workTimingCount == 1)
    m_workEstimate = sorted[0];
  else
  {
    const unsigned discarded = min(m_workTimingCount - 1,
      max(1U, m_workTimingCount / 10));
    m_workEstimate = sorted[m_workTimingCount - discarded - 1];
  }

  ReleaseSRWLockExclusive(&m_lock);
}
