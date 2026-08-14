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

#include "transport/CTransportManager.h"

#include "Atomic.h"
#include "capture/CFrameGraph.h"
#include "CDebug.h"
#include "ipc/CPipeServer.h"
#include "Seq.h"
#include "transport/IClipboardSource.h"
#include "transport/ITexStage.h"

#include <Windows.h>
#include <new>
#include <utility>

static const uint64_t RETRY_DELAY_MS            = 500;
static const uint64_t FRAME_RETRY_DELAY_MS      = 500;
static const uint64_t SERVICE_RETRY_DELAY_MS    = 250;
static std::atomic<uint64_t> s_graphGeneration  = 0;

class CSourceEvents final : public ITransportEvents
{
private:
  BackendId         m_backend;
  uint32_t          m_epoch;
  bool              m_interactions;
  CInputHub&         m_input;
  CRecoveryHub&      m_recovery;
  ITransportActions& m_actions;

  SourceKey Stamp(const SourceKey& source) const
  {
    SourceKey stamped = source;
    stamped.backend = m_backend;
    stamped.epoch   = m_epoch;
    return stamped;
  }

public:
  CSourceEvents(
    BackendId backend, uint32_t epoch, bool interactions, CInputHub& input,
    CRecoveryHub& recovery, ITransportActions& actions) :
    m_backend(backend), m_epoch(epoch), m_interactions(interactions),
    m_input(input), m_recovery(recovery), m_actions(actions) {}

  InteractionResult OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) override
  {
    if (!m_interactions)
      return InteractionResult::UNAVAILABLE;
    SourceKey stamped = Stamp(source);
    CInputHub::InteractionPermit permit;
    const InteractionResult result =
      m_input.CheckInteraction(stamped, permit);
    if (result != InteractionResult::ACCEPTED)
      return result;
    const InteractionResult applied =
      m_actions.OnSetCursorPos(stamped, x, y);
    if (applied == InteractionResult::ACCEPTED)
      m_input.CommitInteraction(stamped, permit);
    return applied;
  }

  InteractionResult OnSetResolution(const SourceKey& source,
    uint32_t width, uint32_t height) override
  {
    if (!m_interactions)
      return InteractionResult::UNAVAILABLE;
    SourceKey stamped = Stamp(source);
    CInputHub::InteractionPermit permit;
    const InteractionResult result =
      m_input.CheckInteraction(stamped, permit);
    if (result != InteractionResult::ACCEPTED)
      return result;
    const InteractionResult applied =
      m_actions.OnSetResolution(stamped, width, height);
    if (applied == InteractionResult::ACCEPTED)
      m_input.CommitInteraction(stamped, permit);
    return applied;
  }

  RecoveryAdmission OnRecoveryRequest(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active) override
  {
    RecoveryAction action;
    bool dispatch = false;
    const RecoveryAdmission admission = m_recovery.Submit(
      Stamp(source), session, serial, active, GetTickCount64(),
      action, dispatch);
    if (dispatch && !m_actions.OnRecoveryAction(action))
      m_recovery.DispatchFailed(action, RPC_S_SERVER_UNAVAILABLE);
    return admission;
  }
};

CTransportManager::Entry::Entry()
{
  idleEvent = CreateEvent(nullptr, TRUE, TRUE, nullptr);
}

CTransportManager::Entry::~Entry()
{
  if (idleEvent)
    CloseHandle(idleEvent);
}

CTransportManager::CTransportManager() :
  m_tex(m_frameRev), m_clipboard(g_pipe.Clipboard())
{
  m_phaseIdle    = CreateEvent(nullptr, TRUE, TRUE, nullptr);
  m_stoppedEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

uint64_t CTransportManager::FrameRev() const
{
  return Atomic::Load(m_frameRev, std::memory_order_acquire);
}

void CTransportManager::BumpFrameRev()
{
  Atomic::Next(m_frameRev, std::memory_order_release);
}

void CTransportManager::ScheduleFrameRetry(Entry& entry)
{
  CSRWExclusiveLock entryLock(entry.lock);
  if (!entry.frameRetryAt)
    entry.frameRetryAt = GetTickCount64() + FRAME_RETRY_DELAY_MS;
}

void CTransportManager::RetryFrame(Entry& entry, uint64_t now)
{
  bool retry = false;
  {
    CSRWExclusiveLock entryLock(entry.lock);
    if (!entry.frameRetryAt || now < entry.frameRetryAt)
      return;
    entry.frameRetryAt = 0;
    retry = entry.state == State::READY && entry.frameAdded;
  }
  if (retry)
    BumpFrameRev();
}

CTransportManager::~CTransportManager()
{
  Stop();
  if (m_stoppedEvent)
    CloseHandle(m_stoppedEvent);
  if (m_phaseIdle)
    CloseHandle(m_phaseIdle);
}

unsigned CTransportManager::Entries(
  Entry * entries[FRAME_MAX_SINKS]) const
{
  CSRWSharedLock managerLock(m_lock);
  for (unsigned i = 0; i < m_entryCount; ++i)
    entries[i] = m_entries[i].get();
  return m_entryCount;
}

CTransportManager::Entry * CTransportManager::Primary() const
{
  CSRWSharedLock managerLock(m_lock);
  return m_primary;
}

bool CTransportManager::BeginPhase(
  Phase phase, bool wait, bool stopping)
{
  const DWORD thread = GetCurrentThreadId();
  for (;;)
  {
    HANDLE idleEvent = nullptr;
    {
      CSRWExclusiveLock managerLock(m_lock);
      if ((!stopping && (m_stopping || m_stopped)) ||
          (stopping && m_stopped))
        return false;

      if (m_phase == Phase::IDLE)
      {
        m_phase      = phase;
        m_phaseOwner = thread;
        ResetEvent(m_phaseIdle);
        return true;
      }

      if (!wait || m_phaseOwner == thread)
        return false;
      idleEvent = m_phaseIdle;
    }

    if (!idleEvent ||
        WaitForSingleObject(idleEvent, INFINITE) != WAIT_OBJECT_0)
      return false;
  }
}

void CTransportManager::EndPhase()
{
  CSRWExclusiveLock managerLock(m_lock);
  m_phase      = Phase::IDLE;
  m_phaseOwner = 0;
  SetEvent(m_phaseIdle);
}

bool CTransportManager::BeginCall(
  Entry& entry, Call call, bool wait)
{
  const DWORD thread = GetCurrentThreadId();
  for (;;)
  {
    HANDLE idleEvent = nullptr;
    {
      CSRWExclusiveLock entryLock(entry.lock);
      if (entry.stopRequested || entry.state == State::STOPPED)
        return false;

      if (entry.call == Call::IDLE)
      {
        entry.call      = call;
        entry.callOwner = thread;
        ResetEvent(entry.idleEvent);
        return true;
      }

      if (!wait || entry.callOwner == thread)
        return false;
      idleEvent = entry.idleEvent;
    }

    if (!idleEvent ||
        WaitForSingleObject(idleEvent, INFINITE) != WAIT_OBJECT_0)
      return false;
  }
}

void CTransportManager::DrainRecovery(Entry& entry,
  const std::shared_ptr<ITransport>& transport)
{
  static const unsigned MAX_DRAIN = 17;
  for (unsigned i = 0; i < MAX_DRAIN; ++i)
  {
    bool sync = false;
    BackendId id = 0;
    uint32_t epoch = 0;
    {
      CSRWExclusiveLock entryLock(entry.lock);
      if (entry.stopRequested || !transport ||
          transport != entry.transport ||
          (entry.state != State::INITIALIZED &&
           entry.state != State::READY))
      {
        entry.syncPending = false;
        return;
      }

      id                = entry.id;
      epoch             = entry.epoch;
      sync              = entry.syncPending;
      entry.syncPending = false;
    }

    if (sync)
      transport->SyncRecovery();

    CRecoveryHub::Delivery delivery;
    if (!m_recovery.TakeDelivery(id, epoch, delivery))
    {
      if (!sync)
        return;
      continue;
    }
    transport->RecoveryStatus(delivery.source, delivery.session,
      delivery.serial, delivery.active, delivery.state, delivery.error);
  }
}

void CTransportManager::EndCall(Entry& entry,
  const std::shared_ptr<ITransport>& transport, bool drain)
{
  for (;;)
  {
    if (drain)
      DrainRecovery(entry, transport);

    CSRWExclusiveLock entryLock(entry.lock);
    if (drain && entry.syncPending)
      continue;

    entry.call      = Call::IDLE;
    entry.callOwner = 0;
    SetEvent(entry.idleEvent);
    return;
  }
}

void CTransportManager::DetachRecovery(Entry& entry)
{
  BackendId id = 0;
  uint32_t epoch = 0;
  bool attached = false;
  {
    CSRWExclusiveLock entryLock(entry.lock);
    id                     = entry.id;
    epoch                  = entry.epoch;
    attached               = entry.recoveryAttached;
    entry.recoveryAttached = false;
    entry.syncPending      = false;
  }
  if (attached)
    m_recovery.Remove(id, epoch);
}

bool CTransportManager::Add(TransportInstance config, bool primary,
  CreateFn create)
{
  CSRWExclusiveLock managerLock(m_lock);
  if (!m_phaseIdle || !m_stoppedEvent || m_phase != Phase::IDLE ||
      m_started || m_stopping ||
      m_stopped || !config.enabled || !config.id || config.kind.empty() ||
      (config.services & ~TRANSPORT_SERVICE_ALL) || !create ||
      m_entryCount == FRAME_MAX_SINKS || (primary && m_primary))
    return false;

  if (primary && (!config.required ||
      !(config.services & TRANSPORT_SERVICE_FRAME)))
    return false;

  for (unsigned i = 0; i < m_entryCount; ++i)
    if (m_entries[i]->id == config.id)
      return false;

  std::unique_ptr<Entry> entry(new (std::nothrow) Entry);
  if (!entry || !entry->idleEvent)
    return false;

  entry->id       = config.id;
  entry->required = config.required;
  entry->primary  = primary;
  entry->create   = create;
  entry->config   = std::move(config);

  Entry * raw = entry.get();
  m_entries[m_entryCount++] = std::move(entry);
  if (primary)
    m_primary = raw;
  return true;
}

ITransport::OpenResult CTransportManager::OpenEntry(Entry& entry)
{
  std::shared_ptr<ITransport> transport;
  CreateFn create = nullptr;
  TransportInstance config;
  {
    CSRWSharedLock entryLock(entry.lock);
    transport = entry.transport;
    create    = entry.create;
    config    = entry.config;
  }

  if (!transport)
  {
    std::unique_ptr<ITransport> created = create(config);
    transport.reset(created.release());
    CSRWExclusiveLock entryLock(entry.lock);
    entry.transport = transport;
  }

  if (!transport)
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return OpenResult::FAILURE;
  }

  const OpenResult result = transport->Open();
  switch (result)
  {
    case OpenResult::SUCCESS:
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.state = State::OPEN;
      break;
    }

    case OpenResult::RETRY:
      ScheduleRetry(entry);
      break;

    case OpenResult::FAILURE:
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.state = State::FAILED;
      break;
    }
  }
  return result;
}

bool CTransportManager::InitializeEntry(Entry& entry)
{
  std::shared_ptr<ITransport> transport;
  BackendId id = 0;
  uint32_t epoch = 0;
  uint32_t services = 0;
  bool required = false;
  {
    CSRWSharedLock entryLock(entry.lock);
    if (entry.state == State::INITIALIZED || entry.state == State::READY)
      return true;
    if (entry.state != State::OPEN)
      return false;
    transport = entry.transport;
    id        = entry.id;
    epoch     = entry.epoch;
    services  = entry.config.services;
    required  = entry.required;
  }

  if (!transport || !transport->Initialize())
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return false;
  }

  std::shared_ptr<const FrameCaps> frameCaps;
  if (services & TRANSPORT_SERVICE_FRAME)
  {
    frameCaps = transport->GetFrameCaps();
    if (!frameCaps && required)
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.state = State::FAILED;
      return false;
    }
  }
  const bool hasFrameCaps = frameCaps != nullptr;
  bool syncNow = false;
  if (!m_recovery.Attach(id, epoch, syncNow))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return false;
  }
  {
    CSRWExclusiveLock entryLock(entry.lock);
    // The first successfully initialized instance fixes the advertised
    // capability contract. Recreating an instance must not change the mode
    // list during a runtime restart.
    if (!entry.frameCaps)
      entry.frameCaps = std::move(frameCaps);
    // A best-effort instance without a capability contract can continue to
    // provide its other configured services, but must not receive frames.
    entry.frameAbsent = (services & TRANSPORT_SERVICE_FRAME) &&
      !hasFrameCaps;
    entry.recoveryAttached = true;
    entry.syncPending      = syncNow;
    entry.state            = State::INITIALIZED;
  }
  // Monitor readiness may be published after Attach but before this entry is
  // visible to SyncRecovery. Claim any synchronization missed in that gap.
  if (m_recovery.ClaimSync(id, epoch))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    if (entry.id == id && entry.epoch == epoch && entry.recoveryAttached)
      entry.syncPending = true;
  }
  return true;
}

bool CTransportManager::AddServices(Entry& entry)
{
  std::shared_ptr<ITransport> transport;
  BackendId id              = 0;
  uint32_t  epoch           = 0;
  bool      primary         = false;
  bool      required        = false;
  uint32_t  services        = 0;
  bool      controlAdded    = false;
  bool      controlFailed   = false;
  bool      controlAbsent   = false;
  bool      inputAdded      = false;
  bool      inputFailed     = false;
  bool      inputAbsent     = false;
  bool      clipboardAdded  = false;
  bool      clipboardFailed = false;
  bool      clipboardAbsent = false;
  bool      frameAdded      = false;
  bool      frameBound      = false;
  bool      frameAbsent     = false;
  uint64_t  retryAt         = 0;
  {
    CSRWSharedLock entryLock(entry.lock);
    transport       = entry.transport;
    id              = entry.id;
    epoch           = entry.epoch;
    primary         = entry.primary;
    required        = entry.required;
    services        = entry.config.services;
    controlAdded    = entry.controlAdded;
    controlFailed   = entry.controlFailed;
    controlAbsent   = entry.controlAbsent;
    inputAdded      = entry.inputAdded;
    inputFailed     = entry.inputFailed;
    inputAbsent     = entry.inputAbsent;
    clipboardAdded  = entry.clipboardAdded;
    clipboardFailed = entry.clipboardFailed;
    clipboardAbsent = entry.clipboardAbsent;
    frameAdded      = entry.frameAdded;
    frameAbsent     = entry.frameAbsent;
    retryAt         = entry.serviceRetryAt;
  }

  if (!transport)
    return !required;

  const uint64_t now    = GetTickCount64();
  const bool     attach = now >= retryAt;
  bool controlRetry   = false;
  bool frameRetry     = false;
  bool inputRetry     = false;
  bool clipboardRetry = false;
  if (!(services & TRANSPORT_SERVICE_CONTROL))
    controlAbsent = true;
  else if (attach && !controlAdded && !controlFailed && !controlAbsent)
  {
    IControlSink * control = transport->Control();
    if (control && m_control.Add(id, epoch, *control))
    {
      controlAdded = true;
      CSRWExclusiveLock entryLock(entry.lock);
      entry.controlAdded = true;
    }
    else if (control)
      controlRetry = true;
    else
    {
      controlAbsent = true;
      CSRWExclusiveLock entryLock(entry.lock);
      entry.controlAbsent = true;
    }
  }

  if (!(services & TRANSPORT_SERVICE_FRAME))
    frameAbsent = true;
  else if (attach && !frameAdded && !frameAbsent)
  {
    IFrameSink * frame = transport->FrameSink();
    ITexSink * tex = transport->TexSink();
    if (frame && m_frames.Bind(id, epoch, primary, *frame))
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.frameAdded  = true;
      entry.frameLegacy = true;
      entry.texSink     = nullptr;
      frameAdded        = true;
      frameBound        = true;
    }
    else if (!frame && tex)
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.frameAdded  = true;
      entry.frameLegacy = false;
      entry.texSink     = tex;
      frameAdded        = true;
      frameBound        = true;
    }
    else if (frame || primary)
      frameRetry = true;
    else
    {
      frameAbsent = true;
      CSRWExclusiveLock entryLock(entry.lock);
      entry.frameAbsent = true;
    }
  }

  if (!(services & TRANSPORT_SERVICE_INPUT))
    inputAbsent = true;
  else if (attach && !inputAdded && !inputFailed && !inputAbsent)
  {
    IInputSource * input = transport->Input();
    if (input && m_input.Bind(id, epoch, *input))
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.inputAdded = true;
      inputAdded = true;
    }
    else if (input)
      inputRetry = true;
    else
    {
      inputAbsent = true;
      CSRWExclusiveLock entryLock(entry.lock);
      entry.inputAbsent = true;
    }
  }

  if (!(services & TRANSPORT_SERVICE_CLIPBOARD))
    clipboardAbsent = true;
  else if (attach && !clipboardAdded && !clipboardFailed &&
      !clipboardAbsent)
  {
    IClipboardSource * clipboard = transport->Clipboard();
    if (clipboard && m_clipboard.Bind(id, epoch, *clipboard))
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.clipboardAdded = true;
      clipboardAdded       = true;
    }
    else if (clipboard)
      clipboardRetry = true;
    else
    {
      clipboardAbsent = true;
      CSRWExclusiveLock entryLock(entry.lock);
      entry.clipboardAbsent = true;
    }
  }

  if (attach && (controlRetry || inputRetry || frameRetry ||
      clipboardRetry))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.serviceRetryAt = now + SERVICE_RETRY_DELAY_MS;
  }

  if (frameBound)
  {
    m_tex.Rebind(id, epoch);
    BumpFrameRev();
  }

  const bool servicesReady =
    (!(services & TRANSPORT_SERVICE_FRAME)     || frameAdded)     &&
    (!(services & TRANSPORT_SERVICE_CONTROL)   || controlAdded)   &&
    (!(services & TRANSPORT_SERVICE_INPUT)     || inputAdded)     &&
    (!(services & TRANSPORT_SERVICE_CLIPBOARD) || clipboardAdded);
  return !required || servicesReady;
}

void CTransportManager::HandleServiceFailures()
{
  BackendId frameId = 0;
  uint32_t frameEpoch = 0;
  while (m_tex.TakeFailure(frameId, frameEpoch))
  {
    Entry * entries[FRAME_MAX_SINKS] = {};
    const unsigned count = Entries(entries);
    for (unsigned i = 0; i < count; ++i)
    {
      Entry& entry = *entries[i];
      bool restart = false;
      {
        CSRWSharedLock entryLock(entry.lock);
        if (entry.id != frameId || entry.epoch != frameEpoch ||
            !entry.frameAdded || !entry.texSink)
          continue;
        restart = !entry.exposed;
      }

      DetachRecovery(entry);
      RemoveServices(entry);
      if (restart)
        ScheduleRetry(entry);
      else
      {
        CSRWExclusiveLock entryLock(entry.lock);
        entry.state = State::FAILED;
      }
      break;
    }
  }

  ControlToken token;
  while (m_control.TakeFailure(token))
  {
    Entry * entries[FRAME_MAX_SINKS] = {};
    const unsigned count = Entries(entries);
    for (unsigned i = 0; i < count; ++i)
    {
      Entry& entry = *entries[i];
      bool restart = false;
      {
        CSRWExclusiveLock entryLock(entry.lock);
        if (entry.id != token.backend || entry.epoch != token.epoch ||
            !entry.controlAdded)
          continue;
        entry.controlAdded  = false;
        entry.controlFailed = true;
        restart = !entry.exposed;
      }

      m_control.Remove(token.backend, token.epoch);
      m_input.RevokeInteraction(token.backend, token.epoch);
      if (restart)
      {
        RemoveServices(entry);
        ScheduleRetry(entry);
      }
      break;
    }
  }

  SourceKey source;
  while (m_input.TakeFailure(source))
  {
    Entry * entries[FRAME_MAX_SINKS] = {};
    const unsigned count = Entries(entries);
    for (unsigned i = 0; i < count; ++i)
    {
      Entry& entry = *entries[i];
      bool restart = false;
      {
        CSRWExclusiveLock entryLock(entry.lock);
        if (entry.id != source.backend || entry.epoch != source.epoch ||
            !entry.inputAdded)
          continue;
        entry.inputAdded  = false;
        entry.inputFailed = true;
        restart = !entry.exposed;
      }

      m_input.Unbind(source.backend, source.epoch);
      if (restart)
      {
        RemoveServices(entry);
        ScheduleRetry(entry);
      }
      break;
    }
  }

  source = {};
  while (m_clipboard.TakeFailure(source))
  {
    Entry * entries[FRAME_MAX_SINKS] = {};
    const unsigned count = Entries(entries);
    for (unsigned i = 0; i < count; ++i)
    {
      Entry& entry = *entries[i];
      bool restart = false;
      {
        CSRWExclusiveLock entryLock(entry.lock);
        if (entry.id != source.backend || entry.epoch != source.epoch ||
            !entry.clipboardAdded)
          continue;
        entry.clipboardAdded  = false;
        entry.clipboardFailed = true;
        restart = !entry.exposed;
      }

      m_clipboard.Unbind(source.backend, source.epoch);
      if (restart)
      {
        RemoveServices(entry);
        ScheduleRetry(entry);
      }
      break;
    }
  }
}

bool CTransportManager::SetupEntry(Entry& entry, size_t alignment)
{
  std::shared_ptr<ITransport> transport;
  {
    CSRWSharedLock entryLock(entry.lock);
    if (entry.state != State::INITIALIZED && entry.state != State::READY)
      return false;
    transport = entry.transport;
  }

  bool setupDone = false;
  {
    CSRWSharedLock entryLock(entry.lock);
    setupDone = entry.setupDone;
  }

  if (!transport || (!setupDone && !transport->Setup(alignment)))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return false;
  }

  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.setupDone = true;
  }
  if (!AddServices(entry))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::INITIALIZED;
    return false;
  }

  CSRWExclusiveLock entryLock(entry.lock);
  entry.state = State::READY;
  return true;
}

void CTransportManager::ScheduleRetry(Entry& entry)
{
  CSRWExclusiveLock entryLock(entry.lock);
  entry.state   = State::RETRY;
  entry.retryAt = GetTickCount64() + RETRY_DELAY_MS;
}

void CTransportManager::RemoveServices(Entry& entry)
{
  BackendId id             = 0;
  uint32_t  epoch          = 0;
  bool      frameAdded     = false;
  bool      frameLegacy    = false;
  bool      controlAdded   = false;
  bool      inputAdded     = false;
  bool      clipboardAdded = false;
  {
    CSRWExclusiveLock entryLock(entry.lock);
    id                   = entry.id;
    epoch                = entry.epoch;
    frameAdded           = entry.frameAdded;
    frameLegacy          = entry.frameLegacy;
    controlAdded         = entry.controlAdded;
    inputAdded           = entry.inputAdded;
    clipboardAdded       = entry.clipboardAdded;
    entry.frameAdded     = false;
    entry.frameLegacy    = false;
    entry.texSink        = nullptr;
    entry.controlAdded   = false;
    entry.inputAdded     = false;
    entry.clipboardAdded = false;
    entry.frameRetryAt   = 0;
  }

  m_input.RevokeInteraction(id, epoch);
  m_tex.Drop(id, epoch);
  if (inputAdded)
    m_input.Unbind(id, epoch);
  if (clipboardAdded)
    m_clipboard.Unbind(id, epoch);
  if (controlAdded)
    m_control.Remove(id, epoch);
  if (frameLegacy)
    m_frames.Unbind(id, epoch);
  if (frameAdded)
  {
    BumpFrameRev();
  }
}

void CTransportManager::RetryEntry(Entry& entry, uint64_t now,
  bool initialized, bool setup, size_t alignment)
{
  std::shared_ptr<ITransport> transport;
  {
    CSRWSharedLock entryLock(entry.lock);
    if (entry.state != State::RETRY || now < entry.retryAt ||
        (entry.primary && entry.exposed))
      return;
    transport = entry.transport;
  }

  DetachRecovery(entry);
  RemoveServices(entry);
  if (transport)
    transport->Stop();

  {
    CSRWExclusiveLock entryLock(entry.lock);
    const bool frameService =
      (entry.config.services & TRANSPORT_SERVICE_FRAME) != 0;
    entry.transport.reset();
    entry.directMemory      = DirectFrameBufferMemory {};
    entry.directMemoryValid = false;
    entry.setupDone         = false;
    entry.controlFailed     = false;
    entry.controlAbsent     = false;
    entry.inputFailed       = false;
    entry.inputAbsent       = false;
    entry.clipboardFailed   = false;
    entry.clipboardAbsent   = false;
    entry.frameAbsent       = false;
    entry.serviceRetryAt    = 0;
    Seq::Inc(entry.epoch);
    if (frameService)
      BumpFrameRev();
  }

  if (OpenEntry(entry) != OpenResult::SUCCESS)
    return;
  if (initialized && !InitializeEntry(entry))
  {
    ScheduleRetry(entry);
    return;
  }
  if (setup && !SetupEntry(entry, alignment))
  {
    CSRWSharedLock entryLock(entry.lock);
    if (entry.state == State::FAILED)
    {
      entryLock.Unlock();
      ScheduleRetry(entry);
    }
  }
}

void CTransportManager::HandleProcessResult(
  Entry& entry, ProcessResult result)
{
  if (result == ProcessResult::OK)
    return;

  bool exposed = false;
  bool primary = false;
  std::wstring name;
  {
    CSRWSharedLock entryLock(entry.lock);
    exposed  = entry.exposed;
    primary  = entry.primary;
    name     = entry.config.kind;
  }

  DetachRecovery(entry);
  RemoveServices(entry);
  if (primary && exposed)
  {
    DEBUG_WARN("Transport %ls stopped while its frame interfaces are active",
      name.c_str());
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return;
  }

  if (result == ProcessResult::RETRY)
  {
    ScheduleRetry(entry);
    return;
  }

  CSRWExclusiveLock entryLock(entry.lock);
  entry.state = State::FAILED;
}

void CTransportManager::Expose(Entry& entry)
{
  CSRWExclusiveLock entryLock(entry.lock);
  entry.exposed          = true;
  entry.exposedTransport = entry.transport;
}

ITransport::OpenResult CTransportManager::Open()
{
  if (!BeginPhase(Phase::OPEN, true))
    return OpenResult::FAILURE;

  {
    CSRWExclusiveLock managerLock(m_lock);
    m_started = true;
  }

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  OpenResult aggregate = Primary() ?
    OpenResult::SUCCESS : OpenResult::FAILURE;
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    if (!BeginCall(entry, Call::LIFECYCLE, true))
    {
      if (entry.required)
        aggregate = OpenResult::FAILURE;
      continue;
    }

    bool alreadyOpen = false;
    std::shared_ptr<ITransport> transport;
    {
      CSRWSharedLock entryLock(entry.lock);
      alreadyOpen = entry.state == State::OPEN ||
        entry.state == State::INITIALIZED || entry.state == State::READY;
      transport = entry.transport;
    }

    const OpenResult result = alreadyOpen ?
      OpenResult::SUCCESS : OpenEntry(entry);
    {
      CSRWSharedLock entryLock(entry.lock);
      transport = entry.transport;
    }
    EndCall(entry, transport);

    if (entry.required && result == OpenResult::FAILURE)
      aggregate = OpenResult::FAILURE;
    else if (entry.required && result == OpenResult::RETRY &&
             aggregate == OpenResult::SUCCESS)
      aggregate = OpenResult::RETRY;
  }

  EndPhase();
  return aggregate;
}

bool CTransportManager::Initialize()
{
  if (!BeginPhase(Phase::INITIALIZE, true))
    return false;

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  bool success = true;
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    if (!BeginCall(entry, Call::LIFECYCLE, true))
    {
      if (entry.required)
        success = false;
      continue;
    }

    State state;
    std::shared_ptr<ITransport> transport;
    {
      CSRWSharedLock entryLock(entry.lock);
      state     = entry.state;
      transport = entry.transport;
    }

    if (state == State::OPEN && !InitializeEntry(entry))
    {
      if (entry.required)
        success = false;
      else
        ScheduleRetry(entry);
    }
    {
      CSRWSharedLock entryLock(entry.lock);
      transport = entry.transport;
      if (entry.required && entry.state != State::INITIALIZED &&
          entry.state != State::READY)
        success = false;
    }
    EndCall(entry, transport);
  }

  Entry * primary = Primary();
  if (success && primary)
  {
    CSRWSharedLock entryLock(primary->lock);
    success = primary->state == State::INITIALIZED ||
      primary->state == State::READY;
  }
  else
    success = false;

  if (success)
  {
    CSRWExclusiveLock managerLock(m_lock);
    m_initialized = true;
  }
  EndPhase();
  return success;
}

bool CTransportManager::Setup(size_t alignment)
{
  if (!BeginPhase(Phase::SETUP, true))
    return false;

  bool initialized = false;
  {
    CSRWExclusiveLock managerLock(m_lock);
    initialized = m_initialized && m_primary;
    if (initialized)
      m_alignment = alignment;
  }

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  bool success = initialized;
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    if (!BeginCall(entry, Call::LIFECYCLE, true))
    {
      if (entry.required)
        success = false;
      continue;
    }

    State state;
    std::shared_ptr<ITransport> transport;
    {
      CSRWSharedLock entryLock(entry.lock);
      state     = entry.state;
      transport = entry.transport;
    }

    if (state != State::INITIALIZED && state != State::READY)
    {
      if (entry.required)
        success = false;
    }
    else if (!SetupEntry(entry, alignment))
    {
      {
        CSRWSharedLock entryLock(entry.lock);
        state = entry.state;
      }
      if (entry.required)
        success = false;
      else if (state == State::FAILED)
        ScheduleRetry(entry);
    }
    {
      CSRWSharedLock entryLock(entry.lock);
      transport = entry.transport;
    }
    EndCall(entry, transport);
  }

  Entry * primary = Primary();
  if (primary)
  {
    CSRWSharedLock entryLock(primary->lock);
    if (primary->state != State::READY || !primary->frameAdded)
      success = false;
  }
  else
    success = false;

  if (initialized)
  {
    CSRWExclusiveLock managerLock(m_lock);
    m_setup = true;
  }
  EndPhase();
  return success;
}

CfgResult CTransportManager::Cfg(const GraphCfg& cfg,
  uint64_t revision, CFrameGraph& graph, ITexStage * activation)
{
  if (!revision || revision != FrameRev())
    return CfgResult::RETRY;

  CFrameGraph next;
  if (!next.Begin(cfg))
    return CfgResult::REJECTED;
  if (!BeginPhase(Phase::CFG, true))
    return CfgResult::RETRY;

  CTexStage texStage;
  CfgResult held = m_tex.Hold(texStage);
  if (held != CfgResult::ACCEPTED)
  {
    m_tex.Abort(texStage);
    EndPhase();
    return held;
  }

  struct Route
  {
    Entry                     * entry        = nullptr;
    std::shared_ptr<ITransport> transport;
    BackendId                   id           = 0;
    uint32_t                    epoch        = 0;
    bool                        required     = false;
    bool                        primary      = false;
    bool                        prepared     = false;
    bool                        selected     = false;
    bool                        eligible     = false;
    ITexSink                  * texSink      = nullptr;
    std::wstring                name;
    CfgResult                   outcome      = CfgResult::NEXT;
    FrameProfile                profiles[FRAME_PROFILE_MAX] = {};
    unsigned                    profileCount = 0;
  };

  Route routes[FRAME_MAX_SINKS];
  unsigned routeCount = 0;
  CfgResult result = revision == FrameRev() ?
    CfgResult::ACCEPTED : CfgResult::RETRY;

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    bool frameService = false;
    bool required     = false;
    bool primary      = false;
    {
      CSRWSharedLock entryLock(entry.lock);
      frameService =
        (entry.config.services & TRANSPORT_SERVICE_FRAME) != 0;
      required = entry.required;
      primary  = entry.primary;
    }
    // Software capture writes directly into the primary frame target.
    // Secondary routes add fan-out work and can require staging copies.
    if (!frameService)
      continue;
    if (cfg.mode == GpuMode::SOFTWARE && !primary)
    {
      if (required)
      {
        result = CfgResult::REJECTED;
        break;
      }
      continue;
    }

    if (!BeginCall(entry, Call::CFG, true))
    {
      if (required)
      {
        result = CfgResult::RETRY;
        break;
      }
      continue;
    }

    Route& route = routes[routeCount++];
    route.entry = &entry;
    State state;
    bool frameAbsent = false;
    bool frameAdded = false;
    {
      CSRWSharedLock entryLock(entry.lock);
      route.transport = entry.transport;
      route.id        = entry.id;
      route.epoch     = entry.epoch;
      route.required  = entry.required;
      route.primary   = entry.primary;
      route.texSink   = entry.texSink;
      route.name      = entry.config.kind;
      state           = entry.state;
      frameAbsent     = entry.frameAbsent;
      frameAdded      = entry.frameAdded;
    }

    route.eligible = route.transport && !frameAbsent &&
      frameAdded && state == State::READY;
    if (!route.eligible)
    {
      if (!route.required &&
          (frameAbsent || state == State::FAILED))
      {
        if (frameAbsent)
          route.outcome = CfgResult::REJECTED;
        continue;
      }
      result = frameAbsent ? CfgResult::REJECTED :
        (state == State::FAILED ? CfgResult::FAILED : CfgResult::RETRY);
      route.outcome = result;
      break;
    }
    if (route.texSink && !activation)
    {
      route.eligible = false;
      route.outcome  = CfgResult::REJECTED;
      if (route.required)
      {
        result = CfgResult::REJECTED;
        break;
      }
      continue;
    }

    unsigned profileCount = 0;
    const FrameProfile * profiles =
      route.transport->Profiles(profileCount);
    if (profileCount > FRAME_PROFILE_MAX ||
        (profileCount && !profiles))
    {
      route.eligible = false;
      if (route.required)
      {
        result = CfgResult::FAILED;
        break;
      }
      continue;
    }
    route.profileCount = profileCount;
    for (unsigned profile = 0; profile < profileCount; ++profile)
      route.profiles[profile] = profiles[profile];
  }

  if (result == CfgResult::ACCEPTED)
    for (unsigned i = 0; i < routeCount; ++i)
    {
      Route& route = routes[i];
      if (!route.eligible)
        continue;

      CfgResult routeResult = CfgResult::NEXT;
      for (unsigned profileIndex = 0;
           profileIndex < route.profileCount; ++profileIndex)
      {
        const FrameProfile& profile = route.profiles[profileIndex];
        FrameCfg candidate;
        candidate.mode    = cfg.mode;
        candidate.adapter = cfg.adapter;
        candidate.width   = cfg.width;
        candidate.height  = cfg.height;
        candidate.profile = profile;
        if (!next.Can(candidate))
          continue;

        if (route.texSink)
        {
          const CfgResult fault =
            m_tex.Faulted(route.id, route.epoch, candidate);
          if (fault == CfgResult::FAILED)
          {
            routeResult = fault;
            break;
          }
          if (fault == CfgResult::REJECTED)
          {
            routeResult = fault;
            continue;
          }
        }

        const bool tex = route.texSink != nullptr;
        const GraphLeaf * previous = graph.FindLeaf(
          route.id, route.epoch, tex, candidate);
        if (previous && previous->required == route.required &&
            previous->primary == route.primary &&
            (!tex || m_tex.Active(graph.Generation(),
              route.id, route.epoch, candidate)))
        {
          if (!next.Add(route.id, route.epoch, route.required,
                route.primary, tex, graph.Same(cfg), candidate))
          {
            routeResult = CfgResult::FAILED;
            break;
          }
          routeResult    = CfgResult::ACCEPTED;
          route.selected = true;
          break;
        }

        routeResult = route.transport->Probe(candidate);
        if (routeResult == CfgResult::NEXT)
          continue;
        if (routeResult == CfgResult::REJECTED)
          ScheduleFrameRetry(*route.entry);
        if (routeResult != CfgResult::ACCEPTED)
          break;

        routeResult = route.transport->Prepare(candidate);
        if (routeResult == CfgResult::NEXT)
        {
          route.transport->Abort();
          continue;
        }
        if (routeResult == CfgResult::REJECTED)
          ScheduleFrameRetry(*route.entry);
        if (routeResult != CfgResult::ACCEPTED)
          break;

        if (!next.Add(route.id, route.epoch, route.required,
              route.primary, tex, false, candidate))
        {
          route.transport->Abort();
          routeResult = CfgResult::FAILED;
          break;
        }
        route.prepared = true;
        route.selected = true;
        break;
      }

      if (route.selected)
      {
        route.outcome = CfgResult::ACCEPTED;
        continue;
      }

      route.transport->Abort();
      route.outcome = routeResult == CfgResult::NEXT ?
        CfgResult::REJECTED : routeResult;
      if (routeResult == CfgResult::RETRY)
      {
        result = CfgResult::RETRY;
        break;
      }
      if (route.required)
      {
        result = route.outcome;
        break;
      }
    }

  if (result == CfgResult::ACCEPTED && !next.Seal())
    result = CfgResult::FAILED;

  if (result == CfgResult::ACCEPTED &&
      !next.Stamp(Atomic::Next(s_graphGeneration)))
    result = CfgResult::FAILED;

  CTexHub::Bind binds[FRAME_MAX_SINKS];
  unsigned bindCount = 0;
  if (result == CfgResult::ACCEPTED)
    for (unsigned i = 0; i < routeCount; ++i)
      if (routes[i].selected)
      {
        CTexHub::Bind& bind = binds[bindCount++];
        bind.owner = routes[i].transport;
        bind.id    = routes[i].id;
        bind.epoch = routes[i].epoch;
        bind.sink  = routes[i].texSink;
      }

  bool activationReady = false;
  bool committed       = false;
  if (result == CfgResult::ACCEPTED && activation)
  {
    result = activation->Prep(next);
    activationReady = result == CfgResult::ACCEPTED;
  }
  if (result == CfgResult::ACCEPTED)
    result = m_tex.Prep(next, binds, bindCount, texStage);
  if (result == CfgResult::ACCEPTED && revision != FrameRev())
    result = CfgResult::RETRY;

  if (result == CfgResult::ACCEPTED)
  {
    for (unsigned i = 0; i < routeCount; ++i)
    {
      if (routes[i].prepared)
        routes[i].transport->Commit();
      if (routes[i].selected)
      {
        CSRWExclusiveLock entryLock(routes[i].entry->lock);
        routes[i].entry->frameRetryAt = 0;
      }
    }
    graph = next;
    m_tex.Commit(texStage);
    if (activationReady)
      activation->Commit();
    committed = true;
  }
  else
  {
    for (unsigned i = routeCount; i > 0; --i)
      if (routes[i - 1].prepared)
        routes[i - 1].transport->Abort();
    m_tex.Abort(texStage);
    if (activationReady)
      activation->Abort();
  }

  for (unsigned i = routeCount; i > 0; --i)
    EndCall(*routes[i - 1].entry, routes[i - 1].transport);
  EndPhase();

  if (committed)
  {
    GraphRouteName logRoutes[FRAME_MAX_SINKS];
    unsigned logRouteCount = 0;
    for (unsigned route = 0; route < routeCount; ++route)
      if (routes[route].selected)
      {
        GraphRouteName& label = logRoutes[logRouteCount++];
        label.id    = routes[route].id;
        label.epoch = routes[route].epoch;
        label.name  = routes[route].name.c_str();
      }
    next.Log(logRoutes, logRouteCount);
  }
  if (result == CfgResult::ACCEPTED || result == CfgResult::REJECTED)
    for (unsigned i = 0; i < routeCount; ++i)
      if (routes[i].outcome == CfgResult::REJECTED)
        DEBUG_INFO("Frame route rejected %ls:%u",
          routes[i].name.empty() ? L"transport" : routes[i].name.c_str(),
          routes[i].id);
  return result;
}

ITransport::ProcessResult CTransportManager::Process(
  ITransportActions& actions)
{
  if (!BeginPhase(Phase::PROCESS, false))
  {
    CSRWSharedLock managerLock(m_lock);
    return m_stopping || m_stopped ?
      ProcessResult::FAILURE : ProcessResult::OK;
  }

  bool initialized = false;
  bool setup = false;
  size_t alignment = 0;
  {
    CSRWSharedLock managerLock(m_lock);
    initialized = m_initialized;
    setup       = m_setup;
    alignment   = m_alignment;
  }

  const uint64_t now = GetTickCount64();
  m_recovery.Tick(now);
  m_tex.Retry(now);
  HandleServiceFailures();
  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    RetryFrame(entry, now);
    if (!BeginCall(entry, Call::PROCESS, false))
      continue;

    RetryEntry(entry, now, initialized, setup, alignment);

    std::shared_ptr<ITransport> transport;
    BackendId id = 0;
    uint32_t epoch = 0;
    bool process = false;
    {
      CSRWSharedLock entryLock(entry.lock);
      transport = entry.transport;
      id        = entry.id;
      epoch     = entry.epoch;
      process   = transport &&
        (entry.state == State::INITIALIZED || entry.state == State::READY);
    }

    if (!process)
    {
      EndCall(entry, transport);
      continue;
    }

    bool setupDone = false;
    {
      CSRWSharedLock entryLock(entry.lock);
      setupDone = entry.setupDone;
    }
    if (setup && !setupDone)
    {
      DrainRecovery(entry, transport);
      if (!transport || !transport->Setup(alignment))
      {
        HandleProcessResult(entry, ProcessResult::FAILURE);
        EndCall(entry, transport);
        continue;
      }
      CSRWExclusiveLock entryLock(entry.lock);
      entry.setupDone = true;
    }

    if (setup)
      SetupEntry(entry, alignment);

    bool interactions = false;
    {
      CSRWSharedLock entryLock(entry.lock);
      interactions = entry.controlAdded;
    }
    DrainRecovery(entry, transport);
    CSourceEvents sourceEvents(
      id, epoch, interactions, m_input, m_recovery, actions);
    const ProcessResult result = transport->Process(sourceEvents);
    DrainRecovery(entry, transport);
    HandleProcessResult(entry, result);
    EndCall(entry, transport);
  }

  EndPhase();
  return ProcessResult::OK;
}

void CTransportManager::Stop()
{
  const DWORD thread = GetCurrentThreadId();
  bool wait = false;
  {
    CSRWExclusiveLock managerLock(m_lock);
    if (m_stopped)
      return;
    if (m_stopping)
      wait = true;
    else
    {
      if (m_phase != Phase::IDLE && m_phaseOwner == thread)
        return;
      m_stopping = true;
      ResetEvent(m_stoppedEvent);
    }
  }

  if (wait)
  {
    WaitForSingleObject(m_stoppedEvent, INFINITE);
    return;
  }

  if (!BeginPhase(Phase::STOP, true, true))
    return;

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  bool failed = false;
  for (unsigned i = 0; i < count; ++i)
  {
    CSRWExclusiveLock entryLock(entries[i]->lock);
    entries[i]->stopRequested = true;
    entries[i]->syncPending   = false;
  }

  for (unsigned i = 0; i < count; ++i)
  {
    for (;;)
    {
      HANDLE idleEvent = nullptr;
      {
        CSRWSharedLock entryLock(entries[i]->lock);
        if (entries[i]->call == Call::IDLE)
          break;
        idleEvent = entries[i]->idleEvent;
      }
      if (!idleEvent ||
          WaitForSingleObject(idleEvent, INFINITE) != WAIT_OBJECT_0)
      {
        failed = true;
        break;
      }
    }
  }

  if (failed)
  {
    for (unsigned i = 0; i < count; ++i)
    {
      CSRWExclusiveLock entryLock(entries[i]->lock);
      entries[i]->stopRequested = false;
    }
    CSRWExclusiveLock managerLock(m_lock);
    m_stopping   = false;
    m_phase      = Phase::IDLE;
    m_phaseOwner = 0;
    SetEvent(m_phaseIdle);
    SetEvent(m_stoppedEvent);
    return;
  }

  m_tex.Stop();

  for (unsigned i = count; i > 0; --i)
  {
    DetachRecovery(*entries[i - 1]);
    RemoveServices(*entries[i - 1]);
  }

  m_input.Stop();
  m_clipboard.Stop();

  for (unsigned i = count; i > 0; --i)
  {
    Entry& entry = *entries[i - 1];
    std::shared_ptr<ITransport> transport;
    State state;
    {
      CSRWSharedLock entryLock(entry.lock);
      transport = entry.transport;
      state     = entry.state;
    }

    if (transport && state != State::STOPPED)
      transport->Stop();
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.state = State::STOPPED;
    }
  }

  {
    CSRWExclusiveLock managerLock(m_lock);
    m_initialized = false;
    m_setup       = false;
    m_stopped     = true;
    m_phase       = Phase::IDLE;
    m_phaseOwner  = 0;
    SetEvent(m_phaseIdle);
    SetEvent(m_stoppedEvent);
  }
}

void CTransportManager::SyncRecovery()
{
  {
    CSRWSharedLock managerLock(m_lock);
    if (m_stopping || m_stopped)
      return;
  }

  m_recovery.MarkMonitorReady();

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    std::shared_ptr<ITransport> transport;
    bool call = false;
    {
      CSRWExclusiveLock entryLock(entry.lock);
      if (entry.stopRequested || !entry.transport ||
          !entry.recoveryAttached ||
          (entry.state != State::INITIALIZED && entry.state != State::READY))
        continue;

      if (!m_recovery.ClaimSync(entry.id, entry.epoch))
        continue;

      if (entry.call != Call::IDLE)
      {
        entry.syncPending = true;
        continue;
      }

      entry.call      = Call::RECOVERY;
      entry.callOwner = GetCurrentThreadId();
      ResetEvent(entry.idleEvent);
      transport = entry.transport;
      call = true;
    }

    if (call)
      transport->SyncRecovery();
    EndCall(entry, transport);
  }
}

void CTransportManager::RecoveryStatus(uint64_t route, uint64_t session,
  uint32_t serial, bool active,
  Recovery state, uint32_t error)
{
  RecoveryAction action;
  action.route   = route;
  action.session = session;
  action.serial  = serial;
  action.active  = active;
  m_recovery.Complete(action, state, error);
}

bool CTransportManager::CanUseMode(const FrameMode& mode,
  uint32_t * requiredSizeMiB) const
{
  if (requiredSizeMiB)
    *requiredSizeMiB = 0;

  std::shared_ptr<const FrameCaps> caps[FRAME_MAX_SINKS];
  unsigned capsCount = 0;
  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    CSRWSharedLock entryLock(entry.lock);
    if (!entry.required ||
        !(entry.config.services & TRANSPORT_SERVICE_FRAME))
      continue;
    if (!entry.frameCaps)
      return false;
    caps[capsCount++] = entry.frameCaps;
  }

  if (!capsCount)
    return false;

  bool supported = true;
  for (unsigned i = 0; i < capsCount; ++i)
  {
    uint32_t hint = 0;
    if (caps[i]->CanUseMode(mode, capsCount == 1 ? &hint : nullptr))
      continue;
    supported = false;
    if (requiredSizeMiB && capsCount == 1)
      *requiredSizeMiB = hint;
  }
  return supported;
}

DirectFrameBufferMemory CTransportManager::GetDirectMemory() const
{
  CTransportManager * manager = const_cast<CTransportManager *>(this);
  {
    CSRWSharedLock managerLock(m_lock);
    if (m_stopping || m_stopped)
      return DirectFrameBufferMemory {};
  }

  Entry * primary = manager->Primary();
  if (!primary)
    return DirectFrameBufferMemory {};

  {
    CSRWSharedLock entryLock(primary->lock);
    if (primary->directMemoryValid)
      return primary->directMemory;
  }

  if (!manager->BeginPhase(Phase::ACCESS, true))
    return DirectFrameBufferMemory {};

  {
    CSRWSharedLock entryLock(primary->lock);
    if (primary->directMemoryValid)
    {
      const DirectFrameBufferMemory memory = primary->directMemory;
      manager->EndPhase();
      return memory;
    }
  }

  if (!manager->BeginCall(*primary, Call::ACCESS, true))
  {
    manager->EndPhase();
    return DirectFrameBufferMemory {};
  }

  std::shared_ptr<ITransport> transport;
  State state;
  {
    CSRWSharedLock entryLock(primary->lock);
    transport = primary->transport;
    state     = primary->state;
  }

  DirectFrameBufferMemory memory;
  if (transport && state != State::FAILED && state != State::STOPPED)
    memory = transport->GetDirectMemory();
  {
    CSRWExclusiveLock entryLock(primary->lock);
    primary->directMemory     = memory;
    primary->directMemoryValid = true;
    primary->exposed          = true;
    primary->exposedTransport = transport;
  }

  manager->EndCall(*primary, transport);
  manager->EndPhase();
  return memory;
}

IFrameTransport& CTransportManager::Frames()
{
  Entry * primary = Primary();
  if (!primary)
    return m_frames;

  if (BeginPhase(Phase::ACCESS, true))
  {
    Expose(*primary);
    EndPhase();
  }
  return m_frames;
}

IControlTransport& CTransportManager::Control()
{
  return m_control;
}

IInputTransport& CTransportManager::Input()
{
  return m_input;
}
