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

#include "transport/lgmp/CLGMPClipboardTransport.h"

#include "CDebug.h"
#include "Seq.h"
#include "transport/lgmp/CLGMPHost.h"

#include <limits>
#include <string.h>

namespace
{
  static constexpr DWORD WAIT_FIRST_OBJECT_VALUE = 0;

  static const LGMPQueueConfig CLIPBOARD_QUEUE_CONFIG =
  {
    LGMP_Q_CLIPBOARD,
    LGMP_Q_CLIPBOARD_LEN,
    1000,
  };

  bool EmptyControl(const KVMFRClipboardMessage& message,
    bool keepToken = false)
  {
    return !message.clipboardGeneration && !message.transfer &&
      !message.offset && !message.size && !message.format &&
      !message.flags && (keepToken || !message.token) && !message.length &&
      !message.sequence;
  }

  bool AddValid(uint64_t offset, uint32_t length)
  {
    return offset <= (std::numeric_limits<uint64_t>::max)() - length;
  }

  bool ValidOffer(const KVMFRClipboardMessage& message)
  {
    return message.clipboardGeneration && message.token &&
      !(message.token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL) &&
      !message.transfer && !message.offset && !message.size &&
      !message.format && !message.flags && !message.length &&
      !message.sequence;
  }

  bool ValidClear(const KVMFRClipboardMessage& message)
  {
    return message.clipboardGeneration && !message.token &&
      !message.transfer && !message.offset && !message.size &&
      !message.format && !message.flags && !message.length &&
      !message.sequence;
  }

  bool ValidRequest(const KVMFRClipboardMessage& message, bool helper)
  {
    const bool validTransfer = helper ?
      kvmfrClipboardTransferFromHelper(message.transfer) :
      kvmfrClipboardTransferFromClient(message.transfer);
    return message.clipboardGeneration && validTransfer &&
      kvmfrClipboardFormatValid(message.format) &&
      !message.offset && !message.size && !message.flags &&
      !message.token && !message.length && !message.sequence;
  }

  bool ValidCancel(const KVMFRClipboardMessage& message)
  {
    return message.transfer && !message.offset && !message.size &&
      (!message.format || kvmfrClipboardFormatValid(message.format)) &&
      !message.flags && !message.length && !message.sequence;
  }

  bool OwnerScopedLifecycle(const KVMFRClipboardMessage& message)
  {
    return message.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST ||
      message.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
      message.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL;
  }
}

CLGMPClipboardTransport::~CLGMPClipboardTransport()
{
  DeInit();
}

bool CLGMPClipboardTransport::Initialize()
{
  if (m_queue)
    return true;

  LGMP_STATUS status = m_host.CreateQueue(
    CLIPBOARD_QUEUE_CONFIG, &m_queue);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("lgmpHostQueueCreate Failed (Clipboard): %s",
      lgmpStatusString(status));
    return false;
  }

  for (PLGMPMemory& memory : m_statusMemory)
  {
    status = m_host.Allocate(sizeof(KVMFRClipboardStatus), &memory);
    if (status != LGMP_OK)
      goto fail;
    memset(lgmpHostMemPtr(memory), 0, sizeof(KVMFRClipboardStatus));
  }

  for (PLGMPMemory& memory : m_messageMemory)
  {
    status = m_host.Allocate(sizeof(KVMFRClipboardMessage), &memory);
    if (status != LGMP_OK)
      goto fail;
    memset(lgmpHostMemPtr(memory), 0, sizeof(KVMFRClipboardMessage));
  }

  for (PLGMPMemory& memory : m_grantMemory)
  {
    status = m_host.Allocate(SLOT_BYTES, &memory);
    if (status != LGMP_OK)
      goto fail;
    memset(lgmpHostMemPtr(memory), 0, SLOT_BYTES);
  }

  for (PLGMPMemory& memory : m_dataMemory)
  {
    status = m_host.Allocate(SLOT_BYTES, &memory);
    if (status != LGMP_OK)
      goto fail;
    memset(lgmpHostMemPtr(memory), 0, SLOT_BYTES);
  }

  return true;

fail:
  DEBUG_ERROR("lgmpHostMemAlloc Failed (Clipboard): %s",
    lgmpStatusString(status));
  DeInit();
  return false;
}

void CLGMPClipboardTransport::DeInit()
{
  Stop();
  for (PLGMPMemory& memory : m_dataMemory)
    lgmpHostMemFree(&memory);
  for (PLGMPMemory& memory : m_grantMemory)
    lgmpHostMemFree(&memory);
  for (PLGMPMemory& memory : m_messageMemory)
    lgmpHostMemFree(&memory);
  for (PLGMPMemory& memory : m_statusMemory)
    lgmpHostMemFree(&memory);
  m_queue = nullptr;
}

void CLGMPClipboardTransport::Wake()
{
  if (m_wakeEvent)
    SetEvent(m_wakeEvent);
}

bool CLGMPClipboardTransport::Start(IClipboardTarget& target)
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (m_thread)
  {
    if (WaitForSingleObject(m_thread, 0) == WAIT_TIMEOUT)
      return true;

    CloseHandle(m_thread);
    CloseHandle(m_wakeEvent);
    CloseHandle(m_stopEvent);
    m_thread    = nullptr;
    m_wakeEvent = nullptr;
    m_stopEvent = nullptr;
  }
  if (!m_queue)
    return false;

  m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_stopEvent || !m_wakeEvent)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGMP clipboard worker events");
    if (m_wakeEvent)
      CloseHandle(m_wakeEvent);
    if (m_stopEvent)
      CloseHandle(m_stopEvent);
    m_wakeEvent = nullptr;
    m_stopEvent = nullptr;
    return false;
  }

  {
    CSRWExclusiveLock lock(m_lock);
    m_target = &target;
    m_failed = false;
    m_statusDirty = true;
  }
  m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
  if (!m_thread)
  {
    DEBUG_ERROR_HR(GetLastError(),
      "Failed to create LGMP clipboard worker");
    {
      CSRWExclusiveLock lock(m_lock);
      m_target = nullptr;
    }
    CloseHandle(m_wakeEvent);
    CloseHandle(m_stopEvent);
    m_wakeEvent = nullptr;
    m_stopEvent = nullptr;
    return false;
  }
  return true;
}

void CLGMPClipboardTransport::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_lifecycleLock);
  if (m_stopEvent)
    SetEvent(m_stopEvent);
  if (m_thread)
    WaitForSingleObject(m_thread, INFINITE);

  if (m_thread)
    CloseHandle(m_thread);
  if (m_wakeEvent)
    CloseHandle(m_wakeEvent);
  if (m_stopEvent)
    CloseHandle(m_stopEvent);
  m_thread    = nullptr;
  m_wakeEvent = nullptr;
  m_stopEvent = nullptr;

  CSRWExclusiveLock lock(m_lock);
  DropPendingTarget();
  ResetProtocol(false);
  m_available = false;
  m_endpointGeneration = 0;
  m_target = nullptr;
  m_failed = false;
  m_statusDirty = true;
}

bool CLGMPClipboardTransport::IsOwner(
  uint32_t clientID, uint32_t generation) const
{
  return m_ownerClientID == clientID &&
    m_ownerGeneration == generation;
}

bool CLGMPClipboardTransport::OwnerSubscribed() const
{
  if (!m_ownerClientID)
    return false;

  uint32_t clients[LGMP_MAX_CLIENTS] = {};
  unsigned count = 0;
  if (lgmpHostGetClientIDs(m_queue, clients, &count) != LGMP_OK)
    return false;
  for (unsigned i = 0; i < count; ++i)
    if (clients[i] == m_ownerClientID)
      return true;
  return false;
}

void CLGMPClipboardTransport::RenewLease()
{
  m_ownerDeadline = GetTickCount64() + OWNER_LEASE_MS;
}

void CLGMPClipboardTransport::BlockOutbound(
  const KVMFRClipboardMessage& record, uint32_t ownerClientID,
  uint32_t ownerGeneration)
{
  m_outboundBlocked = true;
  m_blockedOutbound = record;
  m_blockedOwnerClientID = ownerClientID;
  m_blockedOwnerGeneration = ownerGeneration;
}

void CLGMPClipboardTransport::ClearOutboundBlock()
{
  m_outboundBlocked = false;
  m_blockedOutbound = {};
  m_blockedOwnerClientID = 0;
  m_blockedOwnerGeneration = 0;
}

bool CLGMPClipboardTransport::BlockedOwnerLost(
  const KVMFRClipboardMessage& record) const
{
  return m_outboundBlocked && OwnerScopedLifecycle(record) &&
    memcmp(&m_blockedOutbound, &record, sizeof(record)) == 0 &&
    (!m_available || record.generation != m_endpointGeneration ||
     m_blockedOwnerClientID != m_ownerClientID ||
     m_blockedOwnerGeneration != m_ownerGeneration);
}

void CLGMPClipboardTransport::QueueHelperClear()
{
  if (!m_clientClipboardGeneration || !m_target ||
      !m_available || !m_endpointGeneration)
    return;

  KVMFRClipboardMessage clear = {};
  clear.version             = KVMFR_CLIPBOARD_VERSION;
  clear.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
  clear.generation          = m_endpointGeneration;
  clear.clipboardGeneration = m_clientClipboardGeneration;
  m_clientClipboardGeneration = 0;
  m_clientFormats = 0;
  QueueInternalTarget(clear);
}

void CLGMPClipboardTransport::QueueTransferCancel(
  const Transfer& transfer)
{
  if (!transfer.Active() || !m_target || !m_available ||
      !m_endpointGeneration)
    return;

  KVMFRClipboardMessage cancel = {};
  cancel.version             = KVMFR_CLIPBOARD_VERSION;
  cancel.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
  cancel.generation          = m_endpointGeneration;
  cancel.clipboardGeneration = transfer.clipboardGeneration;
  cancel.transfer            = transfer.transfer;
  cancel.format              = transfer.format;
  cancel.token               = ERROR_DEVICE_NOT_CONNECTED;
  QueueInternalTarget(cancel);
}

void CLGMPClipboardTransport::QueueStaleRequestCancel(
  const KVMFRClipboardMessage& request)
{
  if (request.type != KVMFR_CLIPBOARD_MESSAGE_REQUEST ||
      !m_target || !m_available || !m_endpointGeneration)
    return;

  KVMFRClipboardMessage cancel = {};
  cancel.version             = KVMFR_CLIPBOARD_VERSION;
  cancel.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
  cancel.generation          = m_endpointGeneration;
  cancel.clipboardGeneration = request.clipboardGeneration;
  cancel.transfer            = request.transfer;
  cancel.format              = request.format;
  cancel.token               = ERROR_DEVICE_NOT_CONNECTED;
  QueueInternalTarget(cancel);
}

bool CLGMPClipboardTransport::QueueInternalTarget(
  const KVMFRClipboardMessage& record)
{
  if (!m_pendingTarget.valid && !m_internalTargetCount)
    return BeginTarget(record, nullptr, -2);
  if (m_internalTargetCount == INTERNAL_TARGET_COUNT)
  {
    m_failed = true;
    return false;
  }
  m_internalTarget[m_internalTargetCount++] = record;
  return true;
}

bool CLGMPClipboardTransport::PumpInternalTarget()
{
  if (m_pendingTarget.valid || !m_internalTargetCount)
    return true;

  const KVMFRClipboardMessage record = m_internalTarget[0];
  for (unsigned i = 1; i < m_internalTargetCount; ++i)
    m_internalTarget[i - 1] = m_internalTarget[i];
  m_internalTarget[--m_internalTargetCount] = {};
  return BeginTarget(record, nullptr, -2);
}

void CLGMPClipboardTransport::ReleaseOwner(
  const char * reason, bool clearHelper)
{
  if (!m_ownerClientID)
    return;

  const uint32_t clientID = m_ownerClientID;
  const uint32_t generation = m_ownerGeneration;
  const Transfer clientToHelper = m_clientToHelper;
  const Transfer helperToClient = m_helperToClient;
  m_ownerClientID   = 0;
  m_ownerGeneration = 0;
  m_ownerDeadline   = 0;
  m_replayPending   = false;
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  if (helperToClient.Active())
    m_discardHelperToClient = helperToClient.transfer;
  for (Grant& grant : m_grants)
  {
    grant.generation = 0;
    grant.offered    = false;
    grant.committed  = false;
  }
  m_statusDirty = true;

  DEBUG_INFO("Clipboard owner %u generation %u released (%s)",
    clientID, generation, reason);
  if (clearHelper)
  {
    QueueTransferCancel(clientToHelper);
    QueueTransferCancel(helperToClient);
    QueueHelperClear();
  }
  else
  {
    m_clientClipboardGeneration = 0;
    m_clientFormats = 0;
  }
}

void CLGMPClipboardTransport::ResetProtocol(bool keepClipboard)
{
  DropPendingTarget();
  if (m_ownerClientID)
    ReleaseOwner("endpoint reset", false);
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  m_clientClipboardGeneration = 0;
  m_clientFormats = 0;
  m_discardHelperToClient = 0;
  m_replayPending = false;
  ClearOutboundBlock();
  m_internalTargetCount = 0;
  for (KVMFRClipboardMessage& record : m_internalTarget)
    record = {};
  if (!keepClipboard)
  {
    m_cachedValid = false;
    m_cachedClipboard = {};
    m_helperFormats = 0;
  }
  m_statusDirty = true;
}

PLGMPMemory CLGMPClipboardTransport::FindAvailable(
  PLGMPMemory (&memory)[MEMORY_COUNT]) const
{
  for (PLGMPMemory candidate : memory)
    if (candidate && !lgmpHostQueuePayloadPending(m_queue, candidate))
      return candidate;
  return nullptr;
}

CLGMPClipboardTransport::PostResult
CLGMPClipboardTransport::PostForOwner(
  uint64_t udata, PLGMPMemory memory)
{
  if (!m_ownerClientID)
    return PostResult::GONE;

  unsigned recipients = 0;
  const uint32_t clientID = m_ownerClientID;
  const LGMP_STATUS status = lgmpHostQueuePostForClients(
    m_queue, udata, memory, &clientID, 1, &recipients);
  if (status == LGMP_ERR_QUEUE_FULL)
    return PostResult::BUSY;
  if (status != LGMP_OK)
  {
    Fail("lgmpHostQueuePostForClients", status);
    return PostResult::FAILED;
  }
  return recipients ? PostResult::POSTED : PostResult::GONE;
}

void CLGMPClipboardTransport::Fail(
  const char * operation, LGMP_STATUS status)
{
  if (!m_failed)
    DEBUG_ERROR("%s Failed (Clipboard): %s",
      operation, lgmpStatusString(status));
  m_failed = true;
  if (m_stopEvent)
    SetEvent(m_stopEvent);
}

bool CLGMPClipboardTransport::PublishStatus()
{
  if (lgmpHostQueueNewSubs(m_queue))
    m_statusDirty = true;
  if (!m_statusDirty)
    return true;

  uint32_t clients[LGMP_MAX_CLIENTS] = {};
  unsigned clientCount = 0;
  LGMP_STATUS status =
    lgmpHostGetClientIDs(m_queue, clients, &clientCount);
  if (status != LGMP_OK)
  {
    Fail("lgmpHostGetClientIDs", status);
    return false;
  }
  if (!clientCount)
    return true;

  PLGMPMemory memory = FindAvailable(m_statusMemory);
  if (!memory)
    return true;

  KVMFRClipboardStatus clipboardStatus = {};
  clipboardStatus.version    = KVMFR_CLIPBOARD_VERSION;
  clipboardStatus.generation = m_endpointGeneration;
  clipboardStatus.lease      = static_cast<uint32_t>(OWNER_LEASE_MS);
  clipboardStatus.slotBytes  = KVMFR_CLIPBOARD_DATA_BYTES;
  if (m_available)
  {
    clipboardStatus.flags |= KVMFR_CLIPBOARD_STATUS_AVAILABLE;
    clipboardStatus.formats = m_helperFormats;
  }
  if (m_available && m_ownerClientID)
  {
    clipboardStatus.flags |= KVMFR_CLIPBOARD_STATUS_HAS_OWNER;
    clipboardStatus.ownerClientID   = m_ownerClientID;
    clipboardStatus.ownerGeneration = m_ownerGeneration;
  }
  memcpy(lgmpHostMemPtr(memory), &clipboardStatus,
    sizeof(clipboardStatus));

  const uint32_t serial = Seq::Next(m_statusSerial);
  unsigned recipients = 0;
  status = lgmpHostQueuePostForClients(m_queue,
    KVMFR_CLIPBOARD_QUEUE_UDATA(KVMFR_CLIPBOARD_QUEUE_STATUS, serial),
    memory, clients, clientCount, &recipients);
  if (status == LGMP_ERR_QUEUE_FULL)
    return true;
  if (status != LGMP_OK)
  {
    Fail("lgmpHostQueuePostForClients", status);
    return false;
  }
  if (recipients)
  {
    m_statusSerial = serial;
    m_statusDirty = false;
  }
  return true;
}

bool CLGMPClipboardTransport::PostGrants()
{
  if (!m_available || !m_ownerClientID)
    return true;

  for (unsigned i = 0; i < MEMORY_COUNT; ++i)
  {
    Grant& grant = m_grants[i];
    if (grant.offered || grant.committed ||
        lgmpHostQueuePayloadPending(m_queue, m_grantMemory[i]))
      continue;

    KVMFRClipboardSlotHeader header = {};
    header.version    = KVMFR_CLIPBOARD_VERSION;
    header.type       = KVMFR_CLIPBOARD_MESSAGE_GRANT;
    header.generation = m_ownerGeneration;
    header.size       = KVMFR_CLIPBOARD_DATA_BYTES;
    header.token      = i + 1;
    memcpy(lgmpHostMemPtr(m_grantMemory[i]), &header, sizeof(header));

    const PostResult result = PostForOwner(
      KVMFR_CLIPBOARD_QUEUE_UDATA(
        KVMFR_CLIPBOARD_QUEUE_GRANT, i + 1), m_grantMemory[i]);
    if (result == PostResult::POSTED)
    {
      grant.generation = m_ownerGeneration;
      grant.offered    = true;
    }
    else if (result == PostResult::BUSY)
      return true;
    else if (result == PostResult::GONE)
    {
      ReleaseOwner("subscriber disappeared", true);
      return true;
    }
    else
      return false;
  }
  return true;
}

bool CLGMPClipboardTransport::ReplayClipboard()
{
  if (!m_replayPending || !m_ownerClientID)
    return true;
  if (!m_cachedValid)
  {
    m_replayPending = false;
    return true;
  }

  PLGMPMemory memory = FindAvailable(m_messageMemory);
  if (!memory)
    return true;
  KVMFRClipboardMessage message = m_cachedClipboard;
  message.generation = m_ownerGeneration;
  memcpy(lgmpHostMemPtr(memory), &message, sizeof(message));

  const uint32_t serial = Seq::Next(m_messageSerial);
  const PostResult result = PostForOwner(
    KVMFR_CLIPBOARD_QUEUE_UDATA(
      KVMFR_CLIPBOARD_QUEUE_MESSAGE, serial), memory);
  if (result == PostResult::POSTED)
  {
    m_messageSerial = serial;
    m_replayPending = false;
  }
  else if (result == PostResult::GONE)
    ReleaseOwner("subscriber disappeared", true);
  return result != PostResult::FAILED;
}

bool CLGMPClipboardTransport::ValidateClaim(
  const KVMFRClipboardMessage& message) const
{
  return message.version == KVMFR_CLIPBOARD_VERSION &&
    message.type == KVMFR_CLIPBOARD_MESSAGE_CLAIM &&
    message.generation && message.token == m_endpointGeneration &&
    EmptyControl(message, true);
}

bool CLGMPClipboardTransport::ValidateOwnedControl(
  const KVMFRClipboardMessage& message) const
{
  if (message.version != KVMFR_CLIPBOARD_VERSION ||
      message.generation != m_ownerGeneration)
    return false;

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_RELEASE:
    case KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE:
      return EmptyControl(message);

    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      return ValidateInboundRecord(message);

    case KVMFR_CLIPBOARD_MESSAGE_COMMIT:
      return message.token >= 1 && message.token <= MEMORY_COUNT &&
        message.clipboardGeneration && message.transfer &&
        kvmfrClipboardFormatValid(message.format) &&
        message.length <= KVMFR_CLIPBOARD_DATA_BYTES &&
        !(message.flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
          KVMFR_CLIPBOARD_FLAG_END)) &&
        AddValid(message.offset, message.length);

    default:
      return false;
  }
}

bool CLGMPClipboardTransport::ValidateInboundRecord(
  const KVMFRClipboardMessage& message) const
{
  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      return ValidOffer(message);

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      return ValidClear(message);

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      return ValidRequest(message, false);

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      return ValidCancel(message);

    default:
      return false;
  }
}

bool CLGMPClipboardTransport::ValidateOutboundRecord(
  const KVMFRClipboardMessage& message) const
{
  if (message.version != KVMFR_CLIPBOARD_VERSION ||
      message.generation != m_endpointGeneration)
    return false;

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      return ValidOffer(message);

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      return ValidClear(message);

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      return ValidRequest(message, true);

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
      return message.clipboardGeneration &&
        kvmfrClipboardTransferFromClient(message.transfer) &&
        kvmfrClipboardFormatValid(message.format) &&
        message.length <= KVMFR_CLIPBOARD_DATA_BYTES &&
        !(message.flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
          KVMFR_CLIPBOARD_FLAG_END)) && !message.token &&
        AddValid(message.offset, message.length);

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      return ValidCancel(message);

    default:
      return false;
  }
}

bool CLGMPClipboardTransport::ValidateChunk(
  const Transfer& transfer, const KVMFRClipboardMessage& message) const
{
  if (!transfer.Active() || message.transfer != transfer.transfer ||
      message.clipboardGeneration != transfer.clipboardGeneration ||
      message.format != transfer.format ||
      message.offset != transfer.nextOffset ||
      message.sequence != transfer.nextSequence ||
      !AddValid(message.offset, message.length))
    return false;

  const uint64_t end = message.offset + message.length;
  if (!message.length &&
      !(message.flags & (KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END)))
    return false;
  if (!transfer.began)
  {
    if (message.offset || message.sequence ||
        !(message.flags & KVMFR_CLIPBOARD_FLAG_BEGIN))
      return false;
  }
  else if (message.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)
    return false;
  else if (!(message.flags & KVMFR_CLIPBOARD_FLAG_END) &&
      message.size != KVMFR_CLIPBOARD_SIZE_UNKNOWN)
    return false;

  const uint64_t hint = transfer.began ? transfer.sizeHint : message.size;
  if (hint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && end > hint)
    return false;
  if (message.flags & KVMFR_CLIPBOARD_FLAG_END)
    return message.size == end &&
      (hint == KVMFR_CLIPBOARD_SIZE_UNKNOWN || hint == end);
  return true;
}

void CLGMPClipboardTransport::AdvanceChunk(
  Transfer& transfer, const KVMFRClipboardMessage& message)
{
  if (!transfer.began)
  {
    transfer.began = true;
    transfer.sizeHint = message.size;
  }
  transfer.nextOffset += message.length;
  ++transfer.nextSequence;
  if (message.flags & KVMFR_CLIPBOARD_FLAG_END)
    transfer.Clear();
}

void CLGMPClipboardTransport::ApplyInbound(
  const KVMFRClipboardMessage& message)
{
  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      m_clientClipboardGeneration = message.clipboardGeneration;
      m_clientFormats = message.token;
      m_clientToHelper.Clear();
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      m_clientClipboardGeneration = 0;
      m_clientFormats = 0;
      m_clientToHelper.Clear();
      break;

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      m_helperToClient.Clear();
      m_helperToClient.transfer = message.transfer;
      m_helperToClient.clipboardGeneration =
        message.clipboardGeneration;
      m_helperToClient.format = message.format;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
      AdvanceChunk(m_clientToHelper, message);
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      if (m_clientToHelper.transfer == message.transfer)
        m_clientToHelper.Clear();
      if (m_helperToClient.transfer == message.transfer)
        m_helperToClient.Clear();
      break;
  }
}

void CLGMPClipboardTransport::ApplyOutbound(
  const KVMFRClipboardMessage& message)
{
  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      m_cachedClipboard = message;
      m_cachedValid = true;
      m_helperFormats = message.token;
      m_helperToClient.Clear();
      m_replayPending = false;
      m_statusDirty = true;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      m_cachedClipboard = message;
      m_cachedValid = true;
      m_helperFormats = 0;
      m_helperToClient.Clear();
      m_replayPending = false;
      m_statusDirty = true;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      m_clientToHelper.Clear();
      m_clientToHelper.transfer = message.transfer;
      m_clientToHelper.clipboardGeneration =
        message.clipboardGeneration;
      m_clientToHelper.format = message.format;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
      AdvanceChunk(m_helperToClient, message);
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      if (m_clientToHelper.transfer == message.transfer)
        m_clientToHelper.Clear();
      if (m_helperToClient.transfer == message.transfer)
        m_helperToClient.Clear();
      break;
  }
}

bool CLGMPClipboardTransport::BeginTarget(
  const KVMFRClipboardMessage& message, const uint8_t * data, int grant)
{
  if (m_pendingTarget.valid ||
      (message.length && !data) ||
      message.length > KVMFR_CLIPBOARD_DATA_BYTES)
    return false;

  m_pendingTarget.valid       = true;
  m_pendingTarget.acknowledge = grant >= -1;
  m_pendingTarget.grant       = grant;
  m_pendingTarget.record      = message;
  if (message.length)
    memcpy(m_pendingTarget.data, data, message.length);
  return RetryTarget();
}

void CLGMPClipboardTransport::FinishTarget(bool accepted)
{
  const KVMFRClipboardMessage message = m_pendingTarget.record;
  const int grant = m_pendingTarget.grant;
  if (accepted)
  {
    ApplyInbound(message);
    RenewLease();
  }
  if (grant >= 0 && static_cast<unsigned>(grant) < MEMORY_COUNT)
    m_grants[grant] = {};
  m_pendingTarget.Clear();

  if (!accepted)
  {
    ReleaseOwner("Helper delivery failed", false);
    m_failed = true;
  }
}

bool CLGMPClipboardTransport::RetryTarget()
{
  if (!m_pendingTarget.valid)
    return true;

  const bool acknowledge = m_pendingTarget.acknowledge;
  const uint8_t * data = m_pendingTarget.record.length ?
    m_pendingTarget.data : nullptr;
  const ClipboardChannelResult result = m_target ?
    m_target->SendClipboard(m_pendingTarget.record, data) :
    ClipboardChannelResult::FAILED;
  if (result == ClipboardChannelResult::BUSY)
  {
    if (m_ownerClientID)
      RenewLease();
    return true;
  }

  FinishTarget(result == ClipboardChannelResult::ACCEPTED);
  if (acknowledge)
  {
    const LGMP_STATUS status = lgmpHostAckData(m_queue);
    if (status != LGMP_OK)
    {
      Fail("lgmpHostAckData", status);
      return false;
    }
  }
  return true;
}

void CLGMPClipboardTransport::DropPendingTarget()
{
  if (!m_pendingTarget.valid)
    return;

  const bool acknowledge = m_pendingTarget.acknowledge;
  const int grant = m_pendingTarget.grant;
  if (grant >= 0 && static_cast<unsigned>(grant) < MEMORY_COUNT)
    m_grants[grant] = {};
  m_pendingTarget.Clear();
  if (acknowledge && m_queue)
  {
    const LGMP_STATUS status = lgmpHostAckData(m_queue);
    if (status != LGMP_OK)
      Fail("lgmpHostAckData", status);
  }
}

bool CLGMPClipboardTransport::ProcessMessage(
  uint32_t clientID, const KVMFRClipboardMessage& message)
{
  if (message.type == KVMFR_CLIPBOARD_MESSAGE_CLAIM)
  {
    if (!m_available || !ValidateClaim(message))
      return true;
    if (m_ownerClientID)
    {
      if (IsOwner(clientID, message.generation))
        RenewLease();
      return true;
    }

    m_ownerClientID   = clientID;
    m_ownerGeneration = message.generation;
    RenewLease();
    m_statusDirty   = true;
    m_replayPending = m_cachedValid;
    DEBUG_INFO("Clipboard owner %u generation %u acquired",
      m_ownerClientID, m_ownerGeneration);
    return true;
  }

  if (!IsOwner(clientID, message.generation))
    return true;
  if (!ValidateOwnedControl(message))
  {
    ReleaseOwner("invalid clipboard message", true);
    return true;
  }

  if (message.type == KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE)
  {
    RenewLease();
    return true;
  }
  if (message.type == KVMFR_CLIPBOARD_MESSAGE_RELEASE)
  {
    ReleaseOwner("client release", true);
    return true;
  }

  if (message.type == KVMFR_CLIPBOARD_MESSAGE_COMMIT)
  {
    const unsigned grantIndex = message.token - 1;
    Grant& grant = m_grants[grantIndex];
    if (!grant.offered || grant.committed ||
        grant.generation != m_ownerGeneration)
    {
      ReleaseOwner("invalid clipboard grant", true);
      return true;
    }

    const uint8_t * slot = static_cast<const uint8_t *>(
      lgmpHostMemPtr(m_grantMemory[grantIndex]));
    KVMFRClipboardMessage dataMessage = {};
    memcpy(&dataMessage, slot, sizeof(dataMessage));
    const bool matchesCommit =
      dataMessage.version             == KVMFR_CLIPBOARD_VERSION &&
      dataMessage.type                == KVMFR_CLIPBOARD_MESSAGE_DATA &&
      dataMessage.generation          == m_ownerGeneration &&
      dataMessage.clipboardGeneration == message.clipboardGeneration &&
      dataMessage.transfer            == message.transfer &&
      dataMessage.offset              == message.offset &&
      dataMessage.size                == message.size &&
      dataMessage.format              == message.format &&
      dataMessage.flags               == message.flags &&
      dataMessage.length              == message.length &&
      dataMessage.sequence            == message.sequence &&
      !dataMessage.token;
    if (!matchesCommit ||
        !kvmfrClipboardTransferFromHelper(dataMessage.transfer) ||
        !ValidateChunk(m_clientToHelper, dataMessage))
    {
      ReleaseOwner("invalid clipboard commit", true);
      return true;
    }

    grant.committed = true;
    dataMessage.generation = m_endpointGeneration;
    RenewLease();
    BeginTarget(dataMessage,
      slot + sizeof(KVMFRClipboardSlotHeader),
      static_cast<int>(grantIndex));
    return false;
  }

  if (message.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
      m_helperToClient.Active())
  {
    ReleaseOwner("overlapping clipboard request", true);
    return true;
  }
  if (message.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
      (!m_cachedValid ||
       m_cachedClipboard.type != KVMFR_CLIPBOARD_MESSAGE_OFFER ||
       message.clipboardGeneration !=
         m_cachedClipboard.clipboardGeneration ||
       !(m_helperFormats & kvmfrClipboardFormatFlag(message.format))))
  {
    ReleaseOwner("invalid clipboard request", true);
    return true;
  }
  if (message.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL &&
      message.transfer != m_clientToHelper.transfer &&
      message.transfer != m_helperToClient.transfer)
    return true;
  KVMFRClipboardMessage forwarded = message;
  forwarded.generation = m_endpointGeneration;
  RenewLease();
  BeginTarget(forwarded, nullptr, -1);
  return false;
}

bool CLGMPClipboardTransport::DrainMessage()
{
  for (unsigned count = 0; count < 64 && !m_pendingTarget.valid; ++count)
  {
    uint8_t data[LGMP_MSGS_SIZE] = {};
    size_t size = 0;
    uint32_t clientID = 0;
    const LGMP_STATUS status = lgmpHostReadDataWithSource(
      m_queue, data, &size, &clientID);
    if (status == LGMP_ERR_QUEUE_EMPTY)
      return true;
    if (status != LGMP_OK)
    {
      Fail("lgmpHostReadDataWithSource", status);
      return false;
    }

    bool acknowledge = true;
    if (size != sizeof(KVMFRClipboardMessage))
    {
      if (clientID == m_ownerClientID)
        ReleaseOwner("invalid clipboard message size", true);
    }
    else
    {
      KVMFRClipboardMessage message = {};
      memcpy(&message, data, sizeof(message));
      acknowledge = ProcessMessage(clientID, message);
    }

    if (acknowledge)
    {
      const LGMP_STATUS ackStatus = lgmpHostAckData(m_queue);
      if (ackStatus != LGMP_OK)
      {
        Fail("lgmpHostAckData", ackStatus);
        return false;
      }
    }
  }
  return true;
}

ClipboardChannelResult CLGMPClipboardTransport::SendControl(
  const KVMFRClipboardMessage& record)
{
  PLGMPMemory memory = FindAvailable(m_messageMemory);
  if (!memory)
    return ClipboardChannelResult::BUSY;

  KVMFRClipboardMessage message = record;
  message.generation = m_ownerGeneration;
  memcpy(lgmpHostMemPtr(memory), &message, sizeof(message));
  const uint32_t serial = Seq::Next(m_messageSerial);
  const PostResult result = PostForOwner(
    KVMFR_CLIPBOARD_QUEUE_UDATA(
      KVMFR_CLIPBOARD_QUEUE_MESSAGE, serial), memory);
  if (result == PostResult::POSTED)
  {
    m_messageSerial = serial;
    ApplyOutbound(record);
    return ClipboardChannelResult::ACCEPTED;
  }
  if (result == PostResult::BUSY)
    return ClipboardChannelResult::BUSY;
  if (result == PostResult::GONE)
  {
    ReleaseOwner("subscriber disappeared", true);
    return record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR ?
      ClipboardChannelResult::ACCEPTED : ClipboardChannelResult::BUSY;
  }
  return ClipboardChannelResult::FAILED;
}

ClipboardChannelResult CLGMPClipboardTransport::SendData(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  if (!ValidateChunk(m_helperToClient, record))
    return ClipboardChannelResult::FAILED;

  PLGMPMemory memory = FindAvailable(m_dataMemory);
  if (!memory)
    return ClipboardChannelResult::BUSY;

  KVMFRClipboardMessage message = record;
  message.generation = m_ownerGeneration;
  uint8_t * slot = static_cast<uint8_t *>(lgmpHostMemPtr(memory));
  memcpy(slot, &message, sizeof(message));
  if (message.length)
    memcpy(slot + sizeof(KVMFRClipboardSlotHeader), data, message.length);

  const uint32_t serial = Seq::Next(m_dataSerial);
  const PostResult result = PostForOwner(
    KVMFR_CLIPBOARD_QUEUE_UDATA(
      KVMFR_CLIPBOARD_QUEUE_DATA, serial), memory);
  if (result == PostResult::POSTED)
  {
    m_dataSerial = serial;
    ApplyOutbound(record);
    return ClipboardChannelResult::ACCEPTED;
  }
  if (result == PostResult::BUSY)
    return ClipboardChannelResult::BUSY;
  if (result == PostResult::GONE)
  {
    ReleaseOwner("subscriber disappeared", true);
    return ClipboardChannelResult::ACCEPTED;
  }
  return ClipboardChannelResult::FAILED;
}

ClipboardChannelResult CLGMPClipboardTransport::SendClipboard(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  CSRWExclusiveLock lock(m_lock);
  if (m_failed || !m_target)
    return ClipboardChannelResult::FAILED;

  // The channel retains a BUSY record for an exact retry. Once the owner
  // epoch that caused the backpressure is gone, that record cannot be
  // delivered to a replacement owner. Consume it and promptly cancel a
  // stale request back toward Helper instead.
  if (BlockedOwnerLost(record))
  {
    QueueStaleRequestCancel(record);
    ClearOutboundBlock();
    Wake();
    return ClipboardChannelResult::ACCEPTED;
  }

  if (!m_available ||
      record.generation != m_endpointGeneration)
    return ClipboardChannelResult::FAILED;

  if (!ValidateOutboundRecord(record) ||
      (record.length && !data))
    return ClipboardChannelResult::FAILED;

  if (record.transfer &&
      record.transfer == m_discardHelperToClient &&
      (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
       record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL))
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL ||
        (record.flags & KVMFR_CLIPBOARD_FLAG_END))
      m_discardHelperToClient = 0;
    ClearOutboundBlock();
    return ClipboardChannelResult::ACCEPTED;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL &&
      record.transfer != m_clientToHelper.transfer &&
      record.transfer != m_helperToClient.transfer)
  {
    ClearOutboundBlock();
    return ClipboardChannelResult::ACCEPTED;
  }

  // Preserve the latest Helper clipboard even when no client owns the
  // endpoint. It is replayed to the next successful claimant.
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR)
  {
    m_cachedClipboard = record;
    m_cachedValid = true;
    m_helperFormats = record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ?
      record.token : 0;
    m_statusDirty = true;
    if (!m_ownerClientID)
    {
      ClearOutboundBlock();
      Wake();
      return ClipboardChannelResult::ACCEPTED;
    }
  }

  if (!m_ownerClientID)
  {
    if (OwnerScopedLifecycle(record))
    {
      QueueStaleRequestCancel(record);
      ClearOutboundBlock();
      Wake();
      return ClipboardChannelResult::ACCEPTED;
    }
    return ClipboardChannelResult::FAILED;
  }

  const uint32_t ownerClientID = m_ownerClientID;
  const uint32_t ownerGeneration = m_ownerGeneration;
  if (!OwnerSubscribed())
  {
    BlockOutbound(record, ownerClientID, ownerGeneration);
    Wake();
    return ClipboardChannelResult::BUSY;
  }

  ClipboardChannelResult result;
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
  {
    if (!m_helperToClient.Active() ||
        record.transfer != m_helperToClient.transfer)
    {
      ClearOutboundBlock();
      return ClipboardChannelResult::ACCEPTED;
    }
    result = SendData(record, data);
  }
  else
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
        m_clientToHelper.Active())
      return ClipboardChannelResult::FAILED;
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
        (record.clipboardGeneration != m_clientClipboardGeneration ||
         !(m_clientFormats & kvmfrClipboardFormatFlag(record.format))))
    {
      QueueStaleRequestCancel(record);
      ClearOutboundBlock();
      return ClipboardChannelResult::ACCEPTED;
    }
    result = SendControl(record);
  }

  if (result == ClipboardChannelResult::BUSY)
    BlockOutbound(record, ownerClientID, ownerGeneration);
  else
    ClearOutboundBlock();
  Wake();
  return result;
}

void CLGMPClipboardTransport::ClipboardState(
  bool available, uint32_t generation)
{
  CSRWExclusiveLock lock(m_lock);
  if (!generation)
    return;

  if (generation != m_endpointGeneration || available != m_available)
  {
    DropPendingTarget();
    m_internalTargetCount = 0;
    for (KVMFRClipboardMessage& record : m_internalTarget)
      record = {};
    if (m_ownerClientID)
      ReleaseOwner("Helper endpoint changed", false);
    m_clientToHelper.Clear();
    m_helperToClient.Clear();
    m_clientClipboardGeneration = 0;
    m_clientFormats = 0;
    m_discardHelperToClient = 0;
    m_cachedClipboard = {};
    m_cachedValid = false;
    m_helperFormats = 0;
  }
  m_available = available;
  m_endpointGeneration = generation;
  m_statusDirty = true;
  Wake();
}

void CLGMPClipboardTransport::ClipboardReset(
  uint32_t generation, uint32_t reason)
{
  UNREFERENCED_PARAMETER(reason);
  CSRWExclusiveLock lock(m_lock);
  if (!generation)
    return;
  DropPendingTarget();
  m_internalTargetCount = 0;
  for (KVMFRClipboardMessage& record : m_internalTarget)
    record = {};
  if (m_ownerClientID)
    ReleaseOwner("Helper reset", false);
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  m_clientClipboardGeneration = 0;
  m_clientFormats = 0;
  m_discardHelperToClient = 0;
  m_cachedClipboard = {};
  m_cachedValid = false;
  m_helperFormats = 0;
  m_endpointGeneration = generation;
  m_statusDirty = true;
  Wake();
}

DWORD CALLBACK CLGMPClipboardTransport::ThreadProc(void * context)
{
  static_cast<CLGMPClipboardTransport *>(context)->Thread();
  return 0;
}

void CLGMPClipboardTransport::Thread()
{
  bool notifyFailed = false;
  for (;;)
  {
    DWORD timeout = IDLE_POLL_MS;
    bool notifyReady = false;
    IClipboardTarget * readyTarget = nullptr;
    {
      CSRWExclusiveLock lock(m_lock);
      if (m_failed)
      {
        notifyFailed = true;
        break;
      }

      if (m_ownerClientID && !OwnerSubscribed())
      {
        DropPendingTarget();
        ReleaseOwner("subscriber disappeared", true);
      }
      else if (m_ownerClientID && GetTickCount64() >= m_ownerDeadline)
        ReleaseOwner("lease expired", true);

      if (!RetryTarget() || !PumpInternalTarget() ||
          !DrainMessage() || !PublishStatus() ||
          !ReplayClipboard() || !PostGrants())
      {
        notifyFailed = true;
        break;
      }

      if (m_outboundBlocked)
      {
        notifyReady = m_target != nullptr;
        readyTarget = m_target;
      }
      if (m_pendingTarget.valid || m_ownerClientID)
        timeout = ACTIVE_POLL_MS;
    }

    // ClipboardReceiveReady may synchronously retry SendClipboard, which
    // acquires m_lock. Never invoke it while holding the transport lock.
    if (notifyReady)
      readyTarget->ClipboardReceiveReady();

    const HANDLE handles[] = { m_stopEvent, m_wakeEvent };
    const DWORD wait = WaitForMultipleObjects(
      _countof(handles), handles, FALSE, timeout);
    if (wait == WAIT_FIRST_OBJECT_VALUE)
      break;
    if (wait != WAIT_FIRST_OBJECT_VALUE + 1 && wait != WAIT_TIMEOUT)
    {
      DEBUG_ERROR_HR(GetLastError(),
        "LGMP clipboard worker wait failed");
      notifyFailed = true;
      break;
    }
  }

  IClipboardTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    DropPendingTarget();
    if (m_ownerClientID)
      ReleaseOwner("transport stopped", false);
    target = m_target;
  }
  if (notifyFailed && target)
    target->ClipboardFailed();
}
