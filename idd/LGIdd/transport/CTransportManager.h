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
#include "transport/CControlHub.h"
#include "transport/CFrameHub.h"
#include "transport/CInputHub.h"
#include "transport/ITransport.h"
#include "transport/TransportConfig.h"

#include <memory>

static_assert(TRANSPORT_MAX_INSTANCES == FRAME_MAX_SINKS,
  "The transport and frame limits must match");

class CTransportManager final : public FrameCaps
{
public:
  using CreateFn = std::unique_ptr<ITransport> (*)(
    const TransportInstance& config);
  using OpenResult = ITransport::OpenResult;
  using ProcessResult = ITransport::ProcessResult;
  using Recovery = ITransport::Recovery;

private:
  mutable CSRWLock m_lock;

  enum class Phase
  {
    IDLE,
    OPEN,
    INITIALIZE,
    SETUP,
    PROCESS,
    ACCESS,
    STOP,
  };

  enum class State
  {
    CLOSED,
    OPEN,
    INITIALIZED,
    READY,
    RETRY,
    FAILED,
    STOPPED,
  };

  enum class Call
  {
    IDLE,
    LIFECYCLE,
    PROCESS,
    RECOVERY,
    ACCESS,
  };

  struct RecoveryUpdate
  {
    uint64_t session = 0;
    uint32_t serial  = 0;
    bool     active  = false;
    Recovery state  = Recovery::FAILED;
    uint32_t error   = 0;
  };

  struct Entry
  {
    Entry();
    ~Entry();

    mutable CSRWLock            lock;
    HANDLE                      idleEvent     = nullptr;
    Call                        call          = Call::IDLE;
    DWORD                       callOwner     = 0;
    bool                        stopRequested = false;
    BackendId                   id       = 0;
    TransportInstance           config;
    bool                        required = false;
    bool                        primary  = false;
    CreateFn                    create   = nullptr;
    std::shared_ptr<ITransport> transport;
    State                       state   = State::CLOSED;
    uint32_t                    epoch   = 1;
    uint64_t                    retryAt = 0;
    bool                        controlAdded  = false;
    bool                        controlFailed = false;
    bool                        controlAbsent = false;
    bool                        inputAdded    = false;
    bool                        inputFailed   = false;
    bool                        inputAbsent   = false;
    bool                        frameAdded    = false;
    bool                        frameAbsent   = false;
    uint64_t                    serviceRetryAt = 0;
    bool                        exposed      = false;
    bool                        setupDone    = false;
    bool                        syncPending  = false;
    bool                        recoveryPending = false;
    RecoveryUpdate              recovery;
    std::shared_ptr<const FrameCaps> frameCaps;
    DirectFrameBufferMemory     directMemory;
    bool                        directMemoryValid = false;
    std::shared_ptr<ITransport> exposedTransport;
  };

  std::unique_ptr<Entry>               m_entries[FRAME_MAX_SINKS];
  unsigned                             m_entryCount  = 0;
  CControlHub                         m_control;
  CFrameHub                           m_frames;
  CInputHub                           m_input;
  Entry                              * m_primary     = nullptr;
  bool                                 m_initialized = false;
  bool                                 m_setup       = false;
  size_t                               m_alignment   = 0;
  Phase                                m_phase       = Phase::IDLE;
  DWORD                                m_phaseOwner  = 0;
  HANDLE                               m_phaseIdle   = nullptr;
  HANDLE                               m_stoppedEvent = nullptr;
  bool                                 m_started     = false;
  bool                                 m_stopping    = false;
  bool                                 m_stopped     = false;

  unsigned Entries(Entry * entries[FRAME_MAX_SINKS]) const;
  Entry * Primary() const;

  bool BeginPhase(Phase phase, bool wait, bool stopping = false);
  void EndPhase();
  bool BeginCall(Entry& entry, Call call, bool wait);
  void EndCall(Entry& entry,
    const std::shared_ptr<ITransport>& transport, bool drain = true);
  void DrainRecovery(Entry& entry,
    const std::shared_ptr<ITransport>& transport);

  OpenResult OpenEntry(Entry& entry);
  bool InitializeEntry(Entry& entry);
  bool SetupEntry(Entry& entry, size_t alignment);
  bool AddServices(Entry& entry);
  void HandleServiceFailures();
  void RetryEntry(Entry& entry, uint64_t now, bool initialized,
    bool setup, size_t alignment);
  void HandleProcessResult(Entry& entry, ProcessResult result);
  void ScheduleRetry(Entry& entry);
  void RemoveServices(Entry& entry);
  void Expose(Entry& entry);

public:
  CTransportManager();
  ~CTransportManager();

  CTransportManager(const CTransportManager&) = delete;
  CTransportManager& operator=(const CTransportManager&) = delete;

  bool Add(TransportInstance config, bool primary, CreateFn create);

  OpenResult Open();
  bool Initialize();
  bool Setup(size_t alignment);
  ProcessResult Process(ITransportEvents& events);
  void Stop();
  void SyncRecovery();
  void RecoveryStatus(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active,
    Recovery state, uint32_t error);

  bool CanUseMode(const FrameMode& mode,
    uint32_t * requiredSizeMiB = nullptr) const override;
  DirectFrameBufferMemory GetDirectMemory() const;

  IFrameTransport& Frames();
  IControlTransport& Control();
  IInputTransport& Input();
};
