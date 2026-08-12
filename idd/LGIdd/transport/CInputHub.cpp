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

#include "transport/CInputHub.h"

#include "input/IInputSink.h"

static bool SameSource(const SourceKey& left, const SourceKey& right)
{
  return left.backend == right.backend && left.epoch == right.epoch &&
    left.client == right.client && left.generation == right.generation;
}

CInputHub::CInputHub()
{
  for (Source& source : m_sources)
    source.owner = this;
}

CInputHub::~CInputHub()
{
  Stop();
  for (Source& source : m_sources)
  {
    BackendId backend;
    uint32_t epoch;
    {
      CSRWSharedLock lock(m_lock);
      backend = source.backend;
      epoch   = source.epoch;
    }
    if (backend && epoch)
      Unbind(backend, epoch);
  }
}

SourceKey CInputHub::Source::Key(const InputSourceId& sourceId) const
{
  SourceKey source;
  source.backend    = backend;
  source.epoch      = epoch;
  source.client     = sourceId.client;
  source.generation = sourceId.generation;
  return source;
}

InputTargetState CInputHub::Source::GetState(
  const InputSourceId& source)
{
  return owner->GetState(Key(source));
}

void CInputHub::Source::Failed()
{
  owner->Failed(*this);
}

InputResult CInputHub::Source::Claim(const InputSourceId& source)
{
  return owner->Claim(Key(source));
}

InputResult CInputHub::Source::Touch(const InputSourceId& source)
{
  return owner->Touch(Key(source));
}

InputResult CInputHub::Source::Release(
  const InputSourceId& source, bool reset)
{
  return owner->Release(Key(source), reset);
}

InputResult CInputHub::Source::SendMouseRelative(
  const InputSourceId& source,
  int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons)
{
  return owner->SendMouseRelative(
    Key(source), deltaX, deltaY, wheel, buttons);
}

InputResult CInputHub::Source::SendMouseAbsolute(
  const InputSourceId& source,
  uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons)
{
  return owner->SendMouseAbsolute(Key(source), x, y, wheel, buttons);
}

InputResult CInputHub::Source::SendKeyboard(const InputSourceId& source,
  uint8_t modifiers, const uint8_t * keys)
{
  return owner->SendKeyboard(Key(source), modifiers, keys);
}

InputResult CInputHub::Source::Reset(const InputSourceId& source)
{
  return owner->Reset(Key(source));
}

bool CInputHub::Bind(
  BackendId backend, uint32_t epoch, IInputSource& input)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (!backend || !epoch)
    return false;

  Source * selected = nullptr;
  bool start = false;
  {
    CSRWExclusiveLock lock(m_lock);
    for (Source& source : m_sources)
      if ((source.active || source.failed || source.reserved) &&
          source.backend == backend && source.epoch == epoch)
        return false;
    for (Source& source : m_sources)
      if (!source.active && !source.failed && !source.reserved)
      {
        source.backend  = backend;
        source.epoch    = epoch;
        source.endpoint = &input;
        source.active   = true;
        source.reserved = true;
        selected        = &source;
        start           = m_started;
        break;
      }
  }
  if (!selected)
    return false;

  if (start && !input.Start(*selected))
  {
    input.Stop();
    CSRWExclusiveLock lock(m_lock);
    if (selected->reserved && selected->endpoint == &input &&
        selected->backend == backend && selected->epoch == epoch)
    {
      selected->backend  = 0;
      selected->epoch    = 0;
      selected->endpoint = nullptr;
      selected->running  = false;
      selected->active   = false;
      selected->failed   = false;
      selected->failurePending = false;
      selected->reserved = false;
    }
    return false;
  }
  {
    CSRWExclusiveLock lock(m_lock);
    if (!selected->reserved || selected->endpoint != &input ||
        selected->backend != backend || selected->epoch != epoch ||
        selected->failed)
    {
      lock.Unlock();
      if (start)
        input.Stop();
      {
        CSRWExclusiveLock clearLock(m_lock);
        if (selected->endpoint == &input &&
            selected->backend == backend && selected->epoch == epoch)
        {
          selected->backend = 0;
          selected->epoch   = 0;
          selected->endpoint = nullptr;
          selected->running = false;
          selected->active  = false;
          selected->failed  = false;
          selected->failurePending = false;
          selected->reserved = false;
        }
      }
      return false;
    }
    selected->running  = start;
    selected->reserved = false;
  }
  return true;
}

void CInputHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Source * selected = nullptr;
  IInputSource * input = nullptr;
  bool stop = false;
  {
    CSRWExclusiveLock lock(m_lock);
    for (Source& source : m_sources)
      if ((source.active || source.failed || source.reserved) &&
          source.backend == backend && source.epoch == epoch)
      {
        source.active   = false;
        source.failed   = false;
        source.failurePending = false;
        source.reserved = true;
        input           = source.endpoint;
        stop            = source.running;
        source.running  = false;
        selected        = &source;
        if (m_owner.backend == backend && m_owner.epoch == epoch)
        {
          if (m_sink)
            m_sink->Reset();
          m_owner = {};
          m_ownerDeadline = 0;
        }
        break;
      }
  }
  if (!selected)
    return;

  if (input && stop)
    input->Stop();

  CSRWExclusiveLock lock(m_lock);
  if (selected->reserved && selected->backend == backend &&
      selected->epoch == epoch)
  {
    selected->backend  = 0;
    selected->epoch    = 0;
    selected->endpoint = nullptr;
    selected->running  = false;
    selected->reserved = false;
    selected->failed   = false;
    selected->failurePending = false;
  }
}

bool CInputHub::TakeFailure(SourceKey& source)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  for (Source& slot : m_sources)
  {
    if (!slot.failurePending)
      continue;
    source.backend = slot.backend;
    source.epoch   = slot.epoch;
    slot.failurePending = false;
    return true;
  }
  return false;
}

bool CInputHub::Start(IInputSink& sink)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Source * sources[MAX_SOURCES] = {};
  unsigned count = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_started)
      return m_sink == &sink;
    m_sink      = &sink;
    m_sinkState = sink.GetState();
    m_started   = true;
    for (Source& source : m_sources)
      if (source.active)
      {
        source.reserved = true;
        sources[count++] = &source;
      }
  }

  for (unsigned i = 0; i < count; ++i)
  {
    Source& source = *sources[i];
    const bool started = source.endpoint->Start(source);
    bool stop = !started;
    {
      CSRWExclusiveLock lock(m_lock);
      if (!started || source.failed || !source.active || !source.reserved)
      {
        if (m_owner.backend == source.backend &&
            m_owner.epoch == source.epoch)
        {
          if (m_sink)
            m_sink->Reset();
          m_owner = {};
          m_ownerDeadline = 0;
        }
        source.active         = false;
        source.failed         = true;
        source.failurePending = true;
        stop                  = true;
      }
      else
        source.reserved = false;
      source.running = started && !stop;
    }

    if (!stop)
      continue;
    source.endpoint->Stop();
    {
      CSRWExclusiveLock lock(m_lock);
      source.running = false;
    }
  }
  return true;
}

void CInputHub::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Source * sources[MAX_SOURCES] = {};
  unsigned count = 0;
  IInputSink * sink = nullptr;
  bool reset = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_started)
      return;
    m_started = false;
    for (Source& source : m_sources)
      if (source.endpoint && source.running)
      {
        source.running = false;
        sources[count++] = &source;
      }
    sink  = m_sink;
    reset = m_owner.backend != 0;
    m_owner         = {};
    m_ownerDeadline = 0;
  }

  for (unsigned i = count; i > 0; --i)
    sources[i - 1]->endpoint->Stop();
  if (reset && sink)
    sink->Reset();

  CSRWExclusiveLock lock(m_lock);
  m_sink      = nullptr;
  m_sinkState = 0;
}

bool CInputHub::SourceValid(const SourceKey& source) const
{
  if (!source.client || !source.generation)
    return false;
  return BindingValid(source);
}

bool CInputHub::BindingValid(const SourceKey& source) const
{
  if (!BindingPresent(source))
    return false;
  for (const Source& slot : m_sources)
    if (slot.active && !slot.reserved && slot.endpoint &&
        slot.backend == source.backend &&
        slot.epoch == source.epoch)
      return true;
  return false;
}

bool CInputHub::BindingPresent(const SourceKey& source) const
{
  if (!source.backend || !source.epoch)
    return false;
  for (const Source& slot : m_sources)
    if (slot.active && slot.endpoint &&
        slot.backend == source.backend &&
        slot.epoch == source.epoch)
      return true;
  return false;
}

bool CInputHub::OwnerValid(const SourceKey& source) const
{
  return SourceValid(source) && SameSource(m_owner, source);
}

bool CInputHub::CheckState()
{
  if (!m_started || !m_sink)
    return false;

  const uint64_t state = m_sink->GetState();
  if (state != m_sinkState || !(state & 1))
  {
    if (m_owner.backend)
      m_sink->Reset();
    m_owner         = {};
    m_ownerDeadline = 0;
    m_sinkState     = state;
  }
  else if (m_owner.backend && GetTickCount64() >= m_ownerDeadline)
  {
    m_sink->Reset();
    m_owner         = {};
    m_ownerDeadline = 0;
  }
  return (state & 1) != 0;
}

InputTargetState CInputHub::GetState(const SourceKey& source)
{
  CSRWExclusiveLock lock(m_lock);
  InputTargetState result;
  if (!BindingPresent(source))
    return result;
  const bool available = CheckState();
  result.state = m_sinkState;
  if (!available)
    return result;
  if (!BindingValid(source))
    return result;
  if (!m_owner.backend)
    result.available = true;
  else if (source.client && source.generation &&
      SameSource(m_owner, source))
  {
    result.available = true;
    result.owned     = true;
    result.owner.client     = m_owner.client;
    result.owner.generation = m_owner.generation;
  }
  return result;
}

void CInputHub::Failed(Source& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!source.active)
    return;
  if (m_owner.backend == source.backend && m_owner.epoch == source.epoch)
  {
    if (m_sink)
      m_sink->Reset();
    m_owner = {};
    m_ownerDeadline = 0;
  }
  source.active         = false;
  source.failed         = true;
  source.failurePending = true;
}

InputResult CInputHub::Claim(const SourceKey& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (m_owner.backend)
    return SameSource(m_owner, source) ?
      InputResult::ACCEPTED : InputResult::BUSY;
  if (!m_sink->Reset() || m_sink->GetState() != m_sinkState)
  {
    const uint64_t state = m_sink->GetState();
    m_sinkState = state;
    return InputResult::UNAVAILABLE;
  }
  m_owner         = source;
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}

InputResult CInputHub::Touch(const SourceKey& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}

InputResult CInputHub::Release(const SourceKey& source, bool reset)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  bool accepted = true;
  if (reset && m_sink)
    accepted = m_sink->Reset();
  m_owner         = {};
  m_ownerDeadline = 0;
  return accepted ? InputResult::ACCEPTED : InputResult::UNAVAILABLE;
}

InputResult CInputHub::SendMouseRelative(const SourceKey& source,
  int32_t deltaX, int32_t deltaY, int32_t wheel, uint32_t buttons)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  if (!m_sink->SendMouseRelative(deltaX, deltaY, wheel, buttons))
  {
    m_sink->Reset();
    m_owner         = {};
    m_ownerDeadline = 0;
    return InputResult::UNAVAILABLE;
  }
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}

InputResult CInputHub::SendMouseAbsolute(const SourceKey& source,
  uint16_t x, uint16_t y, int32_t wheel, uint32_t buttons)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  if (!m_sink->SendMouseAbsolute(x, y, wheel, buttons))
  {
    m_sink->Reset();
    m_owner         = {};
    m_ownerDeadline = 0;
    return InputResult::UNAVAILABLE;
  }
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}

InputResult CInputHub::SendKeyboard(const SourceKey& source,
  uint8_t modifiers, const uint8_t * keys)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  if (!m_sink->SendKeyboard(modifiers, keys))
  {
    m_sink->Reset();
    m_owner         = {};
    m_ownerDeadline = 0;
    return InputResult::UNAVAILABLE;
  }
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}

InputResult CInputHub::Reset(const SourceKey& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!SourceValid(source))
    return InputResult::STALE;
  if (!CheckState())
    return InputResult::UNAVAILABLE;
  if (!OwnerValid(source))
    return m_owner.backend ? InputResult::BUSY : InputResult::STALE;
  if (!m_sink->Reset())
  {
    m_owner         = {};
    m_ownerDeadline = 0;
    return InputResult::UNAVAILABLE;
  }
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
  return InputResult::ACCEPTED;
}
