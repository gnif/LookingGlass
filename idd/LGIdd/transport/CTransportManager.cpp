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

#include "CDebug.h"

#include <Windows.h>
#include <new>
#include <utility>

static const uint64_t RETRY_DELAY_MS = 500;
static const uint64_t SERVICE_RETRY_DELAY_MS = 250;

class CSourceEvents final : public ITransportEvents
{
private:
  BackendId         m_backend;
  uint32_t          m_epoch;
  ITransportEvents& m_events;

  SourceKey Stamp(const SourceKey& source) const
  {
    SourceKey stamped = source;
    stamped.backend = m_backend;
    stamped.epoch   = m_epoch;
    return stamped;
  }

public:
  CSourceEvents(
    BackendId backend, uint32_t epoch, ITransportEvents& events) :
    m_backend(backend), m_epoch(epoch), m_events(events) {}

  void OnSetCursorPos(
    const SourceKey& source, int32_t x, int32_t y) override
  {
    m_events.OnSetCursorPos(Stamp(source), x, y);
  }

  void OnSetResolution(const SourceKey& source,
    uint32_t width, uint32_t height) override
  {
    m_events.OnSetResolution(Stamp(source), width, height);
  }

  void OnRecoveryRequest(const SourceKey& source,
    uint64_t session, uint32_t serial, bool active) override
  {
    m_events.OnRecoveryRequest(
      Stamp(source), session, serial, active);
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

CTransportManager::CTransportManager()
{
  m_phaseIdle    = CreateEvent(nullptr, TRUE, TRUE, nullptr);
  m_stoppedEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
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
  static const unsigned MAX_DRAIN = 8;
  for (unsigned i = 0; i < MAX_DRAIN; ++i)
  {
    bool sync = false;
    bool update = false;
    RecoveryUpdate recovery;
    {
      CSRWExclusiveLock entryLock(entry.lock);
      if (entry.stopRequested || !transport ||
          transport != entry.transport ||
          (entry.state != State::INITIALIZED &&
           entry.state != State::READY))
      {
        entry.syncPending     = false;
        entry.recoveryPending = false;
        return;
      }

      sync                  = entry.syncPending;
      update                = entry.recoveryPending;
      recovery              = entry.recovery;
      entry.syncPending     = false;
      entry.recoveryPending = false;
    }

    if (!sync && !update)
      return;
    if (sync)
      transport->SyncRecovery();
    if (update)
      transport->RecoveryStatus(recovery.session, recovery.serial,
        recovery.active, recovery.state, recovery.error);
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
    if (drain && (entry.syncPending || entry.recoveryPending))
      continue;

    entry.call      = Call::IDLE;
    entry.callOwner = 0;
    SetEvent(entry.idleEvent);
    return;
  }
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
  {
    CSRWSharedLock entryLock(entry.lock);
    if (entry.state == State::INITIALIZED || entry.state == State::READY)
      return true;
    if (entry.state != State::OPEN)
      return false;
    transport = entry.transport;
  }

  if (!transport || !transport->Initialize())
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.state = State::FAILED;
    return false;
  }

  const FrameMemoryLimits limits = transport->GetMemoryLimits();
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.limits      = limits;
    entry.limitsValid = true;
    entry.state       = State::INITIALIZED;
  }
  return true;
}

bool CTransportManager::AddServices(Entry& entry)
{
  std::shared_ptr<ITransport> transport;
  BackendId id = 0;
  uint32_t epoch = 0;
  bool primary = false;
  bool required = false;
  uint32_t services = 0;
  bool controlAdded = false;
  bool controlFailed = false;
  bool controlAbsent = false;
  bool inputAdded = false;
  bool inputFailed = false;
  bool inputAbsent = false;
  bool frameAdded = false;
  bool frameAbsent = false;
  uint64_t retryAt = 0;
  {
    CSRWSharedLock entryLock(entry.lock);
    transport    = entry.transport;
    id           = entry.id;
    epoch        = entry.epoch;
    primary      = entry.primary;
    required     = entry.required;
    services     = entry.config.services;
    controlAdded = entry.controlAdded;
    controlFailed = entry.controlFailed;
    controlAbsent = entry.controlAbsent;
    inputAdded   = entry.inputAdded;
    inputFailed  = entry.inputFailed;
    inputAbsent  = entry.inputAbsent;
    frameAdded   = entry.frameAdded;
    frameAbsent  = entry.frameAbsent;
    retryAt      = entry.serviceRetryAt;
  }

  if (!transport)
    return !required;

  const uint64_t now = GetTickCount64();
  const bool attach = now >= retryAt;
  bool controlRetry = false;
  bool frameRetry   = false;
  bool inputRetry   = false;
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
    if (frame && m_frames.Bind(id, epoch, primary, *frame))
    {
      CSRWExclusiveLock entryLock(entry.lock);
      entry.frameAdded = true;
      frameAdded = true;
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

  if (attach && (controlRetry || inputRetry || frameRetry))
  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.serviceRetryAt = now + SERVICE_RETRY_DELAY_MS;
  }

  const bool servicesReady =
    (!(services & TRANSPORT_SERVICE_FRAME)   || frameAdded) &&
    (!(services & TRANSPORT_SERVICE_CONTROL) || controlAdded) &&
    (!(services & TRANSPORT_SERVICE_INPUT)   || inputAdded);
  return !required || servicesReady;
}

void CTransportManager::HandleServiceFailures()
{
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
  BackendId id = 0;
  uint32_t epoch = 0;
  bool frameAdded = false;
  bool controlAdded = false;
  bool inputAdded = false;
  {
    CSRWExclusiveLock entryLock(entry.lock);
    id                   = entry.id;
    epoch                = entry.epoch;
    frameAdded           = entry.frameAdded;
    controlAdded         = entry.controlAdded;
    inputAdded           = entry.inputAdded;
    entry.frameAdded     = false;
    entry.controlAdded   = false;
    entry.inputAdded     = false;
  }

  if (inputAdded)
    m_input.Unbind(id, epoch);
  if (controlAdded)
    m_control.Remove(id, epoch);
  if (frameAdded)
    m_frames.Unbind(id, epoch);
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

  RemoveServices(entry);
  if (transport)
    transport->Stop();

  {
    CSRWExclusiveLock entryLock(entry.lock);
    entry.transport.reset();
    entry.limits       = FrameMemoryLimits {};
    entry.limitsValid  = false;
    entry.directMemory = DirectFrameBufferMemory {};
    entry.directMemoryValid = false;
    entry.setupDone    = false;
    entry.controlFailed = false;
    entry.controlAbsent = false;
    entry.inputFailed   = false;
    entry.inputAbsent   = false;
    entry.frameAbsent   = false;
    entry.serviceRetryAt = 0;
    entry.recoveryPending = false;
    entry.recovery        = RecoveryUpdate {};
    ++entry.epoch;
    if (!entry.epoch)
      ++entry.epoch;
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

ITransport::ProcessResult CTransportManager::Process(
  ITransportEvents& events)
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
  HandleServiceFailures();
  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
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

    DrainRecovery(entry, transport);
    CSourceEvents sourceEvents(id, epoch, events);
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
    entries[i]->stopRequested   = true;
    entries[i]->syncPending     = false;
    entries[i]->recoveryPending = false;
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

  for (unsigned i = count; i > 0; --i)
    RemoveServices(*entries[i - 1]);

  m_input.Stop();

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
          (entry.state != State::INITIALIZED && entry.state != State::READY))
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

void CTransportManager::RecoveryStatus(const SourceKey& source,
  uint64_t session, uint32_t serial, bool active,
  Recovery state, uint32_t error)
{
  {
    CSRWSharedLock managerLock(m_lock);
    if (m_stopping || m_stopped)
      return;
  }

  Entry * entries[FRAME_MAX_SINKS] = {};
  const unsigned count = Entries(entries);
  for (unsigned i = 0; i < count; ++i)
  {
    Entry& entry = *entries[i];
    std::shared_ptr<ITransport> transport;
    bool call = false;
    {
      CSRWExclusiveLock entryLock(entry.lock);
      if (entry.id != source.backend || entry.epoch != source.epoch ||
          entry.stopRequested || !entry.transport ||
          (entry.state != State::INITIALIZED && entry.state != State::READY))
        continue;

      if (entry.call != Call::IDLE)
      {
        entry.recovery.session = session;
        entry.recovery.serial  = serial;
        entry.recovery.active  = active;
        entry.recovery.state   = state;
        entry.recovery.error   = error;
        entry.recoveryPending  = true;
        continue;
      }

      entry.call      = Call::RECOVERY;
      entry.callOwner = GetCurrentThreadId();
      ResetEvent(entry.idleEvent);
      transport = entry.transport;
      call = true;
    }

    if (call)
      transport->RecoveryStatus(session, serial, active, state, error);
    EndCall(entry, transport);
    return;
  }
}

FrameMemoryLimits CTransportManager::GetMemoryLimits() const
{
  {
    CSRWSharedLock managerLock(m_lock);
    if (m_stopping || m_stopped)
      return FrameMemoryLimits {};
  }

  Entry * primary = Primary();
  if (!primary)
    return FrameMemoryLimits {};

  CSRWSharedLock entryLock(primary->lock);
  return primary->limitsValid ? primary->limits : FrameMemoryLimits {};
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
