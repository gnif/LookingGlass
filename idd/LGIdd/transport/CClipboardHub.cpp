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

#include "transport/CClipboardHub.h"

#include "Seq.h"

namespace
{
  bool ValidHelperDirection(const KVMFRClipboardMessage& record)
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
      return kvmfrClipboardTransferFromHelper(record.transfer);
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
      return kvmfrClipboardTransferFromClient(record.transfer);
    return true;
  }
}

CClipboardHub::CClipboardHub(CClipboardChannel& channel) :
  m_channel(channel)
{
  m_channel.SetHandler(this);
}

CClipboardHub::~CClipboardHub()
{
  Stop();
}

void CClipboardHub::AdvanceGenerationNL()
{
  Seq::Inc(m_generation);
}

bool CClipboardHub::MarkFailed(IClipboardSource& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_source != &source || (!m_active && !m_reserved))
    return false;

  const bool changed = !m_failed;
  m_failed         = true;
  m_failurePending = true;
  return changed;
}

ClipboardChannelResult CClipboardHub::HoldOrDiscard(
  const KVMFRClipboardMessage& record)
{
  // Preserve the pending clipboard publication until a source is rebound.
  // Transfer-scoped records belong to the source which admitted them and
  // must never cross a transport generation.
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR)
    return ClipboardChannelResult::BUSY;

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
  {
    KVMFRClipboardMessage cancel = {};
    cancel.version             = KVMFR_CLIPBOARD_VERSION;
    cancel.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
    cancel.generation          = record.generation;
    cancel.clipboardGeneration = record.clipboardGeneration;
    cancel.transfer            = record.transfer;
    cancel.format              = record.format;
    cancel.token               = ERROR_DEVICE_NOT_CONNECTED;
    const ClipboardChannelResult result = m_channel.Send(cancel);
    if (result != ClipboardChannelResult::ACCEPTED)
      return result;
  }

  return ClipboardChannelResult::ACCEPTED;
}

bool CClipboardHub::Bind(
  BackendId backend, uint32_t epoch, IClipboardSource& source)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (!backend || !epoch)
    return false;

  bool     available;
  uint32_t generation;
  uint64_t channelEpoch;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || m_source || m_active || m_reserved)
      return false;

    AdvanceGenerationNL();
    m_source         = &source;
    m_backend        = backend;
    m_epoch          = epoch;
    m_active         = true;
    m_reserved       = true;
    m_failed         = false;
    m_failurePending = false;
    available        = m_available;
    generation       = m_generation;
    channelEpoch     = m_channelEpoch;
  }

  CSRWExclusiveLock callbackLock(m_callbackLock);
  const bool started = source.Start(*this);
  bool attached = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (started && m_source == &source && m_backend == backend &&
        m_epoch == epoch && m_active && m_reserved && !m_failed &&
        !m_stopped)
    {
      m_running    = true;
      m_reserved   = false;
      available    = m_available;
      generation   = m_generation;
      channelEpoch = m_channelEpoch;
      attached     = true;
    }
    else
    {
      m_active = false;
      m_running = false;
    }
  }

  if (!attached)
  {
    source.Stop();
    CSRWExclusiveLock lock(m_lock);
    if (m_source == &source && m_backend == backend && m_epoch == epoch)
    {
      m_source         = nullptr;
      m_backend        = 0;
      m_epoch          = 0;
      m_active         = false;
      m_running        = false;
      m_reserved       = false;
      m_failed         = false;
      m_failurePending = false;
    }
    return false;
  }

  source.ClipboardState(available, generation);
  callbackLock.Unlock();
  if (available && channelEpoch)
    m_channel.Kick(channelEpoch);
  return true;
}

void CClipboardHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IClipboardSource * source = nullptr;
  bool stop = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if ((!m_active && !m_failed && !m_reserved) ||
        m_backend != backend || m_epoch != epoch || !m_source)
      return;

    source           = m_source;
    stop             = m_running || m_reserved;
    m_active         = false;
    m_running        = false;
    m_reserved       = true;
    m_failed         = false;
    m_failurePending = false;
    AdvanceGenerationNL();
  }

  CSRWExclusiveLock callbackLock(m_callbackLock);
  if (stop)
    source->Stop();

  CSRWExclusiveLock lock(m_lock);
  if (m_source == source && m_backend == backend && m_epoch == epoch)
  {
    m_source         = nullptr;
    m_backend        = 0;
    m_epoch          = 0;
    m_active         = false;
    m_running        = false;
    m_reserved       = false;
    m_failed         = false;
    m_failurePending = false;
  }
}

bool CClipboardHub::TakeFailure(SourceKey& source)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  if (!m_failurePending)
    return false;

  source = {};
  source.backend    = m_backend;
  source.epoch      = m_epoch;
  m_failurePending  = false;
  return true;
}

void CClipboardHub::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  IClipboardSource * source = nullptr;
  bool stop = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;

    m_stopped        = true;
    source           = m_source;
    stop             = source && (m_running || m_reserved);
    m_active         = false;
    m_running        = false;
    m_reserved       = source != nullptr;
    m_failed         = false;
    m_failurePending = false;
    AdvanceGenerationNL();
  }

  // Clearing the channel handler is a callback-quiescence barrier. It must
  // precede taking m_callbackLock because an in-flight channel callback owns
  // the channel's handler lock while waiting for m_callbackLock.
  m_channel.ClearHandler(this);

  CSRWExclusiveLock callbackLock(m_callbackLock);
  if (stop)
    source->Stop();

  CSRWExclusiveLock lock(m_lock);
  m_source         = nullptr;
  m_backend        = 0;
  m_epoch          = 0;
  m_active         = false;
  m_running        = false;
  m_reserved       = false;
  m_failed         = false;
  m_failurePending = false;
}

void CClipboardHub::ClipboardState(bool available, uint64_t epoch)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IClipboardSource * source = nullptr;
  uint32_t generation;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;

    if (m_available != available || m_channelEpoch != epoch)
      AdvanceGenerationNL();
    m_available    = available;
    m_channelEpoch = epoch;
    generation     = m_generation;
    if (m_active && m_running && !m_failed)
      source = m_source;
  }

  if (source)
    source->ClipboardState(available, generation);
}

ClipboardChannelResult CClipboardHub::ClipboardRecord(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  if (!ValidHelperDirection(record))
    return ClipboardChannelResult::FAILED;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  IClipboardSource * source = nullptr;
  uint32_t generation;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_available)
      return ClipboardChannelResult::ACCEPTED;
    if (m_failed || !m_active || !m_running || !m_source)
      return HoldOrDiscard(record);
    source     = m_source;
    generation = m_generation;
  }

  KVMFRClipboardMessage stamped = record;
  stamped.generation = generation;
  const ClipboardChannelResult result =
    source->SendClipboard(stamped, data);
  if (result != ClipboardChannelResult::FAILED)
    return result;

  if (MarkFailed(*source))
  {
    const uint64_t epoch = m_channel.Epoch();
    if (epoch)
      m_channel.Kick(epoch);
  }
  return HoldOrDiscard(record);
}

void CClipboardHub::ClipboardReset(uint64_t epoch, uint32_t reason)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  IClipboardSource * source = nullptr;
  uint32_t generation;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || m_channelEpoch != epoch)
      return;

    AdvanceGenerationNL();
    generation = m_generation;
    if (m_active && m_running && !m_failed)
      source = m_source;
  }

  if (source)
    source->ClipboardReset(generation, reason);
}

ClipboardChannelResult CClipboardHub::SendClipboard(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  // Keep the shared lock through Send so Unbind cannot advance the binding
  // generation after validation but before the record enters the channel.
  CSRWSharedLock lock(m_lock);
  if (m_stopped || !m_available || !m_active || !m_running || m_failed ||
      !m_source || record.generation != m_generation)
    return ClipboardChannelResult::FAILED;
  return m_channel.Send(record, data);
}

void CClipboardHub::ClipboardReceiveReady()
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_available || !m_active || m_failed ||
        !m_running)
      return;
  }

  const uint64_t epoch = m_channel.Epoch();
  if (epoch)
    m_channel.Kick(epoch);
}

void CClipboardHub::ClipboardFailed()
{
  IClipboardSource * source;
  {
    CSRWSharedLock lock(m_lock);
    source = m_source;
  }
  if (!source || !MarkFailed(*source))
    return;

  const uint64_t epoch = m_channel.Epoch();
  if (epoch)
    m_channel.Kick(epoch);
}
