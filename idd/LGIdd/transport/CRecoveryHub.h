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

#include "CSRWLock.h"
#include "transport/ITransport.h"

class CRecoveryHub
{
public:
  struct Delivery
  {
    SourceKey     source;
    uint64_t      session = 0;
    uint32_t      serial  = 0;
    bool          active  = false;
    RecoveryState state   = RecoveryState::FAILED;
    uint32_t      error   = 0;
  };

private:
  static constexpr unsigned MAX_EPOCHS   = TRANSPORT_MAX_INSTANCES;
  // Reserve two independent request correlations per configured instance.
  static constexpr unsigned MAX_REQUESTS = TRANSPORT_MAX_INSTANCES * 2;
  static constexpr uint64_t HELPER_TIMEOUT_MS = 20000;

  enum class SlotState
  {
    FREE,
    WAITING,
    READY,
  };

  enum class OperationPhase
  {
    NONE,
    IN_FLIGHT,
    LATCHED,
  };

  struct Epoch
  {
    BackendId backend = 0;
    uint32_t  epoch   = 0;
    bool      synced  = false;
  };

  struct Request
  {
    SourceKey source;
    uint64_t  session  = 0;
    uint64_t  operation = 0;
    uint64_t  sequence = 0;
    uint32_t  serial   = 0;
    uint32_t  error    = 0;
    bool      active   = false;
    SlotState state    = SlotState::FREE;
    RecoveryState result = RecoveryState::FAILED;
  };

  struct Operation
  {
    OperationPhase phase = OperationPhase::NONE;
    RecoveryAction action;
    uint64_t        id = 0;
  };

  mutable CSRWLock m_lock;
  Epoch            m_epochs[MAX_EPOCHS];
  Request          m_requests[MAX_REQUESTS];
  Operation        m_operation;
  RecoveryState    m_known = RecoveryState::FAILED;
  uint64_t         m_session         = 0;
  uint64_t         m_nextOperation = 1;
  uint64_t         m_nextRoute     = 1;
  uint64_t         m_nextSequence  = 1;
  uint32_t         m_nextSerial    = 2;
  bool             m_knownValid    = false;
  bool             m_monitorReady  = false;

  static bool SameSource(const SourceKey& left, const SourceKey& right);
  static bool SameEpoch(
    const SourceKey& source, BackendId backend, uint32_t epoch);
  static bool SameRequest(const Request& request, const SourceKey& source,
    uint64_t session, uint32_t serial, bool active);
  static bool SuccessMatches(RecoveryState state, bool active);

  bool EpochAttachedLocked(const SourceKey& source) const;
  unsigned FindSourceLocked(const SourceKey& source) const;
  unsigned FindFreeLocked() const;
  bool ActionMatchesLocked(const RecoveryAction& action) const;
  uint64_t NextNonzero(uint64_t& value);
  uint32_t NextSerial();
  void ClearRequestLocked(Request& request);
  void SetWaitingLocked(Request& request, const SourceKey& source,
    uint64_t session, uint32_t serial, bool active, uint64_t operation);
  void SetReadyLocked(
    Request& request, RecoveryState state, uint32_t error);
  void FinishWaitersLocked(RecoveryState state, uint32_t error);

public:
  CRecoveryHub();

  bool Attach(BackendId backend, uint32_t epoch, bool& syncNow);
  void Remove(BackendId backend, uint32_t epoch);

  RecoveryAdmission Submit(const SourceKey& source, uint64_t session,
    uint32_t serial, bool active, uint64_t now,
    RecoveryAction& action, bool& dispatch);
  bool DispatchFailed(const RecoveryAction& action, uint32_t error);
  bool Complete(const RecoveryAction& action,
    RecoveryState state, uint32_t error);
  bool Tick(uint64_t now);

  bool TakeDelivery(
    BackendId backend, uint32_t epoch, Delivery& delivery);
  bool HasDelivery(BackendId backend, uint32_t epoch) const;

  bool MarkMonitorReady();
  bool ClaimSync(BackendId backend, uint32_t epoch);
  bool MonitorReady() const;
};
