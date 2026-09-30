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

#include "clipboard.h"

#include "common/debug.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <LGProtocol/KVMFRClipboard.h>
#include <LGProtocol/NetworkClipboard.h>
#include <LGProtocol/NetworkServices.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NETWORK_CLIPBOARD_TIMER_MS             20U
#define NETWORK_CLIPBOARD_LEASE_MS             2000U
#define NETWORK_CLIPBOARD_KEEPALIVE_MS         750U
#define NETWORK_CLIPBOARD_CLAIM_RETRY_MS       500U
#define NETWORK_CLIPBOARD_PENDING_MAX          128U
#define NETWORK_CLIPBOARD_PENDING_BYTES        (8U * 1024U * 1024U)
#define NETWORK_CLIPBOARD_INBOUND_MAX          128U
#define NETWORK_CLIPBOARD_INBOUND_BYTES        (8U * 1024U * 1024U)
#define NETWORK_CLIPBOARD_PROCESS_MAX          64U
#define NETWORK_CLIPBOARD_EMITTED_CHUNK        (256U * 1024U)
#define NETWORK_CLIPBOARD_FILE_READ_MAX        (1024U * 1024U)
#define NETWORK_CLIPBOARD_FILE_ACQUISITIONS    8U
#define NETWORK_CLIPBOARD_FILE_REQUESTS        32U
#define NETWORK_CLIPBOARD_FORMATS              \
  LG_NET_CLIPBOARD_KVMFR_FORMAT_COUNT

#define NETWORK_CLIPBOARD_CLAIM_FLAGS \
  (LG_NET_CLIPBOARD_CLAIM_READ | LG_NET_CLIPBOARD_CLAIM_WRITE)

struct NetworkClipboardSend
{
  struct NetworkClipboardSend * next;
  LGNetService                  service;
  uint16_t                      messageType;
  uint16_t                      serviceVersion;
  LGNetEnvelopeFlags            flags;
  NetworkPacketDelivery         delivery;
  uint64_t                      componentEpoch;
  uint64_t                      requestID;
  uint32_t                      payloadLength;
  uint8_t                       payload[];
};

struct NetworkClipboardBatch
{
  struct NetworkClipboardSend * head;
  struct NetworkClipboardSend * tail;
  unsigned                      count;
  size_t                        bytes;
};

struct NetworkClipboardInbound
{
  struct NetworkClipboardInbound * next;
  LGNetEnvelope                    envelope;
  NetworkPacketDelivery            delivery;
  size_t                           payloadLength;
  uint8_t                          payload[];
};

struct NetworkClipboardTransfer
{
  struct NetworkClipboardTransfer * next;
  LG_ClipboardRequest               localRequest;
  uint64_t                          wireRequest;
  uint64_t                          offerID;
  LG_ClipboardData                  type;
  uint32_t                          formatIndex;
  uint32_t                          maxChunkLength;
  uint64_t                          sizeHint;
  uint64_t                          offset;
  NetworkPacketDelivery             delivery;
  bool                              began;
  bool                              blocked;
};

struct NetworkFileTransfer
{
  struct NetworkFileTransfer * next;
  LG_ClipboardFileRequest      request;
  uint64_t                     wireRequest;
  uint64_t                     entryID;
  uint64_t                     sizeHint;
  uint64_t                     responseOffset;
  NetworkPacketDelivery        delivery;
  bool                         began;
  bool                         blocked;
  bool                         wireEnded;
};

struct NetworkFileAcquisition
{
  struct NetworkFileAcquisition * next;
  uint64_t                        dataset;
  uint64_t                        localAcquisition;
  uint64_t                        wireClientID;
  uint64_t                        leaseEpoch;
  bool                            fromRemote;
  bool                            active;
};

struct NetworkRemoteOffer
{
  uint64_t         epoch;
  uint64_t         offerID;
  uint64_t         serial;
  uint32_t         formatCount;
  uint32_t         received;
  uint32_t         formatMask;
  LG_ClipboardData types[NETWORK_CLIPBOARD_FORMATS];
  uint32_t         indices[LG_CLIPBOARD_DATA_NONE];
  bool             complete;
  bool             published;
  bool             fileOffer;
};

struct NetworkLocalOffer
{
  uint64_t         offerID;
  uint64_t         serial;
  uint32_t         count;
  LG_ClipboardData types[NETWORK_CLIPBOARD_FORMATS];
  uint32_t         indices[LG_CLIPBOARD_DATA_NONE];
  bool             files;
};

struct NetworkClipboard
{
  NetworkConnection * connection;
  LGTimer           * timer;
  LG_Lock             stateLock;
  LG_Lock             eventLock;
  LG_Lock             statusLock;

  bool                destroying;
  bool                connected;
  bool                clipboardService;
  bool                fileService;
  bool                clipboardBulk;
  bool                fileBulk;
  bool                available;
  bool                attached;
  bool                processing;
  bool                claimPending;
  bool                claimed;
  bool                inboundBlocked;
  bool                inboundReady;
  bool                sendProgress;
  LG_ClipboardRequest inboundBlockedRequest;
  uint16_t            clipboardVersion;
  uint16_t            fileVersion;
  uint32_t            statusGeneration;
  bool                statusNotifyPending;

  uint64_t ownerID;
  uint64_t claimEpoch;
  uint64_t claimSerial;
  uint64_t lastClaimStatusSerial;
  uint64_t nextClaimNs;
  uint64_t nextKeepaliveNs;
  uint64_t localSerial;
  uint64_t wireSerial;
  uint64_t remoteSerial;

  struct NetworkClipboardSend * sendHead;
  struct NetworkClipboardSend * sendTail;
  unsigned                      sendCount;
  size_t                        sendBytes;

  struct NetworkClipboardInbound * inboundHead;
  struct NetworkClipboardInbound * inboundTail;
  unsigned                         inboundCount;
  size_t                           inboundBytes;

  struct NetworkClipboardTransfer * clipboardReads;
  struct NetworkClipboardTransfer * clipboardWrites;
  struct NetworkFileTransfer      * fileReads;
  struct NetworkFileTransfer      * fileWrites;
  struct NetworkFileAcquisition   * fileAcquisitions;

  struct NetworkRemoteOffer remoteOffer;
  struct NetworkLocalOffer  localOffer;

  const LG_ClipboardEventOps * events;
  void                       * eventOpaque;
  LG_ClipboardStatusFn         statusCallback;
  void                       * statusOpaque;
};

static uint64_t networkClipboardNext64(uint64_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint32_t networkClipboardNext32(uint32_t value)
{
  ++value;
  return value ? value : 1U;
}

static uint64_t networkClipboardDeadline(uint32_t milliseconds)
{
  return nanotime() + (uint64_t)milliseconds * UINT64_C(1000000);
}

static uint64_t networkClipboardNextLocalIDNL(
    NetworkClipboard * clipboard, bool remote)
{
  clipboard->localSerial  = networkClipboardNext64(clipboard->localSerial);
  clipboard->localSerial &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!clipboard->localSerial)
    clipboard->localSerial = 1;

  return remote ? clipboard->localSerial |
    KVMFR_CLIPBOARD_TRANSFER_HELPER : clipboard->localSerial;
}

static uint64_t networkClipboardNextWireIDNL(
    NetworkClipboard * clipboard)
{
  clipboard->wireSerial = networkClipboardNext64(clipboard->wireSerial);
  return clipboard->wireSerial;
}

static LG_ClipboardData networkClipboardDataFromFormat(
    KVMFRClipboardFormat format)
{
  switch (format)
  {
    case KVMFR_CLIPBOARD_FORMAT_TEXT : return LG_CLIPBOARD_DATA_TEXT;
    case KVMFR_CLIPBOARD_FORMAT_PNG  : return LG_CLIPBOARD_DATA_PNG;
    case KVMFR_CLIPBOARD_FORMAT_BMP  : return LG_CLIPBOARD_DATA_BMP;
    case KVMFR_CLIPBOARD_FORMAT_TIFF : return LG_CLIPBOARD_DATA_TIFF;
    case KVMFR_CLIPBOARD_FORMAT_JPEG : return LG_CLIPBOARD_DATA_JPEG;
    case KVMFR_CLIPBOARD_FORMAT_FILES: return LG_CLIPBOARD_DATA_FILES;
    default: return LG_CLIPBOARD_DATA_NONE;
  }
}

static KVMFRClipboardFormat networkClipboardFormatFromData(
    LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT : return KVMFR_CLIPBOARD_FORMAT_TEXT;
    case LG_CLIPBOARD_DATA_PNG  : return KVMFR_CLIPBOARD_FORMAT_PNG;
    case LG_CLIPBOARD_DATA_BMP  : return KVMFR_CLIPBOARD_FORMAT_BMP;
    case LG_CLIPBOARD_DATA_TIFF : return KVMFR_CLIPBOARD_FORMAT_TIFF;
    case LG_CLIPBOARD_DATA_JPEG : return KVMFR_CLIPBOARD_FORMAT_JPEG;
    case LG_CLIPBOARD_DATA_FILES: return KVMFR_CLIPBOARD_FORMAT_FILES;
    default: return KVMFR_CLIPBOARD_FORMAT_NONE;
  }
}

static bool networkClipboardDataValid(LG_ClipboardData type)
{
  return type >= LG_CLIPBOARD_DATA_TEXT &&
    type < LG_CLIPBOARD_DATA_NONE;
}

static bool networkClipboardFileErrorValid(LG_ClipboardFileError error)
{
  return error >= LG_CLIPBOARD_FILE_ERROR_NONE &&
    error <= LG_CLIPBOARD_FILE_ERROR_STALE;
}

static KVMFRClipboardFileError networkClipboardFileErrorToKVMFR(
    LG_ClipboardFileError error)
{
  return networkClipboardFileErrorValid(error) ?
    (KVMFRClipboardFileError)error :
    KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
}

static LG_ClipboardFileError networkClipboardFileErrorFromStatus(
    LGNetFileStatusCode status, uint32_t detail)
{
  return (LG_ClipboardFileError)lgNetFileKVMFRErrorFromStatus(
      status, detail);
}

static LGNetFileStatusCode networkClipboardFileStatus(
    LG_ClipboardFileError error)
{
  return lgNetFileStatusFromKVMFRError(
      networkClipboardFileErrorToKVMFR(error));
}

static LG_ClipboardStatus networkClipboardStatusNL(
    const NetworkClipboard * clipboard)
{
  return (LG_ClipboardStatus)
  {
    .available  = clipboard->available,
    .generation = clipboard->statusGeneration,
  };
}

static void networkClipboardSetAvailableNL(NetworkClipboard * clipboard,
    bool available)
{
  if (clipboard->available == available)
    return;

  clipboard->available           = available;
  clipboard->statusGeneration    =
    networkClipboardNext32(clipboard->statusGeneration);
  clipboard->statusNotifyPending = true;
}

static void networkClipboardNotifyStatus(NetworkClipboard * clipboard)
{
  LG_ClipboardStatusFn callback;
  void                * opaque;
  LG_ClipboardStatus    status;

  LG_LOCK(clipboard->statusLock);
  LG_LOCK(clipboard->stateLock);
  if (!clipboard->statusNotifyPending)
  {
    LG_UNLOCK(clipboard->stateLock);
    LG_UNLOCK(clipboard->statusLock);
    return;
  }

  clipboard->statusNotifyPending = false;
  callback                       = clipboard->statusCallback;
  opaque                         = clipboard->statusOpaque;
  status                         = networkClipboardStatusNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);

  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(clipboard->statusLock);
}

static struct NetworkClipboardSend * networkClipboardSendNew(
    LGNetService service, uint16_t messageType, uint16_t serviceVersion,
    LGNetEnvelopeFlags flags, uint64_t componentEpoch, uint64_t requestID,
    uint32_t payloadLength)
{
  const size_t allocation = sizeof(struct NetworkClipboardSend) +
    (size_t)payloadLength;
  if (payloadLength > LG_NET_MAX_PAYLOAD_LENGTH ||
      allocation < payloadLength)
    return NULL;

  struct NetworkClipboardSend * send =
    calloc(1, allocation);
  if (!send)
    return NULL;

  send->service          = service;
  send->messageType      = messageType;
  send->serviceVersion   = serviceVersion;
  send->flags            = flags | LG_NET_ENVELOPE_FINAL;
  send->delivery         = NETWORK_PACKET_DELIVERY_RELIABLE;
  send->componentEpoch   = componentEpoch;
  send->requestID        = requestID;
  send->payloadLength    = payloadLength;
  return send;
}

static struct NetworkClipboardSend * networkClipboardSendWithDelivery(
    struct NetworkClipboardSend * send, NetworkPacketDelivery delivery)
{
  if (send)
    send->delivery = delivery;
  return send;
}

static void networkClipboardBatchAppend(
    struct NetworkClipboardBatch * batch,
    struct NetworkClipboardSend * send)
{
  if (batch->tail)
    batch->tail->next = send;
  else
    batch->head = send;
  batch->tail = send;
  ++batch->count;
  batch->bytes += sizeof(*send) + send->payloadLength;
}

static void networkClipboardBatchFree(struct NetworkClipboardBatch * batch)
{
  while (batch->head)
  {
    struct NetworkClipboardSend * send = batch->head;
    batch->head = send->next;
    free(send);
  }
  memset(batch, 0, sizeof(*batch));
}

static bool networkClipboardQueueBatchNL(NetworkClipboard * clipboard,
    struct NetworkClipboardBatch * batch)
{
  if (!batch->head || !clipboard->connected ||
      batch->count > NETWORK_CLIPBOARD_PENDING_MAX ||
      batch->bytes > NETWORK_CLIPBOARD_PENDING_BYTES ||
      clipboard->sendCount > NETWORK_CLIPBOARD_PENDING_MAX - batch->count ||
      clipboard->sendBytes > NETWORK_CLIPBOARD_PENDING_BYTES - batch->bytes)
    return false;

  if (batch->head->service == LG_NET_SERVICE_CLIPBOARD &&
      !clipboard->clipboardService)
    return false;
  if (batch->head->service == LG_NET_SERVICE_FILE &&
      !clipboard->fileService)
    return false;

  if (clipboard->sendTail)
    clipboard->sendTail->next = batch->head;
  else
    clipboard->sendHead = batch->head;
  clipboard->sendTail   = batch->tail;
  clipboard->sendCount += batch->count;
  clipboard->sendBytes += batch->bytes;
  memset(batch, 0, sizeof(*batch));
  return true;
}

static void networkClipboardClearSendNL(NetworkClipboard * clipboard)
{
  while (clipboard->sendHead)
  {
    struct NetworkClipboardSend * send = clipboard->sendHead;
    clipboard->sendHead = send->next;
    free(send);
  }

  clipboard->sendTail     = NULL;
  clipboard->sendCount    = 0;
  clipboard->sendBytes    = 0;
  clipboard->sendProgress = false;
}

static bool networkClipboardFlushNL(NetworkClipboard * clipboard)
{
  bool    progress = false;
  uint8_t blocked  = 0;
  while (clipboard->sendHead && clipboard->connected &&
      blocked != ((UINT8_C(1) << NETWORK_PACKET_DELIVERY_RELIABLE) |
        (UINT8_C(1) << NETWORK_PACKET_DELIVERY_BULK)))
  {
    struct NetworkClipboardSend ** link = &clipboard->sendHead;
    while (*link && (blocked & (UINT8_C(1) << (*link)->delivery)))
      link = &(*link)->next;
    if (!*link)
      break;

    struct NetworkClipboardSend * send = *link;
    LG_TransportStatus            status;
    switch (send->delivery)
    {
      case NETWORK_PACKET_DELIVERY_RELIABLE:
        status = networkConnectionSend(
            clipboard->connection, send->service, send->messageType,
            send->serviceVersion, send->flags, send->componentEpoch,
            send->requestID, send->payload, send->payloadLength);
        break;

      case NETWORK_PACKET_DELIVERY_BULK:
        status = networkConnectionSendBulk(
            clipboard->connection, send->service, send->messageType,
            send->serviceVersion, send->flags, send->componentEpoch,
            send->requestID, send->payload, send->payloadLength);
        break;

      case NETWORK_PACKET_DELIVERY_DATAGRAM:
      default:
        return progress;
    }
    if (status == LG_TRANSPORT_UNAVAILABLE)
    {
      blocked |= UINT8_C(1) << send->delivery;
      continue;
    }
    if (status != LG_TRANSPORT_OK)
      break;

    *link = send->next;
    if (clipboard->sendTail == send)
    {
      clipboard->sendTail = NULL;
      for (struct NetworkClipboardSend * item = clipboard->sendHead;
          item; item = item->next)
        clipboard->sendTail = item;
    }
    --clipboard->sendCount;
    clipboard->sendBytes -= sizeof(*send) + send->payloadLength;
    free(send);
    progress = true;
  }
  return progress;
}

static void networkClipboardFlushQueued(NetworkClipboard * clipboard)
{
  LG_LOCK(clipboard->stateLock);
  if (!clipboard->destroying)
    clipboard->sendProgress |= networkClipboardFlushNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);
}

static void networkClipboardClearInboundNL(NetworkClipboard * clipboard)
{
  while (clipboard->inboundHead)
  {
    struct NetworkClipboardInbound * packet = clipboard->inboundHead;
    clipboard->inboundHead = packet->next;
    free(packet);
  }

  clipboard->inboundTail           = NULL;
  clipboard->inboundCount          = 0;
  clipboard->inboundBytes          = 0;
  clipboard->inboundBlocked        = false;
  clipboard->inboundReady          = false;
  clipboard->inboundBlockedRequest = LG_CLIPBOARD_REQUEST_INVALID;
}

static struct NetworkClipboardTransfer * networkClipboardTransferFind(
    struct NetworkClipboardTransfer * transfers, uint64_t localRequest)
{
  for (; transfers; transfers = transfers->next)
    if (transfers->localRequest == localRequest)
      return transfers;
  return NULL;
}

static struct NetworkClipboardTransfer * networkClipboardTransferFindWire(
    struct NetworkClipboardTransfer * transfers, uint64_t wireRequest)
{
  for (; transfers; transfers = transfers->next)
    if (transfers->wireRequest == wireRequest)
      return transfers;
  return NULL;
}

static struct NetworkClipboardTransfer * networkClipboardTransferTake(
    struct NetworkClipboardTransfer ** transfers, uint64_t localRequest)
{
  while (*transfers && (*transfers)->localRequest != localRequest)
    transfers = &(*transfers)->next;
  if (!*transfers)
    return NULL;

  struct NetworkClipboardTransfer * result = *transfers;
  *transfers   = result->next;
  result->next = NULL;
  return result;
}

static struct NetworkClipboardTransfer * networkClipboardTransferTakeWire(
    struct NetworkClipboardTransfer ** transfers, uint64_t wireRequest)
{
  while (*transfers && (*transfers)->wireRequest != wireRequest)
    transfers = &(*transfers)->next;
  if (!*transfers)
    return NULL;

  struct NetworkClipboardTransfer * result = *transfers;
  *transfers   = result->next;
  result->next = NULL;
  return result;
}

static struct NetworkFileTransfer * networkClipboardFileTransferFind(
    struct NetworkFileTransfer * transfers, uint64_t localRequest)
{
  for (; transfers; transfers = transfers->next)
    if (transfers->request.request == localRequest)
      return transfers;
  return NULL;
}

static struct NetworkFileTransfer * networkClipboardFileTransferFindWire(
    struct NetworkFileTransfer * transfers, uint64_t wireRequest)
{
  for (; transfers; transfers = transfers->next)
    if (transfers->wireRequest == wireRequest)
      return transfers;
  return NULL;
}

static struct NetworkFileTransfer * networkClipboardFileTransferTake(
    struct NetworkFileTransfer ** transfers, uint64_t localRequest)
{
  while (*transfers && (*transfers)->request.request != localRequest)
    transfers = &(*transfers)->next;
  if (!*transfers)
    return NULL;

  struct NetworkFileTransfer * result = *transfers;
  *transfers   = result->next;
  result->next = NULL;
  return result;
}

static struct NetworkFileTransfer * networkClipboardFileTransferTakeWire(
    struct NetworkFileTransfer ** transfers, uint64_t wireRequest)
{
  while (*transfers && (*transfers)->wireRequest != wireRequest)
    transfers = &(*transfers)->next;
  if (!*transfers)
    return NULL;

  struct NetworkFileTransfer * result = *transfers;
  *transfers   = result->next;
  result->next = NULL;
  return result;
}

static struct NetworkFileAcquisition * networkClipboardAcquisitionFind(
    NetworkClipboard * clipboard, uint64_t dataset,
    uint64_t localAcquisition, bool fromRemote)
{
  for (struct NetworkFileAcquisition * item = clipboard->fileAcquisitions;
      item; item = item->next)
    if (item->dataset == dataset &&
        item->localAcquisition == localAcquisition &&
        item->fromRemote == fromRemote)
      return item;
  return NULL;
}

static struct NetworkFileAcquisition *
networkClipboardAcquisitionFindWire(NetworkClipboard * clipboard,
    uint64_t dataset, uint64_t wireClientID, bool fromRemote)
{
  for (struct NetworkFileAcquisition * item = clipboard->fileAcquisitions;
      item; item = item->next)
    if (item->dataset == dataset && item->wireClientID == wireClientID &&
        item->fromRemote == fromRemote)
      return item;
  return NULL;
}

static struct NetworkFileAcquisition * networkClipboardAcquisitionTake(
    NetworkClipboard * clipboard, uint64_t dataset,
    uint64_t localAcquisition, bool fromRemote)
{
  struct NetworkFileAcquisition ** item = &clipboard->fileAcquisitions;
  while (*item && ((*item)->dataset != dataset ||
      (*item)->localAcquisition != localAcquisition ||
      (*item)->fromRemote != fromRemote))
    item = &(*item)->next;
  if (!*item)
    return NULL;

  struct NetworkFileAcquisition * result = *item;
  *item        = result->next;
  result->next = NULL;
  return result;
}

static unsigned networkClipboardAcquisitionCountNL(
    const NetworkClipboard * clipboard)
{
  unsigned count = 0;
  for (const struct NetworkFileAcquisition * item =
      clipboard->fileAcquisitions; item; item = item->next)
    ++count;
  return count;
}

static unsigned networkClipboardFileTransferCountNL(
    const NetworkClipboard * clipboard)
{
  unsigned count = 0;
  for (const struct NetworkFileTransfer * item = clipboard->fileReads;
      item; item = item->next)
    ++count;
  for (const struct NetworkFileTransfer * item = clipboard->fileWrites;
      item; item = item->next)
    ++count;
  return count;
}

static unsigned networkClipboardTransferCount(
    const struct NetworkClipboardTransfer * transfers)
{
  unsigned count = 0;
  for (; transfers; transfers = transfers->next)
    ++count;
  return count;
}

static bool networkClipboardDatasetAcquiredNL(
    const NetworkClipboard * clipboard, uint64_t dataset,
    bool fromRemote)
{
  for (const struct NetworkFileAcquisition * item =
      clipboard->fileAcquisitions; item; item = item->next)
    if (item->dataset == dataset && item->fromRemote == fromRemote &&
        item->active)
      return true;
  return false;
}

static void networkClipboardTransfersFree(
    struct NetworkClipboardTransfer * transfers)
{
  while (transfers)
  {
    struct NetworkClipboardTransfer * next = transfers->next;
    free(transfers);
    transfers = next;
  }
}

static void networkClipboardFileTransfersFree(
    struct NetworkFileTransfer * transfers)
{
  while (transfers)
  {
    struct NetworkFileTransfer * next = transfers->next;
    free(transfers);
    transfers = next;
  }
}

static void networkClipboardAcquisitionsFree(
    struct NetworkFileAcquisition * acquisitions)
{
  while (acquisitions)
  {
    struct NetworkFileAcquisition * next = acquisitions->next;
    free(acquisitions);
    acquisitions = next;
  }
}

static bool networkClipboardSameFileRequest(
    const LG_ClipboardFileRequest * left,
    const LG_ClipboardFileRequest * right)
{
  return left->dataset == right->dataset &&
    left->request == right->request && left->node == right->node &&
    left->offset == right->offset && left->length == right->length &&
    left->operation == right->operation;
}

static struct NetworkClipboardSend * networkClipboardControlSendNew(
    NetworkClipboard * clipboard, uint16_t messageType,
    const LGNetClipboardControl * control, LGNetEnvelopeFlags flags,
    uint64_t requestID)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, messageType, clipboard->clipboardVersion,
      flags, clipboard->claimEpoch, requestID,
      LG_NET_CLIPBOARD_CONTROL_WIRE_SIZE);
  if (!send || !lgNetClipboardControlEncode(
        send->payload, send->payloadLength, control))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardTransferSendNew(
    NetworkClipboard * clipboard, uint16_t messageType,
    const LGNetClipboardTransfer * transfer, LGNetEnvelopeFlags flags)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, messageType, clipboard->clipboardVersion,
      flags, clipboard->claimEpoch, transfer->requestID,
      LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE);
  if (!send || !lgNetClipboardTransferEncode(
        send->payload, send->payloadLength, transfer))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardChunkSendNew(
    NetworkClipboard * clipboard, const LGNetClipboardChunk * chunk)
{
  const size_t payloadLength = lgNetClipboardChunkSize(chunk);
  if (!payloadLength || payloadLength > UINT32_MAX)
    return NULL;

  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK,
      clipboard->clipboardVersion, 0, clipboard->claimEpoch,
      chunk->requestID, (uint32_t)payloadLength);
  if (!send || !lgNetClipboardChunkEncode(
        send->payload, send->payloadLength, chunk))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardStatusSendNew(
    NetworkClipboard * clipboard, const LGNetClipboardStatus * status,
    LGNetEnvelopeFlags flags)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CLIPBOARD_MESSAGE_STATUS,
      clipboard->clipboardVersion, flags, clipboard->claimEpoch,
      status->requestID, LG_NET_CLIPBOARD_STATUS_WIRE_SIZE);
  if (!send || !lgNetClipboardStatusEncode(
        send->payload, send->payloadLength, status))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileLeaseSendNew(
    NetworkClipboard * clipboard, uint16_t messageType,
    const LGNetFileLease * lease, LGNetEnvelopeFlags flags)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, messageType, clipboard->fileVersion, flags,
      clipboard->claimEpoch, lease->clientID, LG_NET_FILE_LEASE_WIRE_SIZE);
  if (!send || !lgNetFileLeaseEncode(
        send->payload, send->payloadLength, lease))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileTransferSendNew(
    NetworkClipboard * clipboard, uint16_t messageType,
    const LGNetFileTransfer * transfer, LGNetEnvelopeFlags flags)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, messageType, clipboard->fileVersion, flags,
      clipboard->claimEpoch, transfer->requestID,
      LG_NET_FILE_TRANSFER_WIRE_SIZE);
  if (!send || !lgNetFileTransferEncode(
        send->payload, send->payloadLength, transfer))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileChunkSendNew(
    NetworkClipboard * clipboard, const LGNetFileChunk * chunk)
{
  const size_t payloadLength = lgNetFileChunkSize(chunk);
  if (!payloadLength || payloadLength > UINT32_MAX)
    return NULL;

  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, LG_NET_FILE_MESSAGE_DATA_CHUNK,
      clipboard->fileVersion, 0, clipboard->claimEpoch,
      chunk->requestID, (uint32_t)payloadLength);
  if (!send || !lgNetFileChunkEncode(
        send->payload, send->payloadLength, chunk))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileStatusSendNew(
    NetworkClipboard * clipboard, const LGNetFileStatus * status,
    LGNetEnvelopeFlags flags)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, LG_NET_FILE_MESSAGE_STATUS,
      clipboard->fileVersion, flags, clipboard->claimEpoch,
      status->requestID, LG_NET_FILE_STATUS_WIRE_SIZE);
  if (!send || !lgNetFileStatusEncode(
        send->payload, send->payloadLength, status))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardOfferSendNew(
    NetworkClipboard * clipboard, uint64_t offerID, uint64_t serial,
    uint32_t formatIndex, uint32_t formatCount, LG_ClipboardData type)
{
  const KVMFRClipboardFormat format =
    networkClipboardFormatFromData(type);
  const uint8_t * mime;
  uint16_t        mimeLength;
  if (!lgNetClipboardKVMFRFormatMime(format, &mime, &mimeLength))
    return NULL;

  const LGNetClipboardOffer offer =
  {
    .offerID       = offerID,
    .serial        = serial,
    .estimatedSize = 0,
    .formatIndex   = formatIndex,
    .formatCount   = formatCount,
    .mimeLength    = mimeLength,
    .flags         = LG_NET_CLIPBOARD_OFFER_LAZY,
    .mime          = mime,
  };
  const size_t payloadLength = lgNetClipboardOfferSize(&offer);
  if (!payloadLength || payloadLength > UINT32_MAX)
    return NULL;

  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CLIPBOARD_MESSAGE_OFFER,
      clipboard->clipboardVersion, 0, clipboard->claimEpoch, offerID,
      (uint32_t)payloadLength);
  if (!send || !lgNetClipboardOfferEncode(
        send->payload, send->payloadLength, &offer))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardRequestSendNew(
    NetworkClipboard * clipboard, const LGNetClipboardRequest * request)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CLIPBOARD_MESSAGE_REQUEST,
      clipboard->clipboardVersion, LG_NET_ENVELOPE_ACK_REQUIRED,
      clipboard->claimEpoch, request->requestID,
      LG_NET_CLIPBOARD_REQUEST_WIRE_SIZE);
  if (!send || !lgNetClipboardRequestEncode(
        send->payload, send->payloadLength, request))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileOfferSendNew(
    NetworkClipboard * clipboard, uint64_t offerID)
{
  const LGNetFileOffer offer =
  {
    .offerID     = offerID,
    .totalBytes  = 0,
    .entryCount  = 1,
    .flags       = LG_NET_FILE_OFFER_COPY |
      LG_NET_FILE_OFFER_RECURSIVE,
    .labelLength = 0,
    .label       = NULL,
  };
  const size_t payloadLength = lgNetFileOfferSize(&offer);
  if (!payloadLength || payloadLength > UINT32_MAX)
    return NULL;

  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, LG_NET_FILE_MESSAGE_OFFER,
      clipboard->fileVersion, 0, clipboard->claimEpoch, offerID,
      (uint32_t)payloadLength);
  if (!send || !lgNetFileOfferEncode(
        send->payload, send->payloadLength, &offer))
  {
    free(send);
    return NULL;
  }
  return send;
}

static struct NetworkClipboardSend * networkClipboardFileRequestSendNew(
    NetworkClipboard * clipboard, const LGNetFileRequest * request)
{
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_FILE, LG_NET_FILE_MESSAGE_REQUEST,
      clipboard->fileVersion, LG_NET_ENVELOPE_ACK_REQUIRED,
      clipboard->claimEpoch, request->requestID,
      LG_NET_FILE_REQUEST_WIRE_SIZE);
  if (!send || !lgNetFileRequestEncode(
        send->payload, send->payloadLength, request))
  {
    free(send);
    return NULL;
  }
  return send;
}

static bool networkClipboardQueueClaimNL(NetworkClipboard * clipboard)
{
  if (!clipboard->connected || !clipboard->clipboardService ||
      clipboard->destroying || !clipboard->attached ||
      clipboard->claimPending || clipboard->claimed)
    return false;

  const uint64_t            claimEpoch = networkClipboardNext64(
      clipboard->claimEpoch);
  const LGNetClipboardClaim claim      =
  {
    .ownerID    = clipboard->ownerID,
    .claimEpoch = claimEpoch,
    .leaseMs    = NETWORK_CLIPBOARD_LEASE_MS,
    .flags      = NETWORK_CLIPBOARD_CLAIM_FLAGS,
  };
  struct NetworkClipboardSend * send = networkClipboardSendNew(
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CLIPBOARD_MESSAGE_CLAIM,
      clipboard->clipboardVersion, LG_NET_ENVELOPE_ACK_REQUIRED,
      claimEpoch, claimEpoch, LG_NET_CLIPBOARD_CLAIM_WIRE_SIZE);
  if (!send || !lgNetClipboardClaimEncode(
        send->payload, send->payloadLength, &claim))
  {
    free(send);
    return false;
  }

  struct NetworkClipboardBatch batch = { 0 };
  networkClipboardBatchAppend(&batch, send);
  if (!networkClipboardQueueBatchNL(clipboard, &batch))
  {
    networkClipboardBatchFree(&batch);
    return false;
  }

  clipboard->claimEpoch            = claimEpoch;
  clipboard->claimPending          = true;
  clipboard->claimed               = false;
  clipboard->claimSerial           = 0;
  clipboard->lastClaimStatusSerial = 0;
  clipboard->nextClaimNs           = networkClipboardDeadline(
      NETWORK_CLIPBOARD_CLAIM_RETRY_MS);
  clipboard->nextKeepaliveNs       = 0;
  return true;
}

static bool networkClipboardQueueControlNL(NetworkClipboard * clipboard,
    uint16_t messageType, uint64_t offerID)
{
  const uint64_t              serial  =
    networkClipboardNext64(clipboard->claimSerial);
  const LGNetClipboardControl control =
  {
    .ownerID    = clipboard->ownerID,
    .claimEpoch = clipboard->claimEpoch,
    .serial     = serial,
    .offerID    = offerID,
  };
  struct NetworkClipboardSend * send = networkClipboardControlSendNew(
      clipboard, messageType, &control, 0, offerID);
  if (!send)
    return false;

  struct NetworkClipboardBatch batch = { 0 };
  networkClipboardBatchAppend(&batch, send);
  if (!networkClipboardQueueBatchNL(clipboard, &batch))
  {
    networkClipboardBatchFree(&batch);
    return false;
  }

  clipboard->claimSerial = serial;
  return true;
}

static bool networkClipboardQueueReleaseNL(NetworkClipboard * clipboard)
{
  if ((!clipboard->claimPending && !clipboard->claimed) ||
      !clipboard->connected || !clipboard->clipboardService)
    return false;

  const bool queued = networkClipboardQueueControlNL(clipboard,
      LG_NET_CLIPBOARD_MESSAGE_RELEASE, 0);
  clipboard->claimPending    = false;
  clipboard->claimed         = false;
  clipboard->nextClaimNs     = 0;
  clipboard->nextKeepaliveNs = 0;
  return queued;
}

static bool networkClipboardQueueLocalOfferNL(
    NetworkClipboard * clipboard)
{
  if (!clipboard->localOffer.offerID || !clipboard->localOffer.count ||
      !clipboard->claimed)
    return false;

  struct NetworkClipboardBatch batch = { 0 };
  if (clipboard->localOffer.files)
  {
    struct NetworkClipboardSend * file = networkClipboardFileOfferSendNew(
        clipboard, clipboard->localOffer.offerID);
    if (!file)
      return false;
    networkClipboardBatchAppend(&batch, file);
  }

  for (uint32_t index = 0; index < clipboard->localOffer.count; ++index)
  {
    struct NetworkClipboardSend * offer = networkClipboardOfferSendNew(
        clipboard, clipboard->localOffer.offerID,
        clipboard->localOffer.serial, index,
        clipboard->localOffer.count, clipboard->localOffer.types[index]);
    if (!offer)
    {
      networkClipboardBatchFree(&batch);
      return false;
    }
    networkClipboardBatchAppend(&batch, offer);
  }

  if (!networkClipboardQueueBatchNL(clipboard, &batch))
  {
    networkClipboardBatchFree(&batch);
    return false;
  }
  return true;
}

static void networkClipboardDispatchNotice(NetworkClipboard * clipboard,
    uint64_t dataset, const LG_ClipboardData types[], size_t count,
    bool files)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events)
  {
    if (files && events->fileOffer)
      events->fileOffer(opaque, dataset);
    if (events->notice)
      events->notice(opaque, types, count);
  }
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchRelease(NetworkClipboard * clipboard)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->release)
    events->release(opaque);
  LG_UNLOCK(clipboard->eventLock);
}

static bool networkClipboardDispatchRequest(NetworkClipboard * clipboard,
    LG_ClipboardRequest request, LG_ClipboardData type)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const bool accepted = events && events->request &&
    events->request(opaque, request, type);
  LG_UNLOCK(clipboard->eventLock);
  return accepted;
}

static LG_ClipboardResult networkClipboardDispatchDataBegin(
    NetworkClipboard * clipboard, LG_ClipboardRequest request,
    LG_ClipboardData type, uint64_t sizeHint)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->dataBegin ?
    events->dataBegin(opaque, request, type, sizeHint) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static LG_ClipboardResult networkClipboardDispatchDataChunk(
    NetworkClipboard * clipboard, LG_ClipboardRequest request,
    uint64_t offset, const void * data, size_t size)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->dataChunk ?
    events->dataChunk(opaque, request, offset, data, size) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static LG_ClipboardResult networkClipboardDispatchDataEnd(
    NetworkClipboard * clipboard, LG_ClipboardRequest request,
    uint64_t finalSize)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->dataEnd ?
    events->dataEnd(opaque, request, finalSize) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static void networkClipboardDispatchDataCancel(
    NetworkClipboard * clipboard, LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->dataCancel)
    events->dataCancel(opaque, request, reason);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchRequestCancel(
    NetworkClipboard * clipboard, LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->requestCancel)
    events->requestCancel(opaque, request, reason);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchDataReady(
    NetworkClipboard * clipboard, LG_ClipboardRequest request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->dataReady)
    events->dataReady(opaque, request);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchFileAcquire(
    NetworkClipboard * clipboard, uint64_t dataset, uint64_t acquisition)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileAcquire)
    events->fileAcquire(opaque, dataset, acquisition);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchFileAcquired(
    NetworkClipboard * clipboard, uint64_t dataset, uint64_t acquisition,
    LG_ClipboardFileError error)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileAcquired)
    events->fileAcquired(opaque, dataset, acquisition, error);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchFileRelease(
    NetworkClipboard * clipboard, uint64_t dataset, uint64_t acquisition)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileRelease)
    events->fileRelease(opaque, dataset, acquisition);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchFileRequest(
    NetworkClipboard * clipboard,
    const LG_ClipboardFileRequest * request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileRequest)
    events->fileRequest(opaque, request);
  LG_UNLOCK(clipboard->eventLock);
}

static LG_ClipboardResult networkClipboardDispatchFileDataBegin(
    NetworkClipboard * clipboard,
    const LG_ClipboardFileRequest * request, uint64_t sizeHint)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->fileDataBegin ?
    events->fileDataBegin(opaque, request, sizeHint) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static LG_ClipboardResult networkClipboardDispatchFileDataChunk(
    NetworkClipboard * clipboard,
    const LG_ClipboardFileRequest * request, uint64_t offset,
    const void * data, size_t size)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->fileDataChunk ?
    events->fileDataChunk(opaque, request, offset, data, size) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static LG_ClipboardResult networkClipboardDispatchFileDataEnd(
    NetworkClipboard * clipboard,
    const LG_ClipboardFileRequest * request, uint64_t finalSize)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = events && events->fileDataEnd ?
    events->fileDataEnd(opaque, request, finalSize) :
    LG_CLIPBOARD_RESULT_FAILED;
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static void networkClipboardDispatchFileReady(
    NetworkClipboard * clipboard, uint64_t request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileDataReady)
    events->fileDataReady(opaque, request);
  LG_UNLOCK(clipboard->eventLock);
}

static void networkClipboardDispatchFileCancel(
    NetworkClipboard * clipboard, uint64_t dataset, uint64_t request,
    LG_ClipboardFileError reason)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events && events->fileCancel)
    events->fileCancel(opaque, dataset, request, reason);
  LG_UNLOCK(clipboard->eventLock);
}

static bool networkClipboardPublishRemoteOffer(
    NetworkClipboard * clipboard)
{
  LG_ClipboardData types[NETWORK_CLIPBOARD_FORMATS];
  uint64_t         dataset;
  uint32_t         count;
  bool             files;

  LG_LOCK(clipboard->stateLock);
  struct NetworkRemoteOffer * offer = &clipboard->remoteOffer;
  if (!offer->complete || offer->published ||
      ((offer->formatMask &
        (UINT32_C(1) << LG_CLIPBOARD_DATA_FILES)) && !offer->fileOffer))
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  count   = offer->formatCount;
  dataset = offer->offerID;
  files   = (offer->formatMask &
    (UINT32_C(1) << LG_CLIPBOARD_DATA_FILES)) != 0;
  memcpy(types, offer->types, count * sizeof(*types));
  offer->published = true;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardDispatchNotice(
      clipboard, dataset, types, count, files);
  return true;
}

static bool networkClipboardQueueSendNL(NetworkClipboard * clipboard,
    struct NetworkClipboardSend * send)
{
  if (!send)
    return false;

  struct NetworkClipboardBatch batch = { 0 };
  networkClipboardBatchAppend(&batch, send);
  if (networkClipboardQueueBatchNL(clipboard, &batch))
    return true;

  networkClipboardBatchFree(&batch);
  return false;
}

static void networkClipboardRemoteOfferResetNL(
    NetworkClipboard * clipboard)
{
  memset(&clipboard->remoteOffer, 0, sizeof(clipboard->remoteOffer));
  for (size_t index = 0; index < LG_CLIPBOARD_DATA_NONE; ++index)
    clipboard->remoteOffer.indices[index] = UINT32_MAX;
}

static bool networkClipboardEnvelopeValid(
    const struct NetworkClipboardInbound * packet, LGNetService service)
{
  const bool               reliable   =
    packet->delivery == NETWORK_PACKET_DELIVERY_RELIABLE;
  const bool               bulk       =
    packet->delivery == NETWORK_PACKET_DELIVERY_BULK;
  const LGNetEnvelopeFlags required   = LG_NET_ENVELOPE_FINAL |
    (reliable ? LG_NET_ENVELOPE_RELIABLE : 0);
  const LGNetEnvelopeFlags prohibited = LG_NET_ENVELOPE_DATAGRAM |
    LG_NET_ENVELOPE_EARLY_DATA |
    (bulk ? LG_NET_ENVELOPE_RELIABLE : 0);
  return packet->envelope.service == service &&
    (reliable || bulk) &&
    (packet->envelope.flags & required) == required &&
    !(packet->envelope.flags & prohibited);
}

static bool networkClipboardEpochCurrentNL(
    const NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  return (clipboard->claimPending || clipboard->claimed) &&
    packet->envelope.componentEpoch == clipboard->claimEpoch;
}

static bool networkClipboardQueueClipboardStatusNL(
    NetworkClipboard * clipboard, uint64_t wireRequest, uint64_t offerID,
    LGNetClipboardStatusCode code, uint32_t detail, uint64_t processed)
{
  const LGNetClipboardStatus status =
  {
    .requestID       = wireRequest,
    .offerID         = offerID,
    .status          = code,
    .detail          = detail,
    .processedLength = processed,
  };
  return networkClipboardQueueSendNL(clipboard,
      networkClipboardStatusSendNew(clipboard, &status,
        LG_NET_ENVELOPE_RESPONSE));
}

static bool networkClipboardQueueFileStatusNL(
    NetworkClipboard * clipboard, uint64_t wireRequest, uint64_t offerID,
    uint64_t entryID, LG_ClipboardFileError error, uint64_t transferred)
{
  const LGNetFileStatus status =
  {
    .requestID         = wireRequest,
    .offerID           = offerID,
    .entryID           = entryID,
    .status            = networkClipboardFileStatus(error),
    .detail            = networkClipboardFileErrorToKVMFR(error),
    .transferredLength = transferred,
  };
  return networkClipboardQueueSendNL(clipboard,
      networkClipboardFileStatusSendNew(clipboard, &status,
        LG_NET_ENVELOPE_RESPONSE));
}

enum NetworkClipboardProcessResult
{
  NETWORK_CLIPBOARD_PROCESS_INVALID,
  NETWORK_CLIPBOARD_PROCESS_CONSUMED,
  NETWORK_CLIPBOARD_PROCESS_BLOCKED,
};

static enum NetworkClipboardProcessResult
networkClipboardProcessClaimResult(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardClaimStatus status;
  if (!(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) ||
      lgNetClipboardClaimStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      status.claimEpoch != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet) ||
      status.claimEpoch != clipboard->claimEpoch)
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  if (clipboard->lastClaimStatusSerial &&
      status.serial <= clipboard->lastClaimStatusSerial)
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  clipboard->lastClaimStatusSerial = status.serial;
  clipboard->claimPending          = false;
  if (status.status == LG_NET_CLIPBOARD_CLAIM_ACCEPTED &&
      (status.flags & NETWORK_CLIPBOARD_CLAIM_FLAGS) ==
        NETWORK_CLIPBOARD_CLAIM_FLAGS)
  {
    clipboard->claimed         = true;
    clipboard->nextClaimNs     = 0;
    clipboard->nextKeepaliveNs = networkClipboardDeadline(
        NETWORK_CLIPBOARD_KEEPALIVE_MS);
    if (clipboard->localOffer.offerID)
      networkClipboardQueueLocalOfferNL(clipboard);
  }
  else
  {
    clipboard->claimed         = false;
    clipboard->nextClaimNs     = networkClipboardDeadline(
        NETWORK_CLIPBOARD_CLAIM_RETRY_MS);
    clipboard->nextKeepaliveNs = 0;
  }
  LG_UNLOCK(clipboard->stateLock);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessOffer(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardOffer offer;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardOfferDecode(&offer,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      offer.formatCount > NETWORK_CLIPBOARD_FORMATS ||
      packet->envelope.requestID != offer.offerID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  const KVMFRClipboardFormat format =
    lgNetClipboardKVMFRFormatFromMime(offer.mime, offer.mimeLength);
  const LG_ClipboardData     type   = networkClipboardDataFromFormat(format);
  if (!networkClipboardDataValid(type))
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkRemoteOffer * remote = &clipboard->remoteOffer;
  if (!remote->formatCount || remote->epoch != packet->envelope.componentEpoch ||
      remote->offerID != offer.offerID || remote->serial != offer.serial)
  {
    const bool fileOffer = remote->fileOffer &&
      remote->epoch == packet->envelope.componentEpoch &&
      remote->offerID == offer.offerID;
    networkClipboardRemoteOfferResetNL(clipboard);
    remote->epoch       = packet->envelope.componentEpoch;
    remote->offerID     = offer.offerID;
    remote->serial      = offer.serial;
    remote->formatCount = offer.formatCount;
    remote->fileOffer   = fileOffer;
  }
  else if (remote->formatCount != offer.formatCount)
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_INVALID;
  }

  const uint32_t indexBit = UINT32_C(1) << offer.formatIndex;
  const uint32_t typeBit  = UINT32_C(1) << type;
  if ((remote->received & indexBit) || (remote->formatMask & typeBit))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_INVALID;
  }

  remote->types[offer.formatIndex] = type;
  remote->indices[type]            = offer.formatIndex;
  remote->received                |= indexBit;
  remote->formatMask              |= typeBit;
  const uint32_t completeMask =
    (UINT32_C(1) << remote->formatCount) - 1U;
  remote->complete = remote->received == completeMask;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardPublishRemoteOffer(clipboard);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessClear(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardControl control;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardControlDecode(&control,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  bool released = false;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  if (clipboard->remoteOffer.offerID &&
      (!control.offerID ||
       control.offerID == clipboard->remoteOffer.offerID))
  {
    released = clipboard->remoteOffer.published;
    networkClipboardRemoteOfferResetNL(clipboard);
  }
  LG_UNLOCK(clipboard->stateLock);

  if (released)
    networkClipboardDispatchRelease(clipboard);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessRequest(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardRequest request;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      !(packet->envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED) ||
      lgNetClipboardRequestDecode(&request,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      request.requestID != packet->envelope.requestID || request.offset)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  struct NetworkClipboardTransfer * transfer = calloc(1, sizeof(*transfer));
  if (!transfer)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet) ||
      request.offerID != clipboard->localOffer.offerID ||
      request.formatIndex >= clipboard->localOffer.count ||
      networkClipboardTransferCount(clipboard->clipboardWrites) >=
        NETWORK_CLIPBOARD_FILE_REQUESTS ||
      networkClipboardTransferFindWire(
        clipboard->clipboardWrites, request.requestID))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  transfer->localRequest   = networkClipboardNextLocalIDNL(clipboard, true);
  transfer->wireRequest    = request.requestID;
  transfer->offerID        = request.offerID;
  transfer->type           = clipboard->localOffer.types[request.formatIndex];
  transfer->formatIndex    = request.formatIndex;
  transfer->maxChunkLength = request.maxChunkLength <
    NETWORK_CLIPBOARD_EMITTED_CHUNK ? request.maxChunkLength :
    NETWORK_CLIPBOARD_EMITTED_CHUNK;
  transfer->next             = clipboard->clipboardWrites;
  clipboard->clipboardWrites = transfer;

  const LG_ClipboardRequest localRequest = transfer->localRequest;
  const LG_ClipboardData    type         = transfer->type;
  LG_UNLOCK(clipboard->stateLock);

  if (networkClipboardDispatchRequest(clipboard, localRequest, type))
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardTransferTake(
      &clipboard->clipboardWrites, localRequest);
  if (transfer)
    networkClipboardQueueClipboardStatusNL(clipboard,
        transfer->wireRequest, transfer->offerID,
        LG_NET_CLIPBOARD_STATUS_UNSUPPORTED, 0, 0);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static void networkClipboardRejectRead(NetworkClipboard * clipboard,
    uint64_t wireRequest, LG_ClipboardCancelReason reason)
{
  LG_ClipboardRequest localRequest = LG_CLIPBOARD_REQUEST_INVALID;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferTakeWire(
        &clipboard->clipboardReads, wireRequest);
  if (transfer)
  {
    localRequest = transfer->localRequest;
    networkClipboardQueueClipboardStatusNL(clipboard,
        transfer->wireRequest, transfer->offerID,
        LG_NET_CLIPBOARD_STATUS_CANCELLED, (uint32_t)reason,
        transfer->offset);
  }
  LG_UNLOCK(clipboard->stateLock);

  free(transfer);
  if (localRequest)
    networkClipboardDispatchDataCancel(
        clipboard, localRequest, reason);
}

static enum NetworkClipboardProcessResult
networkClipboardProcessDataBegin(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID || wire.processedLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardRequest localRequest;
  LG_ClipboardData    type;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFindWire(
        clipboard->clipboardReads, wire.requestID);
  if (!transfer || transfer->offerID != wire.offerID || transfer->began)
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  localRequest = transfer->localRequest;
  type         = transfer->type;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchDataBegin(
      clipboard, localRequest, type, wire.totalLength);
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    LG_LOCK(clipboard->stateLock);
    clipboard->inboundBlockedRequest = localRequest;
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_BLOCKED;
  }
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectRead(
        clipboard, wire.requestID, LG_CLIPBOARD_CANCEL_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardTransferFindWire(
      clipboard->clipboardReads, wire.requestID);
  if (transfer && !transfer->began)
  {
    transfer->began    = true;
    transfer->sizeHint = wire.totalLength;
    transfer->offset   = 0;
    transfer->delivery = packet->delivery;
  }
  LG_UNLOCK(clipboard->stateLock);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessDataChunk(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardChunk chunk;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardChunkDecode(&chunk,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      chunk.requestID != packet->envelope.requestID ||
      chunk.dataLength > NETWORK_CLIPBOARD_EMITTED_CHUNK)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardRequest localRequest;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFindWire(
        clipboard->clipboardReads, chunk.requestID);
  if (!transfer || !transfer->began || transfer->offerID != chunk.offerID ||
      transfer->delivery != packet->delivery ||
      transfer->offset != chunk.offset ||
      (transfer->sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
       transfer->sizeHint != chunk.totalLength))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  localRequest = transfer->localRequest;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchDataChunk(
      clipboard, localRequest, chunk.offset, chunk.data, chunk.dataLength);
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    LG_LOCK(clipboard->stateLock);
    clipboard->inboundBlockedRequest = localRequest;
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_BLOCKED;
  }
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectRead(
        clipboard, chunk.requestID, LG_CLIPBOARD_CANCEL_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardTransferFindWire(
      clipboard->clipboardReads, chunk.requestID);
  if (transfer && transfer->offset == chunk.offset)
    transfer->offset += chunk.dataLength;
  LG_UNLOCK(clipboard->stateLock);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessDataEnd(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID ||
      wire.processedLength != wire.totalLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardRequest localRequest;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFindWire(
        clipboard->clipboardReads, wire.requestID);
  if (!transfer || !transfer->began || transfer->offerID != wire.offerID ||
      transfer->delivery != packet->delivery ||
      transfer->offset != wire.processedLength ||
      (transfer->sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
       transfer->sizeHint != wire.totalLength))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  localRequest = transfer->localRequest;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchDataEnd(
      clipboard, localRequest, wire.totalLength);
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    LG_LOCK(clipboard->stateLock);
    clipboard->inboundBlockedRequest = localRequest;
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_BLOCKED;
  }
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectRead(
        clipboard, wire.requestID, LG_CLIPBOARD_CANCEL_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardTransferTakeWire(
      &clipboard->clipboardReads, wire.requestID);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessDataReadyMessage(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetClipboardTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID || wire.processedLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardRequest localRequest = LG_CLIPBOARD_REQUEST_INVALID;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkClipboardTransfer * transfer =
      networkClipboardTransferFindWire(
          clipboard->clipboardWrites, wire.requestID);
    if (transfer && transfer->offerID == wire.offerID &&
        transfer->blocked)
    {
      transfer->blocked = false;
      localRequest      = transfer->localRequest;
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localRequest)
    networkClipboardDispatchDataReady(clipboard, localRequest);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessCancel(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardTransfer wire;
  if (lgNetClipboardTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardRequest localRead  = LG_CLIPBOARD_REQUEST_INVALID;
  LG_ClipboardRequest localWrite = LG_CLIPBOARD_REQUEST_INVALID;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkClipboardTransfer * read  =
      networkClipboardTransferFindWire(
          clipboard->clipboardReads, wire.requestID);
    struct NetworkClipboardTransfer * write =
      networkClipboardTransferFindWire(
          clipboard->clipboardWrites, wire.requestID);
    if (read && read->offerID == wire.offerID)
    {
      read      = networkClipboardTransferTakeWire(
          &clipboard->clipboardReads, wire.requestID);
      localRead = read->localRequest;
    }
    else
      read = NULL;
    if (write && write->offerID == wire.offerID)
    {
      write      = networkClipboardTransferTakeWire(
          &clipboard->clipboardWrites, wire.requestID);
      localWrite = write->localRequest;
    }
    else
      write = NULL;
    free(read);
    free(write);
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localRead)
    networkClipboardDispatchDataCancel(
        clipboard, localRead, LG_CLIPBOARD_CANCEL_ABORTED);
  if (localWrite)
    networkClipboardDispatchRequestCancel(
        clipboard, localWrite, LG_CLIPBOARD_CANCEL_ABORTED);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessStatus(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetClipboardStatus status;
  if (!(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) ||
      lgNetClipboardStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      status.requestID != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  if (status.status == LG_NET_CLIPBOARD_STATUS_OK)
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;

  const LG_ClipboardCancelReason reason     =
    status.status == LG_NET_CLIPBOARD_STATUS_CANCELLED &&
    status.detail <= LG_CLIPBOARD_CANCEL_INVALID ?
      (LG_ClipboardCancelReason)status.detail :
      LG_CLIPBOARD_CANCEL_UNAVAILABLE;
  LG_ClipboardRequest            localRead  = LG_CLIPBOARD_REQUEST_INVALID;
  LG_ClipboardRequest            localWrite = LG_CLIPBOARD_REQUEST_INVALID;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkClipboardTransfer * read  =
      networkClipboardTransferFindWire(
          clipboard->clipboardReads, status.requestID);
    struct NetworkClipboardTransfer * write =
      networkClipboardTransferFindWire(
          clipboard->clipboardWrites, status.requestID);
    if (read && read->offerID == status.offerID)
    {
      read      = networkClipboardTransferTakeWire(
          &clipboard->clipboardReads, status.requestID);
      localRead = read->localRequest;
    }
    else
      read = NULL;
    if (write && write->offerID == status.offerID)
    {
      write      = networkClipboardTransferTakeWire(
          &clipboard->clipboardWrites, status.requestID);
      localWrite = write->localRequest;
    }
    else
      write = NULL;
    free(read);
    free(write);
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localRead)
    networkClipboardDispatchDataCancel(
        clipboard, localRead, reason);
  if (localWrite)
    networkClipboardDispatchRequestCancel(
        clipboard, localWrite, reason);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessFileOffer(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileOffer offer;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileOfferDecode(&offer,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      packet->envelope.requestID != offer.offerID || offer.totalBytes ||
      offer.entryCount != 1 || offer.labelLength ||
      offer.flags != (LG_NET_FILE_OFFER_COPY |
        LG_NET_FILE_OFFER_RECURSIVE))
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkRemoteOffer * remote = &clipboard->remoteOffer;
  if (remote->offerID &&
      (remote->epoch != packet->envelope.componentEpoch ||
       remote->offerID != offer.offerID))
    networkClipboardRemoteOfferResetNL(clipboard);
  remote->epoch     = packet->envelope.componentEpoch;
  remote->offerID   = offer.offerID;
  remote->fileOffer = true;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardPublishRemoteOffer(clipboard);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileAcquire(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileLease lease;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      !(packet->envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED) ||
      lgNetFileLeaseDecode(&lease,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      lease.clientID != packet->envelope.requestID || lease.leaseEpoch ||
      lease.flags != LG_NET_FILE_LEASE_READ)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  struct NetworkFileAcquisition * acquisition =
    calloc(1, sizeof(*acquisition));
  if (!acquisition)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet) ||
      lease.offerID != clipboard->localOffer.offerID ||
      !clipboard->localOffer.files ||
      networkClipboardAcquisitionCountNL(clipboard) >=
        NETWORK_CLIPBOARD_FILE_ACQUISITIONS ||
      networkClipboardAcquisitionFindWire(
        clipboard, lease.offerID, lease.clientID, true))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(acquisition);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  acquisition->dataset          = lease.offerID;
  acquisition->localAcquisition =
    networkClipboardNextLocalIDNL(clipboard, true);
  acquisition->wireClientID     = lease.clientID;
  acquisition->fromRemote       = true;

  acquisition->next             = clipboard->fileAcquisitions;
  clipboard->fileAcquisitions   = acquisition;

  const uint64_t localAcquisition = acquisition->localAcquisition;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardDispatchFileAcquire(
      clipboard, lease.offerID, localAcquisition);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileAcquired(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileLease lease;
  if (!(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) ||
      lgNetFileLeaseDecode(&lease,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      lease.clientID != packet->envelope.requestID || !lease.leaseEpoch ||
      lease.flags != (LG_NET_FILE_LEASE_READ |
        LG_NET_FILE_LEASE_ACQUIRED))
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  uint64_t localAcquisition = 0;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkFileAcquisition * acquisition =
      networkClipboardAcquisitionFindWire(
          clipboard, lease.offerID, lease.clientID, false);
    if (acquisition && !acquisition->active)
    {
      acquisition->leaseEpoch = lease.leaseEpoch;
      acquisition->active     = true;
      localAcquisition        = acquisition->localAcquisition;
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localAcquisition)
    networkClipboardDispatchFileAcquired(clipboard, lease.offerID,
        localAcquisition, LG_CLIPBOARD_FILE_ERROR_NONE);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileReleaseMessage(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileLease lease;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileLeaseDecode(&lease,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      lease.clientID != packet->envelope.requestID || !lease.leaseEpoch ||
      lease.leaseMs || lease.flags != LG_NET_FILE_LEASE_READ)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  uint64_t localAcquisition = 0;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkFileAcquisition * acquisition =
      networkClipboardAcquisitionFindWire(
          clipboard, lease.offerID, lease.clientID, true);
    if (acquisition && acquisition->active &&
        acquisition->leaseEpoch == lease.leaseEpoch)
    {
      localAcquisition = acquisition->localAcquisition;
      acquisition      = networkClipboardAcquisitionTake(clipboard,
          lease.offerID, localAcquisition, true);
      free(acquisition);
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localAcquisition)
    networkClipboardDispatchFileRelease(
        clipboard, lease.offerID, localAcquisition);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileRequestMessage(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileRequest wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      !(packet->envelope.flags & LG_NET_ENVELOPE_ACK_REQUIRED) ||
      lgNetFileRequestDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  const KVMFRClipboardFileOperation operation =
    lgNetFileKVMFROperationFromRequestFlags(wire.flags);
  uint64_t                          node;
  if (!operation ||
      !lgNetFileKVMFRNodeFromEntryID(wire.entryID, &node) ||
      (operation == KVMFR_CLIPBOARD_FILE_OP_READ &&
       wire.length > NETWORK_CLIPBOARD_FILE_READ_MAX))
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  struct NetworkFileTransfer * transfer = calloc(1, sizeof(*transfer));
  if (!transfer)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet) ||
      wire.offerID != clipboard->localOffer.offerID ||
      !clipboard->localOffer.files ||
      !networkClipboardDatasetAcquiredNL(
        clipboard, wire.offerID, true) ||
      networkClipboardFileTransferCountNL(clipboard) >=
        NETWORK_CLIPBOARD_FILE_REQUESTS ||
      networkClipboardFileTransferFindWire(
        clipboard->fileWrites, wire.requestID))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  transfer->request       = (LG_ClipboardFileRequest)
  {
    .dataset   = wire.offerID,
    .request   = networkClipboardNextLocalIDNL(clipboard, true),
    .node      = node,
    .offset    = wire.offset,
    .length    = wire.length,
    .operation = operation == KVMFR_CLIPBOARD_FILE_OP_READ ?
      LG_CLIPBOARD_FILE_READ : LG_CLIPBOARD_FILE_LIST,
  };
  transfer->wireRequest   = wire.requestID;
  transfer->entryID       = wire.entryID;
  transfer->next          = clipboard->fileWrites;
  clipboard->fileWrites   = transfer;

  const LG_ClipboardFileRequest request = transfer->request;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardDispatchFileRequest(clipboard, &request);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static void networkClipboardRejectFileRead(NetworkClipboard * clipboard,
    uint64_t wireRequest, LG_ClipboardFileError reason)
{
  LG_ClipboardFileRequest request = { 0 };

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferTakeWire(
        &clipboard->fileReads, wireRequest);
  if (transfer)
  {
    request = transfer->request;
    networkClipboardQueueFileStatusNL(clipboard,
        transfer->wireRequest, transfer->request.dataset,
        transfer->entryID, reason, transfer->responseOffset);
  }
  LG_UNLOCK(clipboard->stateLock);

  free(transfer);
  if (request.request)
    networkClipboardDispatchFileCancel(
        clipboard, request.dataset, request.request, reason);
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileDataBegin(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID || wire.processedLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardFileRequest request;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFindWire(
        clipboard->fileReads, wire.requestID);
  if (!transfer || transfer->began ||
      transfer->request.dataset != wire.offerID ||
      transfer->entryID != wire.entryID ||
      transfer->request.offset != wire.offset ||
      (transfer->request.operation == LG_CLIPBOARD_FILE_READ &&
       (wire.totalLength - wire.offset > NETWORK_CLIPBOARD_FILE_READ_MAX ||
        wire.totalLength - wire.offset > transfer->request.length)))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  request = transfer->request;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchFileDataBegin(
      clipboard, &request, wire.totalLength - wire.offset);
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    networkClipboardRejectFileRead(clipboard, wire.requestID,
        LG_CLIPBOARD_FILE_ERROR_IO);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectFileRead(clipboard, wire.requestID,
        LG_CLIPBOARD_FILE_ERROR_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardFileTransferFindWire(
      clipboard->fileReads, wire.requestID);
  if (transfer && !transfer->began)
  {
    transfer->began          = true;
    transfer->sizeHint       = wire.totalLength - wire.offset;
    transfer->responseOffset = 0;
    transfer->delivery       = packet->delivery;
  }
  LG_UNLOCK(clipboard->stateLock);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileDataChunk(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileChunk chunk;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileChunkDecode(&chunk,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      chunk.requestID != packet->envelope.requestID ||
      chunk.dataLength > NETWORK_CLIPBOARD_EMITTED_CHUNK ||
      chunk.offset > chunk.totalLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardFileRequest request;
  uint64_t                responseOffset;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFindWire(
        clipboard->fileReads, chunk.requestID);
  if (!transfer || !transfer->began ||
      transfer->delivery != packet->delivery ||
      transfer->request.dataset != chunk.offerID ||
      transfer->entryID != chunk.entryID ||
      transfer->responseOffset != chunk.offset ||
      transfer->sizeHint != chunk.totalLength)
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  request        = transfer->request;
  responseOffset = transfer->responseOffset;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchFileDataChunk(
      clipboard, &request, responseOffset, chunk.data, chunk.dataLength);
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectFileRead(clipboard, chunk.requestID,
        result == LG_CLIPBOARD_RESULT_BLOCKED ?
          LG_CLIPBOARD_FILE_ERROR_IO : LG_CLIPBOARD_FILE_ERROR_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardFileTransferFindWire(
      clipboard->fileReads, chunk.requestID);
  if (transfer && transfer->responseOffset == responseOffset)
    transfer->responseOffset += chunk.dataLength;
  LG_UNLOCK(clipboard->stateLock);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileDataEnd(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID ||
      wire.processedLength != wire.totalLength - wire.offset)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardFileRequest request;
  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardEpochCurrentNL(clipboard, packet))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFindWire(
        clipboard->fileReads, wire.requestID);
  if (!transfer || !transfer->began ||
      transfer->delivery != packet->delivery ||
      transfer->request.dataset != wire.offerID ||
      transfer->entryID != wire.entryID ||
      transfer->request.offset != wire.offset ||
      transfer->responseOffset != wire.processedLength ||
      transfer->request.offset > UINT64_MAX - transfer->sizeHint ||
      transfer->request.offset + transfer->sizeHint != wire.totalLength ||
      (transfer->request.operation == LG_CLIPBOARD_FILE_READ &&
       transfer->sizeHint > NETWORK_CLIPBOARD_FILE_READ_MAX))
  {
    LG_UNLOCK(clipboard->stateLock);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }
  request = transfer->request;
  LG_UNLOCK(clipboard->stateLock);

  const LG_ClipboardResult result = networkClipboardDispatchFileDataEnd(
      clipboard, &request, wire.processedLength);
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    networkClipboardRejectFileRead(clipboard, wire.requestID,
        result == LG_CLIPBOARD_RESULT_BLOCKED ?
          LG_CLIPBOARD_FILE_ERROR_IO : LG_CLIPBOARD_FILE_ERROR_INVALID);
    return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
  }

  LG_LOCK(clipboard->stateLock);
  transfer = networkClipboardFileTransferTakeWire(
      &clipboard->fileReads, wire.requestID);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult
networkClipboardProcessFileReady(NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileTransfer wire;
  if (packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE ||
      lgNetFileTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID || wire.processedLength)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  uint64_t localRequest = 0;
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkFileTransfer * transfer =
      networkClipboardFileTransferFindWire(
          clipboard->fileWrites, wire.requestID);
    if (transfer && transfer->request.dataset == wire.offerID &&
        transfer->entryID == wire.entryID && transfer->blocked)
    {
      transfer->blocked = false;
      localRequest      = transfer->request.request;
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  if (localRequest)
    networkClipboardDispatchFileReady(clipboard, localRequest);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessFileCancel(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileTransfer wire;
  if (lgNetFileTransferDecode(&wire,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      wire.requestID != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  LG_ClipboardFileRequest readRequest  = { 0 };
  LG_ClipboardFileRequest writeRequest = { 0 };
  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    struct NetworkFileTransfer * read  =
      networkClipboardFileTransferFindWire(
          clipboard->fileReads, wire.requestID);
    struct NetworkFileTransfer * write =
      networkClipboardFileTransferFindWire(
          clipboard->fileWrites, wire.requestID);
    if (read && read->request.dataset == wire.offerID &&
        read->entryID == wire.entryID)
    {
      read        = networkClipboardFileTransferTakeWire(
          &clipboard->fileReads, wire.requestID);
      readRequest = read->request;
    }
    else
      read = NULL;
    if (write && write->request.dataset == wire.offerID &&
        write->entryID == wire.entryID)
    {
      write        = networkClipboardFileTransferTakeWire(
          &clipboard->fileWrites, wire.requestID);
      writeRequest = write->request;
    }
    else
      write = NULL;
    free(read);
    free(write);
  }
  LG_UNLOCK(clipboard->stateLock);

  if (readRequest.request)
    networkClipboardDispatchFileCancel(clipboard,
        readRequest.dataset, readRequest.request,
        LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  if (writeRequest.request)
    networkClipboardDispatchFileCancel(clipboard,
        writeRequest.dataset, writeRequest.request,
        LG_CLIPBOARD_FILE_ERROR_CANCELLED);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessFileStatus(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  LGNetFileStatus status;
  if (!(packet->envelope.flags & LG_NET_ENVELOPE_RESPONSE) ||
      lgNetFileStatusDecode(&status,
        packet->payload, packet->payloadLength) != LG_NET_PARSE_OK ||
      status.requestID != packet->envelope.requestID)
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  const LG_ClipboardFileError error        =
    networkClipboardFileErrorFromStatus(status.status, status.detail);
  uint64_t                    acquisition  = 0;
  LG_ClipboardFileRequest     readRequest  = { 0 };
  LG_ClipboardFileRequest     writeRequest = { 0 };

  LG_LOCK(clipboard->stateLock);
  if (networkClipboardEpochCurrentNL(clipboard, packet))
  {
    if (!status.entryID && status.status != LG_NET_FILE_STATUS_OK)
    {
      struct NetworkFileAcquisition * item =
        networkClipboardAcquisitionFindWire(
            clipboard, status.offerID, status.requestID, false);
      if (item)
      {
        acquisition = item->localAcquisition;
        item        = networkClipboardAcquisitionTake(clipboard,
            status.offerID, acquisition, false);
        free(item);
      }
    }
    else if (status.status != LG_NET_FILE_STATUS_OK)
    {
      struct NetworkFileTransfer * read  =
        networkClipboardFileTransferFindWire(
            clipboard->fileReads, status.requestID);
      struct NetworkFileTransfer * write =
        networkClipboardFileTransferFindWire(
            clipboard->fileWrites, status.requestID);
      if (read && read->request.dataset == status.offerID &&
          read->entryID == status.entryID)
      {
        read        = networkClipboardFileTransferTakeWire(
            &clipboard->fileReads, status.requestID);
        readRequest = read->request;
      }
      else
        read = NULL;
      if (write && write->request.dataset == status.offerID &&
          write->entryID == status.entryID)
      {
        write        = networkClipboardFileTransferTakeWire(
            &clipboard->fileWrites, status.requestID);
        writeRequest = write->request;
      }
      else
        write = NULL;
      free(read);
      free(write);
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  if (acquisition)
    networkClipboardDispatchFileAcquired(
        clipboard, status.offerID, acquisition, error);
  if (readRequest.request)
    networkClipboardDispatchFileCancel(
        clipboard, readRequest.dataset, readRequest.request, error);
  if (writeRequest.request)
    networkClipboardDispatchFileCancel(
        clipboard, writeRequest.dataset, writeRequest.request, error);
  return NETWORK_CLIPBOARD_PROCESS_CONSUMED;
}

static enum NetworkClipboardProcessResult networkClipboardProcessPacket(
    NetworkClipboard * clipboard,
    const struct NetworkClipboardInbound * packet)
{
  if (packet->envelope.service == LG_NET_SERVICE_CLIPBOARD)
  {
    if (!networkClipboardEnvelopeValid(packet, LG_NET_SERVICE_CLIPBOARD))
      return NETWORK_CLIPBOARD_PROCESS_INVALID;

    switch (packet->envelope.messageType)
    {
      case LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT:
        return networkClipboardProcessClaimResult(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_KEEPALIVE:
      {
        LGNetClipboardControl control;
        return lgNetClipboardControlDecode(&control,
            packet->payload, packet->payloadLength) == LG_NET_PARSE_OK ?
          NETWORK_CLIPBOARD_PROCESS_CONSUMED :
          NETWORK_CLIPBOARD_PROCESS_INVALID;
      }

      case LG_NET_CLIPBOARD_MESSAGE_RELEASE:
      case LG_NET_CLIPBOARD_MESSAGE_CLEAR:
        return networkClipboardProcessClear(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_OFFER:
        return networkClipboardProcessOffer(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_REQUEST:
        return networkClipboardProcessRequest(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN:
        return networkClipboardProcessDataBegin(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_DATA_CHUNK:
        return networkClipboardProcessDataChunk(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_DATA_END:
        return networkClipboardProcessDataEnd(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_DATA_READY:
        return networkClipboardProcessDataReadyMessage(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_CANCEL:
        return networkClipboardProcessCancel(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_STATUS:
        return networkClipboardProcessStatus(clipboard, packet);

      case LG_NET_CLIPBOARD_MESSAGE_CLAIM:
      default:
        return NETWORK_CLIPBOARD_PROCESS_INVALID;
    }
  }

  if (packet->envelope.service != LG_NET_SERVICE_FILE ||
      !networkClipboardEnvelopeValid(packet, LG_NET_SERVICE_FILE))
    return NETWORK_CLIPBOARD_PROCESS_INVALID;

  switch (packet->envelope.messageType)
  {
    case LG_NET_FILE_MESSAGE_OFFER:
      return networkClipboardProcessFileOffer(clipboard, packet);

    case LG_NET_FILE_MESSAGE_ACQUIRE:
      return networkClipboardProcessFileAcquire(clipboard, packet);

    case LG_NET_FILE_MESSAGE_ACQUIRED:
      return networkClipboardProcessFileAcquired(clipboard, packet);

    case LG_NET_FILE_MESSAGE_RELEASE:
      return networkClipboardProcessFileReleaseMessage(clipboard, packet);

    case LG_NET_FILE_MESSAGE_REQUEST:
      return networkClipboardProcessFileRequestMessage(clipboard, packet);

    case LG_NET_FILE_MESSAGE_DATA_BEGIN:
      return networkClipboardProcessFileDataBegin(clipboard, packet);

    case LG_NET_FILE_MESSAGE_DATA_CHUNK:
      return networkClipboardProcessFileDataChunk(clipboard, packet);

    case LG_NET_FILE_MESSAGE_DATA_END:
      return networkClipboardProcessFileDataEnd(clipboard, packet);

    case LG_NET_FILE_MESSAGE_DATA_READY:
      return networkClipboardProcessFileReady(clipboard, packet);

    case LG_NET_FILE_MESSAGE_CANCEL:
      return networkClipboardProcessFileCancel(clipboard, packet);

    case LG_NET_FILE_MESSAGE_STATUS:
      return networkClipboardProcessFileStatus(clipboard, packet);

    case LG_NET_FILE_MESSAGE_ENTRY:
    default:
      return NETWORK_CLIPBOARD_PROCESS_INVALID;
  }
}

static void networkClipboardProcessInbound(NetworkClipboard * clipboard)
{
  for (unsigned processed = 0;
      processed < NETWORK_CLIPBOARD_PROCESS_MAX; ++processed)
  {
    LG_LOCK(clipboard->stateLock);
    struct NetworkClipboardInbound * packet = clipboard->inboundHead;
    if (!packet || (clipboard->inboundBlocked &&
          !clipboard->inboundReady))
    {
      LG_UNLOCK(clipboard->stateLock);
      return;
    }
    clipboard->inboundReady = false;
    LG_UNLOCK(clipboard->stateLock);

    const enum NetworkClipboardProcessResult result =
      networkClipboardProcessPacket(clipboard, packet);
    if (result == NETWORK_CLIPBOARD_PROCESS_BLOCKED)
    {
      LG_LOCK(clipboard->stateLock);
      if (clipboard->inboundHead == packet)
        clipboard->inboundBlocked = true;
      LG_UNLOCK(clipboard->stateLock);
      return;
    }

    LG_LOCK(clipboard->stateLock);
    if (clipboard->inboundHead == packet)
    {
      clipboard->inboundHead = packet->next;
      if (!clipboard->inboundHead)
        clipboard->inboundTail = NULL;
      --clipboard->inboundCount;
      clipboard->inboundBytes          -=
        sizeof(*packet) + packet->payloadLength;
      clipboard->inboundBlocked        = false;
      clipboard->inboundBlockedRequest = LG_CLIPBOARD_REQUEST_INVALID;
      free(packet);
    }
    LG_UNLOCK(clipboard->stateLock);

    if (result == NETWORK_CLIPBOARD_PROCESS_INVALID)
    {
      DEBUG_ERROR("Invalid network clipboard service message");
      networkConnectionCancel(clipboard->connection);
      return;
    }
  }
}

static void networkClipboardSetStatusListener(void * opaque,
    LG_ClipboardStatusFn callback, void * callbackOpaque)
{
  NetworkClipboard * clipboard = opaque;

  LG_LOCK(clipboard->statusLock);
  LG_LOCK(clipboard->stateLock);
  clipboard->statusCallback      = callback;
  clipboard->statusOpaque        = callbackOpaque;
  clipboard->statusNotifyPending = false;

  const LG_ClipboardStatus status = networkClipboardStatusNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);

  if (callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(clipboard->statusLock);
}

static bool networkClipboardAttach(void * opaque,
    const LG_ClipboardEventOps * events, void * eventOpaque)
{
  NetworkClipboard * clipboard = opaque;
  if (!events)
    return false;

  LG_ClipboardData replay[NETWORK_CLIPBOARD_FORMATS];
  uint64_t         dataset = 0;
  uint32_t         count   = 0;
  bool             files   = false;

  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  bool admitted = !clipboard->destroying && clipboard->connected &&
    clipboard->clipboardService && !clipboard->attached;
  if (admitted)
  {
    clipboard->events      = events;
    clipboard->eventOpaque = eventOpaque;
    clipboard->attached    = true;
    if (!clipboard->claimPending && !clipboard->claimed &&
        !networkClipboardQueueClaimNL(clipboard))
    {
      clipboard->events      = NULL;
      clipboard->eventOpaque = NULL;
      clipboard->attached    = false;
      admitted               = false;
    }
  }

  if (admitted && clipboard->remoteOffer.published)
  {
    dataset = clipboard->remoteOffer.offerID;
    count   = clipboard->remoteOffer.formatCount;
    files   = (clipboard->remoteOffer.formatMask &
      (UINT32_C(1) << LG_CLIPBOARD_DATA_FILES)) != 0;
    memcpy(replay, clipboard->remoteOffer.types,
        count * sizeof(*replay));
  }
  LG_UNLOCK(clipboard->stateLock);

  if (admitted && count)
  {
    if (files && events->fileOffer)
      events->fileOffer(eventOpaque, dataset);
    if (events->notice)
      events->notice(eventOpaque, replay, count);
  }
  LG_UNLOCK(clipboard->eventLock);

  if (admitted)
    networkClipboardFlushQueued(clipboard);
  return admitted;
}

static void networkClipboardDetach(void * opaque)
{
  NetworkClipboard * clipboard = opaque;

  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * clipboardReads  =
    clipboard->clipboardReads;
  struct NetworkClipboardTransfer * clipboardWrites =
    clipboard->clipboardWrites;

  struct NetworkFileTransfer * fileReads  = clipboard->fileReads;
  struct NetworkFileTransfer * fileWrites = clipboard->fileWrites;

  struct NetworkFileAcquisition * acquisitions =
    clipboard->fileAcquisitions;
  clipboard->clipboardReads   = NULL;
  clipboard->clipboardWrites  = NULL;
  clipboard->fileReads        = NULL;
  clipboard->fileWrites       = NULL;
  clipboard->fileAcquisitions = NULL;
  networkClipboardClearSendNL(clipboard);
  if (clipboard->attached)
    networkClipboardQueueReleaseNL(clipboard);
  clipboard->events         = NULL;
  clipboard->eventOpaque    = NULL;
  clipboard->attached       = false;
  clipboard->inboundReady   = true;
  clipboard->localOffer     = (struct NetworkLocalOffer) { 0 };
  networkClipboardRemoteOfferResetNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);
  LG_UNLOCK(clipboard->eventLock);

  networkClipboardTransfersFree(clipboardReads);
  networkClipboardTransfersFree(clipboardWrites);
  networkClipboardFileTransfersFree(fileReads);
  networkClipboardFileTransfersFree(fileWrites);
  networkClipboardAcquisitionsFree(acquisitions);

  networkClipboardFlushQueued(clipboard);
}

static bool networkClipboardRelease(void * opaque)
{
  NetworkClipboard * clipboard = opaque;

  LG_LOCK(clipboard->stateLock);
  if (!clipboard->claimPending && !clipboard->claimed)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }
  if (!clipboard->localOffer.offerID)
  {
    LG_UNLOCK(clipboard->stateLock);
    return true;
  }
  if (clipboard->claimPending)
  {
    clipboard->localOffer = (struct NetworkLocalOffer) { 0 };
    LG_UNLOCK(clipboard->stateLock);
    return true;
  }

  const uint64_t offerID = clipboard->localOffer.offerID;
  const bool     queued  = networkClipboardQueueControlNL(clipboard,
      LG_NET_CLIPBOARD_MESSAGE_CLEAR, offerID);
  if (queued)
    clipboard->localOffer = (struct NetworkLocalOffer) { 0 };
  LG_UNLOCK(clipboard->stateLock);

  if (queued)
    networkClipboardFlushQueued(clipboard);
  return queued;
}

static bool networkClipboardNotifyTypes(void * opaque,
    const LG_ClipboardData types[], size_t count)
{
  NetworkClipboard * clipboard = opaque;
  if (!types || !count || count > NETWORK_CLIPBOARD_FORMATS)
    return false;

  uint32_t mask = 0;
  for (size_t index = 0; index < count; ++index)
  {
    if (!networkClipboardDataValid(types[index]) ||
        types[index] == LG_CLIPBOARD_DATA_FILES)
      return false;
    const uint32_t bit = UINT32_C(1) << types[index];
    if (mask & bit)
      return false;
    mask |= bit;
  }

  LG_LOCK(clipboard->stateLock);
  if ((!clipboard->claimPending && !clipboard->claimed) ||
      !clipboard->attached)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  struct NetworkLocalOffer previous = clipboard->localOffer;
  memset(&clipboard->localOffer, 0, sizeof(clipboard->localOffer));
  clipboard->localOffer.offerID =
    networkClipboardNextLocalIDNL(clipboard, false);
  clipboard->localOffer.serial  =
    networkClipboardNext64(clipboard->remoteSerial);

  clipboard->remoteSerial         = clipboard->localOffer.serial;
  clipboard->localOffer.count     = (uint32_t)count;

  for (uint32_t index = 0; index < count; ++index)
  {
    clipboard->localOffer.types[index]          = types[index];
    clipboard->localOffer.indices[types[index]] = index;
  }

  const bool deferred = clipboard->claimPending;
  const bool accepted = deferred ||
    networkClipboardQueueLocalOfferNL(clipboard);
  if (!accepted)
    clipboard->localOffer = previous;
  LG_UNLOCK(clipboard->stateLock);

  if (accepted && !deferred)
    networkClipboardFlushQueued(clipboard);
  return accepted;
}

static bool networkClipboardOfferFiles(void * opaque, uint64_t dataset)
{
  NetworkClipboard * clipboard = opaque;
  if (!dataset || kvmfrClipboardTransferFromHelper(dataset))
    return false;

  LG_LOCK(clipboard->stateLock);
  if ((!clipboard->claimPending && !clipboard->claimed) ||
      !clipboard->attached || !clipboard->fileService)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  struct NetworkLocalOffer previous = clipboard->localOffer;
  memset(&clipboard->localOffer, 0, sizeof(clipboard->localOffer));
  clipboard->localOffer.offerID = dataset;
  clipboard->localOffer.serial  =
    networkClipboardNext64(clipboard->remoteSerial);

  clipboard->remoteSerial = clipboard->localOffer.serial;

  clipboard->localOffer.count                            = 1;
  clipboard->localOffer.types[0]                         =
    LG_CLIPBOARD_DATA_FILES;
  clipboard->localOffer.indices[LG_CLIPBOARD_DATA_FILES] = 0;
  clipboard->localOffer.files                            = true;

  const bool deferred = clipboard->claimPending;
  const bool accepted = deferred ||
    networkClipboardQueueLocalOfferNL(clipboard);
  if (!accepted)
    clipboard->localOffer = previous;
  LG_UNLOCK(clipboard->stateLock);

  if (accepted && !deferred)
    networkClipboardFlushQueued(clipboard);
  return accepted;
}

static LG_ClipboardResult networkClipboardDataBegin(void * opaque,
    LG_ClipboardRequest request, LG_ClipboardData type, uint64_t sizeHint)
{
  NetworkClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) ||
      !networkClipboardDataValid(type) || type == LG_CLIPBOARD_DATA_FILES)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardWrites, request);
  if (!transfer || transfer->began || transfer->type != type)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  transfer->delivery = clipboard->clipboardBulk ?
    NETWORK_PACKET_DELIVERY_BULK : NETWORK_PACKET_DELIVERY_RELIABLE;

  const LGNetClipboardTransfer wire =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = transfer->offerID,
    .totalLength     = sizeHint,
    .processedLength = 0,
  };
  struct NetworkClipboardSend * send = networkClipboardSendWithDelivery(
      networkClipboardTransferSendNew(clipboard,
        LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN, &wire, 0),
      transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer->began    = true;
  transfer->blocked  = false;
  transfer->sizeHint = sizeHint;
  transfer->offset   = 0;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult networkClipboardDataChunk(void * opaque,
    LG_ClipboardRequest request, uint64_t offset,
    const void * data, size_t size)
{
  NetworkClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) || !data || !size ||
      size > NETWORK_CLIPBOARD_EMITTED_CHUNK || size > UINT32_MAX ||
      offset > UINT64_MAX - size)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardWrites, request);
  if (!transfer || !transfer->began || transfer->offset != offset ||
      size > transfer->maxChunkLength ||
      (transfer->sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
       offset + size > transfer->sizeHint))
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  LGNetClipboardChunkFlags flags = offset ? 0 :
    LG_NET_CLIPBOARD_CHUNK_FIRST;
  if (transfer->sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
      offset + size == transfer->sizeHint)
    flags |= LG_NET_CLIPBOARD_CHUNK_FINAL;
  const LGNetClipboardChunk chunk =
  {
    .requestID   = transfer->wireRequest,
    .offerID     = transfer->offerID,
    .offset      = offset,
    .totalLength = transfer->sizeHint,
    .dataLength  = (uint32_t)size,
    .flags       = flags,
    .data        = data,
  };
  struct NetworkClipboardSend * send =
    networkClipboardSendWithDelivery(
      networkClipboardChunkSendNew(clipboard, &chunk),
      transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer->offset  += size;
  transfer->blocked  = false;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult networkClipboardDataEnd(void * opaque,
    LG_ClipboardRequest request, uint64_t finalSize)
{
  NetworkClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request))
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardWrites, request);
  if (!transfer || !transfer->began || transfer->offset != finalSize ||
      (transfer->sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN &&
       transfer->sizeHint != finalSize))
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  const LGNetClipboardTransfer wire =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = transfer->offerID,
    .totalLength     = finalSize,
    .processedLength = finalSize,
  };
  struct NetworkClipboardSend * send = networkClipboardSendWithDelivery(
      networkClipboardTransferSendNew(clipboard,
        LG_NET_CLIPBOARD_MESSAGE_DATA_END, &wire, 0),
      transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer = networkClipboardTransferTake(
      &clipboard->clipboardWrites, request);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static bool networkClipboardData(void * opaque,
    LG_ClipboardRequest request, LG_ClipboardData type,
    const void * data, size_t size)
{
  NetworkClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) ||
      (type != LG_CLIPBOARD_DATA_NONE &&
       (!networkClipboardDataValid(type) ||
        type == LG_CLIPBOARD_DATA_FILES)) ||
      (size && !data) || (type == LG_CLIPBOARD_DATA_NONE && size))
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardWrites, request);
  if (!transfer || transfer->began ||
      (type != LG_CLIPBOARD_DATA_NONE && transfer->type != type))
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    const bool queued = networkClipboardQueueClipboardStatusNL(clipboard,
        transfer->wireRequest, transfer->offerID,
        LG_NET_CLIPBOARD_STATUS_UNSUPPORTED, 0, 0);
    if (queued)
      transfer = networkClipboardTransferTake(
          &clipboard->clipboardWrites, request);
    LG_UNLOCK(clipboard->stateLock);
    if (queued)
    {
      free(transfer);
      networkClipboardFlushQueued(clipboard);
    }
    return queued;
  }

  const size_t maxChunkLength = transfer->maxChunkLength <
    NETWORK_CLIPBOARD_EMITTED_CHUNK ? transfer->maxChunkLength :
    NETWORK_CLIPBOARD_EMITTED_CHUNK;
  const size_t chunkCount     = size ?
    1U + (size - 1U) / maxChunkLength : 0;
  if (chunkCount > NETWORK_CLIPBOARD_PENDING_MAX - 2U)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  const size_t fixedBytes =
    2U * (sizeof(struct NetworkClipboardSend) +
      LG_NET_CLIPBOARD_TRANSFER_WIRE_SIZE) +
    chunkCount * (sizeof(struct NetworkClipboardSend) +
      LG_NET_CLIPBOARD_CHUNK_HEADER_WIRE_SIZE);
  if (fixedBytes > NETWORK_CLIPBOARD_PENDING_BYTES ||
      size > NETWORK_CLIPBOARD_PENDING_BYTES - fixedBytes)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  const NetworkPacketDelivery  delivery = clipboard->clipboardBulk ?
    NETWORK_PACKET_DELIVERY_BULK : NETWORK_PACKET_DELIVERY_RELIABLE;
  struct NetworkClipboardBatch batch    = { 0 };
  const LGNetClipboardTransfer begin    =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = transfer->offerID,
    .totalLength     = size,
    .processedLength = 0,
  };
  struct NetworkClipboardSend * send = networkClipboardSendWithDelivery(
      networkClipboardTransferSendNew(clipboard,
        LG_NET_CLIPBOARD_MESSAGE_DATA_BEGIN, &begin, 0), delivery);
  if (!send)
    goto fail;
  networkClipboardBatchAppend(&batch, send);

  size_t offset = 0;
  while (offset < size)
  {
    size_t chunkLength = size - offset;
    if (chunkLength > transfer->maxChunkLength)
      chunkLength = transfer->maxChunkLength;
    if (chunkLength > NETWORK_CLIPBOARD_EMITTED_CHUNK)
      chunkLength = NETWORK_CLIPBOARD_EMITTED_CHUNK;

    const bool                final = offset + chunkLength == size;
    const LGNetClipboardChunk chunk =
    {
      .requestID   = transfer->wireRequest,
      .offerID     = transfer->offerID,
      .offset      = offset,
      .totalLength = size,
      .dataLength  = (uint32_t)chunkLength,
      .flags       = (offset ? 0 : LG_NET_CLIPBOARD_CHUNK_FIRST) |
        (final ? LG_NET_CLIPBOARD_CHUNK_FINAL : 0),
      .data        = (const uint8_t *)data + offset,
    };
    send = networkClipboardSendWithDelivery(
        networkClipboardChunkSendNew(clipboard, &chunk), delivery);
    if (!send)
      goto fail;
    networkClipboardBatchAppend(&batch, send);
    offset += chunkLength;
  }

  const LGNetClipboardTransfer end =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = transfer->offerID,
    .totalLength     = size,
    .processedLength = size,
  };
  send = networkClipboardSendWithDelivery(
      networkClipboardTransferSendNew(clipboard,
        LG_NET_CLIPBOARD_MESSAGE_DATA_END, &end, 0), delivery);
  if (!send)
    goto fail;
  networkClipboardBatchAppend(&batch, send);

  if (!networkClipboardQueueBatchNL(clipboard, &batch))
    goto fail;
  transfer = networkClipboardTransferTake(
      &clipboard->clipboardWrites, request);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  networkClipboardFlushQueued(clipboard);
  return true;

fail:
  networkClipboardBatchFree(&batch);
  LG_UNLOCK(clipboard->stateLock);
  return false;
}

static bool networkClipboardDataCancel(void * opaque,
    LG_ClipboardRequest request, LG_ClipboardCancelReason reason)
{
  NetworkClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) ||
      reason > LG_CLIPBOARD_CANCEL_INVALID)
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardWrites, request);
  if (!transfer)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  const LGNetClipboardTransfer wire =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = transfer->offerID,
    .totalLength     = transfer->sizeHint,
    .processedLength = transfer->offset,
  };
  struct NetworkClipboardSend * send = networkClipboardTransferSendNew(
      clipboard, LG_NET_CLIPBOARD_MESSAGE_CANCEL, &wire, 0);
  const bool queued = networkClipboardQueueSendNL(clipboard, send);
  if (queued)
    transfer = networkClipboardTransferTake(
        &clipboard->clipboardWrites, request);
  LG_UNLOCK(clipboard->stateLock);

  if (queued)
  {
    free(transfer);
    networkClipboardFlushQueued(clipboard);
  }
  return queued;
}

static bool networkClipboardDataReady(void * opaque,
    LG_ClipboardRequest request)
{
  NetworkClipboard * clipboard = opaque;
  if (!request)
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * transfer =
    networkClipboardTransferFind(clipboard->clipboardReads, request);
  if (!transfer || !clipboard->inboundBlocked ||
      clipboard->inboundBlockedRequest != request)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  clipboard->inboundReady = true;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardFlushQueued(clipboard);
  return true;
}

static bool networkClipboardRequest(void * opaque,
    LG_ClipboardRequest request, LG_ClipboardData type)
{
  NetworkClipboard * clipboard = opaque;
  if (!request || kvmfrClipboardTransferFromHelper(request) ||
      !networkClipboardDataValid(type) || type == LG_CLIPBOARD_DATA_FILES)
    return false;

  struct NetworkClipboardTransfer * transfer = calloc(1, sizeof(*transfer));
  if (!transfer)
    return false;

  LG_LOCK(clipboard->stateLock);
  const struct NetworkRemoteOffer * offer = &clipboard->remoteOffer;
  if (!offer->published || offer->indices[type] == UINT32_MAX ||
      networkClipboardTransferCount(clipboard->clipboardReads) >=
        NETWORK_CLIPBOARD_FILE_REQUESTS ||
      networkClipboardTransferFind(clipboard->clipboardReads, request))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return false;
  }

  transfer->localRequest   = request;
  transfer->wireRequest    = networkClipboardNextWireIDNL(clipboard);
  transfer->offerID        = offer->offerID;
  transfer->type           = type;
  transfer->formatIndex    = offer->indices[type];
  transfer->maxChunkLength = NETWORK_CLIPBOARD_EMITTED_CHUNK;

  const LGNetClipboardRequest wire =
  {
    .offerID        = transfer->offerID,
    .requestID      = transfer->wireRequest,
    .formatIndex    = transfer->formatIndex,
    .maxChunkLength = transfer->maxChunkLength,
    .offset         = 0,
  };
  struct NetworkClipboardSend * send =
    networkClipboardRequestSendNew(clipboard, &wire);
  if (!send || !networkClipboardQueueSendNL(clipboard, send))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return false;
  }

  transfer->sizeHint        = LG_CLIPBOARD_SIZE_UNKNOWN;
  transfer->next            = clipboard->clipboardReads;
  clipboard->clipboardReads = transfer;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return true;
}

static bool networkClipboardFileAcquire(void * opaque, uint64_t dataset,
    uint64_t acquisitionID)
{
  NetworkClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromClient(acquisitionID))
    return false;

  struct NetworkFileAcquisition * acquisition =
    calloc(1, sizeof(*acquisition));
  if (!acquisition)
    return false;

  LG_LOCK(clipboard->stateLock);
  if (!clipboard->remoteOffer.published ||
      !clipboard->remoteOffer.fileOffer ||
      clipboard->remoteOffer.offerID != dataset ||
      networkClipboardAcquisitionCountNL(clipboard) >=
        NETWORK_CLIPBOARD_FILE_ACQUISITIONS ||
      networkClipboardAcquisitionFind(
        clipboard, dataset, acquisitionID, false))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(acquisition);
    return false;
  }

  acquisition->dataset          = dataset;
  acquisition->localAcquisition = acquisitionID;
  acquisition->wireClientID     = networkClipboardNextWireIDNL(clipboard);
  acquisition->fromRemote       = false;

  const LGNetFileLease lease =
  {
    .clientID   = acquisition->wireClientID,
    .offerID    = dataset,
    .leaseEpoch = 0,
    .leaseMs    = LG_NET_FILE_MAX_LEASE_MS,
    .flags      = LG_NET_FILE_LEASE_READ,
  };
  struct NetworkClipboardSend * send = networkClipboardFileLeaseSendNew(
      clipboard, LG_NET_FILE_MESSAGE_ACQUIRE, &lease,
      LG_NET_ENVELOPE_ACK_REQUIRED);
  if (!send || !networkClipboardQueueSendNL(clipboard, send))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(acquisition);
    return false;
  }

  acquisition->next           = clipboard->fileAcquisitions;
  clipboard->fileAcquisitions = acquisition;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return true;
}

static bool networkClipboardFileAcquired(void * opaque, uint64_t dataset,
    uint64_t acquisitionID, LG_ClipboardFileError error)
{
  NetworkClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromHelper(acquisitionID) ||
      !networkClipboardFileErrorValid(error))
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileAcquisition * acquisition =
    networkClipboardAcquisitionFind(
        clipboard, dataset, acquisitionID, true);
  if (!acquisition || acquisition->active)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  bool queued;
  if (error == LG_CLIPBOARD_FILE_ERROR_NONE)
  {
    const uint64_t       leaseEpoch =
      networkClipboardNextWireIDNL(clipboard);
    const LGNetFileLease lease      =
    {
      .clientID   = acquisition->wireClientID,
      .offerID    = dataset,
      .leaseEpoch = leaseEpoch,
      .leaseMs    = LG_NET_FILE_MAX_LEASE_MS,
      .flags      = LG_NET_FILE_LEASE_READ |
        LG_NET_FILE_LEASE_ACQUIRED,
    };
    queued = networkClipboardQueueSendNL(clipboard,
        networkClipboardFileLeaseSendNew(clipboard,
          LG_NET_FILE_MESSAGE_ACQUIRED, &lease,
          LG_NET_ENVELOPE_RESPONSE));
    if (queued)
    {
      acquisition->leaseEpoch = leaseEpoch;
      acquisition->active     = true;
    }
  }
  else
  {
    queued = networkClipboardQueueFileStatusNL(clipboard,
        acquisition->wireClientID, dataset, 0, error, 0);
    if (queued)
      acquisition = networkClipboardAcquisitionTake(
          clipboard, dataset, acquisitionID, true);
  }
  LG_UNLOCK(clipboard->stateLock);

  if (queued)
  {
    if (error != LG_CLIPBOARD_FILE_ERROR_NONE)
      free(acquisition);
    networkClipboardFlushQueued(clipboard);
  }
  return queued;
}

static bool networkClipboardFileRelease(void * opaque, uint64_t dataset,
    uint64_t acquisitionID)
{
  NetworkClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromClient(acquisitionID))
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileAcquisition * acquisition =
    networkClipboardAcquisitionFind(
        clipboard, dataset, acquisitionID, false);
  if (!acquisition || !acquisition->active)
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  const LGNetFileLease lease =
  {
    .clientID   = acquisition->wireClientID,
    .offerID    = dataset,
    .leaseEpoch = acquisition->leaseEpoch,
    .leaseMs    = 0,
    .flags      = LG_NET_FILE_LEASE_READ,
  };
  const bool queued = networkClipboardQueueSendNL(clipboard,
      networkClipboardFileLeaseSendNew(clipboard,
        LG_NET_FILE_MESSAGE_RELEASE, &lease, 0));
  if (queued)
    acquisition = networkClipboardAcquisitionTake(
        clipboard, dataset, acquisitionID, false);
  LG_UNLOCK(clipboard->stateLock);

  if (queued)
  {
    free(acquisition);
    networkClipboardFlushQueued(clipboard);
  }
  return queued;
}

static bool networkClipboardFileRequest(void * opaque,
    const LG_ClipboardFileRequest * descriptor)
{
  NetworkClipboard * clipboard = opaque;
  if (!descriptor || !descriptor->dataset ||
      !kvmfrClipboardTransferFromClient(descriptor->request) ||
      (descriptor->operation != LG_CLIPBOARD_FILE_LIST &&
       descriptor->operation != LG_CLIPBOARD_FILE_READ) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_LIST &&
       (descriptor->offset || descriptor->length)) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_READ &&
       (!descriptor->length ||
        descriptor->length > NETWORK_CLIPBOARD_FILE_READ_MAX ||
        descriptor->offset > UINT64_MAX - descriptor->length)))
    return false;

  uint64_t entryID;
  if (!lgNetFileEntryIDFromKVMFRNode(descriptor->node, &entryID))
    return false;

  struct NetworkFileTransfer * transfer = calloc(1, sizeof(*transfer));
  if (!transfer)
    return false;

  LG_LOCK(clipboard->stateLock);
  if (!networkClipboardDatasetAcquiredNL(
        clipboard, descriptor->dataset, false) ||
      networkClipboardFileTransferCountNL(clipboard) >=
        NETWORK_CLIPBOARD_FILE_REQUESTS ||
      networkClipboardFileTransferFind(
        clipboard->fileReads, descriptor->request))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return false;
  }

  transfer->request     = *descriptor;
  transfer->wireRequest = networkClipboardNextWireIDNL(clipboard);
  transfer->entryID     = entryID;
  transfer->sizeHint    = LG_CLIPBOARD_SIZE_UNKNOWN;

  const LGNetFileRequest request =
  {
    .requestID = transfer->wireRequest,
    .offerID   = descriptor->dataset,
    .entryID   = entryID,
    .offset    = descriptor->offset,
    .length    = descriptor->length,
    .flags     = descriptor->operation == LG_CLIPBOARD_FILE_READ ?
      LG_NET_FILE_REQUEST_DATA : LG_NET_FILE_REQUEST_METADATA,
  };
  struct NetworkClipboardSend * send =
    networkClipboardFileRequestSendNew(clipboard, &request);
  if (!send || !networkClipboardQueueSendNL(clipboard, send))
  {
    LG_UNLOCK(clipboard->stateLock);
    free(transfer);
    return false;
  }

  transfer->next       = clipboard->fileReads;
  clipboard->fileReads = transfer;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return true;
}

static LG_ClipboardResult networkClipboardFileDataBegin(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t sizeHint)
{
  NetworkClipboard * clipboard = opaque;
  if (!descriptor || !kvmfrClipboardTransferFromHelper(
        descriptor->request) ||
      descriptor->offset > UINT64_MAX - sizeHint)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFind(
        clipboard->fileWrites, descriptor->request);
  if (!transfer || transfer->began ||
      !networkClipboardSameFileRequest(&transfer->request, descriptor) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_READ &&
       (sizeHint > NETWORK_CLIPBOARD_FILE_READ_MAX ||
        sizeHint > descriptor->length)))
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  transfer->delivery = clipboard->fileBulk ?
    NETWORK_PACKET_DELIVERY_BULK : NETWORK_PACKET_DELIVERY_RELIABLE;

  const LGNetFileTransfer wire =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = descriptor->dataset,
    .entryID         = transfer->entryID,
    .offset          = descriptor->offset,
    .totalLength     = descriptor->offset + sizeHint,
    .processedLength = 0,
  };
  struct NetworkClipboardSend * send = networkClipboardSendWithDelivery(
      networkClipboardFileTransferSendNew(clipboard,
        LG_NET_FILE_MESSAGE_DATA_BEGIN, &wire, 0), transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer->began          = true;
  transfer->blocked        = false;
  transfer->sizeHint       = sizeHint;
  transfer->responseOffset = 0;
  transfer->wireEnded      = false;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult networkClipboardFileDataChunk(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t responseOffset,
    const void * data, size_t size, bool end)
{
  NetworkClipboard * clipboard = opaque;
  if (!descriptor || !kvmfrClipboardTransferFromHelper(
        descriptor->request) || !data || !size ||
      size > NETWORK_CLIPBOARD_EMITTED_CHUNK || size > UINT32_MAX ||
      responseOffset > UINT64_MAX - size)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFind(
        clipboard->fileWrites, descriptor->request);
  if (!transfer || !transfer->began || transfer->wireEnded ||
      !networkClipboardSameFileRequest(&transfer->request, descriptor) ||
      transfer->responseOffset != responseOffset ||
      responseOffset + size > transfer->sizeHint ||
      (end && responseOffset + size != transfer->sizeHint) ||
      descriptor->offset > UINT64_MAX - transfer->sizeHint)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  const bool          final = responseOffset + size == transfer->sizeHint;
  LGNetFileChunkFlags flags = responseOffset ? 0 :
    LG_NET_FILE_CHUNK_FIRST;
  if (final)
    flags |= LG_NET_FILE_CHUNK_FINAL;
  const LGNetFileChunk chunk =
  {
    .requestID   = transfer->wireRequest,
    .offerID     = descriptor->dataset,
    .entryID     = transfer->entryID,
    .offset      = responseOffset,
    .totalLength = transfer->sizeHint,
    .dataLength  = (uint32_t)size,
    .flags       = flags,
    .data        = data,
  };
  struct NetworkClipboardSend * send =
    networkClipboardSendWithDelivery(
      networkClipboardFileChunkSendNew(clipboard, &chunk),
      transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer->responseOffset += size;
  transfer->blocked         = false;
  transfer->wireEnded       = final;
  LG_UNLOCK(clipboard->stateLock);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult networkClipboardFileDataEnd(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t finalSize)
{
  NetworkClipboard * clipboard = opaque;
  if (!descriptor || !kvmfrClipboardTransferFromHelper(
        descriptor->request) ||
      descriptor->offset > UINT64_MAX - finalSize)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileTransfer * transfer =
    networkClipboardFileTransferFind(
        clipboard->fileWrites, descriptor->request);
  if (!transfer || !transfer->began ||
      !networkClipboardSameFileRequest(&transfer->request, descriptor) ||
      transfer->responseOffset != finalSize ||
      transfer->sizeHint != finalSize)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  const LGNetFileTransfer wire =
  {
    .requestID       = transfer->wireRequest,
    .offerID         = descriptor->dataset,
    .entryID         = transfer->entryID,
    .offset          = descriptor->offset,
    .totalLength     = descriptor->offset + finalSize,
    .processedLength = finalSize,
  };
  struct NetworkClipboardSend * send = networkClipboardSendWithDelivery(
      networkClipboardFileTransferSendNew(clipboard,
        LG_NET_FILE_MESSAGE_DATA_END, &wire, 0), transfer->delivery);
  if (!send)
  {
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (!networkClipboardQueueSendNL(clipboard, send))
  {
    transfer->blocked = true;
    LG_UNLOCK(clipboard->stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }

  transfer = networkClipboardFileTransferTake(
      &clipboard->fileWrites, descriptor->request);
  LG_UNLOCK(clipboard->stateLock);
  free(transfer);
  networkClipboardFlushQueued(clipboard);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static bool networkClipboardFileCancel(void * opaque, uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  NetworkClipboard * clipboard = opaque;
  if (!dataset || !request || reason == LG_CLIPBOARD_FILE_ERROR_NONE ||
      !networkClipboardFileErrorValid(reason))
    return false;

  LG_LOCK(clipboard->stateLock);
  struct NetworkFileTransfer * read  =
    networkClipboardFileTransferFind(clipboard->fileReads, request);
  struct NetworkFileTransfer * write =
    networkClipboardFileTransferFind(clipboard->fileWrites, request);

  struct NetworkFileAcquisition * acquisition =
    networkClipboardAcquisitionFind(clipboard, dataset, request,
        kvmfrClipboardTransferFromHelper(request));

  uint64_t wireRequest = 0;
  uint64_t entryID     = 0;
  uint64_t offset      = 0;
  uint64_t totalLength = 0;
  uint64_t processed   = 0;
  if (read && read->request.dataset == dataset)
  {
    wireRequest = read->wireRequest;
    entryID     = read->entryID;
    offset      = read->request.offset;
    totalLength = read->sizeHint == LG_CLIPBOARD_SIZE_UNKNOWN ?
      UINT64_MAX : offset + read->sizeHint;
    processed   = read->responseOffset;
  }
  else if (write && write->request.dataset == dataset)
  {
    wireRequest = write->wireRequest;
    entryID     = write->entryID;
    offset      = write->request.offset;
    totalLength = offset + write->sizeHint;
    processed   = write->responseOffset;
  }
  else if (acquisition)
    wireRequest = acquisition->wireClientID;
  else
  {
    LG_UNLOCK(clipboard->stateLock);
    return false;
  }

  bool queued;
  if (entryID)
  {
    const LGNetFileTransfer wire =
    {
      .requestID       = wireRequest,
      .offerID         = dataset,
      .entryID         = entryID,
      .offset          = offset,
      .totalLength     = totalLength,
      .processedLength = processed,
    };
    queued = networkClipboardQueueSendNL(clipboard,
        networkClipboardFileTransferSendNew(clipboard,
          LG_NET_FILE_MESSAGE_CANCEL, &wire, 0));
  }
  else
    queued = networkClipboardQueueFileStatusNL(clipboard,
        wireRequest, dataset, 0, reason, 0);

  if (queued)
  {
    free(networkClipboardFileTransferTake(&clipboard->fileReads, request));
    free(networkClipboardFileTransferTake(&clipboard->fileWrites, request));
    if (acquisition)
      free(networkClipboardAcquisitionTake(clipboard,
          dataset, request, acquisition->fromRemote));
  }
  LG_UNLOCK(clipboard->stateLock);

  if (queued)
    networkClipboardFlushQueued(clipboard);
  return queued;
}

static void networkClipboardDispatchDisconnected(
    NetworkClipboard * clipboard,
    struct NetworkClipboardTransfer * clipboardReads,
    struct NetworkClipboardTransfer * clipboardWrites,
    struct NetworkFileTransfer * fileReads,
    struct NetworkFileTransfer * fileWrites,
    struct NetworkFileAcquisition * acquisitions,
    bool release)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->stateLock);
  const LG_ClipboardEventOps * events = clipboard->attached ?
    clipboard->events : NULL;
  void                       * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->stateLock);

  if (events)
  {
    for (const struct NetworkClipboardTransfer * item = clipboardReads;
        item; item = item->next)
      if (events->dataCancel)
        events->dataCancel(opaque, item->localRequest,
            LG_CLIPBOARD_CANCEL_UNAVAILABLE);
    for (const struct NetworkClipboardTransfer * item = clipboardWrites;
        item; item = item->next)
      if (events->requestCancel)
        events->requestCancel(opaque, item->localRequest,
            LG_CLIPBOARD_CANCEL_UNAVAILABLE);
    for (const struct NetworkFileTransfer * item = fileReads;
        item; item = item->next)
      if (events->fileCancel)
        events->fileCancel(opaque, item->request.dataset,
            item->request.request, LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    for (const struct NetworkFileTransfer * item = fileWrites;
        item; item = item->next)
      if (events->fileCancel)
        events->fileCancel(opaque, item->request.dataset,
            item->request.request, LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    for (const struct NetworkFileAcquisition * item = acquisitions;
        item; item = item->next)
      if (events->fileCancel)
        events->fileCancel(opaque, item->dataset,
            item->localAcquisition, LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    if (release && events->release)
      events->release(opaque);
  }
  LG_UNLOCK(clipboard->eventLock);
}

static bool networkClipboardTimer(void * opaque)
{
  NetworkClipboard * clipboard = opaque;
  LG_LOCK(clipboard->stateLock);
  const bool destroying = clipboard->destroying;
  LG_UNLOCK(clipboard->stateLock);
  if (destroying)
    return false;

  networkClipboardProcess(clipboard);
  return true;
}

bool networkClipboardCreate(NetworkClipboard ** result,
    NetworkConnection * connection)
{
  if (!result || !connection)
    return false;

  NetworkClipboard * clipboard = calloc(1, sizeof(*clipboard));
  if (!clipboard)
    return false;

  LG_LOCK_INIT(clipboard->stateLock);
  LG_LOCK_INIT(clipboard->eventLock);
  LG_LOCK_INIT(clipboard->statusLock);
  clipboard->connection       = connection;
  clipboard->statusGeneration = 1;
  clipboard->ownerID          = nanotime() ^
    ((uint64_t)(uintptr_t)clipboard << 23);
  clipboard->localSerial      = nanotime() ^
    ((uint64_t)(uintptr_t)clipboard << 11);
  clipboard->wireSerial       = clipboard->localSerial ^
    UINT64_C(0x4c47434c49504244);
  clipboard->remoteSerial     = clipboard->wireSerial ^
    UINT64_C(0x4e4554574f524b43);
  clipboard->localSerial     &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!clipboard->ownerID)
    clipboard->ownerID = 1;
  if (!clipboard->localSerial)
    clipboard->localSerial = 1;
  if (!clipboard->wireSerial)
    clipboard->wireSerial = 1;
  if (!clipboard->remoteSerial)
    clipboard->remoteSerial = 1;
  networkClipboardRemoteOfferResetNL(clipboard);

  if (!lgCreateTimer(NETWORK_CLIPBOARD_TIMER_MS,
        networkClipboardTimer, clipboard, &clipboard->timer))
  {
    LG_LOCK_FREE(clipboard->statusLock);
    LG_LOCK_FREE(clipboard->eventLock);
    LG_LOCK_FREE(clipboard->stateLock);
    free(clipboard);
    return false;
  }

  *result = clipboard;
  return true;
}

void networkClipboardConnectionState(NetworkClipboard * clipboard,
    bool connected)
{
  if (!clipboard)
    return;

  uint16_t   clipboardVersion = 0;
  uint16_t   fileVersion      = 0;
  const bool clipboardService = connected &&
    networkConnectionServiceAvailable(clipboard->connection,
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(clipboard->connection,
      LG_NET_SERVICE_CLIPBOARD, &clipboardVersion) &&
    clipboardVersion == LG_NET_CLIPBOARD_VERSION_CURRENT;
  const bool fileService      = clipboardService &&
    networkConnectionServiceAvailable(clipboard->connection,
      LG_NET_SERVICE_FILE, LG_NET_CAPABILITY_RELIABLE) &&
    networkConnectionServiceVersion(clipboard->connection,
      LG_NET_SERVICE_FILE, &fileVersion) &&
    fileVersion == LG_NET_FILE_VERSION_CURRENT;
  const bool clipboardBulk    = clipboardService &&
    networkConnectionServiceAvailable(clipboard->connection,
      LG_NET_SERVICE_CLIPBOARD, LG_NET_CAPABILITY_BULK);
  const bool fileBulk         = fileService &&
    networkConnectionServiceAvailable(clipboard->connection,
      LG_NET_SERVICE_FILE, LG_NET_CAPABILITY_BULK);

  LG_LOCK(clipboard->stateLock);
  struct NetworkClipboardTransfer * clipboardReads  =
    clipboard->clipboardReads;
  struct NetworkClipboardTransfer * clipboardWrites =
    clipboard->clipboardWrites;

  struct NetworkFileTransfer * fileReads  = clipboard->fileReads;
  struct NetworkFileTransfer * fileWrites = clipboard->fileWrites;

  struct NetworkFileAcquisition * acquisitions =
    clipboard->fileAcquisitions;
  const bool release = clipboard->remoteOffer.published;

  clipboard->clipboardReads    = NULL;
  clipboard->clipboardWrites   = NULL;
  clipboard->fileReads         = NULL;
  clipboard->fileWrites        = NULL;
  clipboard->fileAcquisitions  = NULL;
  clipboard->connected         = connected;
  clipboard->clipboardService  = clipboardService;
  clipboard->fileService       = fileService;
  clipboard->clipboardBulk     = clipboardBulk;
  clipboard->fileBulk          = fileBulk;
  clipboard->clipboardVersion  = clipboardService ? clipboardVersion : 0;
  clipboard->fileVersion       = fileService ? fileVersion : 0;
  clipboard->claimPending      = false;
  clipboard->claimed           = false;
  clipboard->claimSerial       = 0;
  clipboard->nextClaimNs       = 0;
  clipboard->nextKeepaliveNs   = 0;
  networkClipboardClearSendNL(clipboard);
  networkClipboardClearInboundNL(clipboard);
  networkClipboardRemoteOfferResetNL(clipboard);
  networkClipboardSetAvailableNL(clipboard, clipboardService);
  if (clipboardService && clipboard->attached)
    networkClipboardQueueClaimNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardDispatchDisconnected(clipboard,
      clipboardReads, clipboardWrites, fileReads, fileWrites,
      acquisitions, release);
  networkClipboardTransfersFree(clipboardReads);
  networkClipboardTransfersFree(clipboardWrites);
  networkClipboardFileTransfersFree(fileReads);
  networkClipboardFileTransfersFree(fileWrites);
  networkClipboardAcquisitionsFree(acquisitions);

  networkClipboardProcess(clipboard);
}

void networkClipboardConnect(NetworkClipboard * clipboard)
{
  networkClipboardConnectionState(clipboard, true);
}

void networkClipboardDisconnect(NetworkClipboard * clipboard)
{
  networkClipboardConnectionState(clipboard, false);
}

void networkClipboardDestroy(NetworkClipboard ** result)
{
  if (!result || !*result)
    return;

  NetworkClipboard * clipboard = *result;
  networkClipboardSetStatusListener(clipboard, NULL, NULL);
  networkClipboardDetach(clipboard);

  LG_LOCK(clipboard->stateLock);
  clipboard->destroying = true;
  LG_UNLOCK(clipboard->stateLock);
  lgTimerDestroy(clipboard->timer);

  networkClipboardConnectionState(clipboard, false);
  LG_LOCK(clipboard->stateLock);
  networkClipboardClearSendNL(clipboard);
  networkClipboardClearInboundNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);
  LG_LOCK_FREE(clipboard->statusLock);
  LG_LOCK_FREE(clipboard->eventLock);
  LG_LOCK_FREE(clipboard->stateLock);
  free(clipboard);
  *result = NULL;
}

void networkClipboardProcess(NetworkClipboard * clipboard)
{
  if (!clipboard)
    return;

  LG_LOCK(clipboard->stateLock);
  if (clipboard->destroying || clipboard->processing)
  {
    LG_UNLOCK(clipboard->stateLock);
    return;
  }
  clipboard->processing = true;

  bool progress            = clipboard->sendProgress;
  clipboard->sendProgress  = false;

  const uint64_t now = nanotime();
  if (clipboard->connected && clipboard->clipboardService &&
      clipboard->attached)
  {
    if (clipboard->claimPending && now >= clipboard->nextClaimNs)
    {
      clipboard->claimPending = false;
      clipboard->claimed      = false;
    }
    if (!clipboard->claimPending && !clipboard->claimed &&
        now >= clipboard->nextClaimNs)
      networkClipboardQueueClaimNL(clipboard);
    else if (clipboard->claimed && now >= clipboard->nextKeepaliveNs)
    {
      if (networkClipboardQueueControlNL(clipboard,
            LG_NET_CLIPBOARD_MESSAGE_KEEPALIVE, 0))
        clipboard->nextKeepaliveNs = networkClipboardDeadline(
            NETWORK_CLIPBOARD_KEEPALIVE_MS);
    }
  }
  progress |= networkClipboardFlushNL(clipboard);
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardProcessInbound(clipboard);

  LG_ClipboardRequest clipboardReady[NETWORK_CLIPBOARD_FILE_REQUESTS];
  uint64_t            fileReady[NETWORK_CLIPBOARD_FILE_REQUESTS];
  unsigned            clipboardReadyCount = 0;
  unsigned            fileReadyCount      = 0;

  LG_LOCK(clipboard->stateLock);
  progress                |= clipboard->sendProgress;
  clipboard->sendProgress  = false;
  progress                |= networkClipboardFlushNL(clipboard);
  if (progress)
  {
    for (struct NetworkClipboardTransfer * item =
        clipboard->clipboardWrites; item &&
        clipboardReadyCount < NETWORK_CLIPBOARD_FILE_REQUESTS;
        item = item->next)
    {
      if (!item->blocked)
        continue;
      item->blocked                         = false;
      clipboardReady[clipboardReadyCount++] = item->localRequest;
    }
    for (struct NetworkFileTransfer * item = clipboard->fileWrites;
        item && fileReadyCount < NETWORK_CLIPBOARD_FILE_REQUESTS;
        item = item->next)
    {
      if (!item->blocked)
        continue;
      item->blocked                = false;
      fileReady[fileReadyCount++]  = item->request.request;
    }
  }
  LG_UNLOCK(clipboard->stateLock);

  for (unsigned index = 0; index < clipboardReadyCount; ++index)
    networkClipboardDispatchDataReady(
        clipboard, clipboardReady[index]);
  for (unsigned index = 0; index < fileReadyCount; ++index)
    networkClipboardDispatchFileReady(clipboard, fileReady[index]);

  networkClipboardNotifyStatus(clipboard);

  LG_LOCK(clipboard->stateLock);
  clipboard->processing = false;
  LG_UNLOCK(clipboard->stateLock);
}

void networkClipboardSendReady(NetworkClipboard * clipboard,
    LGNetService service, NetworkPacketDelivery delivery)
{
  if (!clipboard || (service != LG_NET_SERVICE_CLIPBOARD &&
      service != LG_NET_SERVICE_FILE) ||
      (delivery != NETWORK_PACKET_DELIVERY_RELIABLE &&
       delivery != NETWORK_PACKET_DELIVERY_BULK))
    return;
  networkClipboardProcess(clipboard);
}

bool networkClipboardPacket(NetworkClipboard * clipboard,
    const LGNetPacketView * packet, NetworkPacketDelivery delivery)
{
  if (!clipboard || !packet ||
      (packet->envelope.service != LG_NET_SERVICE_CLIPBOARD &&
       packet->envelope.service != LG_NET_SERVICE_FILE) ||
      (delivery != NETWORK_PACKET_DELIVERY_RELIABLE &&
       delivery != NETWORK_PACKET_DELIVERY_BULK) ||
      (packet->payloadLength && !packet->payload) ||
      packet->payloadLength > LG_NET_MAX_PAYLOAD_LENGTH ||
      packet->payloadLength >
        SIZE_MAX - sizeof(struct NetworkClipboardInbound))
    return false;

  struct NetworkClipboardInbound * inbound =
    malloc(sizeof(*inbound) + packet->payloadLength);
  if (!inbound)
    return false;
  inbound->next          = NULL;
  inbound->envelope      = packet->envelope;
  inbound->delivery      = delivery;
  inbound->payloadLength = packet->payloadLength;
  if (packet->payloadLength)
    memcpy(inbound->payload, packet->payload, packet->payloadLength);

  const size_t bytes = sizeof(*inbound) + inbound->payloadLength;
  LG_LOCK(clipboard->stateLock);
  const bool serviceAvailable = clipboard->connected &&
    ((packet->envelope.service == LG_NET_SERVICE_CLIPBOARD &&
      clipboard->clipboardService) ||
     (packet->envelope.service == LG_NET_SERVICE_FILE &&
      clipboard->fileService));
  const bool claimResult      = packet->envelope.service ==
      LG_NET_SERVICE_CLIPBOARD && packet->envelope.messageType ==
      LG_NET_CLIPBOARD_MESSAGE_CLAIM_RESULT;
  const bool stale            = !claimResult &&
    packet->envelope.componentEpoch != clipboard->claimEpoch;
  if (!serviceAvailable || stale)
  {
    LG_UNLOCK(clipboard->stateLock);
    free(inbound);
    return true;
  }
  if (clipboard->inboundCount >= NETWORK_CLIPBOARD_INBOUND_MAX ||
      bytes > NETWORK_CLIPBOARD_INBOUND_BYTES ||
      clipboard->inboundBytes > NETWORK_CLIPBOARD_INBOUND_BYTES - bytes)
  {
    LG_UNLOCK(clipboard->stateLock);
    free(inbound);
    return false;
  }

  if (clipboard->inboundTail)
    clipboard->inboundTail->next = inbound;
  else
    clipboard->inboundHead = inbound;
  clipboard->inboundTail   = inbound;
  ++clipboard->inboundCount;
  clipboard->inboundBytes += bytes;
  LG_UNLOCK(clipboard->stateLock);

  networkClipboardProcess(clipboard);
  return true;
}

static const LG_ClipboardOps NETWORK_CLIPBOARD_OPS =
{
  .name              = "Network",
  .setStatusListener = networkClipboardSetStatusListener,
  .attach            = networkClipboardAttach,
  .detach            = networkClipboardDetach,
  .release           = networkClipboardRelease,
  .notifyTypes       = networkClipboardNotifyTypes,
  .offerFiles        = networkClipboardOfferFiles,
  .data              = networkClipboardData,
  .dataBegin         = networkClipboardDataBegin,
  .dataChunk         = networkClipboardDataChunk,
  .dataEnd           = networkClipboardDataEnd,
  .dataCancel        = networkClipboardDataCancel,
  .dataReady         = networkClipboardDataReady,
  .request           = networkClipboardRequest,
  .fileAcquire       = networkClipboardFileAcquire,
  .fileAcquired      = networkClipboardFileAcquired,
  .fileRelease       = networkClipboardFileRelease,
  .fileRequest       = networkClipboardFileRequest,
  .fileDataBegin     = networkClipboardFileDataBegin,
  .fileDataChunk     = networkClipboardFileDataChunk,
  .fileDataEnd       = networkClipboardFileDataEnd,
  .fileCancel        = networkClipboardFileCancel,
};

const LG_ClipboardOps * networkClipboardGetOps(void)
{
  return &NETWORK_CLIPBOARD_OPS;
}
