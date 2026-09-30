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

#include "transport/network/CNetworkClipboard.h"

#include "CSRWLock.h"

#include <LGProtocol/NetworkClipboard.h>
#include <LGProtocol/NetworkServices.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <deque>
#include <new>
#include <utility>
#include <vector>

namespace
{
  constexpr size_t   MAX_SEND_COUNT        = 128;
  constexpr size_t   MAX_SEND_BYTES        = 8U * 1024U * 1024U;
  constexpr size_t   MAX_TARGET_COUNT      = 128;
  constexpr size_t   MAX_TARGET_BYTES      = 8U * 1024U * 1024U;
  constexpr size_t   PROCESS_MAX           = 64;
  constexpr size_t   FORMAT_COUNT          = LG_NET_CLIPBOARD_KVMFR_FORMAT_COUNT;
  constexpr size_t   MAX_ACQUISITIONS      = KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS;
  constexpr size_t   MAX_REQUESTS          = KVMFR_CLIPBOARD_FILE_MAX_REQUESTS;
  constexpr uint32_t MAX_CHUNK_LENGTH      = KVMFR_CLIPBOARD_STREAM_SLOT_BYTES;
  constexpr uint32_t MAX_FILE_READ_LENGTH  = KVMFR_CLIPBOARD_FILE_READ_BYTES;
  constexpr uint32_t MIN_CHUNK_LENGTH      =
    (MAX_CHUNK_LENGTH + MAX_SEND_COUNT - 3U) / (MAX_SEND_COUNT - 2U);

  constexpr LGNetClipboardClaimFlags CLAIM_FLAGS =
    LG_NET_CLIPBOARD_CLAIM_READ | LG_NET_CLIPBOARD_CLAIM_WRITE;

  bool SamePeer(
    const NetworkClipboardPeer& left, const NetworkClipboardPeer& right)
  {
    return left.client == right.client && left.generation == right.generation;
  }

  uint64_t Next64(uint64_t value)
  {
    ++value;
    return value ? value : 1;
  }

  bool DeadlineReached(uint64_t now, uint64_t deadline)
  {
    return static_cast<int64_t>(now - deadline) >= 0;
  }

  bool DeliveryFlags(const LGNetEnvelope& envelope,
    LGNetEnvelopeFlags expected, bool bulk)
  {
    constexpr LGNetEnvelopeFlags MASK = LG_NET_ENVELOPE_RESPONSE |
      LG_NET_ENVELOPE_FINAL | LG_NET_ENVELOPE_RELIABLE |
      LG_NET_ENVELOPE_DATAGRAM | LG_NET_ENVELOPE_ACK_REQUIRED |
      LG_NET_ENVELOPE_ERROR | LG_NET_ENVELOPE_EARLY_DATA;
    const LGNetEnvelopeFlags delivery =
      bulk ? 0 : LG_NET_ENVELOPE_RELIABLE;
    return (envelope.flags & MASK) ==
      (expected | LG_NET_ENVELOPE_FINAL | delivery);
  }

  bool ValidOffer(const KVMFRClipboardMessage& record)
  {
    return record.version == KVMFR_CLIPBOARD_VERSION &&
      record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER &&
      record.clipboardGeneration && record.token &&
      !(record.token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL) && !record.sequence &&
      !record.transfer && !record.offset && !record.size && !record.format &&
      !record.flags && !record.length;
  }

  bool ValidClear(const KVMFRClipboardMessage& record)
  {
    return record.version == KVMFR_CLIPBOARD_VERSION &&
      record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR &&
      record.clipboardGeneration && !record.token && !record.sequence &&
      !record.transfer && !record.offset && !record.size && !record.format &&
      !record.flags && !record.length;
  }

  bool ValidRequest(const KVMFRClipboardMessage& record, bool helper)
  {
    const bool transfer = helper ?
      kvmfrClipboardTransferFromHelper(record.transfer) :
      kvmfrClipboardTransferFromClient(record.transfer);
    return record.version == KVMFR_CLIPBOARD_VERSION &&
      record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
      record.clipboardGeneration && transfer &&
      kvmfrClipboardRepresentationFormatValid(record.format) &&
      !record.sequence && !record.offset && !record.size && !record.flags &&
      !record.token && !record.length;
  }

  bool ValidCancel(const KVMFRClipboardMessage& record)
  {
    return record.version == KVMFR_CLIPBOARD_VERSION &&
      record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL &&
      record.clipboardGeneration && record.transfer &&
      (!record.format ||
        kvmfrClipboardRepresentationFormatValid(record.format)) &&
      !record.sequence && !record.offset && !record.size && !record.flags &&
      !record.length;
  }

  bool ValidData(const KVMFRClipboardMessage& record)
  {
    return record.version == KVMFR_CLIPBOARD_VERSION &&
      record.type == KVMFR_CLIPBOARD_MESSAGE_DATA &&
      record.clipboardGeneration && record.transfer &&
      kvmfrClipboardRepresentationFormatValid(record.format) &&
      record.length <= KVMFR_CLIPBOARD_DATA_BYTES &&
      (record.length || record.flags) &&
      !(record.flags &
        ~(KVMFR_CLIPBOARD_FLAG_BEGIN | KVMFR_CLIPBOARD_FLAG_END)) &&
      !record.token && record.offset <= UINT64_MAX - record.length;
  }
}

struct CNetworkClipboard::Impl
{
  enum class Direction
  {
    REMOTE_REQUESTER,
    HELPER_REQUESTER,
  };

  struct PeerState
  {
    NetworkClipboardPeer     peer           = {};
    NetworkClipboardPeerInfo info           = {};
    uint64_t                 lastClaimEpoch = 0;
  };

  struct RemoteOffer
  {
    uint64_t                                       offerID     = 0;
    uint64_t                                       serial      = 0;
    uint32_t                                       formatCount = 0;
    uint32_t                                       received    = 0;
    uint32_t                                       formatMask  = 0;
    bool                                           fileOffer   = false;
    bool                                           complete    = false;
    bool                                           published   = false;
    std::array<KVMFRClipboardFormat, FORMAT_COUNT> formats     = {};

    void Clear()
    {
      *this = {};
    }
  };

  struct ClipboardTransfer
  {
    NetworkClipboardPeer peer        = {};
    Direction            direction   = Direction::REMOTE_REQUESTER;
    uint64_t             claimEpoch  = 0;
    uint64_t             wireID      = 0;
    uint64_t             localID     = 0;
    uint64_t             offerID     = 0;
    uint64_t             offset      = 0;
    uint64_t             sizeHint    = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    uint32_t             sequence    = 0;
    uint32_t             formatIndex = 0;
    uint32_t             maxChunk    = MAX_CHUNK_LENGTH;
    KVMFRClipboardFormat format      = KVMFR_CLIPBOARD_FORMAT_NONE;
    bool                 bulk        = false;
    bool                 began       = false;
  };

  struct FileAcquisition
  {
    NetworkClipboardPeer peer       = {};
    Direction            direction  = Direction::REMOTE_REQUESTER;
    uint64_t             claimEpoch = 0;
    uint64_t             offerID    = 0;
    uint64_t             wireID     = 0;
    uint64_t             localID    = 0;
    uint64_t             leaseEpoch = 0;
    bool                 active     = false;
  };

  struct FileTransfer
  {
    NetworkClipboardPeer        peer           = {};
    Direction                   direction      = Direction::REMOTE_REQUESTER;
    uint64_t                    claimEpoch     = 0;
    uint64_t                    wireID         = 0;
    uint64_t                    localID        = 0;
    uint64_t                    offerID        = 0;
    uint64_t                    entryID        = 0;
    uint64_t                    requestOffset  = 0;
    uint64_t                    responseOffset = 0;
    uint64_t                    sizeHint       = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    uint32_t                    sequence       = 0;
    uint32_t                    length         = 0;
    uint32_t                    maxChunk       = MAX_CHUNK_LENGTH;
    KVMFRClipboardFileOperation operation      = 0;
    bool                        bulk           = false;
    bool                        began          = false;
  };

  struct SendItem
  {
    uint64_t             serial         = 0;
    NetworkClipboardPeer peer           = {};
    bool                 bulk           = false;
    LGNetService         service        = 0;
    uint16_t             messageType    = 0;
    LGNetEnvelopeFlags   flags          = LG_NET_ENVELOPE_FINAL;
    uint64_t             componentEpoch = 0;
    uint64_t             requestID      = 0;
    std::vector<uint8_t> payload        = {};
  };

  struct TargetItem
  {
    uint64_t              serial     = 0;
    KVMFRClipboardMessage record     = {};
    std::vector<uint8_t>  data       = {};
    bool                  wasBlocked = false;
  };

  CSRWLock m_lifecycleLock;
  CSRWLock m_processLock;
  CSRWLock m_senderCallbackLock;
  CSRWLock m_targetCallbackLock;
  CSRWLock m_lock;

  INetworkClipboardSender * m_sender                = nullptr;
  IClipboardTarget        * m_target                = nullptr;
  bool                      m_started               = false;
  bool                      m_stateKnown            = false;
  bool                      m_available             = false;
  bool                      m_failed                = false;
  bool                      m_failureNotified       = false;
  bool                      m_targetCallbackActive  = false;
  bool                      m_sourceBlocked         = false;
  bool                      m_receiveReadyPending   = false;
  LGNetService              m_sourceBlockedService  = 0;
  uint64_t                  m_sourceBlockedTransfer = 0;
  uint32_t                  m_generation            = 0;
  uint64_t                  m_nextSerial            = 0;
  uint64_t                  m_nextLocal             = 0;
  uint64_t                  m_nextWire              = 0;
  uint64_t                  m_nextEpoch             = 0;

  std::vector<PeerState>         m_peers              = {};
  NetworkClipboardPeer           m_owner              = {};
  uint64_t                       m_ownerID            = 0;
  uint64_t                       m_claimEpoch         = 0;
  uint64_t                       m_claimSerial        = 0;
  uint64_t                       m_remoteClaimSerial  = 0;
  uint64_t                       m_remoteOfferSerial  = 0;
  uint64_t                       m_claimDeadline      = 0;
  uint32_t                       m_claimLeaseMs       = 0;
  RemoteOffer                    m_remoteOffer        = {};
  bool                           m_cachedValid        = false;
  KVMFRClipboardMessage          m_cachedRecord       = {};
  std::vector<ClipboardTransfer> m_clipboardTransfers = {};
  std::vector<FileAcquisition>   m_fileAcquisitions   = {};
  std::vector<FileTransfer>      m_fileTransfers      = {};
  std::deque<SendItem>           m_sends              = {};
  size_t                         m_sendBytes          = 0;
  std::deque<TargetItem>         m_targets            = {};
  size_t                         m_targetBytes        = 0;

  PeerState * FindPeerNL(const NetworkClipboardPeer& peer)
  {
    for (PeerState& state : m_peers)
      if (SamePeer(state.peer, peer))
        return &state;
    return nullptr;
  }

  const PeerState * FindPeerNL(const NetworkClipboardPeer& peer) const
  {
    for (const PeerState& state : m_peers)
      if (SamePeer(state.peer, peer))
        return &state;
    return nullptr;
  }

  bool IsOwnerNL(const NetworkClipboardPeer& peer, uint64_t epoch) const
  {
    return m_claimEpoch && SamePeer(m_owner, peer) && m_claimEpoch == epoch;
  }

  uint64_t NextLocalNL(bool helper)
  {
    m_nextLocal = Next64(m_nextLocal) & ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
    if (!m_nextLocal)
      m_nextLocal = 1;
    return helper ? m_nextLocal | KVMFR_CLIPBOARD_TRANSFER_HELPER : m_nextLocal;
  }

  uint64_t NextWireNL()
  {
    m_nextWire = Next64(m_nextWire);
    return m_nextWire;
  }

  uint64_t NextEpochNL()
  {
    m_nextEpoch = Next64(m_nextEpoch);
    return m_nextEpoch;
  }

  ClipboardTransfer * FindClipboardLocalNL(uint64_t localID)
  {
    for (ClipboardTransfer& transfer : m_clipboardTransfers)
      if (transfer.localID == localID)
        return &transfer;
    return nullptr;
  }

  ClipboardTransfer * FindClipboardWireNL(uint64_t wireID)
  {
    for (ClipboardTransfer& transfer : m_clipboardTransfers)
      if (transfer.wireID == wireID)
        return &transfer;
    return nullptr;
  }

  FileAcquisition * FindAcquisitionLocalNL(uint64_t localID)
  {
    for (FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.localID == localID)
        return &acquisition;
    return nullptr;
  }

  FileAcquisition * FindAcquisitionWireNL(uint64_t wireID)
  {
    for (FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.wireID == wireID)
        return &acquisition;
    return nullptr;
  }

  FileTransfer * FindFileLocalNL(uint64_t localID)
  {
    for (FileTransfer& transfer : m_fileTransfers)
      if (transfer.localID == localID)
        return &transfer;
    return nullptr;
  }

  FileTransfer * FindFileWireNL(uint64_t wireID)
  {
    for (FileTransfer& transfer : m_fileTransfers)
      if (transfer.wireID == wireID)
        return &transfer;
    return nullptr;
  }

  bool QueueSendBatchNL(std::vector<SendItem>& batch);
  bool QueueTargetNL(TargetItem&& item);
  bool SelectBulkNL(
    const PeerState& peer, LGNetService service, uint32_t payloadLength) const;
  bool     PayloadFitsNL(const PeerState& peer, LGNetService service, bool bulk,
    uint32_t payloadLength) const;
  uint32_t ChunkLengthNL(
    const PeerState& peer, LGNetService service, uint32_t headerLength,
    bool bulk) const;
  bool BuildSendForDeliveryNL(SendItem& item,
    const NetworkClipboardPeer& peer, LGNetService service,
    uint16_t messageType, LGNetEnvelopeFlags flags, uint64_t componentEpoch,
    uint64_t requestID, uint32_t payloadLength, bool bulk);
  bool BuildSendNL(SendItem& item, const NetworkClipboardPeer& peer,
    LGNetService service, uint16_t messageType, LGNetEnvelopeFlags flags,
    uint64_t componentEpoch, uint64_t requestID, uint32_t payloadLength,
    bool preferBulk);

  void PumpSends();
  void PumpTargets();
  void NotifyTargetReady();
  void NotifyFailure();
  void DropOwnerNL(bool clearHelper);
  void ResetTransfersNL();
  void ClearRemoteTransfersNL(uint64_t offerID);
  bool PublishRemoteOfferNL();
  bool ReplayCachedNL(std::vector<SendItem>& batch);

  bool QueueClaimStatusNL(const NetworkClipboardPeer& peer, uint64_t epoch,
    LGNetClipboardClaimCode code, LGNetClipboardClaimFlags flags,
    uint32_t leaseMs, uint32_t detail);
  bool QueueControlNL(
    uint16_t messageType, uint64_t offerID, std::vector<SendItem>& batch);
  bool QueueClipboardStatusNL(const NetworkClipboardPeer& peer, uint64_t epoch,
    uint64_t requestID, uint64_t offerID, LGNetClipboardStatusCode status,
    uint32_t detail, uint64_t processedLength, std::vector<SendItem>& batch);
  bool QueueFileStatusNL(const NetworkClipboardPeer& peer, uint64_t epoch,
    uint64_t requestID, uint64_t offerID, uint64_t entryID,
    KVMFRClipboardFileError error, uint64_t transferredLength,
    std::vector<SendItem>& batch);
  bool BuildCachedOfferNL(
    std::vector<SendItem>& batch, const KVMFRClipboardMessage& record);

  NetworkClipboardPacketResult HandleClaimNL(
    PeerState& peer, const LGNetPacketView& packet, uint64_t nowMs);
  NetworkClipboardPacketResult HandleClipboardNL(
    PeerState& peer, const LGNetPacketView& packet, bool bulk, uint64_t nowMs);
  NetworkClipboardPacketResult HandleFileNL(
    PeerState& peer, const LGNetPacketView& packet, bool bulk);

  ClipboardChannelResult HandleHelperClipboardNL(
    const KVMFRClipboardMessage& record, const uint8_t * data);
  ClipboardChannelResult HandleHelperFileNL(
    const KVMFRClipboardMessage& record, const uint8_t * data);
};

bool CNetworkClipboard::Impl::PayloadFitsNL(const PeerState& peer,
  LGNetService service, bool bulk, uint32_t payloadLength) const
{
  LGNetCapabilityFlags flags;
  uint32_t             maximum;
  if (service == LG_NET_SERVICE_CLIPBOARD)
  {
    flags   = peer.info.clipboardFlags;
    maximum = bulk ? peer.info.clipboardBulkPayload :
                     peer.info.clipboardReliablePayload;
  }
  else if (service == LG_NET_SERVICE_FILE)
  {
    flags   = peer.info.fileFlags;
    maximum = bulk ? peer.info.fileBulkPayload : peer.info.fileReliablePayload;
  }
  else
    return false;

  const LGNetCapabilityFlags required =
    static_cast<LGNetCapabilityFlags>(
      bulk ? LG_NET_CAPABILITY_BULK : LG_NET_CAPABILITY_RELIABLE);
  return (flags & required) && maximum >= payloadLength;
}

bool CNetworkClipboard::Impl::SelectBulkNL(
  const PeerState& peer, LGNetService service, uint32_t payloadLength) const
{
  return PayloadFitsNL(peer, service, true, payloadLength);
}

uint32_t CNetworkClipboard::Impl::ChunkLengthNL(
  const PeerState& peer, LGNetService service, uint32_t headerLength,
  bool bulk) const
{
  LGNetCapabilityFlags flags;
  uint32_t             maximum;
  if (service == LG_NET_SERVICE_CLIPBOARD)
  {
    flags   = peer.info.clipboardFlags;
    maximum = bulk ? peer.info.clipboardBulkPayload :
      peer.info.clipboardReliablePayload;
  }
  else if (service == LG_NET_SERVICE_FILE)
  {
    flags   = peer.info.fileFlags;
    maximum = bulk ? peer.info.fileBulkPayload : peer.info.fileReliablePayload;
  }
  else
    return 0;

  const LGNetCapabilityFlags required =
    static_cast<LGNetCapabilityFlags>(
      bulk ? LG_NET_CAPABILITY_BULK : LG_NET_CAPABILITY_RELIABLE);
  if (!(flags & required) || maximum <= headerLength)
    return 0;
  return (std::min)(maximum - headerLength, MAX_CHUNK_LENGTH);
}

bool CNetworkClipboard::Impl::BuildSendForDeliveryNL(SendItem& item,
  const NetworkClipboardPeer& peer, LGNetService service, uint16_t messageType,
  LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
  uint32_t payloadLength, bool bulk)
{
  const PeerState * state = FindPeerNL(peer);
  if (!state)
    return false;

  if (!PayloadFitsNL(*state, service, bulk, payloadLength))
    return false;

  try
  {
    item.payload.resize(payloadLength);
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }

  item.serial         = Next64(m_nextSerial);
  m_nextSerial        = item.serial;
  item.peer           = peer;
  item.bulk           = bulk;
  item.service        = service;
  item.messageType    = messageType;
  item.flags          = flags | LG_NET_ENVELOPE_FINAL;
  item.componentEpoch = componentEpoch;
  item.requestID      = requestID;
  return true;
}

bool CNetworkClipboard::Impl::BuildSendNL(SendItem& item,
  const NetworkClipboardPeer& peer, LGNetService service, uint16_t messageType,
  LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
  uint32_t payloadLength, bool preferBulk)
{
  const PeerState * state = FindPeerNL(peer);
  if (!state)
    return false;

  const bool bulk = preferBulk &&
    SelectBulkNL(*state, service, payloadLength);
  return BuildSendForDeliveryNL(item, peer, service, messageType, flags,
    componentEpoch, requestID, payloadLength, bulk);
}

bool CNetworkClipboard::Impl::QueueSendBatchNL(std::vector<SendItem>& batch)
{
  if (batch.empty())
    return true;
  if (m_failed)
    return false;

  size_t bytes = 0;
  for (const SendItem& item : batch)
  {
    if (item.payload.size() > MAX_SEND_BYTES - bytes)
      return false;
    bytes += item.payload.size();
  }

  if (batch.size() > MAX_SEND_COUNT - m_sends.size() ||
    bytes > MAX_SEND_BYTES - m_sendBytes)
    return false;

  size_t inserted = 0;
  try
  {
    for (SendItem& item : batch)
    {
      m_sends.emplace_back(std::move(item));
      ++inserted;
    }
  }
  catch (const std::bad_alloc&)
  {
    while (inserted)
    {
      --inserted;
      m_sends.pop_back();
    }
    return false;
  }

  m_sendBytes += bytes;
  batch.clear();
  return true;
}

bool CNetworkClipboard::Impl::QueueTargetNL(TargetItem&& item)
{
  const size_t bytes = sizeof(item.record) + item.data.size();
  if (m_failed || !m_started || !m_stateKnown || !m_available ||
    !m_generation || m_targets.size() >= MAX_TARGET_COUNT ||
    bytes > MAX_TARGET_BYTES - m_targetBytes)
    return false;

  item.record.generation = m_generation;
  item.serial            = Next64(m_nextSerial);
  m_nextSerial           = item.serial;
  try
  {
    m_targets.emplace_back(std::move(item));
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  m_targetBytes += bytes;
  return true;
}

void CNetworkClipboard::Impl::PumpSends()
{
  struct BlockedRoute
  {
    NetworkClipboardPeer peer    = {};
    LGNetService         service = 0;
    bool                 bulk    = false;
  };

  std::array<BlockedRoute, PROCESS_MAX> blockedRoutes = {};
  size_t                                blockedCount  = 0;
  for (size_t count = 0; count < PROCESS_MAX; ++count)
  {
    SendItem action;
    {
      CSRWSharedLock lock(m_lock);
      if (m_failed || !m_sender || m_sends.empty())
        return;
      const auto available = [&blockedRoutes, blockedCount](
          const SendItem& item)
      {
        for (size_t index = 0; index < blockedCount; ++index)
          if (SamePeer(blockedRoutes[index].peer, item.peer) &&
            blockedRoutes[index].service == item.service &&
            blockedRoutes[index].bulk == item.bulk)
            return false;
        return true;
      };
      const auto candidate = std::find_if(
        m_sends.begin(), m_sends.end(), available);
      if (candidate == m_sends.end())
        return;
      try
      {
        action = *candidate;
      }
      catch (const std::bad_alloc&)
      {
        return;
      }
    }

    NetworkClipboardSendResult result;
    {
      CSRWExclusiveLock         callbackLock(m_senderCallbackLock);
      INetworkClipboardSender * sender;
      {
        CSRWSharedLock lock(m_lock);
        sender = m_sender;
      }
      result = sender ?
        sender->SendClipboard(action.peer, action.bulk, action.service,
          action.messageType, action.flags, action.componentEpoch,
          action.requestID,
          action.payload.empty() ? nullptr : action.payload.data(),
          static_cast<uint32_t>(action.payload.size())) :
        NetworkClipboardSendResult::FAILED;
    }

    {
      CSRWExclusiveLock lock(m_lock);
      const auto pending = std::find_if(m_sends.begin(), m_sends.end(),
        [&action](const SendItem& item)
        { return item.serial == action.serial; });
      if (pending == m_sends.end())
        continue;
      if (result == NetworkClipboardSendResult::BLOCKED)
      {
        if (blockedCount < blockedRoutes.size())
        {
          blockedRoutes[blockedCount].peer    = action.peer;
          blockedRoutes[blockedCount].service = action.service;
          blockedRoutes[blockedCount].bulk    = action.bulk;
          ++blockedCount;
          continue;
        }
        return;
      }
      if (result == NetworkClipboardSendResult::FAILED)
      {
        m_failed = true;
        return;
      }

      m_sendBytes -= pending->payload.size();
      m_sends.erase(pending);
      if (m_sourceBlocked)
      {
        m_sourceBlocked         = false;
        m_sourceBlockedService  = 0;
        m_sourceBlockedTransfer = 0;
        m_receiveReadyPending   = true;
      }
    }
  }
}

void CNetworkClipboard::Impl::PumpTargets()
{
  for (size_t count = 0; count < PROCESS_MAX; ++count)
  {
    TargetItem action;
    {
      CSRWSharedLock lock(m_lock);
      if (m_failed || !m_target || m_targets.empty())
        return;
      try
      {
        action = m_targets.front();
      }
      catch (const std::bad_alloc&)
      {
        return;
      }
    }

    ClipboardChannelResult result;
    {
      CSRWExclusiveLock  callbackLock(m_targetCallbackLock);
      IClipboardTarget * target;
      {
        CSRWExclusiveLock lock(m_lock);
        target                 = m_target;
        m_targetCallbackActive = target != nullptr;
      }
      result = target ? target->SendClipboard(action.record,
                          action.data.empty() ? nullptr : action.data.data()) :
                        ClipboardChannelResult::FAILED;
      {
        CSRWExclusiveLock lock(m_lock);
        m_targetCallbackActive = false;
      }
    }

    {
      CSRWExclusiveLock lock(m_lock);
      if (m_targets.empty() || m_targets.front().serial != action.serial)
        continue;
      if (result == ClipboardChannelResult::BUSY)
      {
        m_targets.front().wasBlocked = true;
        return;
      }
      if (result == ClipboardChannelResult::FAILED)
      {
        m_failed = true;
        return;
      }

      const TargetItem& item  = m_targets.front();
      m_targetBytes          -= sizeof(item.record) + item.data.size();
      m_targets.pop_front();
    }
  }
}

void CNetworkClipboard::Impl::NotifyTargetReady()
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_targetCallbackActive)
      return;
  }

  CSRWExclusiveLock  callbackLock(m_targetCallbackLock);
  IClipboardTarget * target = nullptr;
  {
    CSRWExclusiveLock lock(m_lock);
    if (m_targetCallbackActive || !m_receiveReadyPending)
      return;
    m_receiveReadyPending  = false;
    target                 = m_started ? m_target : nullptr;
    m_targetCallbackActive = target != nullptr;
  }
  if (target)
    target->ClipboardReceiveReady();
  {
    CSRWExclusiveLock lock(m_lock);
    m_targetCallbackActive = false;
  }
}

void CNetworkClipboard::Impl::NotifyFailure()
{
  {
    CSRWSharedLock lock(m_lock);
    if (m_targetCallbackActive)
      return;
  }

  bool notify = false;
  {
    CSRWExclusiveLock lock(m_lock);
    if (!m_targetCallbackActive && m_failed && !m_failureNotified && m_target)
    {
      m_failureNotified      = true;
      m_targetCallbackActive = true;
      notify                 = true;
    }
  }
  if (!notify)
    return;

  CSRWExclusiveLock  callbackLock(m_targetCallbackLock);
  IClipboardTarget * target;
  {
    CSRWSharedLock lock(m_lock);
    target = m_target;
  }
  if (target)
    target->ClipboardFailed();
  {
    CSRWExclusiveLock lock(m_lock);
    m_targetCallbackActive = false;
  }
}

bool CNetworkClipboard::Impl::QueueClaimStatusNL(
  const NetworkClipboardPeer& peer, uint64_t epoch,
  LGNetClipboardClaimCode code, LGNetClipboardClaimFlags flags,
  uint32_t leaseMs, uint32_t detail)
{
  LGNetClipboardClaimStatus status = {};
  status.claimEpoch                = epoch;
  status.serial                    = Next64(m_claimSerial);
  status.status                    = code;
  status.flags                     = flags;
  status.leaseRemainingMs          = leaseMs;
  status.detail                    = detail;

  SendItem send;
  if (!BuildSendNL(send, peer, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT, LG_NET_ENVELOPE_RESPONSE, epoch,
        epoch, LG_NET_CLIPBOARD_CLAIM_STATUS_WIRE_SIZE, false) ||
    !lgNetClipboardClaimStatusEncode(
      send.payload.data(), send.payload.size(), &status))
    return false;

  std::vector<SendItem> batch;
  try
  {
    batch.emplace_back(std::move(send));
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  if (!QueueSendBatchNL(batch))
    return false;
  m_claimSerial = status.serial;
  return true;
}

bool CNetworkClipboard::Impl::QueueControlNL(
  uint16_t messageType, uint64_t offerID, std::vector<SendItem>& batch)
{
  if (!m_claimEpoch)
    return false;

  LGNetClipboardControl control = {};
  control.ownerID               = m_ownerID;
  control.claimEpoch            = m_claimEpoch;
  control.serial                = Next64(m_claimSerial);
  control.offerID               = offerID;

  SendItem send;
  if (!BuildSendNL(send, m_owner, LG_NET_SERVICE_CLIPBOARD, messageType, 0,
        m_claimEpoch, offerID, LG_NET_CLIPBOARD_CONTROL_WIRE_SIZE, false) ||
    !lgNetClipboardControlEncode(
      send.payload.data(), send.payload.size(), &control))
    return false;
  try
  {
    batch.emplace_back(std::move(send));
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  m_claimSerial = control.serial;
  return true;
}

bool CNetworkClipboard::Impl::QueueClipboardStatusNL(
  const NetworkClipboardPeer& peer, uint64_t epoch, uint64_t requestID,
  uint64_t offerID, LGNetClipboardStatusCode statusCode, uint32_t detail,
  uint64_t processedLength, std::vector<SendItem>& batch)
{
  LGNetClipboardStatus status = {};
  status.requestID            = requestID;
  status.offerID              = offerID;
  status.status               = statusCode;
  status.detail               = detail;
  status.processedLength      = processedLength;

  SendItem send;
  if (!BuildSendNL(send, peer, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_MESSAGE_STATUS, LG_NET_ENVELOPE_RESPONSE, epoch,
        requestID, LG_NET_CLIPBOARD_STATUS_WIRE_SIZE, false) ||
    !lgNetClipboardStatusEncode(
      send.payload.data(), send.payload.size(), &status))
    return false;
  try
  {
    batch.emplace_back(std::move(send));
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  return true;
}

bool CNetworkClipboard::Impl::QueueFileStatusNL(
  const NetworkClipboardPeer& peer, uint64_t epoch, uint64_t requestID,
  uint64_t offerID, uint64_t entryID, KVMFRClipboardFileError error,
  uint64_t transferredLength, std::vector<SendItem>& batch)
{
  LGNetFileStatus status   = {};
  status.requestID         = requestID;
  status.offerID           = offerID;
  status.entryID           = entryID;
  status.status            = lgNetFileStatusFromKVMFRError(error);
  status.detail            = error;
  status.transferredLength = transferredLength;

  SendItem send;
  if (!BuildSendNL(send, peer, LG_NET_SERVICE_FILE, LG_NET_FILE_MESSAGE_STATUS,
        LG_NET_ENVELOPE_RESPONSE, epoch, requestID,
        LG_NET_FILE_STATUS_WIRE_SIZE, false) ||
    !lgNetFileStatusEncode(send.payload.data(), send.payload.size(), &status))
    return false;
  try
  {
    batch.emplace_back(std::move(send));
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  return true;
}

bool CNetworkClipboard::Impl::BuildCachedOfferNL(
  std::vector<SendItem>& batch, const KVMFRClipboardMessage& record)
{
  if (!m_claimEpoch || !ValidOffer(record))
    return false;

  uint32_t count = 0;
  for (uint32_t mask   = record.token & KVMFR_CLIPBOARD_FORMAT_MASK_ALL; mask;
    mask             >>= 1)
    count += mask & 1U;
  if (!count || count > FORMAT_COUNT)
    return false;

  if (record.token & KVMFR_CLIPBOARD_FORMAT_MASK_FILES)
  {
    LGNetFileOffer offer = {};
    offer.offerID        = record.clipboardGeneration;
    offer.totalBytes     = 0;
    offer.entryCount     = 1;
    offer.flags          = LG_NET_FILE_OFFER_COPY | LG_NET_FILE_OFFER_RECURSIVE;
    const size_t payloadLength = lgNetFileOfferSize(&offer);
    if (!payloadLength || payloadLength > UINT32_MAX)
      return false;

    SendItem send;
    if (!BuildSendNL(send, m_owner, LG_NET_SERVICE_FILE,
          LG_NET_FILE_MESSAGE_OFFER, 0, m_claimEpoch, offer.offerID,
          static_cast<uint32_t>(payloadLength), false) ||
      !lgNetFileOfferEncode(send.payload.data(), send.payload.size(), &offer))
      return false;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return false;
    }
  }

  uint32_t index = 0;
  for (KVMFRClipboardFormat format = KVMFR_CLIPBOARD_FORMAT_TEXT;
    format <= KVMFR_CLIPBOARD_FORMAT_FILES; ++format)
  {
    if (!(record.token & kvmfrClipboardFormatFlag(format)))
      continue;

    const uint8_t * mime;
    uint16_t        mimeLength;
    if (!lgNetClipboardKVMFRFormatMime(format, &mime, &mimeLength))
      return false;

    LGNetClipboardOffer offer  = {};
    offer.offerID              = record.clipboardGeneration;
    offer.serial               = record.clipboardGeneration;
    offer.estimatedSize        = 0;
    offer.formatIndex          = index++;
    offer.formatCount          = count;
    offer.mimeLength           = mimeLength;
    offer.flags                = LG_NET_CLIPBOARD_OFFER_LAZY;
    offer.mime                 = mime;
    const size_t payloadLength = lgNetClipboardOfferSize(&offer);
    if (!payloadLength || payloadLength > UINT32_MAX)
      return false;

    SendItem send;
    if (!BuildSendNL(send, m_owner, LG_NET_SERVICE_CLIPBOARD,
          LG_NET_CLIPBOARD_MESSAGE_OFFER, 0, m_claimEpoch, offer.offerID,
          static_cast<uint32_t>(payloadLength), false) ||
      !lgNetClipboardOfferEncode(
        send.payload.data(), send.payload.size(), &offer))
      return false;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return false;
    }
  }
  return true;
}

bool CNetworkClipboard::Impl::ReplayCachedNL(std::vector<SendItem>& batch)
{
  if (!m_cachedValid || !m_claimEpoch)
    return true;
  if (m_cachedRecord.type == KVMFR_CLIPBOARD_MESSAGE_OFFER)
    return BuildCachedOfferNL(batch, m_cachedRecord);
  if (m_cachedRecord.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR)
    return QueueControlNL(LG_NET_CLIPBOARD_MESSAGE_CLEAR,
      m_cachedRecord.clipboardGeneration, batch);
  return false;
}

void CNetworkClipboard::Impl::ResetTransfersNL()
{
  m_clipboardTransfers.clear();
  m_fileAcquisitions.clear();
  m_fileTransfers.clear();
}

void CNetworkClipboard::Impl::ClearRemoteTransfersNL(uint64_t offerID)
{
  m_clipboardTransfers.erase(
    std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
      [offerID](const ClipboardTransfer& transfer)
      {
        return transfer.direction == Direction::HELPER_REQUESTER &&
          transfer.offerID == offerID;
      }),
    m_clipboardTransfers.end());
  m_fileAcquisitions.erase(
    std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
      [offerID](const FileAcquisition& acquisition)
      {
        return acquisition.direction == Direction::HELPER_REQUESTER &&
          acquisition.offerID == offerID;
      }),
    m_fileAcquisitions.end());
  m_fileTransfers.erase(
    std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
      [offerID](const FileTransfer& transfer)
      {
        return transfer.direction == Direction::HELPER_REQUESTER &&
          transfer.offerID == offerID;
      }),
    m_fileTransfers.end());
}

bool CNetworkClipboard::Impl::PublishRemoteOfferNL()
{
  if (!m_remoteOffer.complete || m_remoteOffer.published || !m_started ||
    !m_stateKnown || !m_available || !m_generation)
    return true;

  TargetItem item;
  item.record.version             = KVMFR_CLIPBOARD_VERSION;
  item.record.type                = KVMFR_CLIPBOARD_MESSAGE_OFFER;
  item.record.clipboardGeneration = m_remoteOffer.offerID;
  item.record.token               = m_remoteOffer.formatMask;
  if (!QueueTargetNL(std::move(item)))
    return false;
  m_remoteOffer.published = true;
  return true;
}

void CNetworkClipboard::Impl::DropOwnerNL(bool clearHelper)
{
  if (!m_claimEpoch)
    return;

  if (clearHelper && m_remoteOffer.published && m_remoteOffer.offerID)
  {
    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
    item.record.clipboardGeneration = m_remoteOffer.offerID;
    QueueTargetNL(std::move(item));
  }

  if (clearHelper)
  {
    for (const ClipboardTransfer& transfer : m_clipboardTransfers)
    {
      TargetItem item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
      item.record.clipboardGeneration = transfer.offerID;
      item.record.transfer            = transfer.localID;
      item.record.format              = transfer.format;
      QueueTargetNL(std::move(item));
    }

    for (const FileTransfer& transfer : m_fileTransfers)
    {
      TargetItem item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
      item.record.clipboardGeneration = transfer.offerID;
      item.record.transfer            = transfer.localID;
      item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
      item.record.token               = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
      QueueTargetNL(std::move(item));
    }

    for (const FileAcquisition& acquisition : m_fileAcquisitions)
    {
      TargetItem item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.clipboardGeneration = acquisition.offerID;
      item.record.transfer            = acquisition.localID;
      item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
      if (acquisition.direction == Direction::REMOTE_REQUESTER &&
        acquisition.active)
      {
        item.record.type = KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE;
      }
      else if (acquisition.direction == Direction::HELPER_REQUESTER &&
        !acquisition.active)
      {
        item.record.type  = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED;
        item.record.token = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
      }
      else
      {
        item.record.type  = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
        item.record.token = KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
      }
      QueueTargetNL(std::move(item));
    }
  }

  m_sends.erase(std::remove_if(m_sends.begin(), m_sends.end(),
                  [this](const SendItem& item)
                  {
                    if (!SamePeer(item.peer, m_owner))
                      return false;
                    m_sendBytes -= item.payload.size();
                    return true;
                  }),
    m_sends.end());
  ResetTransfersNL();
  m_remoteOffer.Clear();
  m_owner             = {};
  m_ownerID           = 0;
  m_claimEpoch        = 0;
  m_claimSerial       = 0;
  m_remoteClaimSerial = 0;
  m_remoteOfferSerial = 0;
  m_claimDeadline     = 0;
  m_claimLeaseMs      = 0;
}

NetworkClipboardPacketResult CNetworkClipboard::Impl::HandleClaimNL(
  PeerState& peer, const LGNetPacketView& packet, uint64_t nowMs)
{
  if (!DeliveryFlags(
        packet.envelope, LG_NET_ENVELOPE_ACK_REQUIRED, false) ||
    packet.envelope.requestID != packet.envelope.componentEpoch)
    return NetworkClipboardPacketResult::INVALID;

  LGNetClipboardClaim claim = {};
  if (lgNetClipboardClaimDecode(&claim, packet.payload, packet.payloadLength) !=
      LG_NET_PARSE_OK ||
    claim.claimEpoch != packet.envelope.componentEpoch)
    return NetworkClipboardPacketResult::INVALID;
  if (claim.flags != CLAIM_FLAGS)
  {
    peer.lastClaimEpoch = claim.claimEpoch;
    return QueueClaimStatusNL(peer.peer, claim.claimEpoch,
             LG_NET_CLIPBOARD_CLAIM_REJECTED, 0, 0, 3) ?
      NetworkClipboardPacketResult::HANDLED :
      NetworkClipboardPacketResult::INVALID;
  }

  const bool exactOwner =
    IsOwnerNL(peer.peer, claim.claimEpoch) && claim.ownerID == m_ownerID;
  if (exactOwner)
  {
    m_claimDeadline = nowMs + claim.leaseMs;
    m_claimLeaseMs  = claim.leaseMs;
    return QueueClaimStatusNL(peer.peer, claim.claimEpoch,
             LG_NET_CLIPBOARD_CLAIM_ACCEPTED, CLAIM_FLAGS, claim.leaseMs, 0) ?
      NetworkClipboardPacketResult::HANDLED :
      NetworkClipboardPacketResult::INVALID;
  }

  if (claim.claimEpoch <= peer.lastClaimEpoch)
  {
    return QueueClaimStatusNL(peer.peer, claim.claimEpoch,
             LG_NET_CLIPBOARD_CLAIM_REJECTED, 0, 0, 1) ?
      NetworkClipboardPacketResult::HANDLED :
      NetworkClipboardPacketResult::INVALID;
  }

  if (m_claimEpoch && !SamePeer(m_owner, peer.peer))
  {
    peer.lastClaimEpoch = claim.claimEpoch;
    return QueueClaimStatusNL(peer.peer, claim.claimEpoch,
             LG_NET_CLIPBOARD_CLAIM_REJECTED, 0, 0, 2) ?
      NetworkClipboardPacketResult::HANDLED :
      NetworkClipboardPacketResult::INVALID;
  }

  if (m_claimEpoch)
    DropOwnerNL(true);

  m_owner             = peer.peer;
  m_ownerID           = claim.ownerID;
  m_claimEpoch        = claim.claimEpoch;
  m_claimSerial       = 0;
  m_remoteClaimSerial = 0;
  m_remoteOfferSerial = 0;
  m_claimDeadline     = nowMs + claim.leaseMs;
  m_claimLeaseMs      = claim.leaseMs;
  peer.lastClaimEpoch = claim.claimEpoch;

  if (!QueueClaimStatusNL(peer.peer, claim.claimEpoch,
        LG_NET_CLIPBOARD_CLAIM_ACCEPTED, CLAIM_FLAGS, claim.leaseMs, 0))
  {
    DropOwnerNL(false);
    return NetworkClipboardPacketResult::INVALID;
  }

  std::vector<SendItem> replay;
  if (!ReplayCachedNL(replay) || !QueueSendBatchNL(replay))
  {
    DropOwnerNL(false);
    return NetworkClipboardPacketResult::INVALID;
  }
  return NetworkClipboardPacketResult::HANDLED;
}

NetworkClipboardPacketResult CNetworkClipboard::Impl::HandleClipboardNL(
  PeerState& peer, const LGNetPacketView& packet, bool bulk, uint64_t nowMs)
{
  const LGNetEnvelope& envelope = packet.envelope;
  if (envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_CLAIM)
  {
    if (bulk)
      return NetworkClipboardPacketResult::INVALID;
    return HandleClaimNL(peer, packet, nowMs);
  }

  const bool transferData =
    envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN ||
    envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK ||
    envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_DATA_END;
  if (!IsOwnerNL(peer.peer, envelope.componentEpoch))
    return NetworkClipboardPacketResult::IGNORED;
  if ((bulk && !transferData) ||
    !DeliveryFlags(envelope,
      envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_REQUEST ?
        LG_NET_ENVELOPE_ACK_REQUIRED :
        envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_STATUS ?
        LG_NET_ENVELOPE_RESPONSE :
        0,
      bulk))
    return NetworkClipboardPacketResult::INVALID;

  m_claimDeadline = nowMs + m_claimLeaseMs;

  switch (envelope.messageType)
  {
  case LG_NET_CLIPBOARD_MESSAGE_KEEPALIVE:
  case LG_NET_CLIPBOARD_MESSAGE_RELEASE:
  case LG_NET_CLIPBOARD_MESSAGE_CLEAR:
  {
    LGNetClipboardControl control = {};
    if (lgNetClipboardControlDecode(
          &control, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      control.ownerID != m_ownerID || control.claimEpoch != m_claimEpoch ||
      envelope.requestID != control.offerID)
      return NetworkClipboardPacketResult::INVALID;
    if (control.serial <= m_remoteClaimSerial)
      return NetworkClipboardPacketResult::IGNORED;
    m_remoteClaimSerial = control.serial;

    if (envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_KEEPALIVE)
    {
      if (control.offerID)
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }
    if (envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_RELEASE)
    {
      if (control.offerID)
        return NetworkClipboardPacketResult::INVALID;
      DropOwnerNL(true);
      return NetworkClipboardPacketResult::HANDLED;
    }

    if (m_remoteOffer.offerID &&
      (!control.offerID || control.offerID == m_remoteOffer.offerID))
    {
      if (m_remoteOffer.published)
      {
        TargetItem item;
        item.record.version             = KVMFR_CLIPBOARD_VERSION;
        item.record.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
        item.record.clipboardGeneration = m_remoteOffer.offerID;
        if (!QueueTargetNL(std::move(item)))
          return NetworkClipboardPacketResult::INVALID;
      }
      ClearRemoteTransfersNL(m_remoteOffer.offerID);
      m_remoteOffer.Clear();
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_OFFER:
  {
    LGNetClipboardOffer offer = {};
    if (lgNetClipboardOfferDecode(
          &offer, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      envelope.requestID != offer.offerID || offer.formatCount > FORMAT_COUNT ||
      offer.estimatedSize || offer.flags != LG_NET_CLIPBOARD_OFFER_LAZY)
      return NetworkClipboardPacketResult::INVALID;

    const KVMFRClipboardFormat format =
      lgNetClipboardKVMFRFormatFromMime(offer.mime, offer.mimeLength);
    if (!format)
      return NetworkClipboardPacketResult::INVALID;

    const bool current = m_remoteOffer.offerID == offer.offerID &&
      m_remoteOffer.serial == offer.serial;
    if (!current && offer.serial <= m_remoteOfferSerial)
      return NetworkClipboardPacketResult::IGNORED;
    if (!current)
    {
      const bool fileOffer =
        m_remoteOffer.fileOffer && m_remoteOffer.offerID == offer.offerID;
      if (m_remoteOffer.published)
      {
        TargetItem clear;
        clear.record.version             = KVMFR_CLIPBOARD_VERSION;
        clear.record.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
        clear.record.clipboardGeneration = m_remoteOffer.offerID;
        if (!QueueTargetNL(std::move(clear)))
          return NetworkClipboardPacketResult::INVALID;
      }
      ClearRemoteTransfersNL(m_remoteOffer.offerID);
      m_remoteOffer.Clear();
      m_remoteOffer.offerID     = offer.offerID;
      m_remoteOffer.serial      = offer.serial;
      m_remoteOffer.formatCount = offer.formatCount;
      m_remoteOffer.fileOffer   = fileOffer;
      m_remoteOfferSerial       = offer.serial;
    }

    if (m_remoteOffer.formatCount != offer.formatCount ||
      offer.formatIndex >= FORMAT_COUNT)
      return NetworkClipboardPacketResult::INVALID;
    const uint32_t indexBit  = UINT32_C(1) << offer.formatIndex;
    const uint32_t formatBit = kvmfrClipboardFormatFlag(format);
    if ((m_remoteOffer.received & indexBit) ||
      (m_remoteOffer.formatMask & formatBit))
      return NetworkClipboardPacketResult::INVALID;

    m_remoteOffer.formats[offer.formatIndex]  = format;
    m_remoteOffer.received                   |= indexBit;
    m_remoteOffer.formatMask                 |= formatBit;
    const uint32_t complete   = (UINT32_C(1) << offer.formatCount) - 1U;
    const bool     needsFiles =
      (m_remoteOffer.formatMask & KVMFR_CLIPBOARD_FORMAT_MASK_FILES) != 0;
    m_remoteOffer.complete = m_remoteOffer.received == complete &&
      (!needsFiles || m_remoteOffer.fileOffer);
    if (!PublishRemoteOfferNL())
    {
      return NetworkClipboardPacketResult::INVALID;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_REQUEST:
  {
    LGNetClipboardRequest request = {};
    if (lgNetClipboardRequestDecode(
          &request, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      request.requestID != envelope.requestID || request.offset ||
      FindClipboardWireNL(request.requestID))
      return NetworkClipboardPacketResult::INVALID;

    if (!m_started || !m_stateKnown || !m_available || !m_generation)
    {
      std::vector<SendItem> batch;
      if (!QueueClipboardStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, LG_NET_CLIPBOARD_STATUS_BUSY, 0, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    if (m_clipboardTransfers.size() >= MAX_REQUESTS)
    {
      std::vector<SendItem> batch;
      if (!QueueClipboardStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, LG_NET_CLIPBOARD_STATUS_BUSY, 0, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    if (!m_cachedValid ||
      m_cachedRecord.type != KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      request.offerID != m_cachedRecord.clipboardGeneration)
    {
      std::vector<SendItem> batch;
      if (!QueueClipboardStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, LG_NET_CLIPBOARD_STATUS_NOT_FOUND, 0, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    uint32_t             index  = 0;
    KVMFRClipboardFormat format = KVMFR_CLIPBOARD_FORMAT_NONE;
    for (KVMFRClipboardFormat candidate = KVMFR_CLIPBOARD_FORMAT_TEXT;
      candidate <= KVMFR_CLIPBOARD_FORMAT_FILES; ++candidate)
    {
      if (!(m_cachedRecord.token & kvmfrClipboardFormatFlag(candidate)))
        continue;
      if (index++ == request.formatIndex)
      {
        format = candidate;
        break;
      }
    }
    if (!kvmfrClipboardRepresentationFormatValid(format))
    {
      std::vector<SendItem> batch;
      if (!QueueClipboardStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, LG_NET_CLIPBOARD_STATUS_UNSUPPORTED, 0, 0,
            batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    const bool     transferBulk =
      SelectBulkNL(peer, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE) &&
      ChunkLengthNL(peer, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, true) >= MIN_CHUNK_LENGTH;
    const uint32_t routeChunk   = ChunkLengthNL(
      peer, LG_NET_SERVICE_CLIPBOARD,
      LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, transferBulk);
    const uint32_t maxChunk     =
      (std::min)((std::min)(request.maxChunkLength, MAX_CHUNK_LENGTH),
        routeChunk);
    if (maxChunk < MIN_CHUNK_LENGTH)
    {
      std::vector<SendItem> batch;
      if (!QueueClipboardStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, LG_NET_CLIPBOARD_STATUS_UNSUPPORTED, 0, 0,
            batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    ClipboardTransfer transfer;
    transfer.peer        = peer.peer;
    transfer.direction   = Direction::REMOTE_REQUESTER;
    transfer.claimEpoch  = m_claimEpoch;
    transfer.wireID      = request.requestID;
    transfer.localID     = NextLocalNL(false);
    transfer.offerID     = request.offerID;
    transfer.formatIndex = request.formatIndex;
    transfer.maxChunk    = maxChunk;
    transfer.format      = format;
    transfer.bulk        = transferBulk;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_REQUEST;
    item.record.clipboardGeneration = transfer.offerID;
    item.record.transfer            = transfer.localID;
    item.record.format              = transfer.format;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    try
    {
      m_clipboardTransfers.emplace_back(transfer);
    }
    catch (const std::bad_alloc&)
    {
      m_targets.pop_back();
      m_targetBytes -= sizeof(item.record);
      return NetworkClipboardPacketResult::INVALID;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN:
  {
    LGNetClipboardTransfer wire = {};
    if (lgNetClipboardTransferDecode(
          &wire, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID || wire.processedLength)
      return NetworkClipboardPacketResult::INVALID;
    ClipboardTransfer * transfer = FindClipboardWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      transfer->offerID != wire.offerID || transfer->began)
      return NetworkClipboardPacketResult::INVALID;
    transfer->began    = true;
    transfer->bulk     = bulk;
    transfer->sizeHint = wire.totalLength;
    transfer->offset   = 0;
    transfer->sequence = 0;
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK:
  {
    LGNetClipboardChunk chunkValue = {};
    if (lgNetClipboardChunkDecode(&chunkValue, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
      chunkValue.requestID != envelope.requestID ||
      chunkValue.dataLength > MAX_CHUNK_LENGTH)
      return NetworkClipboardPacketResult::INVALID;
    ClipboardTransfer * transfer = FindClipboardWireNL(chunkValue.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      !transfer->began || transfer->bulk != bulk ||
      transfer->offerID != chunkValue.offerID ||
      transfer->offset != chunkValue.offset ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
        transfer->sizeHint != chunkValue.totalLength))
      return NetworkClipboardPacketResult::INVALID;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_DATA;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = transfer->localID;
    item.record.offset              = transfer->offset;
    item.record.size                =
      transfer->offset ? KVMFR_CLIPBOARD_SIZE_UNKNOWN : transfer->sizeHint;
    item.record.format              = transfer->format;
    item.record.sequence            = transfer->sequence;
    item.record.flags               = transfer->offset ? 0 :
      KVMFR_CLIPBOARD_FLAG_BEGIN;
    item.record.length              = chunkValue.dataLength;
    try
    {
      item.data.assign(
        chunkValue.data, chunkValue.data + chunkValue.dataLength);
    }
    catch (const std::bad_alloc&)
    {
      return NetworkClipboardPacketResult::INVALID;
    }
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;

    transfer = FindClipboardWireNL(chunkValue.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::HANDLED;
    transfer->offset += chunkValue.dataLength;
    ++transfer->sequence;
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_DATA_END:
  {
    LGNetClipboardTransfer wire = {};
    if (lgNetClipboardTransferDecode(
          &wire, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID ||
      wire.processedLength != wire.totalLength)
      return NetworkClipboardPacketResult::INVALID;
    ClipboardTransfer * transfer = FindClipboardWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      !transfer->began || transfer->bulk != bulk ||
      transfer->offerID != wire.offerID ||
      transfer->offset != wire.processedLength ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
        transfer->sizeHint != wire.totalLength))
      return NetworkClipboardPacketResult::INVALID;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_DATA;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = transfer->localID;
    item.record.offset              = transfer->offset;
    item.record.size                = transfer->offset;
    item.record.format              = transfer->format;
    item.record.sequence            = transfer->sequence;
    item.record.flags               = KVMFR_CLIPBOARD_FLAG_END;
    if (!transfer->offset)
      item.record.flags |= KVMFR_CLIPBOARD_FLAG_BEGIN;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    m_clipboardTransfers.erase(
      std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
        [&wire](const ClipboardTransfer& value)
        { return value.wireID == wire.requestID; }),
      m_clipboardTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_DATA_READY:
  {
    LGNetClipboardTransfer wire = {};
    if (lgNetClipboardTransferDecode(
          &wire, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID || wire.processedLength)
      return NetworkClipboardPacketResult::INVALID;
    ClipboardTransfer * transfer = FindClipboardWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::REMOTE_REQUESTER ||
      transfer->offerID != wire.offerID)
      return NetworkClipboardPacketResult::INVALID;
    if (m_sourceBlocked && m_sourceBlockedService == LG_NET_SERVICE_CLIPBOARD &&
      m_sourceBlockedTransfer == transfer->localID)
    {
      m_sourceBlocked         = false;
      m_sourceBlockedService  = 0;
      m_sourceBlockedTransfer = 0;
      m_receiveReadyPending   = true;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_CANCEL:
  case LG_NET_CLIPBOARD_MESSAGE_STATUS:
  {
    uint64_t requestID;
    uint64_t offerID;
    if (envelope.messageType == LG_NET_CLIPBOARD_MESSAGE_CANCEL)
    {
      LGNetClipboardTransfer wire = {};
      if (lgNetClipboardTransferDecode(
            &wire, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
        wire.requestID != envelope.requestID)
        return NetworkClipboardPacketResult::INVALID;
      requestID = wire.requestID;
      offerID   = wire.offerID;
    }
    else
    {
      LGNetClipboardStatus status = {};
      if (lgNetClipboardStatusDecode(
            &status, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
        status.requestID != envelope.requestID)
        return NetworkClipboardPacketResult::INVALID;
      if (status.status == LG_NET_CLIPBOARD_STATUS_OK ||
        status.status == LG_NET_CLIPBOARD_STATUS_BUSY)
        return NetworkClipboardPacketResult::HANDLED;
      requestID = status.requestID;
      offerID   = status.offerID;
    }

    ClipboardTransfer * transfer = FindClipboardWireNL(requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->offerID != offerID)
      return NetworkClipboardPacketResult::INVALID;
    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = transfer->localID;
    item.record.format              = transfer->format;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    m_clipboardTransfers.erase(
      std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
        [requestID](const ClipboardTransfer& value)
        { return value.wireID == requestID; }),
      m_clipboardTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT:
  default:
    return NetworkClipboardPacketResult::INVALID;
  }
}

NetworkClipboardPacketResult CNetworkClipboard::Impl::HandleFileNL(
  PeerState& peer, const LGNetPacketView& packet, bool bulk)
{
  const LGNetEnvelope& envelope = packet.envelope;
  const bool           transferData =
    envelope.messageType == LG_NET_FILE_MESSAGE_DATA_BEGIN ||
    envelope.messageType == LG_NET_FILE_MESSAGE_DATA_CHUNK ||
    envelope.messageType == LG_NET_FILE_MESSAGE_DATA_END;
  LGNetEnvelopeFlags   expected = 0;
  if (envelope.messageType == LG_NET_FILE_MESSAGE_ACQUIRE ||
    envelope.messageType == LG_NET_FILE_MESSAGE_REQUEST)
    expected = LG_NET_ENVELOPE_ACK_REQUIRED;
  else if (envelope.messageType == LG_NET_FILE_MESSAGE_ACQUIRED ||
    envelope.messageType == LG_NET_FILE_MESSAGE_STATUS)
    expected = LG_NET_ENVELOPE_RESPONSE;
  if (!IsOwnerNL(peer.peer, envelope.componentEpoch))
    return NetworkClipboardPacketResult::IGNORED;
  if ((bulk && !transferData) || !DeliveryFlags(envelope, expected, bulk))
    return NetworkClipboardPacketResult::INVALID;

  switch (envelope.messageType)
  {
  case LG_NET_FILE_MESSAGE_OFFER:
  {
    LGNetFileOffer offer = {};
    if (lgNetFileOfferDecode(&offer, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      envelope.requestID != offer.offerID || offer.totalBytes ||
      offer.entryCount != 1 || offer.labelLength ||
      offer.flags != (LG_NET_FILE_OFFER_COPY | LG_NET_FILE_OFFER_RECURSIVE))
      return NetworkClipboardPacketResult::INVALID;

    if (m_remoteOffer.offerID && m_remoteOffer.offerID != offer.offerID)
    {
      if (m_remoteOffer.published)
      {
        TargetItem clear;
        clear.record.version             = KVMFR_CLIPBOARD_VERSION;
        clear.record.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
        clear.record.clipboardGeneration = m_remoteOffer.offerID;
        if (!QueueTargetNL(std::move(clear)))
          return NetworkClipboardPacketResult::INVALID;
      }
      ClearRemoteTransfersNL(m_remoteOffer.offerID);
      m_remoteOffer.Clear();
    }
    m_remoteOffer.offerID   = offer.offerID;
    m_remoteOffer.fileOffer = true;

    const uint32_t complete = m_remoteOffer.formatCount ?
      (UINT32_C(1) << m_remoteOffer.formatCount) - 1U :
      0;
    m_remoteOffer.complete  = complete && m_remoteOffer.received == complete &&
      (m_remoteOffer.formatMask & KVMFR_CLIPBOARD_FORMAT_MASK_FILES);
    if (!PublishRemoteOfferNL())
    {
      return NetworkClipboardPacketResult::INVALID;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_ACQUIRE:
  {
    LGNetFileLease lease = {};
    if (lgNetFileLeaseDecode(&lease, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      lease.clientID != envelope.requestID || lease.leaseEpoch ||
      lease.flags != LG_NET_FILE_LEASE_READ ||
      FindAcquisitionWireNL(lease.clientID))
      return NetworkClipboardPacketResult::INVALID;

    bool duplicate = false;
    for (const FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.direction == Direction::REMOTE_REQUESTER &&
        acquisition.offerID == lease.offerID)
      {
        duplicate = true;
        break;
      }
    if (!m_started || !m_stateKnown || !m_available || !m_generation ||
      !m_cachedValid || m_cachedRecord.type != KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      !(m_cachedRecord.token & KVMFR_CLIPBOARD_FORMAT_MASK_FILES) ||
      lease.offerID != m_cachedRecord.clipboardGeneration || duplicate ||
      m_fileAcquisitions.size() >= MAX_ACQUISITIONS)
    {
      std::vector<SendItem> batch;
      if (!QueueFileStatusNL(peer.peer, m_claimEpoch, lease.clientID,
            lease.offerID, 0, KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    FileAcquisition acquisition;
    acquisition.peer       = peer.peer;
    acquisition.direction  = Direction::REMOTE_REQUESTER;
    acquisition.claimEpoch = m_claimEpoch;
    acquisition.offerID    = lease.offerID;
    acquisition.wireID     = lease.clientID;
    acquisition.localID    = NextLocalNL(false);

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE;
    item.record.clipboardGeneration = acquisition.offerID;
    item.record.transfer            = acquisition.localID;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    try
    {
      m_fileAcquisitions.emplace_back(acquisition);
    }
    catch (const std::bad_alloc&)
    {
      m_targetBytes -=
        sizeof(m_targets.back().record) + m_targets.back().data.size();
      m_targets.pop_back();
      return NetworkClipboardPacketResult::INVALID;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_ACQUIRED:
  {
    LGNetFileLease lease = {};
    if (lgNetFileLeaseDecode(&lease, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      lease.clientID != envelope.requestID || !lease.leaseEpoch ||
      lease.flags !=
        (LG_NET_FILE_LEASE_READ | LG_NET_FILE_LEASE_ACQUIRED))
      return NetworkClipboardPacketResult::INVALID;
    FileAcquisition * acquisition = FindAcquisitionWireNL(lease.clientID);
    if (!acquisition)
      return NetworkClipboardPacketResult::IGNORED;
    if (acquisition->direction != Direction::HELPER_REQUESTER ||
      acquisition->offerID != lease.offerID || acquisition->active)
      return NetworkClipboardPacketResult::INVALID;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED;
    item.record.clipboardGeneration = acquisition->offerID;
    item.record.transfer            = acquisition->localID;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.token               = KVMFR_CLIPBOARD_FILE_ERROR_NONE;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    acquisition = FindAcquisitionWireNL(lease.clientID);
    if (acquisition)
    {
      acquisition->leaseEpoch = lease.leaseEpoch;
      acquisition->active     = true;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_RELEASE:
  {
    LGNetFileLease lease = {};
    if (lgNetFileLeaseDecode(&lease, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      lease.clientID != envelope.requestID || !lease.leaseEpoch ||
      lease.leaseMs || lease.flags != LG_NET_FILE_LEASE_READ)
      return NetworkClipboardPacketResult::INVALID;
    FileAcquisition * acquisition = FindAcquisitionWireNL(lease.clientID);
    if (!acquisition)
      return NetworkClipboardPacketResult::IGNORED;
    if (acquisition->direction != Direction::REMOTE_REQUESTER ||
      acquisition->offerID != lease.offerID ||
      acquisition->leaseEpoch != lease.leaseEpoch || !acquisition->active)
      return NetworkClipboardPacketResult::INVALID;

    const uint64_t localID = acquisition->localID;
    TargetItem     item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE;
    item.record.clipboardGeneration = acquisition->offerID;
    item.record.transfer            = localID;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    const uint64_t offerID = acquisition->offerID;
    m_fileAcquisitions.erase(
      std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
        [localID](const FileAcquisition& value)
        { return value.localID == localID; }),
      m_fileAcquisitions.end());
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [offerID](const FileTransfer& value)
        {
          return value.direction == Direction::REMOTE_REQUESTER &&
            value.offerID == offerID;
        }),
      m_fileTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_REQUEST:
  {
    LGNetFileRequest request = {};
    if (lgNetFileRequestDecode(
          &request, packet.payload, packet.payloadLength) != LG_NET_PARSE_OK ||
      request.requestID != envelope.requestID ||
      FindFileWireNL(request.requestID) ||
      m_fileTransfers.size() >= MAX_REQUESTS)
      return NetworkClipboardPacketResult::INVALID;

    const KVMFRClipboardFileOperation operation =
      lgNetFileKVMFROperationFromRequestFlags(request.flags);
    uint64_t                          node;
    if (!operation || !lgNetFileKVMFRNodeFromEntryID(request.entryID, &node) ||
      (operation == KVMFR_CLIPBOARD_FILE_OP_READ &&
        (node == KVMFR_CLIPBOARD_FILE_ROOT_NODE ||
          request.length > MAX_FILE_READ_LENGTH)))
      return NetworkClipboardPacketResult::INVALID;

    if (!m_started || !m_stateKnown || !m_available || !m_generation)
    {
      std::vector<SendItem> batch;
      if (!QueueFileStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, request.entryID,
            KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    bool acquired = false;
    for (const FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.direction == Direction::REMOTE_REQUESTER &&
        acquisition.offerID == request.offerID && acquisition.active)
      {
        acquired = true;
        break;
      }
    if (!acquired)
    {
      std::vector<SendItem> batch;
      if (!QueueFileStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, request.entryID, KVMFR_CLIPBOARD_FILE_ERROR_ACCESS,
            0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    const bool     transferBulk =
      SelectBulkNL(peer, LG_NET_SERVICE_FILE,
        LG_NET_FILE_TRANSFER_WIRE_SIZE) &&
      ChunkLengthNL(peer, LG_NET_SERVICE_FILE,
        LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE, true) >= MIN_CHUNK_LENGTH;
    const uint32_t maxChunk     = ChunkLengthNL(
      peer, LG_NET_SERVICE_FILE, LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE,
      transferBulk);
    if (maxChunk < MIN_CHUNK_LENGTH)
    {
      std::vector<SendItem> batch;
      if (!QueueFileStatusNL(peer.peer, m_claimEpoch, request.requestID,
            request.offerID, request.entryID,
            KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED, 0, batch) ||
        !QueueSendBatchNL(batch))
        return NetworkClipboardPacketResult::INVALID;
      return NetworkClipboardPacketResult::HANDLED;
    }

    FileTransfer transfer;
    transfer.peer          = peer.peer;
    transfer.direction     = Direction::REMOTE_REQUESTER;
    transfer.claimEpoch    = m_claimEpoch;
    transfer.wireID        = request.requestID;
    transfer.localID       = NextLocalNL(false);
    transfer.offerID       = request.offerID;
    transfer.entryID       = request.entryID;
    transfer.requestOffset = request.offset;
    transfer.length        = request.length;
    transfer.maxChunk      = maxChunk;
    transfer.operation     = operation;
    transfer.bulk          = transferBulk;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST;
    item.record.clipboardGeneration = transfer.offerID;
    item.record.transfer            = transfer.localID;
    item.record.offset              = transfer.requestOffset;
    item.record.size                = node;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.flags               =
      transfer.operation == KVMFR_CLIPBOARD_FILE_OP_READ ? transfer.length : 0;
    item.record.token               = transfer.operation;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    try
    {
      m_fileTransfers.emplace_back(transfer);
    }
    catch (const std::bad_alloc&)
    {
      m_targetBytes -=
        sizeof(m_targets.back().record) + m_targets.back().data.size();
      m_targets.pop_back();
      return NetworkClipboardPacketResult::INVALID;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_DATA_BEGIN:
  {
    LGNetFileTransfer wire = {};
    if (lgNetFileTransferDecode(&wire, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID || wire.processedLength ||
      wire.offset > wire.totalLength)
      return NetworkClipboardPacketResult::INVALID;
    FileTransfer * transfer = FindFileWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      transfer->offerID != wire.offerID || transfer->entryID != wire.entryID ||
      transfer->began || transfer->requestOffset != wire.offset ||
      (transfer->operation == KVMFR_CLIPBOARD_FILE_OP_READ &&
        (wire.totalLength - wire.offset > MAX_FILE_READ_LENGTH ||
          wire.totalLength - wire.offset > transfer->length)))
      return NetworkClipboardPacketResult::INVALID;
    transfer->began          = true;
    transfer->bulk           = bulk;
    transfer->sizeHint       = wire.totalLength - wire.offset;
    transfer->responseOffset = 0;
    transfer->sequence       = 0;
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_DATA_CHUNK:
  {
    LGNetFileChunk chunkValue = {};
    if (lgNetFileChunkDecode(&chunkValue, packet.payload,
          packet.payloadLength) != LG_NET_PARSE_OK ||
      chunkValue.requestID != envelope.requestID ||
      chunkValue.dataLength > MAX_CHUNK_LENGTH)
      return NetworkClipboardPacketResult::INVALID;
    FileTransfer * transfer = FindFileWireNL(chunkValue.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      !transfer->began || transfer->bulk != bulk ||
      transfer->offerID != chunkValue.offerID ||
      transfer->entryID != chunkValue.entryID ||
      transfer->responseOffset != chunkValue.offset ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
        transfer->sizeHint != chunkValue.totalLength))
      return NetworkClipboardPacketResult::INVALID;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = transfer->localID;
    item.record.offset              = transfer->responseOffset;
    item.record.size                = transfer->responseOffset ?
      KVMFR_CLIPBOARD_SIZE_UNKNOWN : transfer->sizeHint;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.sequence            = transfer->sequence;
    item.record.flags               =
      transfer->responseOffset ? 0 : KVMFR_CLIPBOARD_FLAG_BEGIN;
    item.record.token               = transfer->operation;
    item.record.length              = chunkValue.dataLength;
    try
    {
      item.data.assign(
        chunkValue.data, chunkValue.data + chunkValue.dataLength);
    }
    catch (const std::bad_alloc&)
    {
      return NetworkClipboardPacketResult::INVALID;
    }
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;

    transfer = FindFileWireNL(chunkValue.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::HANDLED;
    transfer->responseOffset += chunkValue.dataLength;
    ++transfer->sequence;
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_DATA_END:
  {
    LGNetFileTransfer wire = {};
    if (lgNetFileTransferDecode(&wire, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID || wire.offset > wire.totalLength ||
      wire.processedLength != wire.totalLength - wire.offset)
      return NetworkClipboardPacketResult::INVALID;
    FileTransfer * transfer = FindFileWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::HELPER_REQUESTER ||
      !transfer->began || transfer->bulk != bulk ||
      transfer->offerID != wire.offerID ||
      transfer->entryID != wire.entryID ||
      transfer->requestOffset != wire.offset ||
      transfer->responseOffset != wire.processedLength ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
        transfer->sizeHint != wire.processedLength))
      return NetworkClipboardPacketResult::INVALID;

    TargetItem item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = transfer->localID;
    item.record.offset              = transfer->responseOffset;
    item.record.size                = transfer->responseOffset;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.sequence            = transfer->sequence;
    item.record.flags               = KVMFR_CLIPBOARD_FLAG_END;
    if (!transfer->responseOffset)
      item.record.flags |= KVMFR_CLIPBOARD_FLAG_BEGIN;
    item.record.token               = transfer->operation;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [&wire](const FileTransfer& value)
        { return value.wireID == wire.requestID; }),
      m_fileTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_DATA_READY:
  {
    LGNetFileTransfer wire = {};
    if (lgNetFileTransferDecode(&wire, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID || wire.processedLength)
      return NetworkClipboardPacketResult::INVALID;
    FileTransfer * transfer = FindFileWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->direction != Direction::REMOTE_REQUESTER ||
      transfer->offerID != wire.offerID || transfer->entryID != wire.entryID)
      return NetworkClipboardPacketResult::INVALID;
    if (m_sourceBlocked && m_sourceBlockedService == LG_NET_SERVICE_FILE &&
      m_sourceBlockedTransfer == transfer->localID)
    {
      m_sourceBlocked         = false;
      m_sourceBlockedService  = 0;
      m_sourceBlockedTransfer = 0;
      m_receiveReadyPending   = true;
    }
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_CANCEL:
  {
    LGNetFileTransfer wire = {};
    if (lgNetFileTransferDecode(&wire, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      wire.requestID != envelope.requestID)
      return NetworkClipboardPacketResult::INVALID;
    FileTransfer * transfer = FindFileWireNL(wire.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->offerID != wire.offerID ||
      transfer->entryID != wire.entryID)
      return NetworkClipboardPacketResult::INVALID;
    const uint64_t localID = transfer->localID;
    TargetItem     item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = localID;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.token               = KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [localID](const FileTransfer& value)
        { return value.localID == localID; }),
      m_fileTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_STATUS:
  {
    LGNetFileStatus status = {};
    if (lgNetFileStatusDecode(&status, packet.payload, packet.payloadLength) !=
        LG_NET_PARSE_OK ||
      status.requestID != envelope.requestID)
      return NetworkClipboardPacketResult::INVALID;
    if (status.status == LG_NET_FILE_STATUS_OK)
      return NetworkClipboardPacketResult::HANDLED;
    const KVMFRClipboardFileError error =
      lgNetFileKVMFRErrorFromStatus(status.status, status.detail);

    if (!status.entryID)
    {
      FileAcquisition * acquisition = FindAcquisitionWireNL(status.requestID);
      if (!acquisition)
        return NetworkClipboardPacketResult::IGNORED;
      if (acquisition->direction != Direction::HELPER_REQUESTER ||
        acquisition->offerID != status.offerID)
        return NetworkClipboardPacketResult::INVALID;
      const uint64_t localID = acquisition->localID;
      TargetItem     item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED;
      item.record.clipboardGeneration = acquisition->offerID;
      item.record.transfer            = localID;
      item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
      item.record.token               = error;
      if (!QueueTargetNL(std::move(item)))
        return NetworkClipboardPacketResult::INVALID;
      m_fileAcquisitions.erase(
        std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
          [localID](const FileAcquisition& value)
          { return value.localID == localID; }),
        m_fileAcquisitions.end());
      return NetworkClipboardPacketResult::HANDLED;
    }

    FileTransfer * transfer = FindFileWireNL(status.requestID);
    if (!transfer)
      return NetworkClipboardPacketResult::IGNORED;
    if (transfer->offerID != status.offerID ||
      transfer->entryID != status.entryID)
      return NetworkClipboardPacketResult::INVALID;
    const uint64_t localID = transfer->localID;
    TargetItem     item;
    item.record.version             = KVMFR_CLIPBOARD_VERSION;
    item.record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
    item.record.clipboardGeneration = transfer->offerID;
    item.record.transfer            = localID;
    item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    item.record.token               = error;
    if (!QueueTargetNL(std::move(item)))
      return NetworkClipboardPacketResult::INVALID;
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [localID](const FileTransfer& value)
        { return value.localID == localID; }),
      m_fileTransfers.end());
    return NetworkClipboardPacketResult::HANDLED;
  }

  case LG_NET_FILE_MESSAGE_ENTRY:
  default:
    return NetworkClipboardPacketResult::INVALID;
  }
}

ClipboardChannelResult CNetworkClipboard::Impl::HandleHelperClipboardNL(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  if (record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ||
    record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR)
  {
    if ((record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER && !ValidOffer(record)) ||
      (record.type == KVMFR_CLIPBOARD_MESSAGE_CLEAR && !ValidClear(record)))
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    if (m_claimEpoch)
    {
      bool built = true;
      for (const ClipboardTransfer& transfer : m_clipboardTransfers)
        if (transfer.direction == Direction::REMOTE_REQUESTER)
          built = built && QueueClipboardStatusNL(
            transfer.peer, transfer.claimEpoch, transfer.wireID,
            transfer.offerID, LG_NET_CLIPBOARD_STATUS_CANCELLED, 0,
            transfer.offset, batch);
      for (const FileTransfer& transfer : m_fileTransfers)
        if (transfer.direction == Direction::REMOTE_REQUESTER)
          built = built && QueueFileStatusNL(
            transfer.peer, transfer.claimEpoch, transfer.wireID,
            transfer.offerID, transfer.entryID,
            KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED, transfer.responseOffset,
            batch);
      for (const FileAcquisition& acquisition : m_fileAcquisitions)
        if (acquisition.direction == Direction::REMOTE_REQUESTER)
          built = built && QueueFileStatusNL(
            acquisition.peer, acquisition.claimEpoch, acquisition.wireID,
            acquisition.offerID, 0, KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED, 0,
            batch);

      built = built && (record.type == KVMFR_CLIPBOARD_MESSAGE_OFFER ?
          BuildCachedOfferNL(batch, record) :
          QueueControlNL(LG_NET_CLIPBOARD_MESSAGE_CLEAR,
            record.clipboardGeneration, batch));
      if (!built || !QueueSendBatchNL(batch))
      {
        m_sourceBlocked         = true;
        m_sourceBlockedService  = LG_NET_SERVICE_CLIPBOARD;
        m_sourceBlockedTransfer = 0;
        return ClipboardChannelResult::BUSY;
      }
    }

    m_cachedRecord = record;
    m_cachedValid  = true;
    m_clipboardTransfers.erase(
      std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
        [](const ClipboardTransfer& transfer)
        { return transfer.direction == Direction::REMOTE_REQUESTER; }),
      m_clipboardTransfers.end());
    m_fileAcquisitions.erase(
      std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
        [](const FileAcquisition& acquisition)
        { return acquisition.direction == Direction::REMOTE_REQUESTER; }),
      m_fileAcquisitions.end());
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [](const FileTransfer& transfer)
        { return transfer.direction == Direction::REMOTE_REQUESTER; }),
      m_fileTransfers.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  if (!m_claimEpoch)
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST &&
      ValidRequest(record, true))
    {
      TargetItem item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
      item.record.clipboardGeneration = record.clipboardGeneration;
      item.record.transfer            = record.transfer;
      item.record.format              = record.format;
      QueueTargetNL(std::move(item));
      return ClipboardChannelResult::ACCEPTED;
    }
    return record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL &&
        ValidCancel(record) ?
      ClipboardChannelResult::ACCEPTED :
      ClipboardChannelResult::FAILED;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_REQUEST)
  {
    if (!ValidRequest(record, true) || !m_remoteOffer.published ||
      record.clipboardGeneration != m_remoteOffer.offerID ||
      FindClipboardLocalNL(record.transfer) ||
      m_clipboardTransfers.size() >= MAX_REQUESTS)
      return ClipboardChannelResult::FAILED;

    uint32_t formatIndex = UINT32_MAX;
    for (uint32_t index = 0; index < m_remoteOffer.formatCount; ++index)
      if (m_remoteOffer.formats[index] == record.format)
      {
        formatIndex = index;
        break;
      }
    if (formatIndex == UINT32_MAX ||
      !kvmfrClipboardRepresentationFormatValid(record.format))
      return ClipboardChannelResult::FAILED;

    ClipboardTransfer transfer;
    transfer.peer          = m_owner;
    transfer.direction     = Direction::HELPER_REQUESTER;
    transfer.claimEpoch    = m_claimEpoch;
    transfer.wireID        = NextWireNL();
    transfer.localID       = record.transfer;
    transfer.offerID       = record.clipboardGeneration;
    transfer.formatIndex   = formatIndex;
    transfer.format        = record.format;
    const PeerState * peer = FindPeerNL(transfer.peer);
    if (!peer)
      return ClipboardChannelResult::FAILED;
    transfer.bulk     = SelectBulkNL(*peer, LG_NET_SERVICE_CLIPBOARD,
      LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE) &&
      ChunkLengthNL(*peer, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, true) >= MIN_CHUNK_LENGTH;
    transfer.maxChunk = ChunkLengthNL(*peer, LG_NET_SERVICE_CLIPBOARD,
      LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, transfer.bulk);
    if (transfer.maxChunk < MIN_CHUNK_LENGTH)
      return ClipboardChannelResult::FAILED;

    LGNetClipboardRequest request = {};
    request.offerID               = transfer.offerID;
    request.requestID             = transfer.wireID;
    request.formatIndex           = transfer.formatIndex;
    request.maxChunkLength        = transfer.maxChunk;
    request.offset                = 0;
    SendItem send;
    if (!BuildSendNL(send, transfer.peer, LG_NET_SERVICE_CLIPBOARD,
          LG_NET_CLIPBOARD_MESSAGE_REQUEST, LG_NET_ENVELOPE_ACK_REQUIRED,
          transfer.claimEpoch, transfer.wireID,
          LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE, false) ||
      !lgNetClipboardRequestEncode(
        send.payload.data(), send.payload.size(), &request))
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::FAILED;
    }
    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_CLIPBOARD;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    try
    {
      m_clipboardTransfers.emplace_back(transfer);
    }
    catch (const std::bad_alloc&)
    {
      m_failed = true;
      return ClipboardChannelResult::FAILED;
    }
    return ClipboardChannelResult::ACCEPTED;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
  {
    if (!ValidData(record) || (record.length && !data))
      return ClipboardChannelResult::FAILED;
    ClipboardTransfer * transfer = FindClipboardLocalNL(record.transfer);
    if (!transfer || transfer->direction != Direction::REMOTE_REQUESTER ||
      transfer->offerID != record.clipboardGeneration ||
      transfer->format != record.format || transfer->offset != record.offset ||
      transfer->sequence != record.sequence)
      return ClipboardChannelResult::FAILED;
    if ((!transfer->began && !(record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)) ||
      (transfer->began && (record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)))
      return ClipboardChannelResult::FAILED;

    const uint64_t sizeHint =
      transfer->began ? transfer->sizeHint : record.size;
    const uint64_t end      = record.offset + record.length;
    if ((sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && end > sizeHint) ||
      ((record.flags & KVMFR_CLIPBOARD_FLAG_END) && record.size != end))
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    if (!transfer->began)
    {
      LGNetClipboardTransfer begin = {};
      begin.requestID              = transfer->wireID;
      begin.offerID                = transfer->offerID;
      begin.totalLength            = record.size;
      begin.processedLength        = 0;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer,
            LG_NET_SERVICE_CLIPBOARD,
            LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN, 0, transfer->claimEpoch,
            transfer->wireID, LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE,
            transfer->bulk) ||
        !lgNetClipboardTransferEncode(
          send.payload.data(), send.payload.size(), &begin))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }

    uint32_t       consumed   = 0;
    const uint64_t wireLength =
      (record.flags & KVMFR_CLIPBOARD_FLAG_END) ? end : sizeHint;
    while (consumed < record.length)
    {
      const uint32_t length =
        (std::min)(transfer->maxChunk, record.length - consumed);
      LGNetClipboardChunk chunk = {};
      chunk.requestID           = transfer->wireID;
      chunk.offerID             = transfer->offerID;
      chunk.offset              = record.offset + consumed;
      chunk.totalLength         = wireLength;
      chunk.dataLength          = length;
      chunk.flags               = static_cast<LGNetClipboardChunkFlags>(
        chunk.offset ? 0 : LG_NET_CLIPBOARD_CHUNK_FIRST);
      if ((record.flags & KVMFR_CLIPBOARD_FLAG_END) &&
        consumed + length == record.length)
        chunk.flags |= LG_NET_CLIPBOARD_CHUNK_FINAL;
      chunk.data                 = data + consumed;
      const size_t payloadLength = lgNetClipboardChunkSize(&chunk);
      if (!payloadLength || payloadLength > UINT32_MAX)
        return ClipboardChannelResult::FAILED;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer,
            LG_NET_SERVICE_CLIPBOARD,
            LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK, 0, transfer->claimEpoch,
            transfer->wireID, static_cast<uint32_t>(payloadLength),
            transfer->bulk) ||
        !lgNetClipboardChunkEncode(
          send.payload.data(), send.payload.size(), &chunk))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
      consumed += length;
    }

    if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
    {
      LGNetClipboardTransfer finish = {};
      finish.requestID              = transfer->wireID;
      finish.offerID                = transfer->offerID;
      finish.totalLength            = end;
      finish.processedLength        = end;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer,
            LG_NET_SERVICE_CLIPBOARD,
            LG_NET_CLIPBOARD_MESSAGE_DATA_END, 0, transfer->claimEpoch,
            transfer->wireID, LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE,
            transfer->bulk) ||
        !lgNetClipboardTransferEncode(
          send.payload.data(), send.payload.size(), &finish))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }

    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_CLIPBOARD;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }

    transfer = FindClipboardLocalNL(record.transfer);
    if (!transfer)
      return ClipboardChannelResult::ACCEPTED;
    if (!transfer->began)
    {
      transfer->began    = true;
      transfer->sizeHint = record.size;
    }
    transfer->offset += record.length;
    ++transfer->sequence;
    if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
      m_clipboardTransfers.erase(
        std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
          [&record](const ClipboardTransfer& value)
          { return value.localID == record.transfer; }),
        m_clipboardTransfers.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_CANCEL)
  {
    if (!ValidCancel(record))
      return ClipboardChannelResult::FAILED;
    ClipboardTransfer * transfer = FindClipboardLocalNL(record.transfer);
    if (!transfer)
      return ClipboardChannelResult::ACCEPTED;

    LGNetClipboardTransfer wire = {};
    wire.requestID              = transfer->wireID;
    wire.offerID                = transfer->offerID;
    wire.totalLength            = transfer->sizeHint;
    wire.processedLength        = transfer->offset;
    SendItem send;
    if (!BuildSendNL(send, transfer->peer, LG_NET_SERVICE_CLIPBOARD,
          LG_NET_CLIPBOARD_MESSAGE_CANCEL, 0, transfer->claimEpoch,
          transfer->wireID, LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE, false) ||
      !lgNetClipboardTransferEncode(
        send.payload.data(), send.payload.size(), &wire))
      return ClipboardChannelResult::FAILED;
    std::vector<SendItem> batch;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::FAILED;
    }
    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_CLIPBOARD;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    m_clipboardTransfers.erase(
      std::remove_if(m_clipboardTransfers.begin(), m_clipboardTransfers.end(),
        [&record](const ClipboardTransfer& value)
        { return value.localID == record.transfer; }),
      m_clipboardTransfers.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  return ClipboardChannelResult::FAILED;
}

ClipboardChannelResult CNetworkClipboard::Impl::HandleHelperFileNL(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  if (!kvmfrClipboardFileMessageValid(&record) || (record.length && !data))
    return ClipboardChannelResult::FAILED;

  if (!m_claimEpoch)
  {
    if (record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST)
    {
      TargetItem item;
      item.record.version             = KVMFR_CLIPBOARD_VERSION;
      item.record.clipboardGeneration = record.clipboardGeneration;
      item.record.transfer            = record.transfer;
      item.record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
      item.record.type                = record.type ==
          KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ?
        KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED :
        KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
      item.record.token               =
        KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
      QueueTargetNL(std::move(item));
      return ClipboardChannelResult::ACCEPTED;
    }
    return ClipboardChannelResult::ACCEPTED;
  }

  switch (record.type)
  {
  case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
  {
    bool duplicate = false;
    for (const FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.direction == Direction::HELPER_REQUESTER &&
        acquisition.offerID == record.clipboardGeneration)
      {
        duplicate = true;
        break;
      }
    if (!kvmfrClipboardTransferFromHelper(record.transfer) ||
      !m_remoteOffer.published || !m_remoteOffer.fileOffer ||
      record.clipboardGeneration != m_remoteOffer.offerID ||
      FindAcquisitionLocalNL(record.transfer) || duplicate ||
      m_fileAcquisitions.size() >= MAX_ACQUISITIONS)
      return ClipboardChannelResult::FAILED;

    FileAcquisition acquisition;
    acquisition.peer       = m_owner;
    acquisition.direction  = Direction::HELPER_REQUESTER;
    acquisition.claimEpoch = m_claimEpoch;
    acquisition.offerID    = record.clipboardGeneration;
    acquisition.wireID     = NextWireNL();
    acquisition.localID    = record.transfer;

    LGNetFileLease lease = {};
    lease.clientID       = acquisition.wireID;
    lease.offerID        = acquisition.offerID;
    lease.leaseEpoch     = 0;
    lease.leaseMs        = LG_NET_FILE_MAX_LEASE_MS;
    lease.flags          = LG_NET_FILE_LEASE_READ;
    SendItem send;
    if (!BuildSendNL(send, acquisition.peer, LG_NET_SERVICE_FILE,
          LG_NET_FILE_MESSAGE_ACQUIRE, LG_NET_ENVELOPE_ACK_REQUIRED,
          acquisition.claimEpoch, acquisition.wireID,
          LG_NET_FILE_LEASE_WIRE_SIZE, false) ||
      !lgNetFileLeaseEncode(send.payload.data(), send.payload.size(), &lease))
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::FAILED;
    }
    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    try
    {
      m_fileAcquisitions.emplace_back(acquisition);
    }
    catch (const std::bad_alloc&)
    {
      m_failed = true;
      return ClipboardChannelResult::FAILED;
    }
    return ClipboardChannelResult::ACCEPTED;
  }

  case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
  {
    if (!kvmfrClipboardTransferFromClient(record.transfer))
      return ClipboardChannelResult::FAILED;
    FileAcquisition * acquisition = FindAcquisitionLocalNL(record.transfer);
    if (!acquisition || acquisition->direction != Direction::REMOTE_REQUESTER ||
      acquisition->offerID != record.clipboardGeneration || acquisition->active)
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    uint64_t              leaseEpoch = 0;
    if (record.token == KVMFR_CLIPBOARD_FILE_ERROR_NONE)
    {
      leaseEpoch           = NextEpochNL();
      LGNetFileLease lease = {};
      lease.clientID       = acquisition->wireID;
      lease.offerID        = acquisition->offerID;
      lease.leaseEpoch     = leaseEpoch;
      lease.leaseMs        = LG_NET_FILE_MAX_LEASE_MS;
      lease.flags          = LG_NET_FILE_LEASE_READ |
        LG_NET_FILE_LEASE_ACQUIRED;
      SendItem send;
      if (!BuildSendNL(send, acquisition->peer, LG_NET_SERVICE_FILE,
            LG_NET_FILE_MESSAGE_ACQUIRED, LG_NET_ENVELOPE_RESPONSE,
            acquisition->claimEpoch, acquisition->wireID,
            LG_NET_FILE_LEASE_WIRE_SIZE, false) ||
        !lgNetFileLeaseEncode(send.payload.data(), send.payload.size(), &lease))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }
    else if (!QueueFileStatusNL(acquisition->peer, acquisition->claimEpoch,
               acquisition->wireID, acquisition->offerID, 0,
               static_cast<KVMFRClipboardFileError>(record.token), 0, batch))
      return ClipboardChannelResult::FAILED;

    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }

    acquisition = FindAcquisitionLocalNL(record.transfer);
    if (!acquisition)
      return ClipboardChannelResult::ACCEPTED;
    if (record.token == KVMFR_CLIPBOARD_FILE_ERROR_NONE)
    {
      acquisition->leaseEpoch = leaseEpoch;
      acquisition->active     = true;
    }
    else
      m_fileAcquisitions.erase(
        std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
          [&record](const FileAcquisition& value)
          { return value.localID == record.transfer; }),
        m_fileAcquisitions.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
  {
    if (!kvmfrClipboardTransferFromHelper(record.transfer))
      return ClipboardChannelResult::FAILED;
    FileAcquisition * acquisition = FindAcquisitionLocalNL(record.transfer);
    if (!acquisition || acquisition->direction != Direction::HELPER_REQUESTER ||
      acquisition->offerID != record.clipboardGeneration ||
      !acquisition->active)
      return ClipboardChannelResult::ACCEPTED;

    LGNetFileLease lease = {};
    lease.clientID       = acquisition->wireID;
    lease.offerID        = acquisition->offerID;
    lease.leaseEpoch     = acquisition->leaseEpoch;
    lease.leaseMs        = 0;
    lease.flags          = LG_NET_FILE_LEASE_READ;
    SendItem send;
    if (!BuildSendNL(send, acquisition->peer, LG_NET_SERVICE_FILE,
          LG_NET_FILE_MESSAGE_RELEASE, 0, acquisition->claimEpoch,
          acquisition->wireID, LG_NET_FILE_LEASE_WIRE_SIZE, false) ||
      !lgNetFileLeaseEncode(send.payload.data(), send.payload.size(), &lease))
      return ClipboardChannelResult::FAILED;
    std::vector<SendItem> batch;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::FAILED;
    }
    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    const uint64_t offerID = acquisition->offerID;
    m_fileAcquisitions.erase(
      std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
        [&record](const FileAcquisition& value)
        { return value.localID == record.transfer; }),
      m_fileAcquisitions.end());
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [offerID](const FileTransfer& value)
        {
          return value.direction == Direction::HELPER_REQUESTER &&
            value.offerID == offerID;
        }),
      m_fileTransfers.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
  {
    if (!kvmfrClipboardTransferFromHelper(record.transfer) ||
      FindFileLocalNL(record.transfer) ||
      m_fileTransfers.size() >= MAX_REQUESTS)
      return ClipboardChannelResult::FAILED;
    bool acquired = false;
    for (const FileAcquisition& acquisition : m_fileAcquisitions)
      if (acquisition.direction == Direction::HELPER_REQUESTER &&
        acquisition.offerID == record.clipboardGeneration && acquisition.active)
      {
        acquired = true;
        break;
      }
    if (!acquired)
      return ClipboardChannelResult::FAILED;

    uint64_t entryID;
    if (!lgNetFileEntryIDFromKVMFRNode(record.size, &entryID))
      return ClipboardChannelResult::FAILED;
    const KVMFRClipboardFileOperation operation =
      static_cast<KVMFRClipboardFileOperation>(record.token);

    FileTransfer transfer;
    transfer.peer          = m_owner;
    transfer.direction     = Direction::HELPER_REQUESTER;
    transfer.claimEpoch    = m_claimEpoch;
    transfer.wireID        = NextWireNL();
    transfer.localID       = record.transfer;
    transfer.offerID       = record.clipboardGeneration;
    transfer.entryID       = entryID;
    transfer.requestOffset = record.offset;
    transfer.length        =
      operation == KVMFR_CLIPBOARD_FILE_OP_READ ? record.flags : 0;
    transfer.operation     = operation;

    LGNetFileRequest request = {};
    request.requestID        = transfer.wireID;
    request.offerID          = transfer.offerID;
    request.entryID          = transfer.entryID;
    request.offset           = transfer.requestOffset;
    request.length           = transfer.length;
    request.flags            =
      lgNetFileRequestFlagsFromKVMFROperation(transfer.operation);
    SendItem send;
    if (!BuildSendNL(send, transfer.peer, LG_NET_SERVICE_FILE,
          LG_NET_FILE_MESSAGE_REQUEST, LG_NET_ENVELOPE_ACK_REQUIRED,
          transfer.claimEpoch, transfer.wireID, LG_NET_FILE_REQUEST_WIRE_SIZE,
          false) ||
      !lgNetFileRequestEncode(
        send.payload.data(), send.payload.size(), &request))
      return ClipboardChannelResult::FAILED;
    std::vector<SendItem> batch;
    try
    {
      batch.emplace_back(std::move(send));
    }
    catch (const std::bad_alloc&)
    {
      return ClipboardChannelResult::FAILED;
    }
    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    try
    {
      m_fileTransfers.emplace_back(transfer);
    }
    catch (const std::bad_alloc&)
    {
      m_failed = true;
      return ClipboardChannelResult::FAILED;
    }
    return ClipboardChannelResult::ACCEPTED;
  }

  case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
  {
    FileTransfer * transfer = FindFileLocalNL(record.transfer);
    if (!transfer || transfer->direction != Direction::REMOTE_REQUESTER ||
      transfer->offerID != record.clipboardGeneration ||
      transfer->operation != record.token ||
      transfer->responseOffset != record.offset ||
      transfer->sequence != record.sequence ||
      (!transfer->began && !(record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)) ||
      (transfer->began && (record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN)))
      return ClipboardChannelResult::FAILED;

    const uint64_t sizeHint =
      transfer->began ? transfer->sizeHint : record.size;
    const uint64_t end      = record.offset + record.length;
    if (end > sizeHint ||
      (transfer->operation == KVMFR_CLIPBOARD_FILE_OP_READ &&
        (sizeHint > MAX_FILE_READ_LENGTH || end > transfer->length)) ||
      ((record.flags & KVMFR_CLIPBOARD_FLAG_END) && record.size != end))
      return ClipboardChannelResult::FAILED;

    std::vector<SendItem> batch;
    if (!transfer->began)
    {
      LGNetFileTransfer begin = {};
      begin.requestID         = transfer->wireID;
      begin.offerID           = transfer->offerID;
      begin.entryID           = transfer->entryID;
      begin.offset            = transfer->requestOffset;
      begin.totalLength       = transfer->requestOffset + record.size;
      begin.processedLength   = 0;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer, LG_NET_SERVICE_FILE,
            LG_NET_FILE_MESSAGE_DATA_BEGIN, 0, transfer->claimEpoch,
            transfer->wireID, LG_NET_FILE_TRANSFER_WIRE_SIZE,
            transfer->bulk) ||
        !lgNetFileTransferEncode(
          send.payload.data(), send.payload.size(), &begin))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }

    uint32_t       consumed   = 0;
    const uint64_t wireLength =
      (record.flags & KVMFR_CLIPBOARD_FLAG_END) ? end : sizeHint;
    while (consumed < record.length)
    {
      const uint32_t length =
        (std::min)(transfer->maxChunk, record.length - consumed);
      LGNetFileChunk chunk = {};
      chunk.requestID      = transfer->wireID;
      chunk.offerID        = transfer->offerID;
      chunk.entryID        = transfer->entryID;
      chunk.offset         = record.offset + consumed;
      chunk.totalLength    = wireLength;
      chunk.dataLength     = length;
      chunk.flags          = static_cast<LGNetFileChunkFlags>(
        chunk.offset ? 0 : LG_NET_FILE_CHUNK_FIRST);
      if ((record.flags & KVMFR_CLIPBOARD_FLAG_END) &&
        consumed + length == record.length)
        chunk.flags |= LG_NET_FILE_CHUNK_FINAL;
      chunk.data                 = data + consumed;
      const size_t payloadLength = lgNetFileChunkSize(&chunk);
      if (!payloadLength || payloadLength > UINT32_MAX)
        return ClipboardChannelResult::FAILED;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer, LG_NET_SERVICE_FILE,
            LG_NET_FILE_MESSAGE_DATA_CHUNK, 0, transfer->claimEpoch,
            transfer->wireID, static_cast<uint32_t>(payloadLength),
            transfer->bulk) ||
        !lgNetFileChunkEncode(send.payload.data(), send.payload.size(), &chunk))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
      consumed += length;
    }

    if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
    {
      LGNetFileTransfer finish = {};
      finish.requestID         = transfer->wireID;
      finish.offerID           = transfer->offerID;
      finish.entryID           = transfer->entryID;
      finish.offset            = transfer->requestOffset;
      finish.totalLength       = transfer->requestOffset + end;
      finish.processedLength   = end;
      SendItem send;
      if (!BuildSendForDeliveryNL(send, transfer->peer, LG_NET_SERVICE_FILE,
            LG_NET_FILE_MESSAGE_DATA_END, 0, transfer->claimEpoch,
            transfer->wireID, LG_NET_FILE_TRANSFER_WIRE_SIZE,
            transfer->bulk) ||
        !lgNetFileTransferEncode(
          send.payload.data(), send.payload.size(), &finish))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }

    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }

    transfer = FindFileLocalNL(record.transfer);
    if (!transfer)
      return ClipboardChannelResult::ACCEPTED;
    if (!transfer->began)
    {
      transfer->began    = true;
      transfer->sizeHint = record.size;
    }
    transfer->responseOffset += record.length;
    ++transfer->sequence;
    if (record.flags & KVMFR_CLIPBOARD_FLAG_END)
      m_fileTransfers.erase(
        std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
          [&record](const FileTransfer& value)
          { return value.localID == record.transfer; }),
        m_fileTransfers.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
  {
    FileTransfer *        transfer    = FindFileLocalNL(record.transfer);
    FileAcquisition *     acquisition = FindAcquisitionLocalNL(record.transfer);
    std::vector<SendItem> batch;
    if (transfer)
    {
      LGNetFileTransfer wire = {};
      wire.requestID         = transfer->wireID;
      wire.offerID           = transfer->offerID;
      wire.entryID           = transfer->entryID;
      wire.offset            = transfer->requestOffset;
      wire.totalLength       = transfer->requestOffset +
        (transfer->sizeHint == KVMFR_CLIPBOARD_SIZE_UNKNOWN ?
            transfer->responseOffset :
            transfer->sizeHint);
      wire.processedLength   = transfer->responseOffset;
      SendItem send;
      if (!BuildSendNL(send, transfer->peer, LG_NET_SERVICE_FILE,
            LG_NET_FILE_MESSAGE_CANCEL, 0, transfer->claimEpoch,
            transfer->wireID, LG_NET_FILE_TRANSFER_WIRE_SIZE, false) ||
        !lgNetFileTransferEncode(
          send.payload.data(), send.payload.size(), &wire))
        return ClipboardChannelResult::FAILED;
      try
      {
        batch.emplace_back(std::move(send));
      }
      catch (const std::bad_alloc&)
      {
        return ClipboardChannelResult::FAILED;
      }
    }
    else if (acquisition)
    {
      if (!QueueFileStatusNL(acquisition->peer, acquisition->claimEpoch,
            acquisition->wireID, acquisition->offerID, 0,
            static_cast<KVMFRClipboardFileError>(record.token), 0, batch))
        return ClipboardChannelResult::FAILED;
    }
    else
      return ClipboardChannelResult::ACCEPTED;

    if (!QueueSendBatchNL(batch))
    {
      m_sourceBlocked         = true;
      m_sourceBlockedService  = LG_NET_SERVICE_FILE;
      m_sourceBlockedTransfer = record.transfer;
      return ClipboardChannelResult::BUSY;
    }
    m_fileTransfers.erase(
      std::remove_if(m_fileTransfers.begin(), m_fileTransfers.end(),
        [&record](const FileTransfer& value)
        { return value.localID == record.transfer; }),
      m_fileTransfers.end());
    m_fileAcquisitions.erase(
      std::remove_if(m_fileAcquisitions.begin(), m_fileAcquisitions.end(),
        [&record](const FileAcquisition& value)
        { return value.localID == record.transfer; }),
      m_fileAcquisitions.end());
    return ClipboardChannelResult::ACCEPTED;
  }

  default:
    return ClipboardChannelResult::FAILED;
  }
}

CNetworkClipboard::CNetworkClipboard() : m_impl(std::make_unique<Impl>())
{
  const uint64_t seed =
    GetTickCount64() ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this));
  m_impl->m_nextSerial = seed ? seed : 1;
  m_impl->m_nextLocal  = seed ^ UINT64_C(0x4b564d4652434c50);
  m_impl->m_nextWire   = seed ^ UINT64_C(0x4e4554434c495042);
  m_impl->m_nextEpoch  = seed ^ UINT64_C(0x46494c4545504f43);
}

CNetworkClipboard::~CNetworkClipboard()
{
  Stop();
  DetachSender();
}

bool CNetworkClipboard::AttachSender(INetworkClipboardSender& sender)
{
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_sender)
    return false;
  m_impl->m_sender = &sender;
  return true;
}

void CNetworkClipboard::DetachSender()
{
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_senderCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (!m_impl->m_sender)
    return;
  m_impl->DropOwnerNL(true);
  m_impl->m_sender = nullptr;
  m_impl->m_peers.clear();
  m_impl->m_sends.clear();
  m_impl->m_sendBytes = 0;
}

bool CNetworkClipboard::PeerConnected(const NetworkClipboardPeer& peer)
{
  if (!peer.client || !peer.generation)
    return false;

  NetworkClipboardPeerInfo info;
  {
    CSRWExclusiveLock         callbackLock(m_impl->m_senderCallbackLock);
    INetworkClipboardSender * sender;
    {
      CSRWSharedLock lock(m_impl->m_lock);
      sender = m_impl->m_sender;
    }
    if (!sender || !sender->ClipboardPeerInfo(peer, info))
      return false;
  }

  if (info.clipboardVersion <
      LG_NET_CLIPBOARD_CHUNK_INTRODUCED_SERVICE_VERSION ||
    !(info.clipboardFlags & LG_NET_CAPABILITY_RELIABLE) ||
    info.clipboardReliablePayload < LG_NET_CLIPBOARD_OFFER_HEADER_WIRE_SIZE +
        LG_NET_CLIPBOARD_MAX_MIME_LENGTH)
    return false;
  if (info.fileVersion < LG_NET_FILE_CHUNK_INTRODUCED_SERVICE_VERSION ||
    !(info.fileFlags & LG_NET_CAPABILITY_RELIABLE) ||
    info.fileReliablePayload < LG_NET_FILE_TRANSFER_WIRE_SIZE)
    return false;

  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (!m_impl->m_sender || m_impl->FindPeerNL(peer))
    return false;
  Impl::PeerState state;
  state.peer = peer;
  state.info = info;
  if (m_impl->ChunkLengthNL(state, LG_NET_SERVICE_CLIPBOARD,
        LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE, false) < MIN_CHUNK_LENGTH ||
    m_impl->ChunkLengthNL(state, LG_NET_SERVICE_FILE,
      LG_NET_FILE_CHUNK_HEADER_WIRE_SIZE, false) < MIN_CHUNK_LENGTH)
    return false;
  try
  {
    m_impl->m_peers.emplace_back(state);
  }
  catch (const std::bad_alloc&)
  {
    return false;
  }
  return true;
}

void CNetworkClipboard::PeerDisconnected(const NetworkClipboardPeer& peer)
{
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (m_impl->m_claimEpoch && SamePeer(m_impl->m_owner, peer))
      m_impl->DropOwnerNL(true);
    m_impl->m_peers.erase(
      std::remove_if(m_impl->m_peers.begin(), m_impl->m_peers.end(),
        [&peer](const Impl::PeerState& state)
        { return SamePeer(state.peer, peer); }),
      m_impl->m_peers.end());
    m_impl->m_sends.erase(
      std::remove_if(m_impl->m_sends.begin(), m_impl->m_sends.end(),
        [this, &peer](const Impl::SendItem& item)
        {
          if (!SamePeer(item.peer, peer))
            return false;
          m_impl->m_sendBytes -= item.payload.size();
          return true;
        }),
      m_impl->m_sends.end());
  }
  m_impl->PumpTargets();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
}

NetworkClipboardPacketResult CNetworkClipboard::HandlePacket(
  const NetworkClipboardPeer& peer, const LGNetPacketView& packet, bool bulk)
{
  if ((packet.envelope.service != LG_NET_SERVICE_CLIPBOARD &&
        packet.envelope.service != LG_NET_SERVICE_FILE) ||
    (packet.payloadLength && !packet.payload) ||
    packet.payloadLength != packet.envelope.payloadLength ||
    packet.payloadLength > LG_NET_MAX_PAYLOAD_LENGTH)
    return NetworkClipboardPacketResult::INVALID;

  CSRWExclusiveLock            processLock(m_impl->m_processLock);
  NetworkClipboardPacketResult result;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    Impl::PeerState * state = m_impl->FindPeerNL(peer);
    if (!state)
      return NetworkClipboardPacketResult::IGNORED;

    const uint16_t             version =
      packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ?
      state->info.clipboardVersion :
      state->info.fileVersion;
    const LGNetCapabilityFlags flags   =
      packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ?
      state->info.clipboardFlags :
      state->info.fileFlags;
    const uint32_t             maximum =
      packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ?
      (bulk ? state->info.clipboardBulkPayload :
              state->info.clipboardReliablePayload) :
      (bulk ? state->info.fileBulkPayload : state->info.fileReliablePayload);
    if (!version || packet.envelope.serviceVersion != version ||
      !(flags & (bulk ? LG_NET_CAPABILITY_BULK : LG_NET_CAPABILITY_RELIABLE)) ||
      packet.payloadLength > maximum)
      return NetworkClipboardPacketResult::INVALID;

    result = packet.envelope.service == LG_NET_SERVICE_CLIPBOARD ?
      m_impl->HandleClipboardNL(*state, packet, bulk, GetTickCount64()) :
      m_impl->HandleFileNL(*state, packet, bulk);
  }

  m_impl->PumpTargets();
  m_impl->PumpSends();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
  return result;
}

void CNetworkClipboard::Process(uint64_t nowMs)
{
  CSRWExclusiveLock processLock = CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (!processLock)
    return;

  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (m_impl->m_claimEpoch && DeadlineReached(nowMs, m_impl->m_claimDeadline))
    {
      const NetworkClipboardPeer owner  = m_impl->m_owner;
      const uint64_t             epoch  = m_impl->m_claimEpoch;
      const uint64_t             serial = m_impl->m_claimSerial;
      m_impl->DropOwnerNL(true);
      m_impl->m_claimSerial = serial;
      if (!m_impl->QueueClaimStatusNL(
            owner, epoch, LG_NET_CLIPBOARD_CLAIM_EXPIRED, 0, 0, 0))
        m_impl->m_failed = true;
      m_impl->m_claimSerial = 0;
    }
  }

  m_impl->PumpTargets();
  m_impl->PumpSends();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
}

void CNetworkClipboard::SendReady(
  const NetworkClipboardPeer& peer, LGNetService service, bool bulk)
{
  if (service != LG_NET_SERVICE_CLIPBOARD && service != LG_NET_SERVICE_FILE)
    return;
  CSRWExclusiveLock processLock = CSRWExclusiveLock::Try(m_impl->m_processLock);
  if (!processLock)
    return;
  {
    CSRWSharedLock lock(m_impl->m_lock);
    if (!m_impl->FindPeerNL(peer))
      return;
  }
  (void)bulk;
  m_impl->PumpSends();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
}

bool CNetworkClipboard::Start(IClipboardTarget& target)
{
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_targetCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  if (m_impl->m_started || m_impl->m_target)
    return false;
  m_impl->m_target          = &target;
  m_impl->m_started         = true;
  m_impl->m_failed          = false;
  m_impl->m_failureNotified = false;
  return true;
}

void CNetworkClipboard::Stop()
{
  CSRWExclusiveLock lifecycleLock(m_impl->m_lifecycleLock);
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  CSRWExclusiveLock callbackLock(m_impl->m_targetCallbackLock);
  CSRWExclusiveLock lock(m_impl->m_lock);
  m_impl->m_target               = nullptr;
  m_impl->m_started              = false;
  m_impl->m_stateKnown           = false;
  m_impl->m_available            = false;
  m_impl->m_generation           = 0;
  m_impl->m_failed               = false;
  m_impl->m_failureNotified      = false;
  m_impl->m_targetCallbackActive = false;
  m_impl->m_targets.clear();
  m_impl->m_targetBytes = 0;
  m_impl->ResetTransfersNL();
  m_impl->m_remoteOffer.published = false;
  m_impl->m_sourceBlocked         = false;
  m_impl->m_sourceBlockedService  = 0;
  m_impl->m_sourceBlockedTransfer = 0;
  m_impl->m_receiveReadyPending   = false;
}

void CNetworkClipboard::ClipboardState(bool available, uint32_t generation)
{
  if (!generation)
    return;
  CSRWExclusiveLock processLock(m_impl->m_processLock);
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (!m_impl->m_started || !m_impl->m_target)
      return;
    const bool generationChanged = m_impl->m_stateKnown &&
      m_impl->m_generation != generation;
    const bool changed           = !m_impl->m_stateKnown ||
      m_impl->m_generation != generation || m_impl->m_available != available;
    m_impl->m_stateKnown = true;
    m_impl->m_available  = available;
    m_impl->m_generation = generation;
    if (!changed)
      return;

    m_impl->m_targets.clear();
    m_impl->m_targetBytes = 0;
    m_impl->ResetTransfersNL();
    m_impl->m_sourceBlocked         = false;
    m_impl->m_sourceBlockedService  = 0;
    m_impl->m_sourceBlockedTransfer = 0;
    m_impl->m_receiveReadyPending   = false;
    m_impl->m_remoteOffer.published = false;
    if (generationChanged && m_impl->m_claimEpoch)
    {
      const NetworkClipboardPeer owner  = m_impl->m_owner;
      const uint64_t             epoch  = m_impl->m_claimEpoch;
      const uint64_t             serial = m_impl->m_claimSerial;
      m_impl->DropOwnerNL(false);
      m_impl->m_claimSerial = serial;
      if (!m_impl->QueueClaimStatusNL(owner, epoch,
            LG_NET_CLIPBOARD_CLAIM_EXPIRED, 0, 0, 0))
        m_impl->m_failed = true;
      m_impl->m_claimSerial = 0;
    }
    if (available && !m_impl->PublishRemoteOfferNL())
    {
      m_impl->m_failed = true;
    }
  }
  m_impl->PumpTargets();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
}

ClipboardChannelResult CNetworkClipboard::SendClipboard(
  const KVMFRClipboardMessage& record, const uint8_t * data)
{
  CSRWExclusiveLock      processLock(m_impl->m_processLock);
  ClipboardChannelResult result;
  {
    CSRWExclusiveLock lock(m_impl->m_lock);
    if (!m_impl->m_started || !m_impl->m_target || !m_impl->m_stateKnown ||
      !m_impl->m_available || !m_impl->m_generation || m_impl->m_failed ||
      record.generation != m_impl->m_generation ||
      record.version != KVMFR_CLIPBOARD_VERSION || (record.length && !data))
      return ClipboardChannelResult::FAILED;

    result = kvmfrClipboardFileMessageType(record.type) ?
      m_impl->HandleHelperFileNL(record, data) :
      m_impl->HandleHelperClipboardNL(record, data);
  }
  m_impl->PumpSends();
  processLock.Unlock();
  m_impl->NotifyTargetReady();
  m_impl->NotifyFailure();
  return result;
}

void CNetworkClipboard::ClipboardReset(uint32_t generation, uint32_t reason)
{
  (void)reason;
  bool available;
  {
    CSRWSharedLock lock(m_impl->m_lock);
    available = m_impl->m_available;
  }
  ClipboardState(available, generation);
}
