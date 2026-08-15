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

#include <ntstatus.h>

#include "transport/lgmp/CLGMPClipboardTransport.h"

#include <wudfwdm.h>

#include "CDebug.h"
#include "Seq.h"
#include "WCCopy.h"
#include "transport/lgmp/CLGMPHost.h"

#include <limits>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
  #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
  static_assert(sizeof(LGMPStreamDescriptor) ==
      sizeof(KVMFRStreamDescriptor),
    "LGMP and KVMFR stream descriptor sizes differ");
  static_assert(offsetof(LGMPStreamDescriptor, offset) ==
      offsetof(KVMFRStreamDescriptor, offset),
    "LGMP and KVMFR stream descriptor offsets differ");
  static_assert(offsetof(LGMPStreamDescriptor, slotSize) ==
      offsetof(KVMFRStreamDescriptor, slotSize),
    "LGMP and KVMFR stream descriptor geometry differs");

  static const LGMPQueueConfig CLIPBOARD_QUEUE_CONFIG =
  {
    LGMP_Q_CLIPBOARD,
    LGMP_Q_CLIPBOARD_LEN,
    1000,
  };

  bool ArmPollTimer(HANDLE timer, uint32_t waitUs)
  {
    LARGE_INTEGER due = {};
    due.QuadPart = -static_cast<LONGLONG>(waitUs) * 10;
    return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
  }

  bool EmptyControl(const KVMFRClipboardMessage& message,
    bool keepToken = false)
  {
    return !message.clipboardGeneration && !message.transfer &&
      !message.offset && !message.size && !message.format &&
      !message.flags && (keepToken || !message.token) &&
      !message.length && !message.sequence;
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
      kvmfrClipboardRepresentationFormatValid(message.format) &&
      !message.offset && !message.size && !message.flags &&
      !message.token && !message.length && !message.sequence;
  }

  bool ValidCancel(const KVMFRClipboardMessage& message)
  {
    return message.transfer && !message.offset && !message.size &&
      (!message.format ||
        kvmfrClipboardRepresentationFormatValid(message.format)) &&
      !message.flags && !message.length && !message.sequence;
  }

  bool OwnerScopedLifecycle(const KVMFRClipboardMessage& message)
  {
    return message.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST ||
      message.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
      message.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL ||
      CLGMPClipboardFiles::IsRecord(message);
  }

  KVMFRStreamDescriptor StreamDescriptorToWire(
    const LGMPStreamDescriptor& descriptor)
  {
    KVMFRStreamDescriptor wire = {};
    wire.magic      = descriptor.magic;
    wire.version    = descriptor.version;
    wire.size       = descriptor.size;
    wire.offset     = descriptor.offset;
    wire.regionSize = descriptor.regionSize;
    wire.direction  = descriptor.direction;
    wire.policy     = descriptor.policy;
    wire.slotCount  = descriptor.slotCount;
    wire.slotSize   = descriptor.slotSize;
    return wire;
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

  if (!InitializeStreams())
  {
    DeInit();
    return false;
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
  DeInitStreams();
  for (PLGMPMemory& memory : m_messageMemory)
    lgmpHostMemFree(&memory);
  for (PLGMPMemory& memory : m_statusMemory)
    lgmpHostMemFree(&memory);
  m_queue = nullptr;
}

bool CLGMPClipboardTransport::InitializeStreams()
{
  if (m_hostToClientStream && m_clientToHostStream)
    return true;

  const LGMPStreamConfig hostToClient =
  {
    LGMP_STREAM_HOST_TO_CLIENT,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_CLIPBOARD_STREAM_SLOT_COUNT,
    SLOT_BYTES,
  };
  LGMP_STATUS status = m_host.CreateStream(
    hostToClient, &m_hostToClientStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create host-to-client clipboard stream: %s",
      lgmpStatusString(status));
    return false;
  }

  const LGMPStreamConfig clientToHost =
  {
    LGMP_STREAM_CLIENT_TO_HOST,
    LGMP_STREAM_RELIABLE_FIFO,
    KVMFR_CLIPBOARD_STREAM_SLOT_COUNT,
    SLOT_BYTES,
  };
  status = m_host.CreateStream(clientToHost, &m_clientToHostStream);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to create client-to-host clipboard stream: %s",
      lgmpStatusString(status));
    lgmpHostStreamFree(&m_hostToClientStream);
    return false;
  }

  LGMPStreamDescriptor descriptor = {};
  lgmpHostStreamGetDescriptor(m_hostToClientStream, &descriptor);
  m_hostToClientDescriptor = StreamDescriptorToWire(descriptor);
  lgmpHostStreamGetDescriptor(m_clientToHostStream, &descriptor);
  m_clientToHostDescriptor = StreamDescriptorToWire(descriptor);
  return true;
}

void CLGMPClipboardTransport::DeInitStreams()
{
  ForceUnbindStreams();
  lgmpHostStreamFree(&m_clientToHostStream);
  lgmpHostStreamFree(&m_hostToClientStream);
  m_clientToHostDescriptor = {};
  m_hostToClientDescriptor = {};
}

bool CLGMPClipboardTransport::BindStreams(uint32_t clientID) const
{
  if (!m_hostToClientStream || !m_clientToHostStream)
    return false;

  LGMP_STATUS status = lgmpHostStreamBind(
    m_hostToClientStream, clientID, nullptr);
  if (status != LGMP_OK)
  {
    DEBUG_ERROR("Failed to bind host-to-client clipboard stream for "
      "client %u: %s", clientID, lgmpStatusString(status));
    return false;
  }

  status = lgmpHostStreamBind(m_clientToHostStream, clientID, nullptr);
  if (status == LGMP_OK)
    return true;

  DEBUG_ERROR("Failed to bind client-to-host clipboard stream for "
    "client %u: %s", clientID, lgmpStatusString(status));
  const LGMP_STATUS rollback =
    lgmpHostStreamUnbind(m_hostToClientStream);
  if (rollback != LGMP_OK && rollback != LGMP_ERR_STREAM_UNBOUND &&
      rollback != LGMP_ERR_STREAM_BUSY)
    DEBUG_ERROR("Failed to roll back host-to-client clipboard stream "
      "binding: %s", lgmpStatusString(rollback));
  return false;
}

bool CLGMPClipboardTransport::TryUnbindStreams()
{
  bool complete = true;
  if (m_clientToHostStream)
  {
    const LGMP_STATUS status =
      lgmpHostStreamUnbind(m_clientToHostStream);
    if (status == LGMP_ERR_STREAM_BUSY)
      complete = false;
    else if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
    {
      Fail("lgmpHostStreamUnbind(client-to-host)", status);
      complete = false;
    }
  }
  if (m_hostToClientStream)
  {
    const LGMP_STATUS status =
      lgmpHostStreamUnbind(m_hostToClientStream);
    if (status == LGMP_ERR_STREAM_BUSY)
      complete = false;
    else if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
    {
      Fail("lgmpHostStreamUnbind(host-to-client)", status);
      complete = false;
    }
  }
  return complete;
}

void CLGMPClipboardTransport::ForceUnbindStreams() const
{
  if (m_clientToHostStream)
  {
    const LGMP_STATUS status =
      lgmpHostStreamForceUnbind(m_clientToHostStream);
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      DEBUG_ERROR("Failed to force-unbind client-to-host clipboard "
        "stream: %s", lgmpStatusString(status));
  }
  if (m_hostToClientStream)
  {
    const LGMP_STATUS status =
      lgmpHostStreamForceUnbind(m_hostToClientStream);
    if (status != LGMP_OK && status != LGMP_ERR_STREAM_UNBOUND)
      DEBUG_ERROR("Failed to force-unbind host-to-client clipboard "
        "stream: %s", lgmpStatusString(status));
  }
}

void CLGMPClipboardTransport::Wake() const
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
    CloseHandle(m_pollTimer);
    CloseHandle(m_wakeEvent);
    CloseHandle(m_stopEvent);
    m_thread    = nullptr;
    m_pollTimer = nullptr;
    m_wakeEvent = nullptr;
    m_stopEvent = nullptr;
  }
  if (!m_queue)
    return false;

  m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!m_stopEvent)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create LGMP clipboard stop event");
    m_stopEvent = nullptr;
    return false;
  }

  m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!m_wakeEvent)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create LGMP clipboard wake event");
    CloseHandle(m_stopEvent);
    m_stopEvent = nullptr;
    return false;
  }

  m_pollTimer = CreateWaitableTimerExW(nullptr, nullptr,
    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!m_pollTimer)
    m_pollTimer = CreateWaitableTimerExW(
      nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  if (!m_pollTimer)
  {
    const DWORD error = GetLastError();
    DEBUG_ERROR_HR(error,
      "Failed to create LGMP clipboard poll timer");
    CloseHandle(m_wakeEvent);
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
    CloseHandle(m_pollTimer);
    CloseHandle(m_wakeEvent);
    CloseHandle(m_stopEvent);
    m_pollTimer = nullptr;
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
  if (m_pollTimer)
    CloseHandle(m_pollTimer);
  if (m_wakeEvent)
    CloseHandle(m_wakeEvent);
  if (m_stopEvent)
    CloseHandle(m_stopEvent);
  m_thread    = nullptr;
  m_pollTimer = nullptr;
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
    (!m_available || m_ownerReleasing ||
     record.generation != m_endpointGeneration ||
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

void CLGMPClipboardTransport::QueueStaleFileRecord(
  const KVMFRClipboardMessage& record)
{
  if (!m_target || !m_available || !m_endpointGeneration)
    return;

  KVMFRClipboardMessage response = {};
  response.version             = KVMFR_CLIPBOARD_VERSION;
  response.generation          = m_endpointGeneration;
  response.clipboardGeneration = record.clipboardGeneration;
  response.transfer            = record.transfer;
  response.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  response.token               = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE)
    response.type = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED;
  else if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST)
    response.type = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
  else
    return;
  QueueInternalTarget(response);
}

void CLGMPClipboardTransport::QueueFileDisconnect()
{
  KVMFRClipboardMessage records[
    CLGMPClipboardFiles::MAX_ACQUISITIONS +
    CLGMPClipboardFiles::MAX_REQUESTS] = {};
  const unsigned count = m_files.Disconnect(records, _countof(records),
    m_endpointGeneration);
  for (unsigned i = 0; i < count; ++i)
    QueueInternalTarget(records[i]);
}

bool CLGMPClipboardTransport::QueueInternalTarget(
  const KVMFRClipboardMessage& record)
{
  if (!m_pendingTarget.valid && !m_streamTargetPrepared &&
      !m_internalTargetCount)
    return BeginTarget(record, false);
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
  if (m_pendingTarget.valid || m_streamTargetPrepared ||
      !m_internalTargetCount)
    return true;

  const KVMFRClipboardMessage record = m_internalTarget[0];
  for (unsigned i = 1; i < m_internalTargetCount; ++i)
    m_internalTarget[i - 1] = m_internalTarget[i];
  m_internalTarget[--m_internalTargetCount] = {};
  return BeginTarget(record, false);
}

void CLGMPClipboardTransport::ClearStreamTargets()
{
  for (StreamTarget& target : m_streamTarget)
    target.Clear();
  m_streamTargetHead     = 0;
  m_streamTargetCount    = 0;
  m_streamTargetPrepared = 0;
}

void CLGMPClipboardTransport::ClearQueuedStreamTargets()
{
  if (!m_streamTargetPrepared)
  {
    ClearStreamTargets();
    return;
  }

  // The prepared prefix has already crossed the target boundary and must
  // receive the same drain treatment as a BUSY PendingTarget. Only discard
  // records which have not yet been presented to the target.
  for (unsigned i = m_streamTargetPrepared;
      i < m_streamTargetCount; ++i)
    m_streamTarget[
      (m_streamTargetHead + i) % STREAM_TARGET_COUNT].Clear();
  m_streamTargetCount = m_streamTargetPrepared;
}

bool CLGMPClipboardTransport::QueueStreamTarget(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  if (m_streamTargetCount == STREAM_TARGET_COUNT ||
      (record.length && !data))
    return false;

  StreamTarget& target = m_streamTarget[
    (m_streamTargetHead + m_streamTargetCount) % STREAM_TARGET_COUNT];
  target.record = record;
  if (record.length)
    WCCopy::Copy(target.data, data, record.length);
  ++m_streamTargetCount;
  return true;
}

void CLGMPClipboardTransport::PopStreamTargets(unsigned count)
{
  for (unsigned i = 0; i < count; ++i)
    m_streamTarget[
      (m_streamTargetHead + i) % STREAM_TARGET_COUNT].Clear();
  m_streamTargetHead =
    (m_streamTargetHead + count) % STREAM_TARGET_COUNT;
  m_streamTargetCount -= count;
  m_streamTargetPrepared = count < m_streamTargetPrepared ?
    m_streamTargetPrepared - count : 0;
}

CLGMPClipboardTransport::StreamDisposition
CLGMPClipboardTransport::PreviewStreamTarget(
  const KVMFRClipboardMessage& record, Transfer& transfer,
  CLGMPClipboardFiles& files)
{
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
  {
    if (record.transfer == m_discardClientToHelper)
      return StreamDisposition::DISCARD;
    if (record.token ||
        !kvmfrClipboardTransferFromHelper(record.transfer) ||
        !ValidateChunk(transfer, record))
      return StreamDisposition::INVALID;
    AdvanceChunk(transfer, record);
    return StreamDisposition::READY;
  }

  const CLGMPClipboardFiles::Direction direction =
    CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER;
  if (files.IsStaleTerminal(record, direction))
    return StreamDisposition::STALE;
  if (!files.Validate(record, direction, 0, false))
    return StreamDisposition::INVALID;
  files.Apply(record, direction);
  return StreamDisposition::READY;
}

bool CLGMPClipboardTransport::PublishStreamTargets()
{
  ClipboardChannelWrite writes[STREAM_TARGET_COUNT] = {};
  for (unsigned i = 0; i < m_streamTargetPrepared; ++i)
  {
    StreamTarget& target = m_streamTarget[
      (m_streamTargetHead + i) % STREAM_TARGET_COUNT];
    writes[i].record = target.record;
    writes[i].data   = target.record.length ? target.data : nullptr;
  }

  size_t accepted = 0;
  const ClipboardChannelResult result = m_target ?
    m_target->SendClipboardBatch(writes, m_streamTargetPrepared,
      accepted) :
    ClipboardChannelResult::FAILED;
  if (accepted > m_streamTargetPrepared ||
      (result == ClipboardChannelResult::ACCEPTED &&
        accepted != m_streamTargetPrepared))
  {
    DEBUG_ERROR("Helper accepted an invalid clipboard stream prefix: "
      "accepted=%zu offered=%u", accepted, m_streamTargetPrepared);
    ReleaseOwner("invalid Helper clipboard batch result", false);
    m_failed = true;
    return true;
  }

  for (size_t i = 0; i < accepted; ++i)
  {
    const StreamTarget& target = m_streamTarget[
      (m_streamTargetHead + i) % STREAM_TARGET_COUNT];
    ApplyInbound(target.record);
  }
  if (accepted)
  {
    PopStreamTargets(static_cast<unsigned>(accepted));
    if (m_ownerClientID && !m_ownerReleasing)
      RenewLease();
  }

  if (result == ClipboardChannelResult::BUSY)
  {
    if (m_ownerClientID)
      RenewLease();
    return true;
  }

  if (result == ClipboardChannelResult::FAILED)
  {
    ReleaseOwner("Helper delivery failed", false);
    m_failed = true;
  }
  return true;
}

bool CLGMPClipboardTransport::RetryStreamTargets()
{
  if (!m_streamTargetPrepared || m_pendingTarget.valid)
    return true;

  // The retained prefix is already validated and stamped. Retry it exactly;
  // owner-release state may have changed while the target was BUSY.
  return PublishStreamTargets();
}

bool CLGMPClipboardTransport::PumpStreamTarget()
{
  while (!m_pendingTarget.valid && !m_streamTargetPrepared &&
      m_streamTargetCount)
  {
    if (!m_ownerClientID ||
        (m_ownerReleasing && !m_releaseClearHelper))
    {
      PopStreamTargets(1);
      continue;
    }

    // Dependent chunks are validated against the state the preceding chunk
    // would establish, without committing that state before Helper accepts
    // the corresponding prefix.
    Transfer transfer = m_clientToHelper;
    CLGMPClipboardFiles files = m_files;
    StreamDisposition disposition = StreamDisposition::READY;
    for (unsigned i = 0; i < m_streamTargetCount; ++i)
    {
      StreamTarget& target = m_streamTarget[
        (m_streamTargetHead + i) % STREAM_TARGET_COUNT];
      disposition = PreviewStreamTarget(target.record, transfer, files);
      if (disposition != StreamDisposition::READY)
        break;
      target.record.generation = m_endpointGeneration;
      ++m_streamTargetPrepared;
    }

    if (m_streamTargetPrepared)
    {
      if (!m_ownerReleasing)
        RenewLease();
      if (!PublishStreamTargets())
        return false;
      if (m_streamTargetPrepared || m_failed)
        return true;
      continue;
    }

    PopStreamTargets(1);
    if (disposition == StreamDisposition::STALE)
    {
      if (!m_ownerReleasing)
        RenewLease();
      continue;
    }
    if (disposition == StreamDisposition::INVALID)
      ReleaseOwner("invalid clipboard stream data", true);
  }
  return true;
}

bool CLGMPClipboardTransport::DrainStream(bool& received)
{
  received = false;
  if (m_failed || !m_ownerClientID || !m_clientToHostStream)
    return true;

  for (unsigned drained = 0; drained < STREAM_DRAIN_MAX;)
  {
    if (m_streamTargetCount == STREAM_TARGET_COUNT)
    {
      // Submit only after the bounded staging window is full. A retained
      // prefix or pending control keeps the window backpressured here.
      if (!PumpStreamTarget())
        return false;
      if (m_streamTargetCount == STREAM_TARGET_COUNT)
        return true;
    }

    LGMPStreamBuffer buffer = {};
    LGMP_STATUS status =
      lgmpHostStreamReadPeek(m_clientToHostStream, &buffer);
    if (status == LGMP_ERR_STREAM_EMPTY)
      break;
    if (status == LGMP_ERR_STREAM_UNBOUND ||
        status == LGMP_ERR_STREAM_STALE)
    {
      if (m_ownerReleasing && status == LGMP_ERR_STREAM_UNBOUND)
        break;
      if (!PumpStreamTarget())
        return false;
      ReleaseOwner("clipboard stream binding lost", true);
      return true;
    }
    if (status != LGMP_OK)
    {
      if (!PumpStreamTarget())
        return false;
      Fail("lgmpHostStreamReadPeek", status);
      return false;
    }
    received = true;

    KVMFRClipboardMessage record = {};
    if (buffer.data && buffer.size >= sizeof(record))
      memcpy(&record, buffer.data, sizeof(record));
    const bool valid = buffer.data &&
      buffer.size == sizeof(record) + record.length &&
      record.version == KVMFR_CLIPBOARD_VERSION &&
      record.generation == m_ownerGeneration &&
      (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
       record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA) &&
      record.length <= KVMFR_CLIPBOARD_DATA_BYTES &&
      !(record.flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END)) &&
      AddValid(record.offset, record.length);
    const bool queued = valid && QueueStreamTarget(record,
      static_cast<const uint8_t *>(buffer.data) + sizeof(record));

    status = lgmpHostStreamReadRelease(m_clientToHostStream, &buffer);
    if (status != LGMP_OK)
    {
      Fail("lgmpHostStreamReadRelease", status);
      return false;
    }
    if (!queued)
    {
      if (!PumpStreamTarget())
        return false;
      ReleaseOwner("invalid clipboard stream record", true);
      return true;
    }
    ++drained;
  }

  return PumpStreamTarget();
}

void CLGMPClipboardTransport::ReleaseOwner(
  const char * reason, bool clearHelper, bool force)
{
  if (!m_ownerClientID)
    return;

  if (!m_ownerReleasing)
  {
    m_ownerReleasing      = true;
    m_releaseClearHelper  = clearHelper;
    m_ownerReleaseReason  = reason;
    m_ownerDeadline       = 0;
    m_ownerDrainDeadline  = GetTickCount64() + STREAM_DRAIN_TIMEOUT_MS;
    m_replayPending       = false;
    ClearOutboundBlock();
  }
  else if (force)
  {
    m_releaseClearHelper |= clearHelper;
    m_ownerReleaseReason = reason;
  }

  if (force)
  {
    ForceUnbindStreams();
    FinishOwnerRelease();
    return;
  }

  if (TryUnbindStreams() && !m_pendingTarget.valid &&
      !m_streamTargetCount)
    FinishOwnerRelease();
}

void CLGMPClipboardTransport::FinishOwnerRelease()
{
  if (!m_ownerClientID)
    return;

  const uint32_t clientID     = m_ownerClientID;
  const uint32_t generation   = m_ownerGeneration;
  const char * const reason   = m_ownerReleaseReason ?
    m_ownerReleaseReason : "unspecified";
  const Transfer clientToHelper = m_clientToHelper;
  const Transfer helperToClient = m_helperToClient;
  const bool clearHelper      = m_releaseClearHelper;
  m_ownerClientID             = 0;
  m_ownerGeneration           = 0;
  m_ownerReleasing            = false;
  m_releaseClearHelper        = false;
  m_ownerReleaseReason        = nullptr;
  m_ownerDeadline             = 0;
  m_ownerDrainDeadline        = 0;
  m_replayPending             = false;
  ClearQueuedStreamTargets();
  m_discardClientToHelper     = 0;
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  if (helperToClient.Active())
    m_discardHelperToClient = helperToClient.transfer;
  m_statusDirty               = true;

  if (clearHelper)
  {
    QueueFileDisconnect();
    QueueTransferCancel(clientToHelper);
    QueueTransferCancel(helperToClient);
    QueueHelperClear();
  }
  else
  {
    m_files.Reset();
    m_clientClipboardGeneration = 0;
    m_clientFormats = 0;
  }

  DEBUG_INFO("Clipboard owner %u generation %u released (%s)",
    clientID, generation, reason);
}

bool CLGMPClipboardTransport::RetryOwnerRelease()
{
  if (!m_ownerReleasing)
    return true;

  if (GetTickCount64() >= m_ownerDrainDeadline)
  {
    DEBUG_WARN("Timed out draining clipboard streams for owner %u "
      "generation %u; discarding in-flight records",
      m_ownerClientID, m_ownerGeneration);
    DropPendingTarget();
    ClearStreamTargets();
    ForceUnbindStreams();
    FinishOwnerRelease();
    return true;
  }

  if (TryUnbindStreams() && !m_pendingTarget.valid &&
      !m_streamTargetCount)
    FinishOwnerRelease();
  return !m_failed;
}

void CLGMPClipboardTransport::ResetProtocol(bool keepClipboard)
{
  DropPendingTarget();
  if (m_ownerClientID)
    ReleaseOwner("endpoint reset", false);
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  m_files.Reset();
  m_clientClipboardGeneration = 0;
  m_clientFormats = 0;
  m_discardClientToHelper = 0;
  m_discardHelperToClient = 0;
  m_replayPending = false;
  ClearOutboundBlock();
  m_internalTargetCount = 0;
  ClearStreamTargets();
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
  clipboardStatus.version         = KVMFR_CLIPBOARD_VERSION;
  clipboardStatus.generation      = m_endpointGeneration;
  clipboardStatus.lease           = static_cast<uint32_t>(OWNER_LEASE_MS);
  clipboardStatus.slotBytes       = KVMFR_CLIPBOARD_DATA_BYTES;
  clipboardStatus.streamVersion   = KVMFR_CLIPBOARD_STREAM_VERSION;
  clipboardStatus.streamSlotCount = KVMFR_CLIPBOARD_STREAM_SLOT_COUNT;
  clipboardStatus.hostToClient    = m_hostToClientDescriptor;
  clipboardStatus.clientToHost    = m_clientToHostDescriptor;
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

bool CLGMPClipboardTransport::ReplayClipboard()
{
  if (!m_replayPending || !m_ownerClientID || m_ownerReleasing)
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
    ReleaseOwner("subscriber disappeared", true, true);
  return result != PostResult::FAILED;
}

bool CLGMPClipboardTransport::ValidateClaim(
  const KVMFRClipboardMessage& message) const
{
  return message.version == KVMFR_CLIPBOARD_VERSION &&
    message.type == KVMFR_CLIPBOARD_MESSAGE_CLAIM &&
    message.generation && message.token == m_endpointGeneration &&
    m_hostToClientStream && m_clientToHostStream &&
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

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      return ValidateInboundRecord(message);

    default:
      return false;
  }
}

bool CLGMPClipboardTransport::ValidateInboundRecord(
  const KVMFRClipboardMessage& message) const
{
  if (CLGMPClipboardFiles::IsRecord(message))
  {
    if (m_files.IsStaleTerminal(message,
        CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER))
      return true;
    const bool filesOffered = m_cachedValid &&
      m_cachedClipboard.type == KVMFR_CLIPBOARD_MESSAGE_OFFER &&
      (m_helperFormats & KVMFR_CLIPBOARD_FORMAT_MASK_FILES);
    const uint64_t offeredDataset = filesOffered ?
      m_cachedClipboard.clipboardGeneration : 0;
    return m_files.Validate(message,
      CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER,
      offeredDataset, filesOffered);
  }

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

  if (CLGMPClipboardFiles::IsRecord(message))
  {
    const bool filesOffered = m_clientClipboardGeneration &&
      (m_clientFormats & KVMFR_CLIPBOARD_FORMAT_MASK_FILES);
    return m_files.Validate(message,
      CLGMPClipboardFiles::Direction::HELPER_TO_CLIENT,
      m_clientClipboardGeneration, filesOffered);
  }

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
        kvmfrClipboardRepresentationFormatValid(message.format) &&
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
  if (CLGMPClipboardFiles::IsRecord(message))
  {
    m_files.Apply(message,
      CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER);
    return;
  }

  switch (message.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      m_clientClipboardGeneration = message.clipboardGeneration;
      m_clientFormats = message.token;
      if (m_clientToHelper.Active())
        m_discardClientToHelper = m_clientToHelper.transfer;
      m_clientToHelper.Clear();
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      m_clientClipboardGeneration = 0;
      m_clientFormats = 0;
      if (m_clientToHelper.Active())
        m_discardClientToHelper = m_clientToHelper.transfer;
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
      {
        m_discardClientToHelper = message.transfer;
        m_clientToHelper.Clear();
      }
      if (m_helperToClient.transfer == message.transfer)
        m_helperToClient.Clear();
      break;
  }
}

void CLGMPClipboardTransport::ApplyOutbound(
  const KVMFRClipboardMessage& message)
{
  if (CLGMPClipboardFiles::IsRecord(message))
  {
    m_files.Apply(message,
      CLGMPClipboardFiles::Direction::HELPER_TO_CLIENT);
    return;
  }

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
      m_discardClientToHelper = 0;
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
  const KVMFRClipboardMessage& message, bool acknowledge)
{
  if (m_pendingTarget.valid || message.length)
    return false;

  m_pendingTarget.valid       = true;
  m_pendingTarget.acknowledge = acknowledge;
  m_pendingTarget.record      = message;
  return RetryTarget();
}

void CLGMPClipboardTransport::FinishTarget(bool accepted)
{
  const KVMFRClipboardMessage message = m_pendingTarget.record;
  if (accepted)
  {
    ApplyInbound(message);
    if (m_ownerClientID && !m_ownerReleasing)
      RenewLease();
  }
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
  const ClipboardChannelResult result = m_target ?
    m_target->SendClipboard(m_pendingTarget.record, nullptr) :
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
      if (!m_ownerReleasing &&
          IsOwner(clientID, message.generation))
        RenewLease();
      return true;
    }

    if (!BindStreams(clientID))
    {
      DeInitStreams();
      m_failed = true;
      if (m_stopEvent)
        SetEvent(m_stopEvent);
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
  if (m_ownerReleasing)
    return true;

  if (CLGMPClipboardFiles::IsRecord(message) &&
      CLGMPClipboardFiles::DirectionValid(message,
        CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER) &&
      !m_files.TransferActive(message.transfer) &&
      (message.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
       message.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST) &&
      !ValidateInboundRecord(message))
  {
    // A cancellation or replacement can cross a request which was generated
    // from the preceding file offer. The already-posted lifecycle record will
    // cancel the client-side work; consuming this stale request avoids
    // treating normal cross-queue ordering as a protocol failure.
    RenewLease();
    return true;
  }
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

  if (m_files.IsStaleTerminal(message,
      CLGMPClipboardFiles::Direction::CLIENT_TO_HELPER))
  {
    RenewLease();
    return true;
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
  BeginTarget(forwarded, true);
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
    ReleaseOwner("subscriber disappeared", true, true);
    return record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR ?
      ClipboardChannelResult::ACCEPTED : ClipboardChannelResult::BUSY;
  }
  return ClipboardChannelResult::FAILED;
}

ClipboardChannelResult CLGMPClipboardTransport::SendData(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  const bool valid = CLGMPClipboardFiles::IsData(record) ?
    m_files.Validate(record,
      CLGMPClipboardFiles::Direction::HELPER_TO_CLIENT, 0, false) :
    ValidateChunk(m_helperToClient, record);
  if (!valid)
    return ClipboardChannelResult::FAILED;

  KVMFRClipboardMessage message = record;
  message.generation = m_ownerGeneration;
  LGMPStreamBuffer buffer = {};
  LGMP_STATUS status =
    lgmpHostStreamWriteAcquire(m_hostToClientStream, &buffer);
  if (status == LGMP_ERR_STREAM_FULL)
    return ClipboardChannelResult::BUSY;
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    ReleaseOwner("clipboard stream binding lost", true);
    return ClipboardChannelResult::ACCEPTED;
  }
  if (status != LGMP_OK)
  {
    Fail("lgmpHostStreamWriteAcquire", status);
    return ClipboardChannelResult::FAILED;
  }

  const uint32_t bytes = sizeof(message) + message.length;
  if (!buffer.data || buffer.capacity < bytes)
  {
    const LGMP_STATUS cancel =
      lgmpHostStreamWriteCancel(m_hostToClientStream, &buffer);
    if (cancel != LGMP_OK)
      DEBUG_ERROR("Failed to cancel an undersized host-to-client "
        "clipboard stream reservation: %s", lgmpStatusString(cancel));
    DEBUG_ERROR("Host-to-client clipboard stream returned an "
      "undersized slot: capacity=%u required=%u",
      buffer.capacity, bytes);
    m_failed = true;
    if (m_stopEvent)
      SetEvent(m_stopEvent);
    return ClipboardChannelResult::FAILED;
  }

  memcpy(buffer.data, &message, sizeof(message));
  if (message.length)
    memcpy(static_cast<uint8_t *>(buffer.data) + sizeof(message), data,
      message.length);
  status = lgmpHostStreamWriteCommit(
    m_hostToClientStream, &buffer, bytes);
  if (status == LGMP_OK)
  {
    ApplyOutbound(record);
    return ClipboardChannelResult::ACCEPTED;
  }

  DEBUG_ERROR("Failed to commit host-to-client clipboard stream data: %s",
    lgmpStatusString(status));
  if (status == LGMP_ERR_STREAM_UNBOUND ||
      status == LGMP_ERR_STREAM_STALE)
  {
    ReleaseOwner("clipboard stream binding lost", true);
    return ClipboardChannelResult::ACCEPTED;
  }
  Fail("lgmpHostStreamWriteCommit", status);
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
    QueueStaleFileRecord(record);
    ClearOutboundBlock();
    Wake();
    return ClipboardChannelResult::ACCEPTED;
  }

  if (!m_available ||
      record.generation != m_endpointGeneration)
    return ClipboardChannelResult::FAILED;

  if (m_files.IsStaleTerminal(record,
      CLGMPClipboardFiles::Direction::HELPER_TO_CLIENT))
  {
    ClearOutboundBlock();
    return ClipboardChannelResult::ACCEPTED;
  }

  const bool validOutbound = ValidateOutboundRecord(record);
  if (!validOutbound && CLGMPClipboardFiles::IsRecord(record) &&
      CLGMPClipboardFiles::DirectionValid(record,
        CLGMPClipboardFiles::Direction::HELPER_TO_CLIENT) &&
      !m_files.TransferActive(record.transfer) &&
      (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
       record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST))
  {
    // The LGMP owner may disappear or replace its dataset before Helper
    // observes the matching lifecycle record. Fail new work locally without
    // tearing down the shared Helper clipboard channel.
    QueueStaleFileRecord(record);
    ClearOutboundBlock();
    Wake();
    return ClipboardChannelResult::ACCEPTED;
  }

  if (!validOutbound || (record.length && !data))
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
    if (!m_ownerClientID || m_ownerReleasing)
    {
      ClearOutboundBlock();
      Wake();
      return ClipboardChannelResult::ACCEPTED;
    }
  }

  if (!m_ownerClientID || m_ownerReleasing)
  {
    if (OwnerScopedLifecycle(record))
    {
      QueueStaleRequestCancel(record);
      QueueStaleFileRecord(record);
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
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
    result = SendData(record, data);
  else if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
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
    ClearStreamTargets();
    m_internalTargetCount = 0;
    for (KVMFRClipboardMessage& record : m_internalTarget)
      record = {};
    if (m_ownerClientID)
      ReleaseOwner("Helper endpoint changed", false);
    m_clientToHelper.Clear();
    m_helperToClient.Clear();
    m_files.Reset();
    m_clientClipboardGeneration = 0;
    m_clientFormats = 0;
    m_discardClientToHelper = 0;
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
  ClearStreamTargets();
  m_internalTargetCount = 0;
  for (KVMFRClipboardMessage& record : m_internalTarget)
    record = {};
  if (m_ownerClientID)
    ReleaseOwner("Helper reset", false);
  m_clientToHelper.Clear();
  m_helperToClient.Clear();
  m_files.Reset();
  m_clientClipboardGeneration = 0;
  m_clientFormats = 0;
  m_discardClientToHelper = 0;
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
  LGMPStreamPollState streamPoll = {};
  const LGMPStreamPollConfig pollConfig =
  {
    32U,
    50U,
    1000U,
  };
  const LGMP_STATUS pollStatus = lgmpStreamPollInit(&streamPoll,
    pollConfig);
  if (pollStatus != LGMP_OK)
  {
    DEBUG_ERROR("Failed to initialize LGMP clipboard polling: %s",
      lgmpStatusString(pollStatus));
    if (m_target)
      m_target->ClipboardFailed();
    return;
  }

  bool notifyFailed = false;
  for (;;)
  {
    uint32_t waitUs = IDLE_POLL_MS * 1000U;
    bool streamReceived = false;
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
        ClearStreamTargets();
        ReleaseOwner("subscriber disappeared", true, true);
      }
      else if (m_ownerClientID && !m_ownerReleasing &&
          GetTickCount64() >= m_ownerDeadline)
        ReleaseOwner("lease expired", true);

      // A restarted client can reuse dataset and transfer IDs. Deliver every
      // old-owner cleanup record before admitting messages from a new owner.
      if (!RetryTarget() || !RetryStreamTargets() ||
          !PumpInternalTarget() ||
          (!m_streamTargetCount && m_internalTargetCount == 0 &&
            !DrainMessage()) ||
          !DrainStream(streamReceived) ||
          !RetryOwnerRelease() ||
          !PublishStatus() ||
          !ReplayClipboard())
      {
        notifyFailed = true;
        break;
      }

      if (m_outboundBlocked)
      {
        notifyReady = m_target != nullptr;
        readyTarget = m_target;
      }
      if (m_pendingTarget.valid || m_internalTargetCount ||
          m_streamTargetCount || m_ownerClientID)
        waitUs = ACTIVE_POLL_MS * 1000U;
      if (m_ownerClientID)
      {
        if (streamReceived)
          lgmpStreamPollActivity(&streamPoll);
        waitUs = lgmpStreamPollIdle(&streamPoll);
      }
      else
        lgmpStreamPollActivity(&streamPoll);
    }

    // ClipboardReceiveReady may synchronously retry SendClipboard, which
    // acquires m_lock. Never invoke it while holding the transport lock.
    if (notifyReady)
      readyTarget->ClipboardReceiveReady();

    if (!waitUs)
      continue;
    if (!ArmPollTimer(m_pollTimer, waitUs))
    {
      const DWORD error = GetLastError();
      if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0)
        break;
      DEBUG_ERROR_HR(error,
        "Failed to arm LGMP clipboard poll timer");
      notifyFailed = true;
      break;
    }

    const HANDLE handles[] =
    {
      m_stopEvent,
      m_wakeEvent,
      m_pollTimer,
    };
    const DWORD wait = WaitForMultipleObjects(
      _countof(handles), handles, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_OBJECT_0 + 1)
    {
      lgmpStreamPollActivity(&streamPoll);
      continue;
    }
    if (wait == WAIT_OBJECT_0 + 2)
      continue;
    if (wait == WAIT_FAILED)
    {
      DEBUG_ERROR_HR(GetLastError(),
        "LGMP clipboard worker wait failed");
      notifyFailed = true;
      break;
    }
    DEBUG_ERROR("LGMP clipboard worker returned an unexpected wait "
      "result: %lu", wait);
    notifyFailed = true;
    break;
  }

  IClipboardTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    DropPendingTarget();
    ClearStreamTargets();
    if (m_ownerClientID)
      ReleaseOwner("transport stopped", false, true);
    target = m_target;
  }
  if (notifyFailed && target)
    target->ClipboardFailed();
}
