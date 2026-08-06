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

#include <Windows.h>
#include <stdint.h>

extern "C" {
  #include <lgmp/lgmp.h>
}

#include "common/KVMFR.h"

class CFrameScheduler
{
public:
  struct Schedule
  {
    uint32_t clientID;
    uint32_t generation;
    uint32_t epoch;
    uint64_t period;
    uint64_t targetSlack;
    uint64_t deadline;
    uint64_t forceTicket;
    uint64_t republishTicket;
    uint32_t deadlineSerial;
    uint32_t deliveryDeadlineSerial;
    bool     phaseEligible;
  };

private:
  struct Client
  {
    uint32_t clientID;
    uint32_t generation;
    uint64_t period;
    uint64_t targetSlack;
    uint64_t expiry;
    uint64_t nextDelivery;
    uint32_t lastFeedbackDeadlineSerial;
    uint32_t lastDeliveredFrameSerial;
    bool     subscribed;
    bool     ownerCapable;
    bool     subscriptionSeen;
    bool     active;
    bool     immediate;
    bool     deliveredFrameValid;
  };

  struct Publication
  {
    uint32_t generation;
    uint32_t epoch;
    uint32_t deadlineSerial;
    uint32_t frameSerial;
    uint64_t deadline;
    bool     committed;
    bool     completed;
    bool     phaseValid;
    bool     accepted;
  };

  static const unsigned PUBLICATION_HISTORY_SIZE = 128;

  mutable SRWLOCK m_lock = SRWLOCK_INIT;
  HANDLE          m_wakeEvent = nullptr;
  Client          m_clients[LGMP_MAX_CLIENTS] = {};
  Schedule        m_schedule                  = {};
  bool            m_scheduling                = false;
  uint32_t        m_epoch                     = 0;

  // A result acknowledges only the request tickets captured by its attempt.
  uint64_t m_forceRequestTicket     = 0;
  uint64_t m_forceAckTicket         = 0;
  uint64_t m_republishRequestTicket = 0;
  uint64_t m_republishAckTicket     = 0;

  uint64_t m_lastArrival             = 0;
  uint64_t m_guestPeriod             = 0;
  uint64_t m_workEstimate            = 0;
  uint64_t m_nextDeadline            = 0;
  uint32_t m_deadlineSerial          = 0;
  int64_t  m_pendingCorrection       = 0;

  Publication m_publications[PUBLICATION_HISTORY_SIZE] = {};
  unsigned    m_publicationIndex                       = 0;

  int64_t  m_lastPhaseError   = 0;
  uint64_t m_acquiredFrames   = 0;
  uint64_t m_skippedFrames    = 0;
  uint64_t m_publishedFrames  = 0;
  uint64_t m_lastLog          = 0;
  uint64_t m_lastLogAcquired  = 0;
  uint64_t m_lastLogSkipped   = 0;
  uint64_t m_lastLogPublished = 0;

  Client * FindClient(uint32_t clientID);
  Publication * FindPublication(const Schedule& schedule,
    uint32_t frameSerial);
  bool ElectOwner(uint64_t now, uint32_t resetClientID = 0);
  bool ApplyFeedback(Client& client, const KVMFRFrameSchedule& schedule);
  void AdvanceCurrentDeadline();
  void AdvanceDeadlineSerial(uint64_t count);
  void AdvanceDeadline(uint64_t now);
  static void AdvanceDelivery(Client& client, uint64_t now);
  void WakePublisher() const;

public:
  CFrameScheduler();
  ~CFrameScheduler();

  static uint64_t Nanotime();

  void Reset();
  void UpdateSubscribers(const uint32_t * clientIDs, unsigned count,
    const uint32_t * ownerClientIDs, unsigned ownerCount, uint64_t now);
  bool UpdateSchedule(uint32_t sourceClientID,
    const KVMFRFrameSchedule& schedule, uint64_t now);
  bool GetSchedule(Schedule& schedule) const;
  HANDLE GetWakeEvent() const { return m_wakeEvent; }
  void ObserveFrame(uint64_t now);
  void ForceFrame();
  bool GetPublishTarget(uint64_t now, uint64_t& target,
    Schedule& schedule, bool& periodic, bool& republish);
  void FrameMissed(const Schedule& schedule, uint64_t now, bool periodic);
  void FrameSuperseded();
  bool TryFrameSubmitted(const Schedule& schedule, uint32_t frameSerial);
  void FramePublished(const Schedule& schedule, uint32_t frameSerial,
    uint64_t now, bool periodic);
  void FrameRetained(const Schedule& schedule, uint64_t now,
    bool periodic);
  void FrameRepublished(const Schedule& schedule, uint32_t frameSerial);
  bool TryFrameCompleted(const Schedule& schedule, uint32_t frameSerial,
    uint64_t completedAt);
  unsigned GetSecondaryRecipients(const uint32_t * clientIDs,
    unsigned count, uint32_t frameSerial, uint64_t now,
    uint32_t * recipients) const;
  bool GetSecondaryTarget(uint32_t frameSerial, uint64_t now,
    const uint32_t * blockedClientIDs, unsigned blockedCount,
    uint64_t& target) const;
  void FrameDelivered(const uint32_t * clientIDs, unsigned count,
    uint32_t frameSerial, uint64_t now);
  void RequestRepublish();
  void NotifyPublisher() const { WakePublisher(); }
  void TryRecordFrameTiming(uint64_t duration);
  void LogStatistics(uint64_t now);
};
