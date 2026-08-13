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

#include "common/KVMFRClipboard.h"
#include "common/LGMPConfig.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/locking.h"
#include "common/thread.h"
#include "common/time.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CLIPBOARD_PENDING_MAX        32U
#define CLIPBOARD_GRANTS             KVMFR_CLIPBOARD_SLOT_COUNT
#define CLIPBOARD_POLL_MS            10U
#define CLIPBOARD_RETRY_MS           1U
#define CLIPBOARD_KEEPALIVE_US       UINT64_C(200000)
#define CLIPBOARD_RELEASE_TIMEOUT_US UINT64_C(100000)

struct PendingRecord
{
  KVMFRClipboardMessage record;
};

struct Grant
{
  KVMFRClipboardSlotHeader * header;
  uint8_t                  * data;
  uint32_t                   token;
  bool                       available;
};

enum HeldPhase
{
  HELD_PHASE_NONE,
  HELD_PHASE_BEGIN,
  HELD_PHASE_CHUNK,
  HELD_PHASE_END,
};

struct LGMPClipboard
{
  PLGMPClient       client;
  PLGMPClientQueue  queue;
  LG_Lock           lock;
  LG_Lock           eventLock;
  LG_Lock           statusLock;
  LGEvent         * event;
  LGThread        * thread;
  atomic_bool       stop;

  bool              connected;
  bool              available;
  bool              statusValid;
  bool              claimed;
  bool              ownerConfirmed;
  uint32_t          clientID;
  uint32_t          endpointGeneration;
  uint32_t          providerGeneration;
  uint32_t          claimGeneration;
  uint32_t          publishedClaimGeneration;
  uint32_t          statusSerial;
  uint64_t          lastSend;

  struct PendingRecord pending[CLIPBOARD_PENDING_MAX];
  unsigned             pendingHead;
  unsigned             pendingCount;

  struct Grant          grants[CLIPBOARD_GRANTS];
  bool                  writeBlocked;
  LG_ClipboardRequest   writeBlockedRequest;
  LG_ClipboardRequest   writeTransfer;
  uint64_t              writeClipboardGeneration;
  uint64_t              writeSizeHint;
  KVMFRClipboardFormat  writeFormat;
  uint64_t              writeOffset;
  uint32_t              writeSequence;
  bool                  writeBegan;
  bool                  writeWireBegan;

  bool                  held;
  bool                  heldReady;
  enum HeldPhase        heldPhase;
  LGMPMessage           heldMessage;
  KVMFRClipboardMessage heldRecord;

  uint64_t                     localClipboardGeneration;
  KVMFRClipboardFormatFlags    localFormats;
  uint64_t                     remoteClipboardGeneration;
  KVMFRClipboardFormatFlags    remoteFormats;
  LG_ClipboardRequest          readRequest;
  uint64_t                     readTransfer;
  uint64_t                     readClipboardGeneration;
  uint64_t                     readSizeHint;
  uint64_t                     readOffset;
  KVMFRClipboardFormat         readFormat;
  uint32_t                     readSequence;
  bool                         readBegan;
  uint64_t                     transferSerial;

  const LG_ClipboardEventOps * events;
  void                       * eventOpaque;
  LG_ClipboardStatusFn         statusCallback;
  void                       * statusOpaque;
};

static LG_ClipboardData fromWireFormat(KVMFRClipboardFormat format)
{
  switch (format)
  {
    case KVMFR_CLIPBOARD_FORMAT_TEXT: return LG_CLIPBOARD_DATA_TEXT;
    case KVMFR_CLIPBOARD_FORMAT_PNG : return LG_CLIPBOARD_DATA_PNG;
    case KVMFR_CLIPBOARD_FORMAT_BMP : return LG_CLIPBOARD_DATA_BMP;
    case KVMFR_CLIPBOARD_FORMAT_TIFF: return LG_CLIPBOARD_DATA_TIFF;
    case KVMFR_CLIPBOARD_FORMAT_JPEG: return LG_CLIPBOARD_DATA_JPEG;
    default: return LG_CLIPBOARD_DATA_NONE;
  }
}

static KVMFRClipboardFormat toWireFormat(LG_ClipboardData format)
{
  switch (format)
  {
    case LG_CLIPBOARD_DATA_TEXT: return KVMFR_CLIPBOARD_FORMAT_TEXT;
    case LG_CLIPBOARD_DATA_PNG : return KVMFR_CLIPBOARD_FORMAT_PNG;
    case LG_CLIPBOARD_DATA_BMP : return KVMFR_CLIPBOARD_FORMAT_BMP;
    case LG_CLIPBOARD_DATA_TIFF: return KVMFR_CLIPBOARD_FORMAT_TIFF;
    case LG_CLIPBOARD_DATA_JPEG: return KVMFR_CLIPBOARD_FORMAT_JPEG;
    default: return KVMFR_CLIPBOARD_FORMAT_NONE;
  }
}

static LG_ClipboardCancelReason fromWireCancel(uint32_t reason)
{
  return reason <= LG_CLIPBOARD_CANCEL_INVALID ?
    (LG_ClipboardCancelReason)reason : LG_CLIPBOARD_CANCEL_INVALID;
}

static KVMFRClipboardFormatFlags formatMask(
    const LG_ClipboardData formats[], size_t count)
{
  KVMFRClipboardFormatFlags result = 0;
  for (size_t i = 0; i < count; ++i)
    result |= kvmfrClipboardFormatFlag(toWireFormat(formats[i]));
  return result;
}

static unsigned formatsFromMask(KVMFRClipboardFormatFlags mask,
    LG_ClipboardData formats[LG_CLIPBOARD_DATA_NONE])
{
  unsigned count = 0;
  for (KVMFRClipboardFormat format = KVMFR_CLIPBOARD_FORMAT_TEXT;
       format <= KVMFR_CLIPBOARD_FORMAT_JPEG; ++format)
    if (mask & kvmfrClipboardFormatFlag(format))
      formats[count++] = fromWireFormat(format);
  return count;
}

static uint32_t nextNonzero(uint32_t * value)
{
  if (++*value == 0)
    ++*value;
  return *value;
}

static uint64_t nextClientTransfer(LGMPClipboard * clipboard)
{
  if (++clipboard->transferSerial == 0 ||
      kvmfrClipboardTransferFromHelper(clipboard->transferSerial))
    clipboard->transferSerial = 1;
  return clipboard->transferSerial;
}

static struct PendingRecord * pendingAt(
    LGMPClipboard * clipboard, unsigned position)
{
  return &clipboard->pending[
    (clipboard->pendingHead + position) % CLIPBOARD_PENDING_MAX];
}

static void signalWorker(LGMPClipboard * clipboard)
{
  if (clipboard->event)
    lgSignalEvent(clipboard->event);
}

static bool enqueueRecordNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessage record)
{
  if (!clipboard->connected || !clipboard->queue ||
      clipboard->pendingCount == CLIPBOARD_PENDING_MAX)
    return false;

  record.version = KVMFR_CLIPBOARD_VERSION;
  record.generation = clipboard->claimGeneration;
  pendingAt(clipboard, clipboard->pendingCount++)->record = record;
  return true;
}

static bool enqueueTypeNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessageType type)
{
  KVMFRClipboardMessage record = { 0 };
  record.type = type;
  return enqueueRecordNL(clipboard, record);
}

static struct Grant * availableGrantNL(LGMPClipboard * clipboard)
{
  for (unsigned i = 0; i < CLIPBOARD_GRANTS; ++i)
    if (clipboard->grants[i].available)
      return &clipboard->grants[i];
  return NULL;
}

static bool enqueueDataNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessage record, const void * data)
{
  if (!clipboard->connected || !clipboard->queue ||
      clipboard->pendingCount == CLIPBOARD_PENDING_MAX ||
      record.length > KVMFR_CLIPBOARD_DATA_BYTES ||
      (record.length && !data))
    return false;

  struct Grant * grant = availableGrantNL(clipboard);
  if (!grant)
    return false;

  record.version    = KVMFR_CLIPBOARD_VERSION;
  record.generation = clipboard->claimGeneration;
  record.token      = 0;
  memcpy(grant->header, &record, sizeof(record));
  if (record.length)
    memcpy(grant->data, data, record.length);

  KVMFRClipboardMessage commit = record;
  commit.type  = KVMFR_CLIPBOARD_MESSAGE_COMMIT;
  commit.token = grant->token;
  pendingAt(clipboard, clipboard->pendingCount++)->record = commit;
  grant->available = false;
  return true;
}

static void clearWriteNL(LGMPClipboard * clipboard)
{
  clipboard->writeBlocked             = false;
  clipboard->writeBlockedRequest      = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard->writeTransfer            = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard->writeClipboardGeneration = 0;
  clipboard->writeSizeHint            = 0;
  clipboard->writeFormat              = KVMFR_CLIPBOARD_FORMAT_NONE;
  clipboard->writeOffset              = 0;
  clipboard->writeSequence            = 0;
  clipboard->writeBegan               = false;
  clipboard->writeWireBegan           = false;
}

static void clearReadNL(LGMPClipboard * clipboard)
{
  clipboard->readRequest             = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard->readTransfer            = 0;
  clipboard->readClipboardGeneration = 0;
  clipboard->readSizeHint            = 0;
  clipboard->readOffset              = 0;
  clipboard->readFormat              = KVMFR_CLIPBOARD_FORMAT_NONE;
  clipboard->readSequence            = 0;
  clipboard->readBegan               = false;
}

static void clearProtocolNL(LGMPClipboard * clipboard)
{
  clipboard->claimed                   = false;
  clipboard->ownerConfirmed            = false;
  clipboard->publishedClaimGeneration  = 0;
  clipboard->pendingHead               = 0;
  clipboard->pendingCount              = 0;
  clipboard->remoteClipboardGeneration = 0;
  clipboard->remoteFormats             = 0;
  clearWriteNL(clipboard);
  clearReadNL(clipboard);
  memset(clipboard->grants, 0, sizeof(clipboard->grants));
}

static bool ensureClaimNL(LGMPClipboard * clipboard)
{
  if (clipboard->claimed)
    return true;
  if (!clipboard->connected || !clipboard->available)
    return false;

  KVMFRClipboardMessage claim = { 0 };
  nextNonzero(&clipboard->claimGeneration);
  claim.type  = KVMFR_CLIPBOARD_MESSAGE_CLAIM;
  claim.token = clipboard->endpointGeneration;
  if (!enqueueRecordNL(clipboard, claim))
    return false;
  clipboard->claimed        = true;
  clipboard->ownerConfirmed = false;
  return true;
}

static bool restoreClipboardNL(LGMPClipboard * clipboard)
{
  if (!clipboard->events || !ensureClaimNL(clipboard))
    return false;
  if (!clipboard->localClipboardGeneration)
    return true;

  KVMFRClipboardMessage record = { 0 };
  record.type                = clipboard->localFormats ?
    KVMFR_CLIPBOARD_MESSAGE_OFFER : KVMFR_CLIPBOARD_MESSAGE_CLEAR;
  record.clipboardGeneration = clipboard->localClipboardGeneration;
  record.token               = clipboard->localFormats;
  return enqueueRecordNL(clipboard, record);
}

static void notifyStatus(LGMPClipboard * clipboard)
{
  LG_ClipboardStatusFn callback;
  void * opaque;
  LG_ClipboardStatus status;

  LG_LOCK(clipboard->statusLock);
  LG_LOCK(clipboard->lock);
  callback = clipboard->statusCallback;
  opaque   = clipboard->statusOpaque;
  status = (LG_ClipboardStatus)
  {
    .available  = clipboard->connected && clipboard->available,
    .generation = clipboard->providerGeneration,
  };
  LG_UNLOCK(clipboard->lock);
  if (callback)
    callback(opaque, &status);
  LG_UNLOCK(clipboard->statusLock);
}

static void connectionFailed(LGMPClipboard * clipboard, LGMP_STATUS status)
{
  const bool changed = clipboard->connected || clipboard->available;
  if (clipboard->connected)
    DEBUG_WARN("LGMP clipboard transport failed: %s",
      lgmpStatusString(status));
  clipboard->connected       = false;
  clipboard->available       = false;
  clipboard->statusValid     = false;
  clearProtocolNL(clipboard);
  clipboard->held            = false;
  clipboard->heldReady       = false;
  if (changed)
    nextNonzero(&clipboard->providerGeneration);
  atomic_store_explicit(&clipboard->stop, true, memory_order_release);
}

static bool validStatus(const KVMFRClipboardStatus * status)
{
  const uint32_t validFlags = KVMFR_CLIPBOARD_STATUS_AVAILABLE |
    KVMFR_CLIPBOARD_STATUS_HAS_OWNER;
  if (status->version != KVMFR_CLIPBOARD_VERSION ||
      status->flags & ~validFlags || !status->generation ||
      status->formats & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL ||
      !status->lease ||
      status->slotBytes != KVMFR_CLIPBOARD_DATA_BYTES)
    return false;

  const bool available =
    (status->flags & KVMFR_CLIPBOARD_STATUS_AVAILABLE) != 0;
  const bool owner =
    (status->flags & KVMFR_CLIPBOARD_STATUS_HAS_OWNER) != 0;
  if (!available && (status->formats || owner))
    return false;
  return owner ? status->ownerClientID && status->ownerGeneration :
    !status->ownerClientID && !status->ownerGeneration;
}

static bool validateRecord(const LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record, size_t size,
    KVMFRClipboardQueueType queueType)
{
  if (size < sizeof(*record) ||
      record->version != KVMFR_CLIPBOARD_VERSION ||
      record->type < KVMFR_CLIPBOARD_MESSAGE_OFFER ||
      record->type > KVMFR_CLIPBOARD_MESSAGE_ACK ||
      record->generation != clipboard->claimGeneration ||
      record->length > KVMFR_CLIPBOARD_DATA_BYTES ||
      record->offset > UINT64_MAX - record->length)
    return false;

  if (record->type == KVMFR_CLIPBOARD_MESSAGE_DATA)
  {
    const unsigned operations =
      !!(record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN) +
      !!record->length + !!(record->flags & KVMFR_CLIPBOARD_FLAG_END);
    if (!record->transfer ||
        kvmfrClipboardTransferFromHelper(record->transfer) ||
        !kvmfrClipboardFormatValid(record->format) ||
        record->flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
          KVMFR_CLIPBOARD_FLAG_END) ||
        !operations ||
        size != sizeof(*record) + KVMFR_CLIPBOARD_DATA_BYTES ||
        queueType != KVMFR_CLIPBOARD_QUEUE_DATA)
      return false;
  }
  else if (queueType != KVMFR_CLIPBOARD_QUEUE_MESSAGE ||
      size != sizeof(*record) || record->length || record->flags)
    return false;

  switch (record->type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      return record->clipboardGeneration && record->token &&
        !(record->token & ~KVMFR_CLIPBOARD_FORMAT_MASK_ALL) &&
        !record->transfer && !record->offset && !record->size &&
        !record->format && !record->flags && !record->length &&
        !record->sequence;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      return record->clipboardGeneration && !record->token &&
        !record->transfer && !record->offset && !record->size &&
        !record->format && !record->flags && !record->length &&
        !record->sequence;

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      return record->clipboardGeneration &&
        kvmfrClipboardTransferFromHelper(record->transfer) &&
        kvmfrClipboardFormatValid(record->format) &&
        !record->offset && !record->size && !record->flags &&
        !record->token && !record->length && !record->sequence;

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
    {
      const uint64_t end = record->offset + record->length;
      if (!record->clipboardGeneration || record->token ||
          ((record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN) &&
           (record->offset || record->sequence)) ||
          ((record->flags & KVMFR_CLIPBOARD_FLAG_END) &&
           record->size != end))
        return false;
      if ((record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN) &&
          record->size != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
          end > record->size)
        return false;
      return true;
    }

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      return record->transfer && !record->offset && !record->size &&
        (!record->format || kvmfrClipboardFormatValid(record->format)) &&
        !record->flags && !record->length &&
        !record->sequence;

    default:
      return false;
  }
}

static bool validateReadChunkNL(const LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  if (clipboard->readRequest == LG_CLIPBOARD_REQUEST_INVALID ||
      record->transfer != clipboard->readTransfer ||
      record->clipboardGeneration !=
        clipboard->readClipboardGeneration ||
      record->format != clipboard->readFormat ||
      record->offset != clipboard->readOffset ||
      record->sequence != clipboard->readSequence)
    return false;

  const uint64_t end = record->offset + record->length;
  if (!record->length &&
      !(record->flags & (KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END)))
    return false;
  if (!clipboard->readBegan)
  {
    if (record->offset || record->sequence ||
        !(record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN))
      return false;
  }
  else if (record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN)
    return false;
  else if (!(record->flags & KVMFR_CLIPBOARD_FLAG_END) &&
      record->size != KVMFR_CLIPBOARD_SIZE_UNKNOWN)
    return false;

  const uint64_t hint = clipboard->readBegan ?
    clipboard->readSizeHint : record->size;
  if (hint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && end > hint)
    return false;
  if (record->flags & KVMFR_CLIPBOARD_FLAG_END)
    return record->size == end &&
      (hint == KVMFR_CLIPBOARD_SIZE_UNKNOWN || hint == end);
  return true;
}

static void advanceReadNL(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  if (!clipboard->readBegan)
  {
    clipboard->readBegan    = true;
    clipboard->readSizeHint = record->size;
  }
  clipboard->readOffset += record->length;
  ++clipboard->readSequence;
  if (record->flags & KVMFR_CLIPBOARD_FLAG_END)
    clearReadNL(clipboard);
}

static void dispatchOffer(LGMPClipboard * clipboard,
    KVMFRClipboardFormatFlags mask)
{
  LG_ClipboardData formats[LG_CLIPBOARD_DATA_NONE];
  const unsigned count = formatsFromMask(mask, formats);

  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->notice)
    events->notice(opaque, formats, count);
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchRelease(LGMPClipboard * clipboard)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->release)
    events->release(opaque);
  LG_UNLOCK(clipboard->eventLock);
}

static bool dispatchRequest(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  const bool result = events && events->request && events->request(opaque,
    record->transfer, fromWireFormat(record->format));
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static void dispatchCancel(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events)
  {
    const LG_ClipboardCancelReason reason = fromWireCancel(record->token);
    if (kvmfrClipboardTransferFromHelper(record->transfer))
    {
      if (events->requestCancel)
        events->requestCancel(opaque, record->transfer, reason);
    }
    else if (events->dataCancel)
      events->dataCancel(opaque, record->transfer, reason);
  }
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchDataCancel(LGMPClipboard * clipboard,
    LG_ClipboardRequest request, uint32_t reason)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->dataCancel)
    events->dataCancel(
      opaque, request, fromWireCancel(reason));
  LG_UNLOCK(clipboard->eventLock);
}

static LG_ClipboardResult dispatchData(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record, const uint8_t * data,
    enum HeldPhase phase)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);

  LG_ClipboardResult result = LG_CLIPBOARD_RESULT_FAILED;
  if (events)
  {
    switch (phase)
    {
      case HELD_PHASE_BEGIN:
        if (events->dataBegin)
          result = events->dataBegin(opaque, record->transfer,
            fromWireFormat(record->format), record->size);
        break;

      case HELD_PHASE_CHUNK:
        if (events->dataChunk)
          result = events->dataChunk(opaque, record->transfer,
            record->offset, data, record->length);
        break;

      case HELD_PHASE_END:
        if (events->dataEnd)
          result = events->dataEnd(
            opaque, record->transfer, record->size);
        break;

      case HELD_PHASE_NONE:
        break;
    }
  }
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static void dispatchReady(LGMPClipboard * clipboard,
    LG_ClipboardRequest request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->dataReady)
    events->dataReady(opaque, request);
  LG_UNLOCK(clipboard->eventLock);
}

static void applyStatusNL(LGMPClipboard * clipboard,
    const KVMFRClipboardStatus * status, uint32_t serial,
    bool * changed)
{
  const bool     wasValid      = clipboard->statusValid;
  const bool     wasAvailable  = clipboard->available;
  const uint32_t oldGeneration = clipboard->endpointGeneration;
  clipboard->statusValid         = true;
  clipboard->statusSerial        = serial;
  const bool endpointAvailable =
    (status->flags & KVMFR_CLIPBOARD_STATUS_AVAILABLE) != 0;
  clipboard->endpointGeneration = status->generation;

  const bool owned = (status->flags & KVMFR_CLIPBOARD_STATUS_HAS_OWNER) &&
    clipboard->claimed && status->ownerClientID == clipboard->clientID &&
    status->ownerGeneration == clipboard->claimGeneration;
  const bool ownedByOther =
    (status->flags & KVMFR_CLIPBOARD_STATUS_HAS_OWNER) && !owned;
  clipboard->available = endpointAvailable && !ownedByOther;
  bool restore         = false;

  if (!endpointAvailable || oldGeneration != status->generation ||
      ownedByOther)
  {
    clearProtocolNL(clipboard);
    restore = endpointAvailable && !ownedByOther;
  }
  else if (status->flags & KVMFR_CLIPBOARD_STATUS_HAS_OWNER)
    clipboard->ownerConfirmed = owned;
  else
  {
    if (clipboard->claimed && clipboard->ownerConfirmed)
    {
      clearProtocolNL(clipboard);
      restore = endpointAvailable;
    }
    clipboard->ownerConfirmed = false;
  }

  if (restore && !restoreClipboardNL(clipboard))
    clearProtocolNL(clipboard);

  *changed = !wasValid || wasAvailable != clipboard->available ||
    oldGeneration != clipboard->endpointGeneration;
  if (*changed)
    nextNonzero(&clipboard->providerGeneration);
}

static bool flushPendingNL(LGMPClipboard * clipboard)
{
  while (clipboard->pendingCount)
  {
    KVMFRClipboardMessage * record =
      &pendingAt(clipboard, 0)->record;
    const LGMP_STATUS status = lgmpClientTrySendData(
      clipboard->queue, record, sizeof(*record), NULL);
    if (status == LGMP_ERR_QUEUE_BUSY || status == LGMP_ERR_QUEUE_FULL)
      return true;
    if (status != LGMP_OK)
    {
      connectionFailed(clipboard, status);
      return false;
    }
    if (record->type == KVMFR_CLIPBOARD_MESSAGE_CLAIM)
      clipboard->publishedClaimGeneration = record->generation;
    else if (record->type == KVMFR_CLIPBOARD_MESSAGE_RELEASE &&
        clipboard->publishedClaimGeneration == record->generation)
      clipboard->publishedClaimGeneration = 0;
    clipboard->lastSend = microtime();
    clipboard->pendingHead =
      (clipboard->pendingHead + 1) % CLIPBOARD_PENDING_MAX;
    --clipboard->pendingCount;
  }
  return true;
}

static bool processHeld(LGMPClipboard * clipboard)
{
  for (;;)
  {
    KVMFRClipboardMessage record;
    const uint8_t * data;
    enum HeldPhase phase;
    LG_LOCK(clipboard->lock);
    if (!clipboard->held || !clipboard->heldReady)
    {
      LG_UNLOCK(clipboard->lock);
      return true;
    }
    record = clipboard->heldRecord;
    data = record.length ?
      (const uint8_t *)clipboard->heldMessage.mem + sizeof(record) : NULL;
    phase = clipboard->heldPhase;
    clipboard->heldReady = false;
    LG_UNLOCK(clipboard->lock);

    const LG_ClipboardResult result =
      dispatchData(clipboard, &record, data, phase);
    LG_LOCK(clipboard->lock);
    if (!clipboard->held)
    {
      LG_UNLOCK(clipboard->lock);
      return true;
    }
    if (result == LG_CLIPBOARD_RESULT_BLOCKED)
    {
      LG_UNLOCK(clipboard->lock);
      return true;
    }

    if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
    {
      switch (phase)
      {
        case HELD_PHASE_BEGIN:
          clipboard->heldPhase = record.length ? HELD_PHASE_CHUNK :
            (record.flags & KVMFR_CLIPBOARD_FLAG_END) ?
              HELD_PHASE_END : HELD_PHASE_NONE;
          break;

        case HELD_PHASE_CHUNK:
          clipboard->heldPhase =
            (record.flags & KVMFR_CLIPBOARD_FLAG_END) ?
              HELD_PHASE_END : HELD_PHASE_NONE;
          break;

        case HELD_PHASE_END:
          clipboard->heldPhase = HELD_PHASE_NONE;
          break;

        case HELD_PHASE_NONE:
          break;
      }
    }
    else
      clipboard->heldPhase = HELD_PHASE_NONE;

    if (clipboard->heldPhase != HELD_PHASE_NONE)
    {
      clipboard->heldReady = true;
      LG_UNLOCK(clipboard->lock);
      continue;
    }

    const bool matchingRead =
      clipboard->readRequest == record.transfer &&
      clipboard->readTransfer;
    if (result == LG_CLIPBOARD_RESULT_FAILED && matchingRead)
    {
      KVMFRClipboardMessage cancel = { 0 };
      cancel.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
      cancel.clipboardGeneration = clipboard->readClipboardGeneration;
      cancel.transfer            = clipboard->readTransfer;
      cancel.format              = clipboard->readFormat;
      cancel.token               = LG_CLIPBOARD_CANCEL_INVALID;
      enqueueRecordNL(clipboard, cancel);
    }
    if (matchingRead && result == LG_CLIPBOARD_RESULT_ACCEPTED)
      advanceReadNL(clipboard, &record);
    else if (matchingRead)
      clearReadNL(clipboard);

    const LGMP_STATUS done = lgmpClientMessageDone(clipboard->queue);
    clipboard->held = false;
    memset(&clipboard->heldMessage, 0, sizeof(clipboard->heldMessage));
    memset(&clipboard->heldRecord, 0, sizeof(clipboard->heldRecord));
    if (done != LGMP_OK)
    {
      connectionFailed(clipboard, done);
      LG_UNLOCK(clipboard->lock);
      return false;
    }
    LG_UNLOCK(clipboard->lock);
    return true;
  }
}

static bool processMessage(LGMPClipboard * clipboard)
{
  LG_LOCK(clipboard->lock);
  if (!clipboard->connected || !clipboard->queue || clipboard->held)
  {
    LG_UNLOCK(clipboard->lock);
    return true;
  }

  LGMPMessage message;
  LGMP_STATUS status = lgmpClientProcess(clipboard->queue, &message);
  if (status == LGMP_ERR_QUEUE_EMPTY)
  {
    LG_UNLOCK(clipboard->lock);
    return true;
  }
  if (status != LGMP_OK)
  {
    connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    return false;
  }

  const KVMFRClipboardQueueType type =
    KVMFR_CLIPBOARD_QUEUE_TYPE(message.udata);
  if (type == KVMFR_CLIPBOARD_QUEUE_STATUS)
  {
    KVMFRClipboardStatus snapshot = { 0 };
    const bool valid = message.size == sizeof(snapshot);
    if (valid)
      memcpy(&snapshot, message.mem, sizeof(snapshot));
    status = lgmpClientMessageDone(clipboard->queue);
    bool changed = false;
    if (status == LGMP_OK && valid && validStatus(&snapshot))
    {
      const uint32_t serial = KVMFR_CLIPBOARD_QUEUE_SERIAL(message.udata);
      if (!clipboard->statusValid ||
          (int32_t)(serial - clipboard->statusSerial) > 0)
        applyStatusNL(clipboard, &snapshot, serial, &changed);
    }
    else if (status == LGMP_OK)
      DEBUG_WARN("Ignoring invalid LGMP clipboard status");
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    if (changed)
      notifyStatus(clipboard);
    return status == LGMP_OK;
  }

  if (type == KVMFR_CLIPBOARD_QUEUE_GRANT)
  {
    KVMFRClipboardSlotHeader * header = message.mem;
    const uint32_t token = KVMFR_CLIPBOARD_QUEUE_SERIAL(message.udata);
    const bool valid = message.size ==
        sizeof(*header) + KVMFR_CLIPBOARD_DATA_BYTES &&
      header->version == KVMFR_CLIPBOARD_VERSION &&
      header->type == KVMFR_CLIPBOARD_MESSAGE_GRANT &&
      header->generation == clipboard->claimGeneration &&
      header->size == KVMFR_CLIPBOARD_DATA_BYTES &&
      header->token == token && token > 0 &&
      token <= KVMFR_CLIPBOARD_SLOT_COUNT &&
      !header->sequence && !header->clipboardGeneration &&
      !header->transfer && !header->offset && !header->format &&
      !header->flags && !header->length;
    bool stored = false;
    if (valid)
    {
      struct Grant * grant = &clipboard->grants[token - 1];
      if (!grant->available)
      {
        grant->header    = header;
        grant->data      = (uint8_t *)(header + 1);
        grant->token     = token;
        grant->available = true;
        stored = true;
      }
    }
    status = lgmpClientMessageDone(clipboard->queue);
    const bool ready = stored && clipboard->writeBlocked;
    const LG_ClipboardRequest request = clipboard->writeBlockedRequest;
    if (ready)
      clipboard->writeBlocked = false;
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    if (!stored)
      DEBUG_WARN("Ignoring invalid LGMP clipboard grant");
    if (ready)
      dispatchReady(clipboard, request);
    return status == LGMP_OK;
  }

  if ((type != KVMFR_CLIPBOARD_QUEUE_MESSAGE &&
       type != KVMFR_CLIPBOARD_QUEUE_DATA) ||
      message.size < sizeof(KVMFRClipboardMessage))
  {
    status = lgmpClientMessageDone(clipboard->queue);
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    DEBUG_WARN("Ignoring invalid LGMP clipboard message");
    return status == LGMP_OK;
  }

  KVMFRClipboardMessage record;
  memcpy(&record, message.mem, sizeof(record));
  const bool valid = validateRecord(
    clipboard, &record, message.size, type);
  if (!valid)
  {
    status = lgmpClientMessageDone(clipboard->queue);
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    DEBUG_WARN("Ignoring malformed LGMP clipboard record");
    return status == LGMP_OK;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA)
  {
    if (!validateReadChunkNL(clipboard, &record))
    {
      status = lgmpClientMessageDone(clipboard->queue);
      if (status != LGMP_OK)
        connectionFailed(clipboard, status);
      LG_UNLOCK(clipboard->lock);
      DEBUG_WARN("Ignoring stale or malformed LGMP clipboard data");
      return status == LGMP_OK;
    }
    record.transfer = clipboard->readRequest;
    clipboard->held        = true;
    clipboard->heldReady   = true;
    clipboard->heldMessage = message;
    clipboard->heldRecord  = record;
    clipboard->heldPhase =
      (record.flags & KVMFR_CLIPBOARD_FLAG_BEGIN) ? HELD_PHASE_BEGIN :
      record.length ? HELD_PHASE_CHUNK : HELD_PHASE_END;
    LG_UNLOCK(clipboard->lock);
    return processHeld(clipboard);
  }

  status = lgmpClientMessageDone(clipboard->queue);
  if (status != LGMP_OK)
  {
    connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  bool dispatch = true;
  LG_ClipboardRequest cancelledRead = LG_CLIPBOARD_REQUEST_INVALID;
  switch (record.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      clipboard->remoteClipboardGeneration = record.clipboardGeneration;
      clipboard->remoteFormats = record.token;
      clearReadNL(clipboard);
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      clipboard->remoteClipboardGeneration = record.clipboardGeneration;
      clipboard->remoteFormats = 0;
      clearReadNL(clipboard);
      break;

    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      if (clipboard->writeTransfer != LG_CLIPBOARD_REQUEST_INVALID ||
          record.clipboardGeneration !=
            clipboard->localClipboardGeneration ||
          !(clipboard->localFormats &
            kvmfrClipboardFormatFlag(record.format)))
      {
        dispatch = false;
        break;
      }
      clearWriteNL(clipboard);
      clipboard->writeTransfer = record.transfer;
      clipboard->writeClipboardGeneration = record.clipboardGeneration;
      clipboard->writeFormat = record.format;
      break;

    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
    {
      const bool write = clipboard->writeTransfer == record.transfer;
      const bool read = clipboard->readTransfer == record.transfer;
      if (!write && !read)
      {
        dispatch = false;
        break;
      }
      const uint64_t clipboardGeneration = write ?
        clipboard->writeClipboardGeneration :
        clipboard->readClipboardGeneration;
      const KVMFRClipboardFormat format = write ?
        clipboard->writeFormat : clipboard->readFormat;
      if ((record.clipboardGeneration &&
           record.clipboardGeneration != clipboardGeneration) ||
          (record.format && record.format != format))
      {
        dispatch = false;
        break;
      }
      if (write)
        clearWriteNL(clipboard);
      if (read)
      {
        cancelledRead = clipboard->readRequest;
        clearReadNL(clipboard);
      }
      break;
    }
  }
  LG_UNLOCK(clipboard->lock);

  if (!dispatch)
  {
    DEBUG_WARN("Ignoring stale LGMP clipboard control record");
    return true;
  }

  switch (record.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      dispatchOffer(clipboard, record.token);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_CLEAR:
      dispatchRelease(clipboard);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_REQUEST:
      if (!dispatchRequest(clipboard, &record))
      {
        LG_LOCK(clipboard->lock);
        KVMFRClipboardMessage cancel = { 0 };
        cancel.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
        cancel.clipboardGeneration = record.clipboardGeneration;
        cancel.transfer            = record.transfer;
        cancel.format              = record.format;
        cancel.token               = LG_CLIPBOARD_CANCEL_UNAVAILABLE;
        enqueueRecordNL(clipboard, cancel);
        clearWriteNL(clipboard);
        LG_UNLOCK(clipboard->lock);
        signalWorker(clipboard);
      }
      break;
    case KVMFR_CLIPBOARD_MESSAGE_CANCEL:
      if (cancelledRead != LG_CLIPBOARD_REQUEST_INVALID)
        dispatchDataCancel(clipboard, cancelledRead, record.token);
      else
        dispatchCancel(clipboard, &record);
      break;
  }
  return true;
}

static int clipboardThread(void * opaque)
{
  LGMPClipboard * clipboard = opaque;
  while (!atomic_load_explicit(&clipboard->stop, memory_order_acquire))
  {
    if (!processHeld(clipboard) || !processMessage(clipboard))
      break;

    LG_ClipboardRequest readyRequest = LG_CLIPBOARD_REQUEST_INVALID;
    LG_LOCK(clipboard->lock);
    flushPendingNL(clipboard);
    const uint64_t now = microtime();
    if (clipboard->connected && clipboard->claimed &&
        !clipboard->pendingCount &&
        now - clipboard->lastSend >= CLIPBOARD_KEEPALIVE_US)
      enqueueTypeNL(clipboard, KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE);
    if (clipboard->writeBlocked &&
        clipboard->pendingCount < CLIPBOARD_PENDING_MAX &&
        availableGrantNL(clipboard))
    {
      readyRequest = clipboard->writeBlockedRequest;
      clipboard->writeBlocked = false;
    }
    const bool retry = clipboard->pendingCount != 0;
    LG_UNLOCK(clipboard->lock);

    if (readyRequest != LG_CLIPBOARD_REQUEST_INVALID)
      dispatchReady(clipboard, readyRequest);

    if (atomic_load_explicit(&clipboard->stop, memory_order_acquire))
      break;
    lgWaitEvent(clipboard->event,
      retry ? CLIPBOARD_RETRY_MS : CLIPBOARD_POLL_MS);
  }
  notifyStatus(clipboard);
  return 0;
}

bool lgmpClipboard_create(PLGMPClient client, LGMPClipboard ** result)
{
  if (!client || !result)
    return false;
  LGMPClipboard * clipboard = calloc(1, sizeof(*clipboard));
  if (!clipboard)
    return false;
  clipboard->client = client;
  clipboard->providerGeneration = 1;
  LG_LOCK_INIT(clipboard->lock);
  LG_LOCK_INIT(clipboard->eventLock);
  LG_LOCK_INIT(clipboard->statusLock);
  atomic_init(&clipboard->stop, false);
  *result = clipboard;
  return true;
}

void lgmpClipboard_destroy(LGMPClipboard ** clipboard)
{
  if (!clipboard || !*clipboard)
    return;
  lgmpClipboard_disconnect(*clipboard);
  LG_LOCK_FREE((*clipboard)->statusLock);
  LG_LOCK_FREE((*clipboard)->eventLock);
  LG_LOCK_FREE((*clipboard)->lock);
  free(*clipboard);
  *clipboard = NULL;
}

bool lgmpClipboard_connect(LGMPClipboard * clipboard, uint32_t clientID)
{
  if (!clipboard || !clientID)
    return false;

  LG_LOCK(clipboard->lock);
  if (clipboard->connected)
  {
    const bool same = clipboard->clientID == clientID;
    LG_UNLOCK(clipboard->lock);
    return same;
  }
  if (clipboard->thread || clipboard->queue)
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }

  LGMP_STATUS status = lgmpClientSubscribe(
    clipboard->client, LGMP_Q_CLIPBOARD, &clipboard->queue);
  if (status != LGMP_OK)
  {
    LG_UNLOCK(clipboard->lock);
    DEBUG_WARN("Failed to subscribe to LGMP clipboard queue: %s",
      lgmpStatusString(status));
    return false;
  }
  clipboard->event = lgCreateEvent(true, 0);
  if (!clipboard->event)
  {
    lgmpClientUnsubscribe(&clipboard->queue);
    LG_UNLOCK(clipboard->lock);
    return false;
  }

  clipboard->connected             = true;
  clipboard->available             = false;
  clipboard->statusValid           = false;
  clipboard->claimed               = false;
  clipboard->ownerConfirmed        = false;
  clipboard->clientID              = clientID;
  clipboard->endpointGeneration    = 0;
  clipboard->statusSerial          = 0;
  clipboard->pendingHead           = 0;
  clipboard->pendingCount          = 0;
  clipboard->held                  = false;
  clipboard->heldReady             = false;
  clearWriteNL(clipboard);
  clipboard->remoteFormats         = 0;
  clipboard->remoteClipboardGeneration = 0;
  clearReadNL(clipboard);
  clipboard->publishedClaimGeneration = 0;
  memset(clipboard->grants, 0, sizeof(clipboard->grants));
  atomic_store_explicit(&clipboard->stop, false, memory_order_release);

  LGThread * thread;
  if (lgCreateThread("lgmpClipboard", clipboardThread,
      clipboard, &thread))
  {
    clipboard->thread = thread;
    LG_UNLOCK(clipboard->lock);
    return true;
  }

  clipboard->connected = false;
  LGEvent * event = clipboard->event;
  PLGMPClientQueue queue = clipboard->queue;
  clipboard->event = NULL;
  clipboard->queue = NULL;
  LG_UNLOCK(clipboard->lock);
  lgmpClientUnsubscribe(&queue);
  lgFreeEvent(event);
  return false;
}

static void releaseOnDisconnect(LGMPClipboard * clipboard)
{
  if (!clipboard->queue || !clipboard->publishedClaimGeneration)
    return;

  const KVMFRClipboardMessage release =
  {
    .version    = KVMFR_CLIPBOARD_VERSION,
    .type       = KVMFR_CLIPBOARD_MESSAGE_RELEASE,
    .generation = clipboard->publishedClaimGeneration,
  };
  const uint64_t deadline =
    microtime() + CLIPBOARD_RELEASE_TIMEOUT_US;
  uint32_t serial = 0;
  LGMP_STATUS status;
  do
  {
    status = lgmpClientTrySendData(clipboard->queue,
      &release, sizeof(release), &serial);
    if (status == LGMP_OK)
      break;
    if (status != LGMP_ERR_QUEUE_BUSY && status != LGMP_ERR_QUEUE_FULL)
      return;
    if (clipboard->event)
      lgWaitEvent(clipboard->event, CLIPBOARD_RETRY_MS);
  }
  while (microtime() < deadline);

  if (status != LGMP_OK)
  {
    DEBUG_WARN("Timed out releasing LGMP clipboard ownership");
    return;
  }

  do
  {
    uint32_t processed;
    status = lgmpClientGetSerial(clipboard->queue, &processed);
    if (status != LGMP_OK)
      return;
    if ((int32_t)(processed - serial) >= 0)
      return;
    if (clipboard->event)
      lgWaitEvent(clipboard->event, CLIPBOARD_RETRY_MS);
  }
  while (microtime() < deadline);
  DEBUG_WARN("Timed out waiting for LGMP clipboard release");
}

void lgmpClipboard_disconnect(LGMPClipboard * clipboard)
{
  if (!clipboard)
    return;

  LG_LOCK(clipboard->lock);
  if (!clipboard->thread && !clipboard->queue)
  {
    LG_UNLOCK(clipboard->lock);
    return;
  }
  if (clipboard->connected || clipboard->available)
    nextNonzero(&clipboard->providerGeneration);
  clipboard->connected = false;
  clipboard->available = false;
  clipboard->claimed   = false;
  atomic_store_explicit(&clipboard->stop, true, memory_order_release);
  LGThread * thread = clipboard->thread;
  LGEvent * event = clipboard->event;
  LG_UNLOCK(clipboard->lock);
  if (event)
    lgSignalEvent(event);
  if (thread)
    lgJoinThread(thread, NULL);

  LG_LOCK(clipboard->lock);
  if (clipboard->held && clipboard->queue)
    lgmpClientMessageDone(clipboard->queue);
  releaseOnDisconnect(clipboard);
  PLGMPClientQueue queue = clipboard->queue;
  clipboard->queue  = NULL;
  clipboard->thread = NULL;
  clipboard->event  = NULL;
  clipboard->held   = false;
  clearProtocolNL(clipboard);
  LG_UNLOCK(clipboard->lock);
  if (queue)
    lgmpClientUnsubscribe(&queue);
  if (event)
    lgFreeEvent(event);
}

static void setStatusListener(void * opaque,
    LG_ClipboardStatusFn callback, void * callbackOpaque)
{
  LGMPClipboard * clipboard = opaque;
  LG_LOCK(clipboard->statusLock);
  LG_LOCK(clipboard->lock);
  clipboard->statusCallback = callback;
  clipboard->statusOpaque   = callbackOpaque;
  LG_ClipboardStatus status =
  {
    .available  = clipboard->connected && clipboard->available,
    .generation = clipboard->providerGeneration,
  };
  LG_UNLOCK(clipboard->lock);
  if (callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(clipboard->statusLock);
}

static bool attach(void * opaque, const LG_ClipboardEventOps * events,
    void * eventOpaque)
{
  LGMPClipboard * clipboard = opaque;
  KVMFRClipboardFormatFlags formats;
  LG_ClipboardData replay[LG_CLIPBOARD_DATA_NONE];
  bool available;
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  available = clipboard->connected && clipboard->available && events;
  if (available)
  {
    clipboard->events      = events;
    clipboard->eventOpaque = eventOpaque;
    if (!ensureClaimNL(clipboard))
    {
      clipboard->events      = NULL;
      clipboard->eventOpaque = NULL;
      available = false;
    }
  }
  formats = clipboard->remoteFormats;
  LG_UNLOCK(clipboard->lock);
  const unsigned replayCount = available ?
    formatsFromMask(formats, replay) : 0;
  if (replayCount && events->notice)
    events->notice(eventOpaque, replay, replayCount);
  LG_UNLOCK(clipboard->eventLock);
  if (available)
    signalWorker(clipboard);
  return available;
}

static void detach(void * opaque)
{
  LGMPClipboard * clipboard = opaque;
  bool wake = false;
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  clipboard->events      = NULL;
  clipboard->eventOpaque = NULL;
  if (clipboard->claimed)
  {
    wake = enqueueTypeNL(
      clipboard, KVMFR_CLIPBOARD_MESSAGE_RELEASE);
    clipboard->claimed = false;
  }
  LG_UNLOCK(clipboard->lock);
  LG_UNLOCK(clipboard->eventLock);
  if (wake)
    signalWorker(clipboard);
}

static bool releaseClipboard(void * opaque)
{
  LGMPClipboard * clipboard = opaque;
  LG_LOCK(clipboard->lock);
  KVMFRClipboardMessage clear = { 0 };
  clear.type                = KVMFR_CLIPBOARD_MESSAGE_CLEAR;
  clear.clipboardGeneration = clipboard->localClipboardGeneration + 1;
  if (!clear.clipboardGeneration)
    ++clear.clipboardGeneration;
  const unsigned required = clipboard->claimed ? 1U : 2U;
  const bool result =
    clipboard->pendingCount <= CLIPBOARD_PENDING_MAX - required &&
    ensureClaimNL(clipboard) && enqueueRecordNL(clipboard, clear);
  if (result)
  {
    clipboard->localClipboardGeneration = clear.clipboardGeneration;
    clipboard->localFormats             = 0;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool notifyTypes(void * opaque, const LG_ClipboardData types[],
    size_t count)
{
  LGMPClipboard * clipboard = opaque;
  if (!types || !count || count > LG_CLIPBOARD_DATA_NONE)
    return false;
  for (size_t i = 0; i < count; ++i)
    if (!kvmfrClipboardFormatValid(toWireFormat(types[i])))
      return false;
  const KVMFRClipboardFormatFlags formats = formatMask(types, count);

  LG_LOCK(clipboard->lock);
  KVMFRClipboardMessage offer = { 0 };
  offer.type                = KVMFR_CLIPBOARD_MESSAGE_OFFER;
  offer.token               = formats;
  offer.clipboardGeneration = clipboard->localClipboardGeneration + 1;
  if (!offer.clipboardGeneration)
    ++offer.clipboardGeneration;
  const unsigned required = clipboard->claimed ? 1U : 2U;
  const bool result =
    clipboard->pendingCount <= CLIPBOARD_PENDING_MAX - required &&
    ensureClaimNL(clipboard) &&
    enqueueRecordNL(clipboard, offer);
  if (result)
  {
    clipboard->localClipboardGeneration = offer.clipboardGeneration;
    clipboard->localFormats             = formats;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static LG_ClipboardResult dataBegin(void * opaque,
    LG_ClipboardRequest request, LG_ClipboardData type, uint64_t sizeHint)
{
  LGMPClipboard * clipboard = opaque;
  const KVMFRClipboardFormat format = toWireFormat(type);
  if (!kvmfrClipboardTransferFromHelper(request) ||
      !kvmfrClipboardFormatValid(format))
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->lock);
  if (!clipboard->connected || !clipboard->available ||
      !clipboard->claimed || clipboard->writeBegan ||
      clipboard->writeTransfer != request ||
      clipboard->writeFormat != format ||
      !clipboard->writeClipboardGeneration)
  {
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  clipboard->writeSizeHint  = sizeHint;
  clipboard->writeOffset    = 0;
  clipboard->writeSequence  = 0;
  clipboard->writeBegan     = true;
  clipboard->writeWireBegan = false;
  clipboard->writeBlocked   = false;
  LG_UNLOCK(clipboard->lock);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult dataChunk(void * opaque,
    LG_ClipboardRequest request, uint64_t offset,
    const void * data, size_t size)
{
  LGMPClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) || !data || !size ||
      size > KVMFR_CLIPBOARD_DATA_BYTES || offset > UINT64_MAX - size)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard->lock);
  if (!clipboard->connected || !clipboard->available ||
      !clipboard->claimed || !clipboard->writeBegan ||
      clipboard->writeTransfer != request ||
      clipboard->writeOffset != offset ||
      (clipboard->writeSizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
       offset + size > clipboard->writeSizeHint))
  {
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_DATA;
  record.clipboardGeneration = clipboard->writeClipboardGeneration;
  record.sequence            = clipboard->writeSequence;
  record.transfer            = request;
  record.offset              = offset;
  record.size                = clipboard->writeWireBegan ?
    KVMFR_CLIPBOARD_SIZE_UNKNOWN : clipboard->writeSizeHint;
  record.format              = clipboard->writeFormat;
  record.flags               = clipboard->writeWireBegan ?
    0 : KVMFR_CLIPBOARD_FLAG_BEGIN;
  record.length              = (uint32_t)size;
  const bool result = enqueueDataNL(clipboard, record, data);
  if (result)
  {
    clipboard->writeOffset += size;
    ++clipboard->writeSequence;
    clipboard->writeWireBegan = true;
  }
  else
  {
    clipboard->writeBlocked = true;
    clipboard->writeBlockedRequest = request;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_BLOCKED;
}

static LG_ClipboardResult dataEnd(void * opaque,
    LG_ClipboardRequest request, uint64_t finalSize)
{
  LGMPClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request))
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(clipboard->lock);
  if (!clipboard->connected || !clipboard->available ||
      !clipboard->claimed || !clipboard->writeBegan ||
      clipboard->writeTransfer != request ||
      clipboard->writeOffset != finalSize ||
      (clipboard->writeSizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
       clipboard->writeSizeHint != finalSize))
  {
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_DATA;
  record.clipboardGeneration = clipboard->writeClipboardGeneration;
  record.transfer            = request;
  record.format              = clipboard->writeFormat;
  record.flags               = KVMFR_CLIPBOARD_FLAG_END |
    (clipboard->writeWireBegan ? 0 : KVMFR_CLIPBOARD_FLAG_BEGIN);
  record.size                 = finalSize;
  record.offset               = finalSize;
  record.sequence             = clipboard->writeSequence;
  const bool result = enqueueDataNL(clipboard, record, NULL);
  if (result)
    clearWriteNL(clipboard);
  else
  {
    clipboard->writeBlocked = true;
    clipboard->writeBlockedRequest = request;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_BLOCKED;
}

static bool dataCancel(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  LGMPClipboard * clipboard = opaque;
  if (!kvmfrClipboardTransferFromHelper(request) ||
      (unsigned)reason > LG_CLIPBOARD_CANCEL_INVALID)
    return false;
  LG_LOCK(clipboard->lock);
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_CANCEL;
  record.clipboardGeneration = clipboard->writeClipboardGeneration;
  record.transfer            = request;
  record.format              = clipboard->writeFormat;
  record.token               = reason;
  const bool result = clipboard->claimed &&
    clipboard->writeTransfer == request &&
    enqueueRecordNL(clipboard, record);
  if (clipboard->writeTransfer == request)
    clearWriteNL(clipboard);
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool dataReady(void * opaque, LG_ClipboardRequest request)
{
  LGMPClipboard * clipboard = opaque;
  LG_LOCK(clipboard->lock);
  const bool result = clipboard->held &&
    clipboard->heldRecord.transfer == request;
  if (result)
    clipboard->heldReady = true;
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool requestData(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type)
{
  LGMPClipboard * clipboard = opaque;
  const KVMFRClipboardFormat format = toWireFormat(type);
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      !kvmfrClipboardFormatValid(format))
    return false;
  LG_LOCK(clipboard->lock);
  if (clipboard->readRequest || !clipboard->remoteClipboardGeneration ||
      !(clipboard->remoteFormats & kvmfrClipboardFormatFlag(format)))
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type       = KVMFR_CLIPBOARD_MESSAGE_REQUEST;
  record.transfer   = nextClientTransfer(clipboard);
  record.format     = format;
  record.clipboardGeneration = clipboard->remoteClipboardGeneration;
  const bool result = ensureClaimNL(clipboard) &&
    enqueueRecordNL(clipboard, record);
  if (result)
  {
    clipboard->readRequest             = request;
    clipboard->readTransfer            = record.transfer;
    clipboard->readClipboardGeneration =
      record.clipboardGeneration;
    clipboard->readSizeHint = 0;
    clipboard->readOffset   = 0;
    clipboard->readFormat   = format;
    clipboard->readSequence = 0;
    clipboard->readBegan    = false;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static const LG_ClipboardOps CLIPBOARD_OPS =
{
  .name              = "LGMP",
  .setStatusListener = setStatusListener,
  .attach            = attach,
  .detach            = detach,
  .release           = releaseClipboard,
  .notifyTypes       = notifyTypes,
  .dataBegin         = dataBegin,
  .dataChunk         = dataChunk,
  .dataEnd           = dataEnd,
  .dataCancel        = dataCancel,
  .dataReady         = dataReady,
  .request           = requestData,
};

const LG_ClipboardOps * lgmpClipboard_getOps(void)
{
  return &CLIPBOARD_OPS;
}
