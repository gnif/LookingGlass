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

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define CLIPBOARD_PENDING_MAX         128U
#define CLIPBOARD_PENDING_NORMAL_MAX  64U
#define CLIPBOARD_DRAIN_MAX           64U
#define CLIPBOARD_GRANTS              KVMFR_CLIPBOARD_SLOT_COUNT
#define CLIPBOARD_IDLE_POLL_MS        10U
#define CLIPBOARD_ACTIVE_POLL_MS      1U
#define CLIPBOARD_KEEPALIVE_US        UINT64_C(200000)
#define CLIPBOARD_RELEASE_TIMEOUT_US  UINT64_C(100000)

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

struct FileTransfer
{
  struct FileTransfer       * next;
  LG_ClipboardFileRequest     request;
  uint64_t                    sizeHint;
  uint64_t                    responseOffset;
  uint32_t                    sequence;
  bool                        began;
  bool                        blocked;
  bool                        wireEnded;
};

struct FileAcquisition
{
  struct FileAcquisition * next;
  uint64_t                 dataset;
  uint64_t                 acquisition;
  bool                     fromHelper;
  bool                     active;
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
  PLGMPClient                  client;
  PLGMPClientQueue             queue;
  LG_Lock                      lock;
  LG_Lock                      eventLock;
  LG_Lock                      statusLock;
  LGEvent                    * event;
  LGThread                   * thread;
  atomic_bool                  stop;

  bool                         connected;
  bool                         available;
  bool                         statusValid;
  bool                         claimed;
  bool                         ownerConfirmed;
  uint32_t                     clientID;
  uint32_t                     endpointGeneration;
  uint32_t                     providerGeneration;
  uint32_t                     claimGeneration;
  uint32_t                     publishedClaimGeneration;
  uint32_t                     statusSerial;
  uint64_t                     lastSend;

  struct PendingRecord         pending[CLIPBOARD_PENDING_MAX];
  unsigned                     pendingHead;
  unsigned                     pendingCount;

  struct Grant                 grants[CLIPBOARD_GRANTS];
  bool                         writeBlocked;
  LG_ClipboardRequest          writeBlockedRequest;
  LG_ClipboardRequest          writeTransfer;
  uint64_t                     writeClipboardGeneration;
  uint64_t                     writeSizeHint;
  KVMFRClipboardFormat         writeFormat;
  uint64_t                     writeOffset;
  uint32_t                     writeSequence;
  bool                         writeBegan;
  bool                         writeWireBegan;

  bool                         held;
  bool                         heldReady;
  enum HeldPhase               heldPhase;
  LGMPMessage                  heldMessage;
  KVMFRClipboardMessage        heldRecord;
  bool                         heldFile;
  LG_ClipboardFileRequest      heldFileRequest;

  uint64_t                     localClipboardGeneration;
  uint64_t                     localGenerationSerial;
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

  struct FileTransfer        * fileReads;
  struct FileTransfer        * fileWrites;
  struct FileAcquisition     * fileAcquisitions;
  struct FileTransfer        * retiredFileReads;
  struct FileTransfer        * retiredFileWrites;
  struct FileAcquisition     * retiredFileAcquisitions;

  const LG_ClipboardEventOps * events;
  void                       * eventOpaque;
  LG_ClipboardStatusFn         statusCallback;
  void                       * statusOpaque;
};

static LG_ClipboardData fromWireFormat(KVMFRClipboardFormat format)
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

static KVMFRClipboardFormat toWireFormat(LG_ClipboardData format)
{
  switch (format)
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

static LG_ClipboardFileOperation fromWireFileOperation(uint32_t operation)
{
  return operation == KVMFR_CLIPBOARD_FILE_OP_READ ?
    LG_CLIPBOARD_FILE_READ : LG_CLIPBOARD_FILE_LIST;
}

static uint32_t toWireFileOperation(LG_ClipboardFileOperation operation)
{
  return operation == LG_CLIPBOARD_FILE_READ ?
    KVMFR_CLIPBOARD_FILE_OP_READ : KVMFR_CLIPBOARD_FILE_OP_LIST;
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
       format <= KVMFR_CLIPBOARD_FORMAT_FILES; ++format)
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

static uint64_t nextClientGeneration(LGMPClipboard * clipboard)
{
  if (++clipboard->localGenerationSerial == 0 ||
      kvmfrClipboardTransferFromHelper(
        clipboard->localGenerationSerial))
    clipboard->localGenerationSerial = 1;
  return clipboard->localGenerationSerial;
}

static bool randomBytes(void * buffer, size_t size)
{
  uint8_t * output = buffer;
  while (size)
  {
    const ssize_t received = getrandom(output, size, 0);
    if (received < 0 && errno == EINTR)
      continue;
    if (received <= 0)
    {
      if (!received)
        errno = EIO;
      return false;
    }
    output += (size_t)received;
    size   -= (size_t)received;
  }
  return true;
}

static LG_ClipboardFileError fromWireFileError(uint32_t error)
{
  switch (error)
  {
    case KVMFR_CLIPBOARD_FILE_ERROR_NONE:
      return LG_CLIPBOARD_FILE_ERROR_NONE;
    case KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND:
      return LG_CLIPBOARD_FILE_ERROR_NOT_FOUND;
    case KVMFR_CLIPBOARD_FILE_ERROR_ACCESS:
      return LG_CLIPBOARD_FILE_ERROR_ACCESS;
    case KVMFR_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY:
      return LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
    case KVMFR_CLIPBOARD_FILE_ERROR_IS_DIRECTORY:
      return LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY;
    case KVMFR_CLIPBOARD_FILE_ERROR_IO:
      return LG_CLIPBOARD_FILE_ERROR_IO;
    case KVMFR_CLIPBOARD_FILE_ERROR_INVALID:
      return LG_CLIPBOARD_FILE_ERROR_INVALID;
    case KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY:
      return LG_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    case KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE:
      return LG_CLIPBOARD_FILE_ERROR_NO_SPACE;
    case KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED:
      return LG_CLIPBOARD_FILE_ERROR_DISCONNECTED;
    case KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED:
      return LG_CLIPBOARD_FILE_ERROR_CANCELLED;
    case KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED:
      return LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    case KVMFR_CLIPBOARD_FILE_ERROR_STALE:
      return LG_CLIPBOARD_FILE_ERROR_STALE;
    default:
      return LG_CLIPBOARD_FILE_ERROR_INVALID;
  }
}

static KVMFRClipboardFileError toWireFileError(
    LG_ClipboardFileError error)
{
  switch (error)
  {
    case LG_CLIPBOARD_FILE_ERROR_NONE:
      return KVMFR_CLIPBOARD_FILE_ERROR_NONE;
    case LG_CLIPBOARD_FILE_ERROR_NOT_FOUND:
      return KVMFR_CLIPBOARD_FILE_ERROR_NOT_FOUND;
    case LG_CLIPBOARD_FILE_ERROR_ACCESS:
      return KVMFR_CLIPBOARD_FILE_ERROR_ACCESS;
    case LG_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY:
      return KVMFR_CLIPBOARD_FILE_ERROR_NOT_DIRECTORY;
    case LG_CLIPBOARD_FILE_ERROR_IS_DIRECTORY:
      return KVMFR_CLIPBOARD_FILE_ERROR_IS_DIRECTORY;
    case LG_CLIPBOARD_FILE_ERROR_IO:
      return KVMFR_CLIPBOARD_FILE_ERROR_IO;
    case LG_CLIPBOARD_FILE_ERROR_INVALID:
      return KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
    case LG_CLIPBOARD_FILE_ERROR_NO_MEMORY:
      return KVMFR_CLIPBOARD_FILE_ERROR_NO_MEMORY;
    case LG_CLIPBOARD_FILE_ERROR_NO_SPACE:
      return KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE;
    case LG_CLIPBOARD_FILE_ERROR_DISCONNECTED:
      return KVMFR_CLIPBOARD_FILE_ERROR_DISCONNECTED;
    case LG_CLIPBOARD_FILE_ERROR_CANCELLED:
      return KVMFR_CLIPBOARD_FILE_ERROR_CANCELLED;
    case LG_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED:
      return KVMFR_CLIPBOARD_FILE_ERROR_NOT_SUPPORTED;
    case LG_CLIPBOARD_FILE_ERROR_STALE:
      return KVMFR_CLIPBOARD_FILE_ERROR_STALE;
  }
  return KVMFR_CLIPBOARD_FILE_ERROR_INVALID;
}

static bool validFileError(LG_ClipboardFileError error)
{
  return error >= LG_CLIPBOARD_FILE_ERROR_NONE &&
    error <= LG_CLIPBOARD_FILE_ERROR_STALE;
}

static struct FileTransfer * fileTransferFindNL(
    struct FileTransfer * transfers, uint64_t request)
{
  for (; transfers; transfers = transfers->next)
    if (transfers->request.request == request)
      return transfers;
  return NULL;
}

static struct FileTransfer * fileTransferTakeNL(
    struct FileTransfer ** transfers, uint64_t request)
{
  while (*transfers && (*transfers)->request.request != request)
    transfers = &(*transfers)->next;
  if (!*transfers)
    return NULL;
  struct FileTransfer * result = *transfers;
  *transfers = result->next;
  result->next = NULL;
  return result;
}

static struct FileAcquisition * fileAcquisitionFindNL(
    LGMPClipboard * clipboard, uint64_t dataset, uint64_t acquisition,
    bool fromHelper)
{
  for (struct FileAcquisition * item = clipboard->fileAcquisitions;
      item; item = item->next)
    if (item->dataset == dataset && item->acquisition == acquisition &&
        item->fromHelper == fromHelper)
      return item;
  return NULL;
}

static bool fileDatasetAcquiredNL(LGMPClipboard * clipboard,
    uint64_t dataset, bool fromHelper)
{
  for (struct FileAcquisition * item = clipboard->fileAcquisitions;
      item; item = item->next)
    if (item->dataset == dataset && item->fromHelper == fromHelper &&
        item->active)
      return true;
  return false;
}

static struct FileAcquisition * fileAcquisitionTakeNL(
    LGMPClipboard * clipboard, uint64_t dataset, uint64_t acquisition,
    bool fromHelper)
{
  struct FileAcquisition ** link = &clipboard->fileAcquisitions;
  while (*link && ((*link)->dataset != dataset ||
      (*link)->acquisition != acquisition ||
      (*link)->fromHelper != fromHelper))
    link = &(*link)->next;
  if (!*link)
    return NULL;
  struct FileAcquisition * result = *link;
  *link = result->next;
  result->next = NULL;
  return result;
}

static unsigned fileAcquisitionCountNL(const LGMPClipboard * clipboard)
{
  unsigned count = 0;
  for (const struct FileAcquisition * item = clipboard->fileAcquisitions;
      item; item = item->next)
    ++count;
  return count;
}

static unsigned fileTransferCountNL(const LGMPClipboard * clipboard)
{
  unsigned count = 0;
  for (const struct FileTransfer * item = clipboard->fileReads;
      item; item = item->next)
    ++count;
  for (const struct FileTransfer * item = clipboard->fileWrites;
      item; item = item->next)
    ++count;
  return count;
}

static void clearFilesNL(LGMPClipboard * clipboard)
{
  struct FileTransfer ** readTail = &clipboard->fileReads;
  while (*readTail)
    readTail = &(*readTail)->next;
  *readTail = clipboard->retiredFileReads;
  clipboard->retiredFileReads = clipboard->fileReads;
  clipboard->fileReads = NULL;

  struct FileTransfer ** writeTail = &clipboard->fileWrites;
  while (*writeTail)
    writeTail = &(*writeTail)->next;
  *writeTail = clipboard->retiredFileWrites;
  clipboard->retiredFileWrites = clipboard->fileWrites;
  clipboard->fileWrites = NULL;

  struct FileAcquisition ** acquisitionTail =
    &clipboard->fileAcquisitions;
  while (*acquisitionTail)
    acquisitionTail = &(*acquisitionTail)->next;
  *acquisitionTail = clipboard->retiredFileAcquisitions;
  clipboard->retiredFileAcquisitions = clipboard->fileAcquisitions;
  clipboard->fileAcquisitions = NULL;
}

static void freeFileTransfers(struct FileTransfer * transfers)
{
  while (transfers)
  {
    struct FileTransfer * next = transfers->next;
    free(transfers);
    transfers = next;
  }
}

static void freeFileAcquisitions(struct FileAcquisition * acquisitions)
{
  while (acquisitions)
  {
    struct FileAcquisition * next = acquisitions->next;
    free(acquisitions);
    acquisitions = next;
  }
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

static bool enqueueRecordLimitNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessage record, unsigned limit)
{
  if (!clipboard->connected || !clipboard->queue ||
      clipboard->pendingCount >= limit)
    return false;

  record.version = KVMFR_CLIPBOARD_VERSION;
  record.generation = clipboard->claimGeneration;
  pendingAt(clipboard, clipboard->pendingCount++)->record = record;
  return true;
}

static bool enqueueRecordNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessage record)
{
  return enqueueRecordLimitNL(
      clipboard, record, CLIPBOARD_PENDING_NORMAL_MAX);
}

static bool enqueueUrgentRecordNL(LGMPClipboard * clipboard,
    KVMFRClipboardMessage record)
{
  return enqueueRecordLimitNL(clipboard, record, CLIPBOARD_PENDING_MAX);
}

static bool enqueueFileFailureNL(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * request, LG_ClipboardFileError error)
{
  KVMFRClipboardMessage response = { 0 };
  response.type                = request->type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE ?
    KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED :
    KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
  response.clipboardGeneration = request->clipboardGeneration;
  response.transfer            = request->transfer;
  response.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  response.token               = toWireFileError(error);
  return enqueueUrgentRecordNL(clipboard, response);
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
      clipboard->pendingCount >= CLIPBOARD_PENDING_NORMAL_MAX ||
      record.length > KVMFR_CLIPBOARD_DATA_BYTES ||
      (record.length && !data))
    return false;

  struct Grant * grant = availableGrantNL(clipboard);
  if (!grant)
    return false;

  record.version    = KVMFR_CLIPBOARD_VERSION;
  record.generation = clipboard->claimGeneration;
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
  clearFilesNL(clipboard);
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
      record->type > KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL ||
      record->generation != clipboard->claimGeneration ||
      record->length > KVMFR_CLIPBOARD_DATA_BYTES ||
      record->offset > UINT64_MAX - record->length)
    return false;

  if (record->type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
      record->type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
  {
    if (size != sizeof(*record) + KVMFR_CLIPBOARD_DATA_BYTES ||
        queueType != KVMFR_CLIPBOARD_QUEUE_DATA)
      return false;
  }
  else if (queueType != KVMFR_CLIPBOARD_QUEUE_MESSAGE ||
      size != sizeof(*record) || record->length ||
      (record->flags &&
       record->type != KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST))
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
        kvmfrClipboardRepresentationFormatValid(record->format) &&
        !record->offset && !record->size && !record->flags &&
        !record->token && !record->length && !record->sequence;

    case KVMFR_CLIPBOARD_MESSAGE_DATA:
    {
      const uint64_t end = record->offset + record->length;
      const unsigned operations =
        !!(record->flags & KVMFR_CLIPBOARD_FLAG_BEGIN) +
        !!record->length + !!(record->flags & KVMFR_CLIPBOARD_FLAG_END);
      if (!record->clipboardGeneration || record->token ||
          kvmfrClipboardTransferFromHelper(record->transfer) ||
          !kvmfrClipboardRepresentationFormatValid(record->format) ||
          record->flags & ~(KVMFR_CLIPBOARD_FLAG_BEGIN |
            KVMFR_CLIPBOARD_FLAG_END) || !operations ||
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
        (!record->format ||
         kvmfrClipboardRepresentationFormatValid(record->format)) &&
        !record->flags && !record->length &&
        !record->sequence;

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_DATA:
    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      return kvmfrClipboardFileMessageValid(record);

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

static bool validateFileReadChunkNL(const LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record,
    LG_ClipboardFileRequest * descriptor)
{
  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileReads, record->transfer);
  if (!transfer || record->clipboardGeneration != transfer->request.dataset ||
      record->format != KVMFR_CLIPBOARD_FORMAT_FILES ||
      record->token != toWireFileOperation(transfer->request.operation) ||
      record->offset != transfer->responseOffset ||
      record->sequence != transfer->sequence)
    return false;
  if (!transfer->began)
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

  const uint64_t end = record->offset + record->length;
  const uint64_t hint = transfer->began ? transfer->sizeHint : record->size;
  if (hint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && end > hint)
    return false;
  if (transfer->request.operation == LG_CLIPBOARD_FILE_READ &&
      end > transfer->request.length)
    return false;
  if ((record->flags & KVMFR_CLIPBOARD_FLAG_END) &&
      (record->size != end ||
       (hint != KVMFR_CLIPBOARD_SIZE_UNKNOWN && hint != end)))
    return false;
  *descriptor = transfer->request;
  return true;
}

static void advanceFileReadNL(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileReads, record->transfer);
  if (!transfer)
    return;
  if (!transfer->began)
  {
    transfer->began    = true;
    transfer->sizeHint = record->size;
  }
  transfer->responseOffset += record->length;
  ++transfer->sequence;
  if (record->flags & KVMFR_CLIPBOARD_FLAG_END)
    free(fileTransferTakeNL(&clipboard->fileReads, record->transfer));
}

static bool rejectMalformedFileReadNL(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record,
    KVMFRClipboardMessage * cancellation, bool * queued)
{
  if (record->type != KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
    return false;

  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileReads, record->transfer);
  if (!transfer ||
      transfer->request.dataset != record->clipboardGeneration)
    return false;

  *cancellation = (KVMFRClipboardMessage)
  {
    .type                = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL,
    .clipboardGeneration = transfer->request.dataset,
    .transfer            = transfer->request.request,
    .format              = KVMFR_CLIPBOARD_FORMAT_FILES,
    .token               = KVMFR_CLIPBOARD_FILE_ERROR_INVALID,
  };
  free(fileTransferTakeNL(&clipboard->fileReads, record->transfer));
  *queued = enqueueUrgentRecordNL(clipboard, *cancellation);
  return true;
}

static void dispatchOffer(LGMPClipboard * clipboard,
    uint64_t dataset, KVMFRClipboardFormatFlags mask)
{
  LG_ClipboardData formats[LG_CLIPBOARD_DATA_NONE];
  const unsigned count = formatsFromMask(mask, formats);

  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileOffer &&
      (mask & KVMFR_CLIPBOARD_FORMAT_MASK_FILES))
    events->fileOffer(opaque, dataset);
  if (events && events->notice)
    events->notice(opaque, formats, count);
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchFileAcquire(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileAcquire)
    events->fileAcquire(opaque,
        record->clipboardGeneration, record->transfer);
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchFileAcquired(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileAcquired)
    events->fileAcquired(opaque, record->clipboardGeneration,
        record->transfer, fromWireFileError(record->token));
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchFileRelease(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileRelease)
    events->fileRelease(opaque,
        record->clipboardGeneration, record->transfer);
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchFileRequest(LGMPClipboard * clipboard,
    const LG_ClipboardFileRequest * request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileRequest)
    events->fileRequest(opaque, request);
  LG_UNLOCK(clipboard->eventLock);
}

static LG_ClipboardResult dispatchFileData(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record,
    const LG_ClipboardFileRequest * request, const uint8_t * data,
    enum HeldPhase phase)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  LG_ClipboardResult result = LG_CLIPBOARD_RESULT_FAILED;
  if (events)
    switch (phase)
    {
      case HELD_PHASE_BEGIN:
        if (events->fileDataBegin)
          result = events->fileDataBegin(opaque, request, record->size);
        break;
      case HELD_PHASE_CHUNK:
        if (events->fileDataChunk)
          result = events->fileDataChunk(opaque, request,
              record->offset, data, record->length);
        break;
      case HELD_PHASE_END:
        if (events->fileDataEnd)
          result = events->fileDataEnd(opaque, request, record->size);
        break;
      case HELD_PHASE_NONE:
        break;
    }
  LG_UNLOCK(clipboard->eventLock);
  return result;
}

static void dispatchFileCancel(LGMPClipboard * clipboard,
    const KVMFRClipboardMessage * record)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileCancel)
    events->fileCancel(opaque, record->clipboardGeneration,
        record->transfer, fromWireFileError(record->token));
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchRetiredFiles(LGMPClipboard * clipboard)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  struct FileTransfer * reads = clipboard->retiredFileReads;
  struct FileTransfer * writes = clipboard->retiredFileWrites;
  struct FileAcquisition * acquisitions =
    clipboard->retiredFileAcquisitions;
  clipboard->retiredFileReads        = NULL;
  clipboard->retiredFileWrites       = NULL;
  clipboard->retiredFileAcquisitions = NULL;
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);

  if (events && events->fileCancel)
  {
    for (struct FileTransfer * transfer = reads; transfer;
        transfer = transfer->next)
      events->fileCancel(opaque, transfer->request.dataset,
          transfer->request.request,
          LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    for (struct FileTransfer * transfer = writes; transfer;
        transfer = transfer->next)
      events->fileCancel(opaque, transfer->request.dataset,
          transfer->request.request,
          LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
    for (struct FileAcquisition * acquisition = acquisitions;
        acquisition; acquisition = acquisition->next)
      events->fileCancel(opaque, acquisition->dataset,
          acquisition->acquisition,
          LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
  }
  freeFileTransfers(reads);
  freeFileTransfers(writes);
  freeFileAcquisitions(acquisitions);
  LG_UNLOCK(clipboard->eventLock);
}

static void dispatchFileReady(LGMPClipboard * clipboard, uint64_t request)
{
  LG_LOCK(clipboard->eventLock);
  LG_LOCK(clipboard->lock);
  const LG_ClipboardEventOps * events = clipboard->events;
  void * opaque = clipboard->eventOpaque;
  LG_UNLOCK(clipboard->lock);
  if (events && events->fileDataReady)
    events->fileDataReady(opaque, request);
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
    bool file;
    LG_ClipboardFileRequest fileRequest;
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
    file = clipboard->heldFile;
    fileRequest = clipboard->heldFileRequest;
    clipboard->heldReady = false;
    LG_UNLOCK(clipboard->lock);

    const LG_ClipboardResult result = file ?
      dispatchFileData(clipboard, &record, &fileRequest, data, phase) :
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

    const bool matchingRead = !file &&
      clipboard->readRequest == record.transfer &&
      clipboard->readTransfer;
    struct FileTransfer * matchingFile = file ? fileTransferFindNL(
        clipboard->fileReads, record.transfer) : NULL;
    if (result == LG_CLIPBOARD_RESULT_FAILED && (matchingRead || matchingFile))
    {
      KVMFRClipboardMessage cancel = { 0 };
      cancel.type = file ? KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL :
        KVMFR_CLIPBOARD_MESSAGE_CANCEL;
      cancel.clipboardGeneration = file ? fileRequest.dataset :
        clipboard->readClipboardGeneration;
      cancel.transfer = file ? fileRequest.request : clipboard->readTransfer;
      cancel.format = file ? KVMFR_CLIPBOARD_FORMAT_FILES :
        clipboard->readFormat;
      cancel.token = file ?
        toWireFileError(LG_CLIPBOARD_FILE_ERROR_INVALID) :
        LG_CLIPBOARD_CANCEL_INVALID;
      if (file)
        enqueueUrgentRecordNL(clipboard, cancel);
      else
        enqueueRecordNL(clipboard, cancel);
    }
    if (matchingFile && result == LG_CLIPBOARD_RESULT_ACCEPTED)
      advanceFileReadNL(clipboard, &record);
    else if (matchingFile)
      free(fileTransferTakeNL(&clipboard->fileReads, record.transfer));
    else if (matchingRead && result == LG_CLIPBOARD_RESULT_ACCEPTED)
      advanceReadNL(clipboard, &record);
    else if (matchingRead)
      clearReadNL(clipboard);

    const LGMP_STATUS done = lgmpClientMessageDone(clipboard->queue);
    clipboard->held = false;
    memset(&clipboard->heldMessage, 0, sizeof(clipboard->heldMessage));
    memset(&clipboard->heldRecord, 0, sizeof(clipboard->heldRecord));
    clipboard->heldFile = false;
    memset(&clipboard->heldFileRequest, 0,
        sizeof(clipboard->heldFileRequest));
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

static bool processMessage(LGMPClipboard * clipboard, bool * processed)
{
  *processed = false;
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
  *processed = true;

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
    dispatchRetiredFiles(clipboard);
    if (changed)
      notifyStatus(clipboard);
    return status == LGMP_OK;
  }

  if (type == KVMFR_CLIPBOARD_QUEUE_GRANT)
  {
    uint64_t fileReady[CLIPBOARD_PENDING_MAX];
    size_t fileReadyCount = 0;
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
    if (stored)
      for (struct FileTransfer * transfer = clipboard->fileWrites;
          transfer && fileReadyCount < CLIPBOARD_PENDING_MAX;
          transfer = transfer->next)
        if (transfer->blocked)
        {
          transfer->blocked = false;
          fileReady[fileReadyCount++] = transfer->request.request;
        }
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    if (!stored)
      DEBUG_WARN("Ignoring invalid LGMP clipboard grant");
    if (ready)
      dispatchReady(clipboard, request);
    for (size_t i = 0; i < fileReadyCount; ++i)
      dispatchFileReady(clipboard, fileReady[i]);
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
    KVMFRClipboardMessage cancellation = { 0 };
    bool queued = false;
    const bool cancelled = rejectMalformedFileReadNL(
        clipboard, &record, &cancellation, &queued);
    status = lgmpClientMessageDone(clipboard->queue);
    if (status != LGMP_OK)
      connectionFailed(clipboard, status);
    LG_UNLOCK(clipboard->lock);
    if (cancelled)
      dispatchFileCancel(clipboard, &cancellation);
    if (queued)
      signalWorker(clipboard);
    DEBUG_WARN("Ignoring malformed LGMP clipboard record");
    return status == LGMP_OK;
  }

  if (record.type == KVMFR_CLIPBOARD_MESSAGE_DATA ||
      record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA)
  {
    LG_ClipboardFileRequest fileRequest = { 0 };
    const bool file = record.type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
    const bool chunkValid = file ? validateFileReadChunkNL(
        clipboard, &record, &fileRequest) :
      validateReadChunkNL(clipboard, &record);
    if (!chunkValid)
    {
      KVMFRClipboardMessage cancellation = { 0 };
      bool queued = false;
      const bool cancelled = file && rejectMalformedFileReadNL(
          clipboard, &record, &cancellation, &queued);
      status = lgmpClientMessageDone(clipboard->queue);
      if (status != LGMP_OK)
        connectionFailed(clipboard, status);
      LG_UNLOCK(clipboard->lock);
      if (cancelled)
        dispatchFileCancel(clipboard, &cancellation);
      if (queued)
        signalWorker(clipboard);
      DEBUG_WARN("Ignoring stale or malformed LGMP clipboard data");
      return status == LGMP_OK;
    }
    if (!file)
      record.transfer = clipboard->readRequest;
    clipboard->held            = true;
    clipboard->heldReady       = true;
    clipboard->heldMessage     = message;
    clipboard->heldRecord      = record;
    clipboard->heldFile        = file;
    clipboard->heldFileRequest = fileRequest;
    clipboard->heldPhase       =
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
  bool fileRejected = false;
  LG_ClipboardRequest cancelledRead = LG_CLIPBOARD_REQUEST_INVALID;
  LG_ClipboardFileRequest fileRequest = { 0 };
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

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
    {
      if (!kvmfrClipboardTransferFromHelper(record.transfer))
      {
        dispatch = false;
        break;
      }
      if (fileAcquisitionFindNL(clipboard, record.clipboardGeneration,
            record.transfer, true))
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_INVALID);
        dispatch = false;
        break;
      }
      if (record.clipboardGeneration !=
            clipboard->localClipboardGeneration ||
          !(clipboard->localFormats & KVMFR_CLIPBOARD_FORMAT_MASK_FILES))
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_STALE);
        dispatch = false;
        break;
      }
      if (fileAcquisitionCountNL(clipboard) >=
          KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS)
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_NO_SPACE);
        dispatch = false;
        break;
      }
      struct FileAcquisition * acquisition = calloc(1, sizeof(*acquisition));
      if (!acquisition)
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        dispatch = false;
        break;
      }
      *acquisition = (struct FileAcquisition)
      {
        .dataset     = record.clipboardGeneration,
        .acquisition = record.transfer,
        .fromHelper  = true,
        .next        = clipboard->fileAcquisitions,
      };
      clipboard->fileAcquisitions = acquisition;
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
    {
      if (kvmfrClipboardTransferFromHelper(record.transfer))
      {
        dispatch = false;
        break;
      }
      struct FileAcquisition * acquisition = fileAcquisitionFindNL(
          clipboard, record.clipboardGeneration, record.transfer, false);
      if (!acquisition || acquisition->active)
      {
        dispatch = false;
        break;
      }
      if (record.token)
        free(fileAcquisitionTakeNL(clipboard,
            record.clipboardGeneration, record.transfer, false));
      else
        acquisition->active = true;
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
    {
      struct FileAcquisition * acquisition = fileAcquisitionTakeNL(
          clipboard, record.clipboardGeneration, record.transfer, true);
      if (!acquisition)
      {
        dispatch = false;
        break;
      }
      free(acquisition);
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
    {
      if (!kvmfrClipboardTransferFromHelper(record.transfer))
      {
        dispatch = false;
        break;
      }
      if (fileTransferFindNL(clipboard->fileWrites, record.transfer))
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_INVALID);
        dispatch = false;
        break;
      }
      if (!fileDatasetAcquiredNL(clipboard,
            record.clipboardGeneration, true))
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_STALE);
        dispatch = false;
        break;
      }
      if (fileTransferCountNL(clipboard) >=
          KVMFR_CLIPBOARD_FILE_MAX_REQUESTS)
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_NO_SPACE);
        dispatch = false;
        break;
      }
      struct FileTransfer * transfer = calloc(1, sizeof(*transfer));
      if (!transfer)
      {
        fileRejected = enqueueFileFailureNL(clipboard, &record,
            LG_CLIPBOARD_FILE_ERROR_NO_MEMORY);
        dispatch = false;
        break;
      }
      fileRequest = (LG_ClipboardFileRequest)
      {
        .dataset   = record.clipboardGeneration,
        .request   = record.transfer,
        .node      = record.size,
        .offset    = record.offset,
        .length    = record.token == KVMFR_CLIPBOARD_FILE_OP_READ ?
          record.flags : 0,
        .operation = fromWireFileOperation(record.token),
      };
      transfer->request  = fileRequest;
      transfer->sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
      transfer->next     = clipboard->fileWrites;
      clipboard->fileWrites = transfer;
      break;
    }

    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
    {
      const bool fromHelper = kvmfrClipboardTransferFromHelper(
          record.transfer);
      struct FileTransfer ** transfers = fromHelper ?
        &clipboard->fileWrites : &clipboard->fileReads;
      struct FileTransfer * found = fileTransferFindNL(
          *transfers, record.transfer);
      struct FileTransfer * transfer = found &&
          found->request.dataset == record.clipboardGeneration ?
        fileTransferTakeNL(transfers, record.transfer) : NULL;
      struct FileAcquisition * acquisition = fileAcquisitionTakeNL(
          clipboard, record.clipboardGeneration, record.transfer,
          fromHelper);
      if (!transfer && !acquisition)
      {
        dispatch = false;
        break;
      }
      free(transfer);
      free(acquisition);
      break;
    }
  }
  LG_UNLOCK(clipboard->lock);

  if (!dispatch)
  {
    if (fileRejected)
      signalWorker(clipboard);
    else
      DEBUG_WARN("Ignoring stale LGMP clipboard control record");
    return true;
  }

  switch (record.type)
  {
    case KVMFR_CLIPBOARD_MESSAGE_OFFER:
      dispatchOffer(clipboard, record.clipboardGeneration, record.token);
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
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE:
      dispatchFileAcquire(clipboard, &record);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED:
      dispatchFileAcquired(clipboard, &record);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE:
      dispatchFileRelease(clipboard, &record);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST:
      dispatchFileRequest(clipboard, &fileRequest);
      break;
    case KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL:
      dispatchFileCancel(clipboard, &record);
      break;
  }
  return true;
}

static int clipboardThread(void * opaque)
{
  LGMPClipboard * clipboard = opaque;
  while (!atomic_load_explicit(&clipboard->stop, memory_order_acquire))
  {
    bool     running = true;
    unsigned drained = 0;
    for (; drained < CLIPBOARD_DRAIN_MAX; ++drained)
    {
      if (!processHeld(clipboard))
      {
        running = false;
        break;
      }
      dispatchRetiredFiles(clipboard);

      bool processed = false;
      if (!processMessage(clipboard, &processed))
      {
        running = false;
        break;
      }
      dispatchRetiredFiles(clipboard);
      if (!processed)
        break;
    }
    if (!running)
      break;

    LG_ClipboardRequest readyRequest = LG_CLIPBOARD_REQUEST_INVALID;
    uint64_t fileReady[CLIPBOARD_PENDING_MAX];
    size_t fileReadyCount = 0;
    LG_LOCK(clipboard->lock);
    flushPendingNL(clipboard);
    const uint64_t now = microtime();
    if (clipboard->connected && clipboard->claimed &&
        !clipboard->pendingCount &&
        now - clipboard->lastSend >= CLIPBOARD_KEEPALIVE_US)
      enqueueTypeNL(clipboard, KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE);
    if (clipboard->writeBlocked &&
        clipboard->pendingCount < CLIPBOARD_PENDING_NORMAL_MAX &&
        availableGrantNL(clipboard))
    {
      readyRequest = clipboard->writeBlockedRequest;
      clipboard->writeBlocked = false;
    }
    if (clipboard->pendingCount < CLIPBOARD_PENDING_NORMAL_MAX &&
        availableGrantNL(clipboard))
      for (struct FileTransfer * transfer = clipboard->fileWrites;
          transfer && fileReadyCount < CLIPBOARD_PENDING_MAX;
          transfer = transfer->next)
        if (transfer->blocked)
        {
          transfer->blocked = false;
          fileReady[fileReadyCount++] = transfer->request.request;
        }
    const bool active = drained == CLIPBOARD_DRAIN_MAX ||
      clipboard->pendingCount || clipboard->held ||
      clipboard->writeBlocked || clipboard->fileReads ||
      clipboard->fileWrites;
    LG_UNLOCK(clipboard->lock);

    if (readyRequest != LG_CLIPBOARD_REQUEST_INVALID)
      dispatchReady(clipboard, readyRequest);
    for (size_t i = 0; i < fileReadyCount; ++i)
      dispatchFileReady(clipboard, fileReady[i]);

    if (atomic_load_explicit(&clipboard->stop, memory_order_acquire))
      break;
    lgWaitEvent(clipboard->event,
      active ? CLIPBOARD_ACTIVE_POLL_MS : CLIPBOARD_IDLE_POLL_MS);
  }
  dispatchRetiredFiles(clipboard);
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
  uint64_t nonce = 0;
  if (!randomBytes(&nonce, sizeof(nonce)))
  {
    free(clipboard);
    return false;
  }
  nonce &= ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!nonce)
    nonce = 1;
  clipboard->transferSerial = nonce;
  clipboard->localGenerationSerial =
    (nonce ^ UINT64_C(0x4c47434c49504244)) &
      ~KVMFR_CLIPBOARD_TRANSFER_HELPER;
  if (!clipboard->localGenerationSerial)
    clipboard->localGenerationSerial = 1;
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
      lgWaitEvent(clipboard->event, CLIPBOARD_ACTIVE_POLL_MS);
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
      lgWaitEvent(clipboard->event, CLIPBOARD_ACTIVE_POLL_MS);
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
  dispatchRetiredFiles(clipboard);
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
  uint64_t dataset;
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
  dataset = clipboard->remoteClipboardGeneration;
  LG_UNLOCK(clipboard->lock);
  const unsigned replayCount = available ?
    formatsFromMask(formats, replay) : 0;
  if (replayCount)
  {
    if ((formats & KVMFR_CLIPBOARD_FORMAT_MASK_FILES) &&
        events->fileOffer)
      events->fileOffer(eventOpaque, dataset);
    if (events->notice)
      events->notice(eventOpaque, replay, replayCount);
  }
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
  clear.clipboardGeneration = nextClientGeneration(clipboard);
  const unsigned required = clipboard->claimed ? 1U : 2U;
  const bool result =
    clipboard->pendingCount <= CLIPBOARD_PENDING_NORMAL_MAX - required &&
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
    if (!kvmfrClipboardRepresentationFormatValid(toWireFormat(types[i])))
      return false;
  const KVMFRClipboardFormatFlags formats = formatMask(types, count);

  LG_LOCK(clipboard->lock);
  KVMFRClipboardMessage offer = { 0 };
  offer.type                = KVMFR_CLIPBOARD_MESSAGE_OFFER;
  offer.token               = formats;
  offer.clipboardGeneration = nextClientGeneration(clipboard);
  const unsigned required = clipboard->claimed ? 1U : 2U;
  const bool result =
    clipboard->pendingCount <= CLIPBOARD_PENDING_NORMAL_MAX - required &&
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
      !kvmfrClipboardRepresentationFormatValid(format))
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
      !kvmfrClipboardRepresentationFormatValid(format))
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

static bool offerFiles(void * opaque, uint64_t dataset)
{
  LGMPClipboard * clipboard = opaque;
  if (!dataset || kvmfrClipboardTransferFromHelper(dataset))
    return false;
  KVMFRClipboardMessage offer = { 0 };
  offer.type                = KVMFR_CLIPBOARD_MESSAGE_OFFER;
  offer.clipboardGeneration = dataset;
  offer.token               = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  LG_LOCK(clipboard->lock);
  const unsigned required = clipboard->claimed ? 1U : 2U;
  const bool result = clipboard->pendingCount <=
      CLIPBOARD_PENDING_NORMAL_MAX - required && ensureClaimNL(clipboard) &&
    enqueueRecordNL(clipboard, offer);
  if (result)
  {
    clipboard->localClipboardGeneration = dataset;
    clipboard->localFormats = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  }
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool fileAcquire(void * opaque, uint64_t dataset,
    uint64_t acquisitionId)
{
  LGMPClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromClient(acquisitionId))
    return false;
  LG_LOCK(clipboard->lock);
  if (fileAcquisitionCountNL(clipboard) >=
      KVMFR_CLIPBOARD_FILE_MAX_ACQUISITIONS)
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  struct FileAcquisition * acquisition = calloc(1, sizeof(*acquisition));
  if (!acquisition)
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE;
  record.clipboardGeneration = dataset;
  record.transfer            = acquisitionId;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  const bool result = dataset == clipboard->remoteClipboardGeneration &&
    (clipboard->remoteFormats & KVMFR_CLIPBOARD_FORMAT_MASK_FILES) &&
    !fileAcquisitionFindNL(clipboard, dataset, acquisitionId, false) &&
    ensureClaimNL(clipboard) && enqueueRecordNL(clipboard, record);
  if (result)
  {
    *acquisition = (struct FileAcquisition)
    {
      .dataset     = dataset,
      .acquisition = acquisitionId,
      .next        = clipboard->fileAcquisitions,
    };
    clipboard->fileAcquisitions = acquisition;
  }
  LG_UNLOCK(clipboard->lock);
  if (!result)
    free(acquisition);
  else
    signalWorker(clipboard);
  return result;
}

static bool fileAcquired(void * opaque, uint64_t dataset,
    uint64_t acquisitionId, LG_ClipboardFileError error)
{
  LGMPClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromHelper(acquisitionId) ||
      !validFileError(error))
    return false;
  LG_LOCK(clipboard->lock);
  struct FileAcquisition * acquisition = fileAcquisitionFindNL(
      clipboard, dataset, acquisitionId, true);
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED;
  record.clipboardGeneration = dataset;
  record.transfer            = acquisitionId;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  record.token               = toWireFileError(error);
  const bool result = acquisition && !acquisition->active &&
    enqueueUrgentRecordNL(clipboard, record);
  if (result && error == LG_CLIPBOARD_FILE_ERROR_NONE)
    acquisition->active = true;
  else if (result)
    free(fileAcquisitionTakeNL(
        clipboard, dataset, acquisitionId, true));
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool fileRelease(void * opaque, uint64_t dataset,
    uint64_t acquisitionId)
{
  LGMPClipboard * clipboard = opaque;
  if (!dataset || !kvmfrClipboardTransferFromClient(acquisitionId))
    return false;
  LG_LOCK(clipboard->lock);
  struct FileAcquisition * acquisition = fileAcquisitionFindNL(
      clipboard, dataset, acquisitionId, false);
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_RELEASE;
  record.clipboardGeneration = dataset;
  record.transfer            = acquisitionId;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  const bool result = acquisition && acquisition->active &&
    enqueueUrgentRecordNL(clipboard, record);
  if (result)
    free(fileAcquisitionTakeNL(
        clipboard, dataset, acquisitionId, false));
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result;
}

static bool fileRequest(void * opaque,
    const LG_ClipboardFileRequest * descriptor)
{
  LGMPClipboard * clipboard = opaque;
  if (!descriptor || !descriptor->dataset ||
      !kvmfrClipboardTransferFromClient(descriptor->request) ||
      (descriptor->operation != LG_CLIPBOARD_FILE_LIST &&
       descriptor->operation != LG_CLIPBOARD_FILE_READ) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_LIST &&
       (descriptor->offset || descriptor->length)) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_READ &&
      (!descriptor->length ||
        descriptor->length > KVMFR_CLIPBOARD_FILE_READ_BYTES ||
        descriptor->offset > UINT64_MAX - descriptor->length)))
    return false;
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST;
  record.clipboardGeneration = descriptor->dataset;
  record.transfer            = descriptor->request;
  record.offset              = descriptor->offset;
  record.size                = descriptor->node;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  record.flags               = descriptor->operation ==
    LG_CLIPBOARD_FILE_READ ? descriptor->length : 0;
  record.token               = toWireFileOperation(descriptor->operation);
  LG_LOCK(clipboard->lock);
  if (fileTransferCountNL(clipboard) >=
      KVMFR_CLIPBOARD_FILE_MAX_REQUESTS)
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  struct FileTransfer * transfer = calloc(1, sizeof(*transfer));
  if (!transfer)
  {
    LG_UNLOCK(clipboard->lock);
    return false;
  }
  const bool result = fileDatasetAcquiredNL(
      clipboard, descriptor->dataset, false) &&
    !fileTransferFindNL(clipboard->fileReads, descriptor->request) &&
    enqueueRecordNL(clipboard, record);
  if (result)
  {
    transfer->request  = *descriptor;
    transfer->sizeHint = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
    transfer->next     = clipboard->fileReads;
    clipboard->fileReads = transfer;
  }
  LG_UNLOCK(clipboard->lock);
  if (!result)
    free(transfer);
  else
    signalWorker(clipboard);
  return result;
}

static bool sameFileRequest(const LG_ClipboardFileRequest * a,
    const LG_ClipboardFileRequest * b)
{
  return a->dataset == b->dataset && a->request == b->request &&
    a->node == b->node && a->offset == b->offset &&
    a->length == b->length && a->operation == b->operation;
}

static LG_ClipboardResult fileDataBegin(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t sizeHint)
{
  LGMPClipboard * clipboard = opaque;
  if (!descriptor || !kvmfrClipboardTransferFromHelper(descriptor->request))
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(clipboard->lock);
  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileWrites, descriptor->request);
  const bool valid = transfer && !transfer->began &&
    sameFileRequest(&transfer->request, descriptor) &&
    (descriptor->operation != LG_CLIPBOARD_FILE_READ ||
     sizeHint <= descriptor->length);
  if (valid)
  {
    transfer->began          = true;
    transfer->sizeHint       = sizeHint;
    transfer->responseOffset = 0;
    transfer->sequence       = 0;
    transfer->blocked        = false;
    transfer->wireEnded      = false;
  }
  LG_UNLOCK(clipboard->lock);
  return valid ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_FAILED;
}

static LG_ClipboardResult fileDataChunk(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t responseOffset,
    const void * data, size_t size, bool end)
{
  LGMPClipboard * clipboard = opaque;
  if (!descriptor || !data || !size ||
      size > KVMFR_CLIPBOARD_DATA_BYTES ||
      responseOffset > UINT64_MAX - size)
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(clipboard->lock);
  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileWrites, descriptor->request);
  if (!transfer || !transfer->began || transfer->wireEnded ||
      !sameFileRequest(&transfer->request, descriptor) ||
      transfer->responseOffset != responseOffset ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
       responseOffset + size > transfer->sizeHint) ||
      (end && transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
       transfer->sizeHint != responseOffset + size) ||
      (descriptor->operation == LG_CLIPBOARD_FILE_READ &&
       responseOffset + size > descriptor->length))
  {
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
  record.clipboardGeneration = descriptor->dataset;
  record.transfer            = descriptor->request;
  record.offset              = responseOffset;
  record.sequence            = transfer->sequence;
  record.size                = end ? responseOffset + size :
    transfer->sequence ? KVMFR_CLIPBOARD_SIZE_UNKNOWN : transfer->sizeHint;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  record.flags               = (transfer->sequence ? 0 :
    KVMFR_CLIPBOARD_FLAG_BEGIN) |
    (end ? KVMFR_CLIPBOARD_FLAG_END : 0);
  record.token               = toWireFileOperation(descriptor->operation);
  record.length              = (uint32_t)size;
  const bool result = enqueueDataNL(clipboard, record, data);
  if (result)
  {
    transfer->responseOffset += size;
    ++transfer->sequence;
    transfer->wireEnded      = end;
  }
  else
    transfer->blocked = true;
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_BLOCKED;
}

static LG_ClipboardResult fileDataEnd(void * opaque,
    const LG_ClipboardFileRequest * descriptor, uint64_t finalSize)
{
  LGMPClipboard * clipboard = opaque;
  if (!descriptor)
    return LG_CLIPBOARD_RESULT_FAILED;
  LG_LOCK(clipboard->lock);
  struct FileTransfer * transfer = fileTransferFindNL(
      clipboard->fileWrites, descriptor->request);
  if (!transfer || !transfer->began ||
      !sameFileRequest(&transfer->request, descriptor) ||
      transfer->responseOffset != finalSize ||
      (transfer->sizeHint != KVMFR_CLIPBOARD_SIZE_UNKNOWN &&
       transfer->sizeHint != finalSize))
  {
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (transfer->wireEnded)
  {
    free(fileTransferTakeNL(
        &clipboard->fileWrites, descriptor->request));
    LG_UNLOCK(clipboard->lock);
    return LG_CLIPBOARD_RESULT_ACCEPTED;
  }
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_DATA;
  record.clipboardGeneration = descriptor->dataset;
  record.transfer            = descriptor->request;
  record.offset              = finalSize;
  record.sequence            = transfer->sequence;
  record.size                = finalSize;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  record.flags               = KVMFR_CLIPBOARD_FLAG_END |
    (transfer->sequence ? 0 : KVMFR_CLIPBOARD_FLAG_BEGIN);
  record.token               = toWireFileOperation(descriptor->operation);
  const bool result = enqueueDataNL(clipboard, record, NULL);
  if (result)
    free(fileTransferTakeNL(
        &clipboard->fileWrites, descriptor->request));
  else
    transfer->blocked = true;
  LG_UNLOCK(clipboard->lock);
  if (result)
    signalWorker(clipboard);
  return result ? LG_CLIPBOARD_RESULT_ACCEPTED :
    LG_CLIPBOARD_RESULT_BLOCKED;
}

static bool fileCancel(void * opaque, uint64_t dataset,
    uint64_t request, LG_ClipboardFileError reason)
{
  LGMPClipboard * clipboard = opaque;
  if (!dataset || !request || reason == LG_CLIPBOARD_FILE_ERROR_NONE ||
      !validFileError(reason))
    return false;
  LG_LOCK(clipboard->lock);
  struct FileTransfer * read = fileTransferFindNL(
      clipboard->fileReads, request);
  struct FileTransfer * write = fileTransferFindNL(
      clipboard->fileWrites, request);
  struct FileAcquisition * acquisition = fileAcquisitionFindNL(
      clipboard, dataset, request,
      kvmfrClipboardTransferFromHelper(request));
  KVMFRClipboardMessage record = { 0 };
  record.type                = KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL;
  record.clipboardGeneration = dataset;
  record.transfer            = request;
  record.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  record.token               = toWireFileError(reason);
  const bool matches = (read && read->request.dataset == dataset) ||
    (write && write->request.dataset == dataset) || acquisition;
  const bool result = matches && enqueueUrgentRecordNL(clipboard, record);
  if (result)
  {
    free(fileTransferTakeNL(&clipboard->fileReads, request));
    free(fileTransferTakeNL(&clipboard->fileWrites, request));
    if (acquisition)
      free(fileAcquisitionTakeNL(clipboard, dataset, request,
          kvmfrClipboardTransferFromHelper(request)));
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
  .offerFiles        = offerFiles,
  .dataBegin         = dataBegin,
  .dataChunk         = dataChunk,
  .dataEnd           = dataEnd,
  .dataCancel        = dataCancel,
  .dataReady         = dataReady,
  .request           = requestData,
  .fileAcquire       = fileAcquire,
  .fileAcquired      = fileAcquired,
  .fileRelease       = fileRelease,
  .fileRequest       = fileRequest,
  .fileDataBegin     = fileDataBegin,
  .fileDataChunk     = fileDataChunk,
  .fileDataEnd       = fileDataEnd,
  .fileCancel        = fileCancel,
};

const LG_ClipboardOps * lgmpClipboard_getOps(void)
{
  return &CLIPBOARD_OPS;
}
