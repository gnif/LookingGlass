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
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST)
      return kvmfrClipboardTransferFromHelper(record.transfer);
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
      return kvmfrClipboardTransferFromClient(record.transfer);
    return true;
  }

  bool ValidClientDirection(const KVMFRClipboardMessage& record)
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
      return kvmfrClipboardTransferFromClient(record.transfer);
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
      return kvmfrClipboardTransferFromHelper(record.transfer);
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST)
      return kvmfrClipboardTransferFromClient(record.transfer);
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED ||
        record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
      return kvmfrClipboardTransferFromHelper(record.transfer);
    return true;
  }
}

CClipboardHub::CClipboardHub(CClipboardChannel& channel) :
  m_channel(channel)
{
  for (Source& source : m_sources)
    source.owner = this;
  m_channel.SetHandler(this);
}

CClipboardHub::~CClipboardHub()
{
  Stop();
}

ClipboardClaimResult CClipboardHub::Source::Claim(
  const ClipboardSourceId& source, KVMFRClipboardMessage& cached)
{
  return owner->Claim(*this, source, cached);
}

void CClipboardHub::Source::Release(const ClipboardSourceId& source)
{
  owner->Release(*this, source);
}

ClipboardChannelResult CClipboardHub::Source::SendClipboard(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  return owner->SendClipboard(*this, record, data);
}

ClipboardChannelResult CClipboardHub::Source::SendClipboardBatch(
  const ClipboardChannelWrite * records, size_t count, size_t& accepted)
{
  return owner->SendClipboardBatch(*this, records, count, accepted);
}

void CClipboardHub::Source::ClipboardReceiveReady()
{
  owner->ClipboardReceiveReady(*this);
}

void CClipboardHub::Source::ClipboardFailed()
{
  owner->ClipboardFailed(*this);
}

uint32_t CClipboardHub::NextGenerationNL()
{
  Seq::Inc(m_generation);
  return m_generation;
}

bool CClipboardHub::BindingPresentNL(const Source& source) const
{
  return source.active && source.endpoint && source.backend && source.epoch;
}

bool CClipboardHub::BindingValidNL(const Source& source) const
{
  return BindingPresentNL(source) && source.running && !source.reserved &&
    !source.failed && source.generation;
}

bool CClipboardHub::OwnerIsNL(
  const Source& source, const ClipboardSourceId& id) const
{
  return m_owner == &source && m_ownerId.client == id.client &&
    m_ownerId.generation == id.generation;
}

bool CClipboardHub::MarkFailed(Source& source)
{
  CSRWExclusiveLock lock(m_lock);
  if (!BindingPresentNL(source) && !source.reserved)
    return false;

  const bool changed      = !source.failed;
  source.failed           = true;
  source.failurePending   = true;
  return changed;
}

void CClipboardHub::CachePublicationNL(
  const KVMFRClipboardMessage& record, Source * source)
{
  if (record.type != KVMFR_CLIPBOARD_MESSAGE_OFFER &&
      record.type != KVMFR_CLIPBOARD_MESSAGE_CLEAR)
    return;

  m_cachedClipboard = record;
  m_cachedSource    = source;
  m_cachedValid     = true;
}

void CClipboardHub::InvalidateSourceCacheNL(const Source& source)
{
  if (m_cachedSource != &source)
    return;
  if (m_cachedValid &&
      m_cachedClipboard.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR)
  {
    m_cachedSource = nullptr;
    return;
  }

  m_cachedClipboard = {};
  m_cachedSource    = nullptr;
  m_cachedValid     = false;
}

ClipboardChannelResult CClipboardHub::HoldOrDiscard(
  const KVMFRClipboardMessage& record)
{
  // Preserve the pending clipboard publication until a client claims it.
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

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST)
  {
    KVMFRClipboardMessage reject = {};
    reject.version             = KVMFR_CLIPBOARD_VERSION;
    reject.type                = record.type ==
      KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ?
        KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED :
        KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
    reject.generation          = record.generation;
    reject.clipboardGeneration = record.clipboardGeneration;
    reject.transfer            = record.transfer;
    reject.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    reject.token               = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
    const ClipboardChannelResult result = m_channel.Send(reject);
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

  Source * selected   = nullptr;
  bool     available  = false;
  uint32_t generation = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return false;

    for (Source& slot : m_sources)
      if ((slot.active || slot.failed || slot.reserved) &&
          slot.backend == backend && slot.epoch == epoch)
        return false;
    for (Source& slot : m_sources)
      if (!slot.active && !slot.failed && !slot.reserved)
      {
        slot.backend        = backend;
        slot.epoch          = epoch;
        slot.generation     = NextGenerationNL();
        slot.endpoint       = &source;
        slot.running        = false;
        slot.active         = true;
        slot.reserved       = true;
        slot.failurePending = false;
        selected            = &slot;
        available           = m_available && !m_resetting;
        generation          = slot.generation;
        break;
      }
  }
  if (!selected)
    return false;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  const bool started  = source.Start(*selected);
  bool       attached = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (started && selected->endpoint == &source &&
        selected->backend == backend && selected->epoch == epoch &&
        selected->active && selected->reserved && !selected->failed &&
        !m_stopped)
    {
      selected->running  = true;
      selected->reserved = false;
      available          = m_available && !m_resetting;
      generation         = selected->generation;
      attached           = true;
    }
    else
    {
      selected->active  = false;
      selected->running = false;
    }
  }

  if (!attached)
  {
    source.Stop();
    CSRWExclusiveLock lock(m_lock);
    if (selected->endpoint == &source && selected->backend == backend &&
        selected->epoch == epoch)
    {
      selected->backend        = 0;
      selected->epoch          = 0;
      selected->generation     = 0;
      selected->endpoint       = nullptr;
      selected->running        = false;
      selected->active         = false;
      selected->reserved       = false;
      selected->failed         = false;
      selected->failurePending = false;
    }
    return false;
  }

  source.ClipboardState(available, generation);
  return true;
}

void CClipboardHub::Unbind(BackendId backend, uint32_t epoch)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Source           * selected     = nullptr;
  IClipboardSource * source       = nullptr;
  bool               stop         = false;
  bool               resetChannel = false;
  uint64_t           channelEpoch = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    for (Source& slot : m_sources)
      if ((slot.active || slot.failed || slot.reserved) &&
          slot.backend == backend && slot.epoch == epoch && slot.endpoint)
      {
        selected            = &slot;
        source              = slot.endpoint;
        stop                = slot.running || slot.reserved;
        resetChannel        = m_owner == &slot;
        channelEpoch        = m_channelEpoch;
        slot.active         = false;
        slot.running        = false;
        slot.reserved       = true;
        slot.failed         = false;
        slot.failurePending = false;
        InvalidateSourceCacheNL(slot);
        if (resetChannel)
        {
          m_owner     = nullptr;
          m_ownerId   = {};
          m_resetting = true;
        }
        break;
      }
  }
  if (!selected)
    return;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  if (stop)
    source->Stop();

  CSRWExclusiveLock lock(m_lock);
  if (selected->endpoint == source && selected->backend == backend &&
      selected->epoch == epoch)
  {
    selected->backend        = 0;
    selected->epoch          = 0;
    selected->generation     = 0;
    selected->endpoint       = nullptr;
    selected->running        = false;
    selected->active         = false;
    selected->reserved       = false;
    selected->failed         = false;
    selected->failurePending = false;
  }
  lock.Unlock();
  callbackLock.Unlock();
  lifecycleLock.Unlock();

  if (resetChannel && channelEpoch)
    m_channel.Reset(channelEpoch, ERROR_DEVICE_NOT_CONNECTED);
}

bool CClipboardHub::TakeFailure(SourceKey& source)
{
  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  CSRWExclusiveLock lock(m_lock);
  for (Source& slot : m_sources)
  {
    if (!slot.failurePending)
      continue;
    source              = {};
    source.backend      = slot.backend;
    source.epoch        = slot.epoch;
    slot.failurePending = false;
    return true;
  }
  return false;
}

void CClipboardHub::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  Source * sources[MAX_SOURCES] = {};
  unsigned count                = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;

    m_stopped         = true;
    m_owner           = nullptr;
    m_cachedSource    = nullptr;
    m_ownerId         = {};
    m_cachedClipboard = {};
    m_cachedValid     = false;
    m_resetting       = false;
    for (Source& source : m_sources)
      if (source.endpoint)
      {
        source.active         = false;
        source.running        = false;
        source.reserved       = true;
        source.failed         = false;
        source.failurePending = false;
        sources[count++]      = &source;
      }
  }

  // Clearing the channel handler is a callback-quiescence barrier. It must
  // precede taking m_callbackLock because an in-flight channel callback owns
  // the channel's handler lock while waiting for m_callbackLock.
  m_channel.ClearHandler(this);

  CSRWExclusiveLock callbackLock(m_callbackLock);
  for (unsigned i = count; i > 0; --i)
    sources[i - 1]->endpoint->Stop();

  CSRWExclusiveLock lock(m_lock);
  for (Source& source : m_sources)
  {
    source.backend        = 0;
    source.epoch          = 0;
    source.generation     = 0;
    source.endpoint       = nullptr;
    source.running        = false;
    source.active         = false;
    source.reserved       = false;
    source.failed         = false;
    source.failurePending = false;
  }
}

ClipboardClaimResult CClipboardHub::Claim(Source& source,
  const ClipboardSourceId& id, KVMFRClipboardMessage& cached)
{
  cached = {};
  if (!id.client || !id.generation)
    return ClipboardClaimResult::STALE;

  uint64_t channelEpoch = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!BindingValidNL(source))
      return ClipboardClaimResult::STALE;
    if (!m_available || m_resetting || !m_channelEpoch ||
        !m_channel.Available())
      return ClipboardClaimResult::UNAVAILABLE;
    if (m_owner)
      return OwnerIsNL(source, id) ?
        ClipboardClaimResult::ACCEPTED : ClipboardClaimResult::BUSY;

    m_owner   = &source;
    m_ownerId = id;
    // A client-originated OFFER cannot be served by a replacement client.
    // Its matching cleanup CLEAR is safe to replay, as is any
    // Helper-originated publication.
    if (m_cachedValid && (!m_cachedSource ||
        m_cachedClipboard.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR))
    {
      cached            = m_cachedClipboard;
      cached.generation = source.generation;
    }
    channelEpoch = m_channelEpoch;
  }

  m_channel.Kick(channelEpoch);
  return ClipboardClaimResult::ACCEPTED;
}

void CClipboardHub::Release(
  Source& source, const ClipboardSourceId& id)
{
  uint64_t channelEpoch = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!OwnerIsNL(source, id))
      return;
    m_owner       = nullptr;
    m_ownerId     = {};
    channelEpoch  = m_channelEpoch;
  }

  if (channelEpoch)
    m_channel.Kick(channelEpoch);
}

void CClipboardHub::ClipboardState(bool available, uint64_t epoch)
{
  CSRWExclusiveLock callbackLock(m_callbackLock);
  Source   * sources[MAX_SOURCES]     = {};
  uint32_t   generations[MAX_SOURCES] = {};
  unsigned   count                    = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped)
      return;

    const bool changed = m_available != available ||
      m_channelEpoch != epoch || m_resetting;
    m_available        = available;
    m_channelEpoch     = epoch;
    m_resetting        = false;
    if (!changed)
      return;

    m_owner           = nullptr;
    m_cachedSource    = nullptr;
    m_ownerId         = {};
    m_cachedClipboard = {};
    m_cachedValid     = false;
    for (Source& source : m_sources)
      if (BindingValidNL(source))
      {
        source.generation  = NextGenerationNL();
        sources[count]     = &source;
        generations[count] = source.generation;
        ++count;
      }
  }

  for (unsigned i = 0; i < count; ++i)
    sources[i]->endpoint->ClipboardState(available, generations[i]);
}

ClipboardChannelResult CClipboardHub::ClipboardRecord(
  const KVMFRClipboardMessage& record, std::vector<uint8_t>&& data)
{
  if (!ValidHelperDirection(record))
    return ClipboardChannelResult::FAILED;

  CSRWExclusiveLock callbackLock(m_callbackLock);
  Source           * source     = nullptr;
  IClipboardSource * endpoint   = nullptr;
  ClipboardSourceId  ownerId    = {};
  uint32_t           generation = 0;
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_available)
      return ClipboardChannelResult::ACCEPTED;
    source = m_owner;
    if (!source || !BindingValidNL(*source))
      return HoldOrDiscard(record);
    endpoint   = source->endpoint;
    ownerId    = m_ownerId;
    generation = source->generation;
  }

  // Helper records are transport-agnostic. Stamp them only after selecting
  // the active transport binding.
  KVMFRClipboardMessage stamped = record;
  stamped.generation = generation;
  const ClipboardChannelResult result =
    endpoint->SendClipboard(stamped,
      data.empty() ? nullptr : data.data());
  if (result == ClipboardChannelResult::ACCEPTED)
  {
    CSRWExclusiveLock lock(m_lock);
    CachePublicationNL(stamped, nullptr);
    const bool publication = stamped.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      stamped.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR;
    return publication && m_owner && !OwnerIsNL(*source, ownerId) ?
      ClipboardChannelResult::BUSY : result;
  }
  if (result == ClipboardChannelResult::BUSY)
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
  Source   * sources[MAX_SOURCES]     = {};
  uint32_t   generations[MAX_SOURCES] = {};
  unsigned   count                    = 0;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_stopped || m_channelEpoch != epoch)
      return;

    m_owner           = nullptr;
    m_cachedSource    = nullptr;
    m_ownerId         = {};
    m_cachedClipboard = {};
    m_cachedValid     = false;
    m_resetting       = true;
    for (Source& source : m_sources)
      if (BindingValidNL(source))
      {
        source.generation  = NextGenerationNL();
        sources[count]     = &source;
        generations[count] = source.generation;
        ++count;
      }
  }

  for (unsigned i = 0; i < count; ++i)
    sources[i]->endpoint->ClipboardReset(generations[i], reason);
}

ClipboardChannelResult CClipboardHub::SendClipboard(Source& source,
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  const ClipboardChannelWrite write    = { record, data };
  size_t                      accepted = 0;
  return SendClipboardBatch(source, &write, 1, accepted);
}

ClipboardChannelResult CClipboardHub::SendClipboardBatch(Source& source,
  const ClipboardChannelWrite * records, size_t count, size_t& accepted)
{
  accepted = 0;
  if (!records || !count || count > KVMFR_CLIPBOARD_SLOT_COUNT)
    return ClipboardChannelResult::FAILED;

  // Keep the lock through Send so Unbind cannot advance the binding
  // generation after validation but before the records enter the channel.
  CSRWExclusiveLock lock(m_lock);
  if (m_stopped || !BindingValidNL(source) || m_owner != &source)
    return ClipboardChannelResult::FAILED;
  if (!m_available || m_resetting || !m_channel.Available())
    return ClipboardChannelResult::BUSY;

  for (size_t i = 0; i < count; ++i)
    if (!ValidClientDirection(records[i].record) ||
        records[i].record.generation != source.generation)
      return ClipboardChannelResult::FAILED;

  const ClipboardChannelResult result =
    m_channel.SendBatch(records, count, accepted);
  for (size_t i = 0; i < accepted; ++i)
    CachePublicationNL(records[i].record, &source);
  if (result == ClipboardChannelResult::FAILED &&
      (m_resetting || !m_channel.Available()))
    return ClipboardChannelResult::BUSY;
  return result;
}

void CClipboardHub::ClipboardReceiveReady(Source& source)
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_stopped || !m_available || !BindingValidNL(source) ||
        m_owner != &source)
      return;
  }

  const uint64_t epoch = m_channel.Epoch();
  if (epoch)
    m_channel.Kick(epoch);
}

void CClipboardHub::ClipboardFailed(Source& source)
{
  if (!MarkFailed(source))
    return;

  const uint64_t epoch = m_channel.Epoch();
  if (epoch)
    m_channel.Kick(epoch);
}
