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
#include "transport/IClipboardSource.h"
#include "transport/lgmp/CLGMPClipboardFiles.h"

#include "common/KVMFRClipboard.h"
#include "common/LGMPConfig.h"

#include <Windows.h>

#include <stdint.h>

extern "C" {
  #include "lgmp/host.h"
}

class CLGMPHost;

class CLGMPClipboardTransport final : public IClipboardSource
{
private:
  static constexpr unsigned MEMORY_COUNT = KVMFR_CLIPBOARD_SLOT_COUNT;
  static constexpr unsigned INTERNAL_TARGET_COUNT =
    CLGMPClipboardFiles::MAX_ACQUISITIONS +
    CLGMPClipboardFiles::MAX_REQUESTS + 3;
  static constexpr ULONGLONG OWNER_LEASE_MS = 1000;
  static constexpr DWORD ACTIVE_POLL_MS = 5;
  static constexpr DWORD IDLE_POLL_MS = 50;
  static constexpr uint32_t SLOT_BYTES =
    sizeof(KVMFRClipboardSlotHeader) + KVMFR_CLIPBOARD_DATA_BYTES;

  enum class PostResult
  {
    POSTED,
    BUSY,
    GONE,
    FAILED,
  };

  struct Transfer
  {
    uint64_t transfer            = 0;
    uint64_t clipboardGeneration = 0;
    uint64_t nextOffset          = 0;
    uint64_t sizeHint            = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    KVMFRClipboardFormat format  = KVMFR_CLIPBOARD_FORMAT_NONE;
    uint32_t nextSequence        = 0;
    bool     began               = false;

    void Clear() { *this = {}; }
    bool Active() const { return transfer != 0; }
  };

  struct Grant
  {
    uint32_t generation = 0;
    bool offered        = false;
    bool committed      = false;
  };

  struct PendingTarget
  {
    bool valid       = false;
    bool acknowledge = false;
    int  grant       = -1;
    KVMFRClipboardMessage record = {};
    uint8_t data[KVMFR_CLIPBOARD_DATA_BYTES] = {};

    void Clear()
    {
      valid       = false;
      acknowledge = false;
      grant       = -1;
      record      = {};
    }
  };

  CLGMPHost& m_host;

  PLGMPHostQueue m_queue = nullptr;
  PLGMPMemory m_statusMemory [MEMORY_COUNT] = {};
  PLGMPMemory m_messageMemory[MEMORY_COUNT] = {};
  PLGMPMemory m_grantMemory  [MEMORY_COUNT] = {};
  PLGMPMemory m_dataMemory   [MEMORY_COUNT] = {};

  CSRWLock m_lifecycleLock;
  CSRWLock m_lock;
  IClipboardTarget * m_target = nullptr;
  HANDLE m_stopEvent = nullptr;
  HANDLE m_wakeEvent = nullptr;
  HANDLE m_thread    = nullptr;

  bool m_available        = false;
  bool m_statusDirty      = true;
  bool m_cachedValid      = false;
  bool m_replayPending    = false;
  bool m_outboundBlocked  = false;
  bool m_failed           = false;
  uint32_t m_endpointGeneration = 0;
  uint32_t m_ownerClientID       = 0;
  uint32_t m_ownerGeneration     = 0;
  uint32_t m_statusSerial        = 0;
  uint32_t m_messageSerial       = 0;
  uint32_t m_dataSerial          = 0;
  uint32_t m_helperFormats       = 0;
  uint32_t m_clientFormats       = 0;
  uint32_t m_blockedOwnerClientID   = 0;
  uint32_t m_blockedOwnerGeneration = 0;
  uint64_t m_ownerDeadline       = 0;
  uint64_t m_clientClipboardGeneration = 0;
  uint64_t m_discardHelperToClient = 0;
  KVMFRClipboardMessage m_cachedClipboard = {};
  KVMFRClipboardMessage m_blockedOutbound = {};
  Transfer m_clientToHelper;
  Transfer m_helperToClient;
  Grant m_grants[MEMORY_COUNT];
  PendingTarget m_pendingTarget;
  KVMFRClipboardMessage m_internalTarget[INTERNAL_TARGET_COUNT] = {};
  unsigned m_internalTargetCount = 0;
  CLGMPClipboardFiles m_files;

  bool Initialize();
  void DeInit();

  static DWORD CALLBACK ThreadProc(void * context);
  void Thread();
  void Wake();

  bool IsOwner(uint32_t clientID, uint32_t generation) const;
  bool OwnerSubscribed() const;
  void RenewLease();
  void BlockOutbound(const KVMFRClipboardMessage& record,
    uint32_t ownerClientID, uint32_t ownerGeneration);
  void ClearOutboundBlock();
  bool BlockedOwnerLost(const KVMFRClipboardMessage& record) const;
  void ReleaseOwner(const char * reason, bool clearHelper);
  void ResetProtocol(bool keepClipboard);
  void QueueHelperClear();
  void QueueTransferCancel(const Transfer& transfer);
  void QueueStaleRequestCancel(
    const KVMFRClipboardMessage& request);
  void QueueStaleFileRecord(const KVMFRClipboardMessage& record);
  void QueueFileDisconnect();
  bool QueueInternalTarget(const KVMFRClipboardMessage& record);
  bool PumpInternalTarget();

  PLGMPMemory FindAvailable(PLGMPMemory (&memory)[MEMORY_COUNT]) const;
  PostResult PostForOwner(uint64_t udata, PLGMPMemory memory);
  bool PublishStatus();
  bool PostGrants();
  bool ReplayClipboard();
  bool DrainMessage();
  bool ProcessMessage(uint32_t clientID,
    const KVMFRClipboardMessage& message);
  bool RetryTarget();
  void FinishTarget(bool accepted);
  void DropPendingTarget();
  void Fail(const char * operation, LGMP_STATUS status);

  bool ValidateClaim(const KVMFRClipboardMessage& message) const;
  bool ValidateOwnedControl(const KVMFRClipboardMessage& message) const;
  bool ValidateInboundRecord(const KVMFRClipboardMessage& message) const;
  bool ValidateOutboundRecord(const KVMFRClipboardMessage& message) const;
  bool ValidateChunk(const Transfer& transfer,
    const KVMFRClipboardMessage& message) const;
  void AdvanceChunk(Transfer& transfer,
    const KVMFRClipboardMessage& message);
  void ApplyInbound(const KVMFRClipboardMessage& message);
  void ApplyOutbound(const KVMFRClipboardMessage& message);
  bool BeginTarget(const KVMFRClipboardMessage& message,
    const uint8_t * data, int grant);

  ClipboardChannelResult SendControl(
    const KVMFRClipboardMessage& record);
  ClipboardChannelResult SendData(
    const KVMFRClipboardMessage& record, const uint8_t * data);

  friend class CLGMPTransport;

public:
  explicit CLGMPClipboardTransport(CLGMPHost& host) :
    m_host(host) {}
  ~CLGMPClipboardTransport() override;

  CLGMPClipboardTransport(const CLGMPClipboardTransport&) = delete;
  CLGMPClipboardTransport& operator=(
    const CLGMPClipboardTransport&) = delete;

  bool Start(IClipboardTarget& target) override;
  void Stop() override;
  void ClipboardState(bool available, uint32_t generation) override;
  ClipboardChannelResult SendClipboard(
    const KVMFRClipboardMessage& record,
    const uint8_t * data) override;
  void ClipboardReset(uint32_t generation, uint32_t reason) override;
};
