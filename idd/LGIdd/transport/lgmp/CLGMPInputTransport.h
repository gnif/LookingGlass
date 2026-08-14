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

#include "Atomic.h"
#include "CSRWLock.h"
#include "transport/IInputSource.h"
#include "common/LGMPConfig.h"

#include <Windows.h>

#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
}

class CLGMPHost;
class IInputTarget;
struct KVMFRInputMessage;

class CLGMPInputTransport final : public IInputSource
{
private:
  static constexpr ULONGLONG OWNER_LEASE_MS = 500;
  static constexpr ULONGLONG ACTIVE_POLL_MS  = 50;
  static constexpr ULONGLONG LOG_INTERVAL_MS = 5000;

  struct Statistics
  {
    ULONGLONG lastLog;
    uint64_t  messages;
    uint64_t  reports;
    uint64_t  drainLimit;
    uint64_t  malformedSize;
    uint64_t  malformedMessage;
    uint64_t  sequenceErrors;
    uint64_t  nonOwner;
    uint64_t  deliveryFailures;
    uint64_t  claims;
    uint64_t  releases;
    unsigned  maxDrain;
  };

  CLGMPHost& m_host;

  PLGMPHostQueue m_queue                          = nullptr;
  PLGMPMemory    m_statusMemory[LGMP_Q_INPUT_LEN] = {};
  IInputTarget * m_target                         = nullptr;

  CSRWLock m_lifecycleLock;
  CSRWLock m_statusLock;
  HANDLE   m_stopEvent = nullptr;
  HANDLE   m_pollTimer = nullptr;
  HANDLE   m_thread    = nullptr;

  uint32_t   m_ownerClientID       = 0;
  uint32_t   m_ownerGeneration     = 0;
  uint32_t   m_ownerSequence       = 0;
  ULONGLONG  m_ownerDeadline       = 0;
  InputTargetState m_targetState;
  uint32_t   m_endpointGeneration = 0;
  uint32_t   m_statusSerial        = 0;
  bool       m_statusDirty         = false;
  std::atomic<bool> m_statusFailed = false;
  Statistics m_statistics         = {};

  bool Initialize();
  void DeInit();
  InputSourceId Owner() const;
  void UpdateTargetState(const InputTargetState& state);
  bool PublishStatus();
  void FlushStatus();
  void LogStatistics(ULONGLONG now);
  bool DrainMessages(bool& received);
  bool ProcessMessage(uint32_t sourceClientID,
    const KVMFRInputMessage& message);
  bool ValidatePayload(const KVMFRInputMessage& message) const;
  bool IsOwner(uint32_t sourceClientID, uint32_t generation) const;
  bool Claim(uint32_t sourceClientID,
    const KVMFRInputMessage& message);
  void RenewLease();
  void ReleaseOwner(bool reset);
  void CheckOwner();
  void Thread();

  static DWORD CALLBACK ThreadProc(void * context);

  friend class CLGMPTransport;

public:
  explicit CLGMPInputTransport(CLGMPHost& host) :
    m_host(host) {}
  ~CLGMPInputTransport() override;

  CLGMPInputTransport(const CLGMPInputTransport&) = delete;
  CLGMPInputTransport& operator=(const CLGMPInputTransport&) = delete;

  bool Start(IInputTarget& target) override;
  void Stop() override;
};
