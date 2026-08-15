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

#include "CClipboardChannel.h"

#include "CClipboardRing.h"
#include "CDebug.h"

#include <new>
#include <ntstatus.h>
#include <string.h>
#include <utility>
#include <vector>

namespace
{
  struct ClipboardCallbackScope
  {
    CClipboardChannel      * channel;
    ClipboardCallbackScope * previous;
  };

  thread_local ClipboardCallbackScope * g_callbackScope = nullptr;

  bool InClipboardCallback(CClipboardChannel * channel)
  {
    for (ClipboardCallbackScope * scope = g_callbackScope;
         scope; scope = scope->previous)
      if (scope->channel == channel)
        return true;
    return false;
  }

  class CClipboardCallbackScope
  {
  private:
    ClipboardCallbackScope m_scope;

  public:
    explicit CClipboardCallbackScope(CClipboardChannel * channel) :
      m_scope { channel, g_callbackScope }
    {
      g_callbackScope = &m_scope;
    }

    ~CClipboardCallbackScope()
    {
      g_callbackScope = m_scope.previous;
    }
  };

  bool ValidRecord(const KVMFRClipboardMessage& record,
    bool dataPresent)
  {
    if (record.version != KVMFR_CLIPBOARD_VERSION ||
        record.length > KVMFR_CLIPBOARD_DATA_BYTES ||
        (record.length != 0) != dataPresent)
      return false;

    switch (record.type)
    {
      case KVMFR_CLIPBOARD_MESSAGE_OFFER:
        return record.clipboardGeneration && record.token &&
          !(record.token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL) &&
          !record.transfer && !record.offset && !record.size &&
          !record.format && !record.flags && !record.length &&
          !record.sequence;

      case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
        return record.clipboardGeneration && !record.token &&
          !record.transfer && !record.offset && !record.size &&
          !record.format && !record.flags && !record.length &&
          !record.sequence;

      case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
        return record.clipboardGeneration && record.transfer &&
          kvmfrClipboardRepresentationFormatValid(record.format) &&
          !record.offset &&
          !record.size && !record.flags && !record.token &&
          !record.length && !record.sequence;

      case KVMFR_CLIPBOARD_MESSAGE_DATA:
      {
        if (!record.clipboardGeneration || !record.transfer ||
            !kvmfrClipboardRepresentationFormatValid(record.format) ||
            record.token ||
            (record.flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
              KVMFR_CLIPBOARD_FLAG_END)) ||
            (!record.length && !(record.flags & KVMFR_CLIPBOARD_FLAG_END)) ||
            record.offset > UINT64_MAX - record.length)
          return false;
        const uint64_t end = record.offset + record.length;
        if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
          return record.size == end;
        if (!(record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN))
          return record.size == KVMFR_CLIPBOARD_SIZE_UNKNOWN;
        return record.size == KVMFR_CLIPBOARD_SIZE_UNKNOWN ||
          record.size >= end;
      }

      case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
        return record.transfer && !record.offset && !record.size &&
          (!record.format ||
            kvmfrClipboardRepresentationFormatValid(record.format)) &&
          !record.flags && !record.length && !record.sequence;

      case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
      case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
      case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
      case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
      case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
      case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
        return kvmfrClipboardFileMessageValid(&record);

      default:
        return false;
    }
  }
}

CClipboardChannel::~CClipboardChannel()
{
  Detach();
}

bool CClipboardChannel::Attach(HANDLE mapping, uint64_t epoch,
  bool helper, IClipboardChannelDoorbell& doorbell)
{
  if (!mapping || mapping == INVALID_HANDLE_VALUE || !epoch)
  {
    if (mapping && mapping != INVALID_HANDLE_VALUE)
      CloseHandle(mapping);
    return false;
  }

  Detach();

  CSRWExclusiveLock lock(m_lifecycleLock);
  ClipboardMapping * view = static_cast<ClipboardMapping *>(MapViewOfFile(
    mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
    sizeof(ClipboardMapping)));
  if (!view)
  {
    DEBUG_ERROR_HR(GetLastError(), "Failed to map clipboard channel");
    CloseHandle(mapping);
    return false;
  }
  if (!CClipboardRing::Valid(*view, epoch))
  {
    DEBUG_ERROR("Invalid clipboard mapping");
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }

  m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!m_stop)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create clipboard channel stop event");
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }
  m_kick = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_kick)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create clipboard channel kick event");
    CloseHandle(m_stop);
    m_stop = nullptr;
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }
  m_writeReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_writeReady)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create clipboard channel credit event");
    CloseHandle(m_kick);
    CloseHandle(m_stop);
    m_kick = nullptr;
    m_stop = nullptr;
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    return false;
  }

  m_mapping  = mapping;
  m_view     = view;
  m_in       = helper ? &view->iddToHelper : &view->helperToIdd;
  m_out      = helper ? &view->helperToIdd : &view->iddToHelper;
  m_epoch    = epoch;
  ++m_instance;
  if (!m_instance)
    ++m_instance;
  m_doorbell = &doorbell;
  Atomic::Store(m_available, true, std::memory_order_release);

  m_thread = CreateThread(
    nullptr, 0, ThreadProc, this, 0, &m_threadId);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create clipboard channel worker");
    Atomic::Store(m_available, false, std::memory_order_release);
    m_doorbell = nullptr;
    m_epoch = 0;
    m_out = nullptr;
    m_in = nullptr;
    m_threadId = 0;
    m_view = nullptr;
    m_mapping = nullptr;
    CloseHandle(m_writeReady);
    CloseHandle(m_kick);
    CloseHandle(m_stop);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    m_writeReady = nullptr;
    m_kick = nullptr;
    m_stop = nullptr;
    return false;
  }

  lock.Unlock();
  PublishState(true, epoch);
  return true;
}

void CClipboardChannel::Detach()
{
  HANDLE thread;
  uint64_t epoch;
  bool available;
  {
    CSRWExclusiveLock lock(m_lifecycleLock);
    available = Atomic::Swap(
      m_available, false, std::memory_order_acq_rel);
    epoch  = m_epoch;
    thread = m_thread;
    if (m_stop)
      SetEvent(m_stop);
    if (thread && InClipboardCallback(this))
      m_deferredDetach = true;
  }

  // A callback may run on the worker or while the worker waits for the
  // callback-quiescence lock. Let the worker release channel resources after
  // this callback returns instead of waiting on it here.
  if (thread && InClipboardCallback(this))
  {
    if (available)
      PublishState(false, epoch);
    return;
  }

  if (thread)
    WaitForSingleObject(thread, INFINITE);

  CSRWExclusiveLock lock(m_lifecycleLock);
  if (m_thread)
    CloseHandle(m_thread);
  if (m_writeReady)
    CloseHandle(m_writeReady);
  if (m_kick)
    CloseHandle(m_kick);
  if (m_stop)
    CloseHandle(m_stop);
  if (m_view)
    UnmapViewOfFile(m_view);
  if (m_mapping)
    CloseHandle(m_mapping);

  m_thread = nullptr;
  m_threadId = 0;
  m_writeReady = nullptr;
  m_kick = nullptr;
  m_stop = nullptr;
  m_in = nullptr;
  m_out = nullptr;
  m_view = nullptr;
  m_mapping = nullptr;
  m_doorbell = nullptr;
  m_epoch = 0;
  m_deferredDetach = false;
  lock.Unlock();
  if (available)
    PublishState(false, epoch);
}

void CClipboardChannel::Kick(uint64_t epoch)
{
  CSRWSharedLock lock(m_lifecycleLock);
  if (Available() && epoch == m_epoch && m_kick && m_writeReady)
  {
    if (!SetEvent(m_kick))
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to signal clipboard channel receive event");
    }
    if (!SetEvent(m_writeReady))
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to signal clipboard channel credit event");
    }
  }
}

void CClipboardChannel::Reset(uint64_t epoch, uint32_t reason)
{
  uint64_t instance;
  {
    CSRWSharedLock lock(m_lifecycleLock);
    if (!Available() || epoch != m_epoch)
      return;
    instance = m_instance;
  }
  // A peer reset is terminal for this mapping too. Keep the control pipe
  // connected, but stop both clipboard workers so neither endpoint can
  // continue publishing into a ring the other side has abandoned.
  Fail(instance, reason, false);
}

bool CClipboardChannel::WaitWritable(HANDLE stop, DWORD timeout)
{
  if (!stop || stop == INVALID_HANDLE_VALUE)
    return false;

  CSRWSharedLock lock(m_lifecycleLock);
  if (!Available() || !m_writeReady)
    return false;
  const HANDLE events[] = { stop, m_writeReady };
  const DWORD result = WaitForMultipleObjects(
    ARRAYSIZE(events), events, FALSE, timeout);
  if (result == WAIT_OBJECT_0)
    return false;
  if (result == WAIT_OBJECT_0 + 1 || result == WAIT_TIMEOUT)
    return true;
  if (result == WAIT_FAILED)
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to wait for clipboard channel credit");
  else
    DEBUG_ERROR("Invalid clipboard channel credit wait result: %lu",
      static_cast<unsigned long>(result));
  return false;
}

ClipboardChannelResult CClipboardChannel::Send(
  const KVMFRClipboardMessage& record, const void * data)
{
  const ClipboardChannelWrite write = { record, data };
  size_t accepted = 0;
  return SendBatch(&write, 1, accepted);
}

ClipboardChannelResult CClipboardChannel::SendBatch(
  const ClipboardChannelWrite * records, size_t count, size_t& accepted)
{
  accepted = 0;
  if (!records || !count || count > KVMFR_CLIPBOARD_SLOT_COUNT)
    return ClipboardChannelResult::FAILED;
  for (size_t i = 0; i < count; ++i)
    if (!ValidRecord(records[i].record, records[i].data != nullptr))
      return ClipboardChannelResult::FAILED;

  CSRWSharedLock lifecycleLock(m_lifecycleLock);
  if (!Available() || !m_out || !m_doorbell)
    return ClipboardChannelResult::FAILED;

  bool failed = false;
  {
    CSRWExclusiveLock writeLock(m_writeLock);
    while (accepted < count)
    {
      uint32_t ticket;
      ClipboardRingSlot * slot = CClipboardRing::BeginWrite(*m_out, ticket);
      if (!slot)
        break;

      const ClipboardChannelWrite& write = records[accepted];
      slot->header = write.record;
      if (write.record.length)
        memcpy(slot->data, write.data, write.record.length);
      if (!CClipboardRing::EndWrite(*m_out, ticket))
      {
        failed = true;
        break;
      }
      ++accepted;
    }
  }

  if (accepted)
  {
    // Once the producer index advances the records belong to the channel.
    // Doorbells are only a latency optimization; the peer also polls.
    m_doorbell->ClipboardKick(m_epoch);
  }
  if (failed)
    return ClipboardChannelResult::FAILED;
  return accepted == count ? ClipboardChannelResult::ACCEPTED :
    ClipboardChannelResult::BUSY;
}

void CClipboardChannel::SetHandler(IClipboardChannelHandler * handler)
{
  if (InClipboardCallback(this))
  {
    m_handler = handler;
  }
  else
  {
    CSRWExclusiveLock lock(m_handlerLock);
    m_handler = handler;
  }

  if (!handler)
    return;

  uint64_t epoch;
  bool available;
  {
    CSRWSharedLock lock(m_lifecycleLock);
    available = Available();
    epoch = m_epoch;
  }
  PublishState(available, epoch);
  if (available)
    Kick(epoch);
}

void CClipboardChannel::ClearHandler(IClipboardChannelHandler * handler)
{
  if (InClipboardCallback(this))
  {
    if (m_handler == handler)
      m_handler = nullptr;
    return;
  }

  // The exclusive callback lock makes return from ClearHandler a
  // synchronous callback-quiescence point.
  CSRWExclusiveLock lock(m_handlerLock);
  if (m_handler == handler)
    m_handler = nullptr;
}

CClipboardChannel::DrainResult CClipboardChannel::Drain()
{
  struct CreditNotifier
  {
    bool pending = false;
    IClipboardChannelDoorbell * doorbell = nullptr;
    uint64_t epoch = 0;

    void Notify()
    {
      if (pending && doorbell)
        doorbell->ClipboardKick(epoch);
      pending = false;
    }

    ~CreditNotifier() { Notify(); }
  } credit;
  size_t consumed = 0;

  for (;;)
  {
    KVMFRClipboardMessage record = {};
    std::vector<uint8_t> data;
    uint32_t ticket;
    {
      CSRWSharedLock lifecycleLock(m_lifecycleLock);
      if (!Available() || !m_in || !m_doorbell)
        return DrainResult::STOPPED;

      const ClipboardRingSlot * slot = nullptr;
      const ClipboardRingReadResult read =
        CClipboardRing::BeginRead(*m_in, ticket, slot);
      if (read == ClipboardRingReadResult::EMPTY)
        return DrainResult::IDLE;
      if (read == ClipboardRingReadResult::CORRUPT)
        return DrainResult::CORRUPT;

      record = slot->header;
      if (CClipboardRing::Valid(*slot) &&
          ValidRecord(record, record.length != 0) && record.length)
      {
        try
        {
          data.resize(record.length);
          memcpy(data.data(), slot->data, record.length);
        }
        catch (const std::bad_alloc&)
        {
          return DrainResult::BUSY;
        }
      }
    }

    ClipboardChannelResult result =
      ValidRecord(record, record.length != 0) ?
        PublishRecord(record, std::move(data)) :
        ClipboardChannelResult::FAILED;
    if (result == ClipboardChannelResult::BUSY)
      return DrainResult::BUSY;

    {
      CSRWSharedLock lifecycleLock(m_lifecycleLock);
      if (!Available() || !m_in || !m_doorbell)
        return DrainResult::STOPPED;
      if (!CClipboardRing::EndRead(*m_in, ticket))
        return DrainResult::CORRUPT;
      credit.pending  = true;
      credit.doorbell = m_doorbell;
      credit.epoch    = m_epoch;
    }
    if (++consumed == KVMFR_CLIPBOARD_SLOT_COUNT)
    {
      credit.Notify();
      consumed = 0;
    }

    if (result == ClipboardChannelResult::FAILED)
    {
      // The invalid record has been consumed so it cannot be retried. Make
      // this endpoint terminal as well; Thread::Fail performs the single
      // local callback and peer reset outside the ring/lifecycle locks.
      return DrainResult::CORRUPT;
    }
  }
}

void CClipboardChannel::Thread()
{
  HANDLE events[] = { m_stop, m_kick };
  const uint64_t instance = m_instance;
  for (;;)
  {
    const DWORD result = WaitForMultipleObjects(
      ARRAYSIZE(events), events, FALSE, POLL_MS);
    if (result == WAIT_FIRST_OBJECT_VALUE)
      break;
    if (result != WAIT_FIRST_OBJECT_VALUE + 1 && result != WAIT_TIMEOUT)
    {
      const DWORD error = GetLastError();
      DEBUG_ERROR_HR(error,
        "Failed to wait for clipboard channel work");
      Fail(instance, error ? error : ERROR_GEN_FAILURE);
      break;
    }

    const DrainResult drain = Drain();
    if (drain == DrainResult::STOPPED)
      break;
    if (drain == DrainResult::CORRUPT)
    {
      Fail(instance, ERROR_INVALID_DATA);
      break;
    }
  }
  CleanupDeferredDetach();
}

void CClipboardChannel::Fail(
  uint64_t instance, uint32_t reason, bool resetPeer)
{
  IClipboardChannelDoorbell * doorbell;
  uint64_t epoch;
  {
    CSRWExclusiveLock lock(m_lifecycleLock);
    if (instance != m_instance)
      return;
    if (!Atomic::Swap(m_available, false, std::memory_order_acq_rel))
      return;
    epoch = m_epoch;
    doorbell = m_doorbell;
    if (m_stop)
      SetEvent(m_stop);
  }

  PublishReset(epoch, reason);
  if (resetPeer && doorbell)
    doorbell->ClipboardResetPeer(epoch, reason);
  PublishState(false, epoch);
}

void CClipboardChannel::CleanupDeferredDetach()
{
  CSRWExclusiveLock lock(m_lifecycleLock);
  if (!m_deferredDetach || m_threadId != GetCurrentThreadId())
    return;

  if (m_kick)
    CloseHandle(m_kick);
  if (m_writeReady)
    CloseHandle(m_writeReady);
  if (m_stop)
    CloseHandle(m_stop);
  if (m_view)
    UnmapViewOfFile(m_view);
  if (m_mapping)
    CloseHandle(m_mapping);

  // A later external Detach closes the now-signaled thread handle.
  m_threadId = 0;
  m_writeReady = nullptr;
  m_kick = nullptr;
  m_stop = nullptr;
  m_in = nullptr;
  m_out = nullptr;
  m_view = nullptr;
  m_mapping = nullptr;
  m_doorbell = nullptr;
  m_epoch = 0;
  m_deferredDetach = false;
}

void CClipboardChannel::PublishState(bool available, uint64_t epoch)
{
  if (InClipboardCallback(this))
  {
    if (m_handler)
      m_handler->ClipboardState(available, epoch);
    return;
  }

  CSRWExclusiveLock lock(m_handlerLock);
  if (m_handler)
  {
    CClipboardCallbackScope scope(this);
    m_handler->ClipboardState(available, epoch);
  }
}

ClipboardChannelResult CClipboardChannel::PublishRecord(
  const KVMFRClipboardMessage& record, std::vector<uint8_t>&& data)
{
  if (InClipboardCallback(this))
    return m_handler ?
      m_handler->ClipboardRecord(record, std::move(data)) :
      ClipboardChannelResult::BUSY;

  CSRWExclusiveLock lock(m_handlerLock);
  if (!m_handler)
    return ClipboardChannelResult::BUSY;

  CClipboardCallbackScope scope(this);
  return m_handler->ClipboardRecord(record, std::move(data));
}

void CClipboardChannel::PublishReset(uint64_t epoch, uint32_t reason)
{
  if (InClipboardCallback(this))
  {
    if (m_handler)
      m_handler->ClipboardReset(epoch, reason);
    return;
  }

  CSRWExclusiveLock lock(m_handlerLock);
  if (m_handler)
  {
    CClipboardCallbackScope scope(this);
    m_handler->ClipboardReset(epoch, reason);
  }
}

uint64_t CClipboardChannel::Epoch()
{
  CSRWSharedLock lock(m_lifecycleLock);
  return m_epoch;
}

DWORD WINAPI CClipboardChannel::ThreadProc(void * context)
{
  static_cast<CClipboardChannel *>(context)->Thread();
  return 0;
}
