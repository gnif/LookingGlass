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

#include <lgmp/client.h>
#include <lgmp/host.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TEST_WAIT_MS          2500U
#define TEST_QUIET_MS         50U
#define TEST_MEMORY_MAX       128U
#define SLOT_BYTES            \
  (sizeof(KVMFRClipboardSlotHeader) + KVMFR_CLIPBOARD_DATA_BYTES)
#define TEST_TRANSIENT_SLOTS  4U
#define TEST_SHM_OVERHEAD     (1024U * 1024U)
/* Match the production inbound and outbound slot pools, retain several
 * synthetic host DATA records at once, and leave room for LGMP metadata. */
#define TEST_SHM_SIZE         \
  ((2U * KVMFR_CLIPBOARD_SLOT_COUNT + TEST_TRANSIENT_SLOTS) * SLOT_BYTES + \
    TEST_SHM_OVERHEAD)

#define CHECK(x) \
  do \
  { \
    if (!(x)) \
    { \
      fprintf(stderr, "check failed at %s:%d: %s\n", \
          __FILE__, __LINE__, #x); \
      return false; \
    } \
  } \
  while (0)

typedef struct EventTrace
{
  atomic_uint notice;
  atomic_uint release;
  atomic_uint request;
  atomic_uint requestCancel;
  atomic_uint dataBegin;
  atomic_uint dataChunk;
  atomic_uint dataEnd;
  atomic_uint dataCancel;
  atomic_uint dataReady;
  atomic_uint fileOffer;
  atomic_uint fileAcquire;
  atomic_uint fileAcquired;
  atomic_uint fileRequest;
  atomic_uint fileCancel;
  atomic_bool acceptRequest;
  atomic_bool blockChunkOnce;
  atomic_bool attached;
  atomic_bool fileOfferBeforeNotice;
  LG_ClipboardData noticeTypes[LG_CLIPBOARD_DATA_NONE];
  size_t noticeCount;
  LG_ClipboardRequest requestID;
  LG_ClipboardData requestType;
  LG_ClipboardRequest streamID;
  LG_ClipboardData streamType;
  uint64_t sizeHint;
  uint64_t chunkOffset;
  size_t chunkSize;
  uint8_t chunkData[64];
  uint64_t finalSize;
  LG_ClipboardRequest cancelID;
  LG_ClipboardCancelReason cancelReason;
  uint64_t fileDataset;
  uint64_t fileAcquisition;
  LG_ClipboardFileError fileError;
  LG_ClipboardFileRequest fileRequestValue;
}
EventTrace;

typedef struct TestState
{
  void                  * memory;
  PLGMPHost               host;
  PLGMPHostQueue          queue;
  PLGMPClient             client;
  LGMPClipboard         * clipboard;
  const LG_ClipboardOps * ops;
  uint32_t                clientID;
  uint32_t                hostSerial;
  uint64_t                localClipboardGeneration;
  PLGMPMemory             memories[TEST_MEMORY_MAX];
  unsigned                memoryCount;
  PLGMPMemory             grantMemory[KVMFR_CLIPBOARD_SLOT_COUNT];
  atomic_uint             statusCount;
  atomic_bool             statusAvailable;
  atomic_uint             statusGeneration;
  atomic_bool             autoAttach;
  EventTrace              events;
}
TestState;

typedef struct DisconnectTask
{
  LGMPClipboard * clipboard;
  atomic_bool      done;
}
DisconnectTask;

static bool disconnectAndDrain(TestState * state,
    uint32_t * releaseGeneration);

static void notice(void * opaque, const LG_ClipboardData types[],
    size_t count)
{
  EventTrace * trace = opaque;
  if (count == 1 && types[0] == LG_CLIPBOARD_DATA_FILES)
    atomic_store_explicit(&trace->fileOfferBeforeNotice,
        atomic_load_explicit(&trace->fileOffer, memory_order_acquire) != 0,
        memory_order_release);
  trace->noticeCount = count;
  if (count)
    memcpy(trace->noticeTypes, types, count * sizeof(*types));
  atomic_fetch_add_explicit(&trace->notice, 1, memory_order_release);
}

static void releaseNotice(void * opaque)
{
  EventTrace * trace = opaque;
  atomic_fetch_add_explicit(&trace->release, 1, memory_order_release);
}

static bool request(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type)
{
  EventTrace * trace = opaque;
  trace->requestID   = id;
  trace->requestType = type;
  atomic_fetch_add_explicit(&trace->request, 1, memory_order_release);
  return atomic_load_explicit(&trace->acceptRequest, memory_order_acquire);
}

static LG_ClipboardResult dataBegin(void * opaque,
    LG_ClipboardRequest id, LG_ClipboardData type, uint64_t sizeHint)
{
  EventTrace * trace = opaque;
  trace->streamID   = id;
  trace->streamType = type;
  trace->sizeHint   = sizeHint;
  atomic_fetch_add_explicit(&trace->dataBegin, 1, memory_order_release);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult dataChunk(void * opaque,
    LG_ClipboardRequest id, uint64_t offset,
    const void * data, size_t size)
{
  EventTrace * trace = opaque;
  trace->streamID    = id;
  trace->chunkOffset = offset;
  trace->chunkSize   = size;
  if (size <= sizeof(trace->chunkData))
    memcpy(trace->chunkData, data, size);
  atomic_fetch_add_explicit(&trace->dataChunk, 1, memory_order_release);
  if (atomic_exchange_explicit(&trace->blockChunkOnce, false,
      memory_order_acq_rel))
    return LG_CLIPBOARD_RESULT_BLOCKED;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult dataEnd(void * opaque,
    LG_ClipboardRequest id, uint64_t size)
{
  EventTrace * trace = opaque;
  trace->streamID  = id;
  trace->finalSize = size;
  atomic_fetch_add_explicit(&trace->dataEnd, 1, memory_order_release);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void dataCancelEvent(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  EventTrace * trace = opaque;
  trace->cancelID     = id;
  trace->cancelReason = reason;
  atomic_fetch_add_explicit(&trace->dataCancel, 1, memory_order_release);
}

static void dataReadyEvent(void * opaque, LG_ClipboardRequest id)
{
  EventTrace * trace = opaque;
  trace->streamID = id;
  atomic_fetch_add_explicit(&trace->dataReady, 1, memory_order_release);
}

static void requestCancelEvent(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  EventTrace * trace = opaque;
  trace->cancelID     = id;
  trace->cancelReason = reason;
  atomic_fetch_add_explicit(
      &trace->requestCancel, 1, memory_order_release);
}

static void fileOfferEvent(void * opaque, uint64_t dataset)
{
  EventTrace * trace = opaque;
  trace->fileDataset = dataset;
  atomic_fetch_add_explicit(&trace->fileOffer, 1, memory_order_release);
}

static void fileAcquireEvent(void * opaque, uint64_t dataset,
    uint64_t acquisition)
{
  EventTrace * trace = opaque;
  trace->fileDataset     = dataset;
  trace->fileAcquisition = acquisition;
  atomic_fetch_add_explicit(&trace->fileAcquire, 1, memory_order_release);
}

static void fileAcquiredEvent(void * opaque, uint64_t dataset,
    uint64_t acquisition, LG_ClipboardFileError error)
{
  EventTrace * trace = opaque;
  trace->fileDataset     = dataset;
  trace->fileAcquisition = acquisition;
  trace->fileError       = error;
  atomic_fetch_add_explicit(&trace->fileAcquired, 1, memory_order_release);
}

static void fileRequestEvent(void * opaque,
    const LG_ClipboardFileRequest * request)
{
  EventTrace * trace = opaque;
  trace->fileRequestValue = *request;
  atomic_fetch_add_explicit(&trace->fileRequest, 1, memory_order_release);
}

static void fileCancelEvent(void * opaque, uint64_t dataset,
    uint64_t request, LG_ClipboardFileError error)
{
  EventTrace * trace = opaque;
  trace->fileDataset     = dataset;
  trace->fileAcquisition = request;
  trace->fileError       = error;
  atomic_fetch_add_explicit(&trace->fileCancel, 1, memory_order_release);
}

static const LG_ClipboardEventOps EVENT_OPS =
{
  .notice        = notice,
  .dataBegin     = dataBegin,
  .dataChunk     = dataChunk,
  .dataEnd       = dataEnd,
  .dataCancel    = dataCancelEvent,
  .dataReady     = dataReadyEvent,
  .requestCancel = requestCancelEvent,
  .release       = releaseNotice,
  .request       = request,
  .fileOffer     = fileOfferEvent,
  .fileAcquire   = fileAcquireEvent,
  .fileAcquired  = fileAcquiredEvent,
  .fileRequest   = fileRequestEvent,
  .fileCancel    = fileCancelEvent,
};

static void statusChanged(void * opaque, const LG_ClipboardStatus * status)
{
  TestState * state = opaque;
  atomic_store_explicit(&state->statusAvailable,
    status->available, memory_order_release);
  atomic_store_explicit(&state->statusGeneration,
    status->generation, memory_order_release);
  atomic_fetch_add_explicit(&state->statusCount, 1, memory_order_release);

  if (status->available &&
      atomic_load_explicit(&state->autoAttach, memory_order_acquire) &&
      !atomic_exchange_explicit(
        &state->events.attached, true, memory_order_acq_rel))
  {
    const bool attached = state->ops->attach(
      state->clipboard, &EVENT_OPS, &state->events);
    atomic_store_explicit(
      &state->events.attached, attached, memory_order_release);
  }
}

static bool hostProcess(TestState * state)
{
  CHECK(lgmpHostProcess(state->host) == LGMP_OK);
  return true;
}

static bool waitAtomic(TestState * state, atomic_uint * value,
    unsigned expected)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (atomic_load_explicit(value, memory_order_acquire) >= expected)
      return true;
    usleep(1000);
  }
  return false;
}

static bool waitMemory(TestState * state, PLGMPMemory memory)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (!lgmpHostQueuePayloadPending(state->queue, memory))
      return true;
    usleep(1000);
  }
  return false;
}

static bool allocMemory(TestState * state, size_t size,
    PLGMPMemory * result)
{
  CHECK(state->memoryCount < TEST_MEMORY_MAX);
  CHECK(lgmpHostMemAlloc(state->host, (uint32_t)size, result) == LGMP_OK);
  state->memories[state->memoryCount++] = *result;
  memset(lgmpHostMemPtr(*result), 0, size);
  return true;
}

static bool postMemory(TestState * state, PLGMPMemory memory,
    KVMFRClipboardQueueType type, uint32_t serial)
{
  const uint64_t udata = KVMFR_CLIPBOARD_QUEUE_UDATA(type, serial);
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    unsigned recipients = 0;
    const LGMP_STATUS status = lgmpHostQueuePostForClients(state->queue,
      udata, memory, &state->clientID, 1, &recipients);
    if (status == LGMP_OK)
      return recipients == 1;
    CHECK(status == LGMP_ERR_QUEUE_FULL);
    CHECK(hostProcess(state));
    usleep(1000);
  }
  return false;
}

static uint32_t nextSerial(TestState * state)
{
  if (++state->hostSerial == 0)
    ++state->hostSerial;
  return state->hostSerial;
}

static bool postStatus(TestState * state, uint32_t endpointGeneration,
    uint32_t ownerClientID, uint32_t ownerGeneration,
    bool available, KVMFRClipboardFormatFlags formats)
{
  PLGMPMemory memory;
  CHECK(allocMemory(state, sizeof(KVMFRClipboardStatus), &memory));
  KVMFRClipboardStatus * status = lgmpHostMemPtr(memory);
  *status = (KVMFRClipboardStatus)
  {
    .version         = KVMFR_CLIPBOARD_VERSION,
    .flags           = (available ? KVMFR_CLIPBOARD_STATUS_AVAILABLE : 0) |
      (ownerClientID ? KVMFR_CLIPBOARD_STATUS_HAS_OWNER : 0),
    .generation      = endpointGeneration,
    .ownerClientID   = ownerClientID,
    .ownerGeneration = ownerGeneration,
    .lease            = 1000,
    .formats          = available ? formats : 0,
    .slotBytes        = KVMFR_CLIPBOARD_DATA_BYTES,
  };
  CHECK(postMemory(state, memory, KVMFR_CLIPBOARD_QUEUE_STATUS,
        nextSerial(state)));
  CHECK(waitMemory(state, memory));
  return true;
}

static bool postRecord(TestState * state,
    const KVMFRClipboardMessage * record,
    KVMFRClipboardQueueType type, const void * data)
{
  const size_t bytes = type == KVMFR_CLIPBOARD_QUEUE_DATA ?
    SLOT_BYTES : sizeof(*record);
  PLGMPMemory memory;
  CHECK(allocMemory(state, bytes, &memory));
  memcpy(lgmpHostMemPtr(memory), record, sizeof(*record));
  if (record->length && data)
    memcpy((uint8_t *)lgmpHostMemPtr(memory) + sizeof(*record),
      data, record->length);
  CHECK(postMemory(state, memory, type, nextSerial(state)));
  return true;
}

static bool postGrant(TestState * state, uint32_t generation,
    uint32_t token)
{
  CHECK(token >= 1 && token <= KVMFR_CLIPBOARD_SLOT_COUNT);
  PLGMPMemory * memory = &state->grantMemory[token - 1];
  if (!*memory)
    CHECK(allocMemory(state, SLOT_BYTES, memory));
  else
    CHECK(waitMemory(state, *memory));

  KVMFRClipboardSlotHeader * header = lgmpHostMemPtr(*memory);
  memset(header, 0, sizeof(*header));
  header->version    = KVMFR_CLIPBOARD_VERSION;
  header->type       = KVMFR_CLIPBOARD_MESSAGE_GRANT;
  header->generation = generation;
  header->size       = KVMFR_CLIPBOARD_DATA_BYTES;
  header->token      = token;
  return postMemory(state, *memory,
    KVMFR_CLIPBOARD_QUEUE_GRANT, token);
}

static bool readClient(TestState * state, KVMFRClipboardMessage * record)
{
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    size_t size = sizeof(*record);
    uint32_t clientID = 0;
    const LGMP_STATUS status = lgmpHostReadDataWithSource(
      state->queue, record, &size, &clientID);
    if (status == LGMP_ERR_QUEUE_EMPTY)
    {
      usleep(1000);
      continue;
    }
    CHECK(status == LGMP_OK);
    CHECK(size == sizeof(*record));
    CHECK(clientID == state->clientID);
    CHECK(lgmpHostAckData(state->queue) == LGMP_OK);
    return true;
  }
  return false;
}

static bool readType(TestState * state, KVMFRClipboardMessageType type,
    KVMFRClipboardMessage * record)
{
  for (unsigned i = 0; i < 32; ++i)
  {
    CHECK(readClient(state, record));
    if (record->type == type)
      return true;
  }
  return false;
}

static bool readNonKeepalive(TestState * state,
    KVMFRClipboardMessage * record)
{
  for (unsigned i = 0; i < 32; ++i)
  {
    CHECK(readClient(state, record));
    if (record->type != KVMFR_CLIPBOARD_MESSAGE_KEEPALIVE)
      return true;
  }
  return false;
}

static bool noClientData(TestState * state)
{
  for (unsigned i = 0; i < TEST_QUIET_MS; ++i)
  {
    CHECK(hostProcess(state));
    KVMFRClipboardMessage record;
    size_t size = sizeof(record);
    uint32_t clientID = 0;
    const LGMP_STATUS status = lgmpHostReadDataWithSource(
      state->queue, &record, &size, &clientID);
    if (status == LGMP_ERR_QUEUE_EMPTY)
    {
      usleep(1000);
      continue;
    }
    if (status == LGMP_OK)
      lgmpHostAckData(state->queue);
    CHECK(status == LGMP_ERR_QUEUE_EMPTY);
  }
  return true;
}

static bool claim(TestState * state, uint32_t endpointGeneration,
    bool autoAttach, KVMFRClipboardMessage * result)
{
  atomic_store(&state->autoAttach, autoAttach);
  CHECK(postStatus(state, endpointGeneration, 0, 0, true, 0));
  if (autoAttach)
  {
    for (unsigned i = 0; i < TEST_WAIT_MS &&
        !atomic_load(&state->events.attached); ++i)
    {
      CHECK(hostProcess(state));
      usleep(1000);
    }
    CHECK(atomic_load(&state->events.attached));
  }
  else
  {
    CHECK(state->ops->attach(
          state->clipboard, &EVENT_OPS, &state->events));
    atomic_store(&state->events.attached, true);
  }
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_CLAIM, result));
  CHECK(result->version == KVMFR_CLIPBOARD_VERSION);
  CHECK(result->generation != 0);
  CHECK(result->token == endpointGeneration);
  return true;
}

static bool own(TestState * state, uint32_t endpointGeneration,
    uint32_t claimGeneration)
{
  CHECK(postStatus(state, endpointGeneration, state->clientID,
        claimGeneration, true, 0));
  return true;
}

static KVMFRClipboardMessage hostRecord(
    KVMFRClipboardMessageType type, uint32_t generation)
{
  return (KVMFRClipboardMessage)
  {
    .version    = KVMFR_CLIPBOARD_VERSION,
    .type       = type,
    .generation = generation,
  };
}

static bool offerAndRequest(TestState * state, uint32_t generation,
    uint64_t clipboardGeneration, LG_ClipboardRequest appRequest,
    KVMFRClipboardMessage * wireRequest)
{
  KVMFRClipboardMessage offer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, generation);
  offer.clipboardGeneration = clipboardGeneration;
  offer.token = KVMFR_CLIPBOARD_FORMAT_MASK_TEXT |
    KVMFR_CLIPBOARD_FORMAT_MASK_PNG;
  const unsigned notices = atomic_load(&state->events.notice) + 1;
  CHECK(postRecord(state, &offer, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.notice, notices));
  CHECK(state->events.noticeCount == 2);
  CHECK(state->events.noticeTypes[0] == LG_CLIPBOARD_DATA_TEXT);
  CHECK(state->events.noticeTypes[1] == LG_CLIPBOARD_DATA_PNG);
  CHECK(!state->ops->request(state->clipboard,
        appRequest + 1, LG_CLIPBOARD_DATA_JPEG));
  CHECK(state->ops->request(state->clipboard,
        appRequest, LG_CLIPBOARD_DATA_TEXT));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_REQUEST, wireRequest));
  CHECK(kvmfrClipboardTransferFromClient(wireRequest->transfer));
  CHECK(wireRequest->generation == generation);
  CHECK(wireRequest->clipboardGeneration == clipboardGeneration);
  CHECK(wireRequest->format == KVMFR_CLIPBOARD_FORMAT_TEXT);
  return true;
}

static bool testClaim(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 10, true, &claimRecord));
  CHECK(own(state, 10, claimRecord.generation));

  const LG_ClipboardData formats[] = { LG_CLIPBOARD_DATA_TEXT };
  CHECK(state->ops->notifyTypes(state->clipboard, formats, 1));
  KVMFRClipboardMessage offer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &offer));
  state->localClipboardGeneration = offer.clipboardGeneration;
  CHECK(offer.generation == claimRecord.generation);
  CHECK(offer.clipboardGeneration != 0);
  CHECK(offer.token == KVMFR_CLIPBOARD_FORMAT_MASK_TEXT);
  return true;
}

static bool testOfferRequest(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 20, false, &claimRecord));
  CHECK(own(state, 20, claimRecord.generation));
  KVMFRClipboardMessage requestRecord;
  CHECK(offerAndRequest(state, claimRecord.generation,
        200, 77, &requestRecord));
  return true;
}

static bool testInboundStream(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 30, false, &claimRecord));
  CHECK(own(state, 30, claimRecord.generation));
  KVMFRClipboardMessage requestRecord;
  CHECK(offerAndRequest(state, claimRecord.generation,
        300, 88, &requestRecord));

  const uint8_t bytes[] = { 1, 2, 3, 4, 5 };
  KVMFRClipboardMessage data =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, claimRecord.generation);
  data.clipboardGeneration = 300;
  data.transfer = requestRecord.transfer;
  data.size     = 5;
  data.format   = KVMFR_CLIPBOARD_FORMAT_TEXT;
  data.flags    = KVMFR_CLIPBOARD_FLAG_BEGIN;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitAtomic(state, &state->events.dataBegin, 1));
  CHECK(atomic_load(&state->events.dataBegin) == 1);
  CHECK(atomic_load(&state->events.dataChunk) == 0);
  CHECK(state->events.streamID == 88);
  CHECK(state->events.sizeHint == 5);

  data.sequence = 1;
  data.size     = KVMFR_CLIPBOARD_SIZE_UNKNOWN;
  data.flags    = 0;
  data.length   = sizeof(bytes);
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, bytes));
  CHECK(waitAtomic(state, &state->events.dataChunk, 1));
  CHECK(state->events.chunkOffset == 0);
  CHECK(state->events.chunkSize == sizeof(bytes));
  CHECK(memcmp(state->events.chunkData, bytes, sizeof(bytes)) == 0);

  data.sequence = 2;
  data.offset   = sizeof(bytes);
  data.size     = sizeof(bytes);
  data.flags    = KVMFR_CLIPBOARD_FLAG_END;
  data.length   = 0;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitAtomic(state, &state->events.dataEnd, 1));
  CHECK(atomic_load(&state->events.dataChunk) == 1);
  CHECK(state->events.finalSize == 5);

  KVMFRClipboardMessage request2;
  CHECK(offerAndRequest(state, claimRecord.generation,
        301, 89, &request2));
  data = hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA,
    claimRecord.generation);
  data.clipboardGeneration = 301;
  data.transfer = request2.transfer;
  data.format   = KVMFR_CLIPBOARD_FORMAT_TEXT;
  data.flags    = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitAtomic(state, &state->events.dataEnd, 2));
  CHECK(atomic_load(&state->events.dataBegin) == 2);
  CHECK(state->events.finalSize == 0);
  return true;
}

static bool prepareOutbound(TestState * state, uint32_t endpoint,
    KVMFRClipboardMessage * claimRecord, uint64_t transfer)
{
  CHECK(claim(state, endpoint, false, claimRecord));
  CHECK(own(state, endpoint, claimRecord->generation));
  const LG_ClipboardData formats[] = { LG_CLIPBOARD_DATA_TEXT };
  CHECK(state->ops->notifyTypes(state->clipboard, formats, 1));
  KVMFRClipboardMessage offer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &offer));
  state->localClipboardGeneration = offer.clipboardGeneration;

  KVMFRClipboardMessage requestRecord =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_REQUEST,
      claimRecord->generation);
  requestRecord.clipboardGeneration = offer.clipboardGeneration;
  requestRecord.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER | transfer;
  requestRecord.format   = KVMFR_CLIPBOARD_FORMAT_TEXT;
  const unsigned requests = atomic_load(&state->events.request) + 1;
  CHECK(postRecord(state, &requestRecord,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.request, requests));
  CHECK(state->events.requestID == requestRecord.transfer);
  CHECK(state->events.requestType == LG_CLIPBOARD_DATA_TEXT);
  return true;
}

static bool checkCommit(TestState * state, uint32_t token,
    KVMFRClipboardMessage * commit, KVMFRClipboardMessage * slot,
    const void ** data)
{
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_COMMIT, commit));
  CHECK(commit->token == token);
  *slot = *(KVMFRClipboardMessage *)
    lgmpHostMemPtr(state->grantMemory[token - 1]);
  CHECK(slot->type == KVMFR_CLIPBOARD_MESSAGE_DATA);
  CHECK(slot->generation == commit->generation);
  CHECK(slot->clipboardGeneration == commit->clipboardGeneration);
  CHECK(slot->transfer == commit->transfer);
  CHECK(slot->offset == commit->offset);
  CHECK(slot->size == commit->size);
  CHECK(slot->format == commit->format);
  CHECK(slot->flags == commit->flags);
  CHECK(slot->length == commit->length);
  CHECK(slot->sequence == commit->sequence);
  *data = (const uint8_t *)lgmpHostMemPtr(
    state->grantMemory[token - 1]) + sizeof(*slot);
  return true;
}

static bool checkFileCommit(TestState * state, uint32_t grantToken,
    KVMFRClipboardFileOperation operation,
    KVMFRClipboardMessage * commit, KVMFRClipboardMessage * slot,
    const void ** data)
{
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_COMMIT, commit));
  CHECK(commit->token == grantToken);
  *slot = *(KVMFRClipboardMessage *)
    lgmpHostMemPtr(state->grantMemory[grantToken - 1]);
  CHECK(slot->type == KVMFR_CLIPBOARD_MESSAGE_FILE_DATA);
  CHECK(slot->generation == commit->generation);
  CHECK(slot->clipboardGeneration == commit->clipboardGeneration);
  CHECK(slot->transfer == commit->transfer);
  CHECK(slot->offset == commit->offset);
  CHECK(slot->size == commit->size);
  CHECK(slot->format == commit->format);
  CHECK(slot->flags == commit->flags);
  CHECK(slot->length == commit->length);
  CHECK(slot->sequence == commit->sequence);
  CHECK(slot->token == operation);
  *data = (const uint8_t *)lgmpHostMemPtr(
    state->grantMemory[grantToken - 1]) + sizeof(*slot);
  return true;
}

static bool testOutboundGrant(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(prepareOutbound(state, 40, &claimRecord, 1));
  const LG_ClipboardRequest transfer = state->events.requestID;
  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));

  CHECK(state->ops->dataBegin(state->clipboard, transfer,
        LG_CLIPBOARD_DATA_TEXT, 5) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(noClientData(state));
  const uint8_t first[] = { 1, 2, 3 };
  CHECK(state->ops->dataChunk(state->clipboard, transfer,
        0, first, sizeof(first)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  KVMFRClipboardMessage commit;
  KVMFRClipboardMessage slot;
  const void * data;
  CHECK(checkCommit(state, 1, &commit, &slot, &data));
  CHECK(slot.flags == KVMFR_CLIPBOARD_FLAG_BEGIN);
  CHECK(slot.sequence == 0 && slot.offset == 0 && slot.size == 5);
  CHECK(memcmp(data, first, sizeof(first)) == 0);

  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  const uint8_t second[] = { 4, 5 };
  CHECK(state->ops->dataChunk(state->clipboard, transfer,
        3, second, sizeof(second)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(checkCommit(state, 1, &commit, &slot, &data));
  CHECK(slot.flags == 0 && slot.sequence == 1 && slot.offset == 3);
  CHECK(slot.size == KVMFR_CLIPBOARD_SIZE_UNKNOWN);
  CHECK(memcmp(data, second, sizeof(second)) == 0);

  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  CHECK(state->ops->dataEnd(state->clipboard, transfer, 5) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(checkCommit(state, 1, &commit, &slot, &data));
  CHECK(slot.flags == KVMFR_CLIPBOARD_FLAG_END);
  CHECK(slot.sequence == 2 && slot.offset == 5 && slot.size == 5);
  CHECK(slot.length == 0);

  KVMFRClipboardMessage requestRecord =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_REQUEST,
      claimRecord.generation);
  requestRecord.clipboardGeneration = slot.clipboardGeneration;
  requestRecord.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER | 2;
  requestRecord.format = KVMFR_CLIPBOARD_FORMAT_TEXT;
  CHECK(postRecord(state, &requestRecord,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.request, 2));
  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  CHECK(state->ops->dataBegin(state->clipboard, requestRecord.transfer,
        LG_CLIPBOARD_DATA_TEXT, 0) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(state->ops->dataEnd(state->clipboard,
        requestRecord.transfer, 0) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(checkCommit(state, 1, &commit, &slot, &data));
  CHECK(slot.flags == (KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END));
  CHECK(slot.sequence == 0 && slot.offset == 0 && slot.size == 0);
  return true;
}

static bool testBlocked(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(prepareOutbound(state, 50, &claimRecord, 3));
  const LG_ClipboardRequest transfer = state->events.requestID;
  CHECK(state->ops->dataBegin(state->clipboard, transfer,
        LG_CLIPBOARD_DATA_TEXT, 2) == LG_CLIPBOARD_RESULT_ACCEPTED);
  const uint8_t bytes[] = { 8, 9 };
  CHECK(state->ops->dataChunk(state->clipboard, transfer,
        0, bytes, sizeof(bytes)) == LG_CLIPBOARD_RESULT_BLOCKED);
  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitAtomic(state, &state->events.dataReady, 1));
  CHECK(state->events.streamID == transfer);
  CHECK(state->ops->dataChunk(state->clipboard, transfer,
        0, bytes, sizeof(bytes)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  KVMFRClipboardMessage commit;
  KVMFRClipboardMessage slot;
  const void * data;
  CHECK(checkCommit(state, 1, &commit, &slot, &data));

  KVMFRClipboardMessage requestRecord;
  CHECK(offerAndRequest(state, claimRecord.generation,
        501, 5001, &requestRecord));
  atomic_store(&state->events.blockChunkOnce, true);
  KVMFRClipboardMessage wire =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, claimRecord.generation);
  wire.clipboardGeneration = 501;
  wire.transfer = requestRecord.transfer;
  wire.size     = 1;
  wire.format   = KVMFR_CLIPBOARD_FORMAT_TEXT;
  wire.flags    = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  wire.length   = 1;
  CHECK(postRecord(state, &wire, KVMFR_CLIPBOARD_QUEUE_DATA, bytes));
  CHECK(waitAtomic(state, &state->events.dataChunk, 1));
  CHECK(atomic_load(&state->events.dataEnd) == 0);
  CHECK(state->ops->dataReady(state->clipboard, 5001));
  CHECK(waitAtomic(state, &state->events.dataEnd, 1));
  CHECK(atomic_load(&state->events.dataChunk) == 2);
  return true;
}

static bool testCancel(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(prepareOutbound(state, 60, &claimRecord, 4));
  const LG_ClipboardRequest outbound = state->events.requestID;
  KVMFRClipboardMessage cancel =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_CANCEL, claimRecord.generation);
  cancel.clipboardGeneration = state->localClipboardGeneration;
  cancel.transfer            = outbound;
  cancel.format              = KVMFR_CLIPBOARD_FORMAT_TEXT;
  cancel.token               = LG_CLIPBOARD_CANCEL_ABORTED;
  CHECK(postRecord(state, &cancel, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.requestCancel, 1));
  CHECK(state->events.cancelID == outbound);

  KVMFRClipboardMessage requestRecord;
  CHECK(offerAndRequest(state, claimRecord.generation,
        601, 6001, &requestRecord));
  cancel.clipboardGeneration = 601;
  cancel.transfer = requestRecord.transfer;
  cancel.token = LG_CLIPBOARD_CANCEL_REPLACED;
  CHECK(postRecord(state, &cancel, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.dataCancel, 1));
  CHECK(state->events.cancelID == 6001);
  CHECK(state->events.cancelReason == LG_CLIPBOARD_CANCEL_REPLACED);

  KVMFRClipboardMessage outboundRequest =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_REQUEST,
      claimRecord.generation);
  outboundRequest.clipboardGeneration = state->localClipboardGeneration;
  outboundRequest.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER | 5;
  outboundRequest.format = KVMFR_CLIPBOARD_FORMAT_TEXT;
  CHECK(postRecord(state, &outboundRequest,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.request, 2));
  CHECK(state->ops->dataCancel(state->clipboard,
        outboundRequest.transfer, LG_CLIPBOARD_CANCEL_ABORTED));
  KVMFRClipboardMessage wireCancel;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_CANCEL, &wireCancel));
  CHECK(wireCancel.transfer == outboundRequest.transfer);
  CHECK(wireCancel.clipboardGeneration ==
      state->localClipboardGeneration);
  CHECK(wireCancel.format == KVMFR_CLIPBOARD_FORMAT_TEXT);
  CHECK(wireCancel.token == LG_CLIPBOARD_CANCEL_ABORTED);
  return true;
}

static bool testRestart(TestState * state)
{
  KVMFRClipboardMessage oldClaim;
  CHECK(claim(state, 70, false, &oldClaim));
  CHECK(own(state, 70, oldClaim.generation));
  const LG_ClipboardData formats[] = { LG_CLIPBOARD_DATA_TEXT };
  CHECK(state->ops->notifyTypes(state->clipboard, formats, 1));
  KVMFRClipboardMessage oldOffer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &oldOffer));

  CHECK(postStatus(state, 71, 0, 0, true, 0));
  KVMFRClipboardMessage newClaim;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_CLAIM, &newClaim));
  CHECK(newClaim.generation != oldClaim.generation);
  CHECK(newClaim.token == 71);
  KVMFRClipboardMessage replay;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &replay));
  CHECK(replay.generation == newClaim.generation);
  CHECK(replay.clipboardGeneration == oldOffer.clipboardGeneration);
  CHECK(own(state, 71, newClaim.generation));

  CHECK(postStatus(state, 71, 0, 0, true, 0));
  KVMFRClipboardMessage reclaimed;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_CLAIM, &reclaimed));
  CHECK(reclaimed.generation != newClaim.generation);
  CHECK(reclaimed.token == 71);
  return true;
}

static bool testFileLimits(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 75, false, &claimRecord));
  CHECK(own(state, 75, claimRecord.generation));

  const uint64_t localDataset = UINT64_C(0x1234000012340001);
  const uint64_t remoteDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x5678000056780001);
  CHECK(state->ops->offerFiles(state->clipboard, localDataset));
  KVMFRClipboardMessage wire;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &wire));
  CHECK(wire.clipboardGeneration == localDataset);

  KVMFRClipboardMessage offer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, claimRecord.generation);
  offer.clipboardGeneration = remoteDataset;
  offer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  CHECK(postRecord(state, &offer, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.notice, 1));
  CHECK(atomic_load(&state->events.fileOffer) == 1);
  CHECK(atomic_load(&state->events.fileOfferBeforeNotice));
  CHECK(state->events.fileDataset == remoteDataset);

  KVMFRClipboardMessage staleAcquire =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE,
      claimRecord.generation);
  staleAcquire.clipboardGeneration = localDataset + 1U;
  staleAcquire.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x98);
  staleAcquire.format = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &staleAcquire,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED, &wire));
  CHECK(wire.clipboardGeneration == staleAcquire.clipboardGeneration);
  CHECK(wire.transfer == staleAcquire.transfer);
  CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_STALE);
  CHECK(atomic_load(&state->events.fileAcquire) == 0);

  KVMFRClipboardMessage staleRequest =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST,
      claimRecord.generation);
  staleRequest.clipboardGeneration = localDataset;
  staleRequest.transfer            = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x99);
  staleRequest.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  staleRequest.token               = KVMFR_CLIPBOARD_FILE_OP_LIST;
  CHECK(postRecord(state, &staleRequest,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL, &wire));
  CHECK(wire.clipboardGeneration == staleRequest.clipboardGeneration);
  CHECK(wire.transfer == staleRequest.transfer);
  CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_STALE);
  CHECK(atomic_load(&state->events.fileRequest) == 0);

  for (uint64_t i = 0; i < 4; ++i)
  {
    const uint64_t acquisition = UINT64_C(0x100) + i;
    CHECK(state->ops->fileAcquire(
          state->clipboard, remoteDataset, acquisition));
    CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE, &wire));
    CHECK(wire.clipboardGeneration == remoteDataset);
    CHECK(wire.transfer == acquisition);

    KVMFRClipboardMessage acquired =
      hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED,
        claimRecord.generation);
    acquired.clipboardGeneration = remoteDataset;
    acquired.transfer            = acquisition;
    acquired.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    CHECK(postRecord(state, &acquired,
          KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
    CHECK(waitAtomic(state, &state->events.fileAcquired, (unsigned)i + 1));
    CHECK(state->events.fileError == LG_CLIPBOARD_FILE_ERROR_NONE);
  }

  for (uint64_t i = 0; i < 4; ++i)
  {
    const uint64_t acquisition = KVMFR_CLIPBOARD_TRANSFER_HELPER |
      (UINT64_C(0x200) + i);
    KVMFRClipboardMessage acquire =
      hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE,
        claimRecord.generation);
    acquire.clipboardGeneration = localDataset;
    acquire.transfer            = acquisition;
    acquire.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    CHECK(postRecord(state, &acquire,
          KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
    CHECK(waitAtomic(state, &state->events.fileAcquire, (unsigned)i + 1));
    CHECK(state->events.fileDataset == localDataset);
    CHECK(state->events.fileAcquisition == acquisition);
    CHECK(state->ops->fileAcquired(state->clipboard,
          localDataset, acquisition, LG_CLIPBOARD_FILE_ERROR_NONE));
    CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED, &wire));
    CHECK(wire.transfer == acquisition);
    CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_NONE);
  }

  CHECK(!state->ops->fileAcquire(
        state->clipboard, remoteDataset, UINT64_C(0x999)));
  KVMFRClipboardMessage overflowAcquire =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE,
      claimRecord.generation);
  overflowAcquire.clipboardGeneration = localDataset;
  overflowAcquire.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x999);
  overflowAcquire.format = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &overflowAcquire,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(readNonKeepalive(state, &wire));
  CHECK(wire.type == KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED);
  CHECK(wire.transfer == overflowAcquire.transfer);
  CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE);
  CHECK(atomic_load(&state->events.fileAcquire) == 4);

  for (uint64_t i = 0; i < 16; ++i)
  {
    const LG_ClipboardFileRequest request =
    {
      .dataset   = remoteDataset,
      .request   = UINT64_C(0x1000) + i,
      .operation = LG_CLIPBOARD_FILE_LIST,
    };
    CHECK(state->ops->fileRequest(state->clipboard, &request));
    CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST, &wire));
    CHECK(wire.clipboardGeneration == remoteDataset);
    CHECK(wire.transfer == request.request);
    CHECK(wire.token == KVMFR_CLIPBOARD_FILE_OP_LIST);
  }

  for (uint64_t i = 0; i < 16; ++i)
  {
    KVMFRClipboardMessage request =
      hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST,
        claimRecord.generation);
    request.clipboardGeneration = localDataset;
    request.transfer            = KVMFR_CLIPBOARD_TRANSFER_HELPER |
      (UINT64_C(0x2000) + i);
    request.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
    request.token               = KVMFR_CLIPBOARD_FILE_OP_LIST;
    CHECK(postRecord(state, &request,
          KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
    CHECK(waitAtomic(state, &state->events.fileRequest,
          (unsigned)i + 1));
    CHECK(state->events.fileRequestValue.dataset == localDataset);
    CHECK(state->events.fileRequestValue.request == request.transfer);
  }

  const LG_ClipboardFileRequest overflowRequest =
  {
    .dataset   = remoteDataset,
    .request   = UINT64_C(0x9999),
    .operation = LG_CLIPBOARD_FILE_LIST,
  };
  CHECK(!state->ops->fileRequest(state->clipboard, &overflowRequest));
  KVMFRClipboardMessage inboundOverflow =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST,
      claimRecord.generation);
  inboundOverflow.clipboardGeneration = localDataset;
  inboundOverflow.transfer            = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x9999);
  inboundOverflow.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  inboundOverflow.token               = KVMFR_CLIPBOARD_FILE_OP_LIST;
  CHECK(postRecord(state, &inboundOverflow,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(readNonKeepalive(state, &wire));
  CHECK(wire.type == KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL);
  CHECK(wire.transfer == inboundOverflow.transfer);
  CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_NO_SPACE);
  CHECK(atomic_load(&state->events.fileRequest) == 16);
  return true;
}

static bool testFileOwnerLoss(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 79, false, &claimRecord));
  CHECK(own(state, 79, claimRecord.generation));

  const uint64_t dataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x7900000179000001);
  KVMFRClipboardMessage offer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, claimRecord.generation);
  offer.clipboardGeneration = dataset;
  offer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  CHECK(postRecord(state, &offer, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileOffer, 1));

  const uint64_t acquisition = UINT64_C(0x7901);
  CHECK(state->ops->fileAcquire(
        state->clipboard, dataset, acquisition));
  KVMFRClipboardMessage wire;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE, &wire));

  KVMFRClipboardMessage acquired =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED,
      claimRecord.generation);
  acquired.clipboardGeneration = dataset;
  acquired.transfer            = acquisition;
  acquired.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &acquired,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileAcquired, 1));

  const LG_ClipboardFileRequest request =
  {
    .dataset   = dataset,
    .request   = UINT64_C(0x7902),
    .operation = LG_CLIPBOARD_FILE_LIST,
  };
  CHECK(state->ops->fileRequest(state->clipboard, &request));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST, &wire));

  const unsigned cancellations =
    atomic_load(&state->events.fileCancel);
  CHECK(postStatus(state, 80, 0, 0, true, 0));
  CHECK(waitAtomic(state, &state->events.fileCancel,
        cancellations + 2));
  CHECK(state->events.fileDataset == dataset);
  CHECK(state->events.fileAcquisition == acquisition);
  CHECK(state->events.fileError == LG_CLIPBOARD_FILE_ERROR_DISCONNECTED);
  return true;
}

static bool testMalformedFileData(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 81, false, &claimRecord));
  CHECK(own(state, 81, claimRecord.generation));

  const uint64_t dataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x8100000181000001);
  KVMFRClipboardMessage offer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, claimRecord.generation);
  offer.clipboardGeneration = dataset;
  offer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  CHECK(postRecord(state, &offer, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileOffer, 1));

  const uint64_t acquisition = UINT64_C(0x8101);
  CHECK(state->ops->fileAcquire(state->clipboard, dataset, acquisition));
  KVMFRClipboardMessage wire;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE, &wire));

  KVMFRClipboardMessage acquired =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED,
      claimRecord.generation);
  acquired.clipboardGeneration = dataset;
  acquired.transfer = acquisition;
  acquired.format = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &acquired,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileAcquired, 1));

  const LG_ClipboardFileRequest request =
  {
    .dataset   = dataset,
    .request   = UINT64_C(0x8102),
    .node      = 2,
    .length    = 4,
    .operation = LG_CLIPBOARD_FILE_READ,
  };
  CHECK(state->ops->fileRequest(state->clipboard, &request));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST, &wire));

  KVMFRClipboardMessage malformed =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_DATA,
      claimRecord.generation);
  malformed.clipboardGeneration = dataset;
  malformed.transfer            = request.request;
  malformed.offset              = 1;
  malformed.size                = request.length;
  malformed.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  malformed.flags               = KVMFR_CLIPBOARD_FLAG_BEGIN;
  malformed.length              = 1;
  malformed.token               = KVMFR_CLIPBOARD_FILE_OP_READ;
  const uint8_t byte = 0;
  const unsigned cancellations =
    atomic_load(&state->events.fileCancel);
  CHECK(postRecord(state, &malformed,
        KVMFR_CLIPBOARD_QUEUE_DATA, &byte));
  CHECK(waitAtomic(state, &state->events.fileCancel, cancellations + 1));
  CHECK(state->events.fileDataset == dataset);
  CHECK(state->events.fileAcquisition == request.request);
  CHECK(state->events.fileError == LG_CLIPBOARD_FILE_ERROR_INVALID);
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL, &wire));
  CHECK(wire.clipboardGeneration == dataset);
  CHECK(wire.transfer == request.request);
  CHECK(wire.token == KVMFR_CLIPBOARD_FILE_ERROR_INVALID);
  CHECK(!state->ops->fileCancel(state->clipboard,
        dataset, request.request, LG_CLIPBOARD_FILE_ERROR_CANCELLED));

  malformed.transfer = request.request + 1U;
  CHECK(postRecord(state, &malformed,
        KVMFR_CLIPBOARD_QUEUE_DATA, &byte));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.fileCancel) == cancellations + 1);
  return true;
}

static bool testFileStream(TestState * state)
{
  static uint8_t bytes[KVMFR_CLIPBOARD_FILE_READ_BYTES];
  for (size_t i = 0; i < sizeof(bytes); ++i)
    bytes[i] = (uint8_t)(i * 131U + 17U);

  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 78, false, &claimRecord));
  CHECK(own(state, 78, claimRecord.generation));
  const uint64_t dataset = UINT64_C(0x7800000178000001);
  CHECK(state->ops->offerFiles(state->clipboard, dataset));
  KVMFRClipboardMessage wire;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &wire));

  const uint64_t acquisition = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x7801);
  KVMFRClipboardMessage acquire =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE,
      claimRecord.generation);
  acquire.clipboardGeneration = dataset;
  acquire.transfer            = acquisition;
  acquire.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &acquire,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileAcquire, 1));
  CHECK(state->ops->fileAcquired(state->clipboard,
        dataset, acquisition, LG_CLIPBOARD_FILE_ERROR_NONE));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED, &wire));

  KVMFRClipboardMessage request =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_REQUEST,
      claimRecord.generation);
  request.clipboardGeneration = dataset;
  request.transfer            = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x7802);
  request.offset              = 123;
  request.size                = 42;
  request.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  request.flags               = sizeof(bytes);
  request.token               = KVMFR_CLIPBOARD_FILE_OP_READ;
  CHECK(postRecord(state, &request,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileRequest, 1));
  const LG_ClipboardFileRequest descriptor =
    state->events.fileRequestValue;
  CHECK(descriptor.dataset == dataset);
  CHECK(descriptor.request == request.transfer);
  CHECK(descriptor.node == 42);
  CHECK(descriptor.offset == 123);
  CHECK(descriptor.length == sizeof(bytes));
  CHECK(descriptor.operation == LG_CLIPBOARD_FILE_READ);
  CHECK(state->ops->fileDataBegin(state->clipboard,
        &descriptor, sizeof(bytes)) == LG_CLIPBOARD_RESULT_ACCEPTED);

  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  CHECK(state->ops->fileDataChunk(state->clipboard,
        &descriptor, 0, bytes, sizeof(bytes), true) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  KVMFRClipboardMessage commit;
  KVMFRClipboardMessage slot;
  const void * data;
  CHECK(checkFileCommit(state, 1, KVMFR_CLIPBOARD_FILE_OP_READ,
        &commit, &slot, &data));
  CHECK(slot.flags == (KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END));
  CHECK(slot.offset == 0 && slot.sequence == 0 &&
      slot.size == sizeof(bytes));
  CHECK(slot.length == sizeof(bytes));
  CHECK(memcmp(data, bytes, sizeof(bytes)) == 0);

  CHECK(state->ops->fileDataEnd(state->clipboard,
        &descriptor, sizeof(bytes)) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(noClientData(state));

  const unsigned requests = atomic_load(&state->events.fileRequest);
  request.transfer = KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(0x7803);
  request.offset   = 0;
  request.size     = KVMFR_CLIPBOARD_FILE_ROOT_NODE;
  request.flags    = 0;
  request.token    = KVMFR_CLIPBOARD_FILE_OP_LIST;
  CHECK(postRecord(state, &request,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileRequest, requests + 1));
  const LG_ClipboardFileRequest empty = state->events.fileRequestValue;
  CHECK(empty.operation == LG_CLIPBOARD_FILE_LIST);
  CHECK(state->ops->fileDataBegin(state->clipboard,
        &empty, KVMFR_CLIPBOARD_SIZE_UNKNOWN) ==
      LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  CHECK(state->ops->fileDataEnd(state->clipboard,
        &empty, 0) == LG_CLIPBOARD_RESULT_ACCEPTED);
  CHECK(checkFileCommit(state, 1, KVMFR_CLIPBOARD_FILE_OP_LIST,
        &commit, &slot, &data));
  CHECK(slot.flags == (KVMFR_CLIPBOARD_FLAG_BEGIN |
        KVMFR_CLIPBOARD_FLAG_END));
  CHECK(slot.offset == 0 && slot.sequence == 0 && slot.size == 0);
  CHECK(slot.length == 0);
  return true;
}

static void * disconnectThread(void * opaque)
{
  DisconnectTask * task = opaque;
  lgmpClipboard_disconnect(task->clipboard);
  atomic_store(&task->done, true);
  return NULL;
}

static bool disconnectAndDrain(TestState * state,
    uint32_t * releaseGeneration)
{
  DisconnectTask task = { .clipboard = state->clipboard };
  atomic_init(&task.done, false);
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, disconnectThread, &task) == 0);
  bool valid = true;
  for (unsigned i = 0; i < TEST_WAIT_MS && !atomic_load(&task.done); ++i)
  {
    if (lgmpHostProcess(state->host) != LGMP_OK)
      valid = false;
    for (;;)
    {
      KVMFRClipboardMessage record;
      size_t size = sizeof(record);
      uint32_t clientID = 0;
      const LGMP_STATUS status = lgmpHostReadDataWithSource(
        state->queue, &record, &size, &clientID);
      if (status == LGMP_ERR_QUEUE_EMPTY)
        break;
      if (status != LGMP_OK)
      {
        valid = false;
        break;
      }
      valid &= size == sizeof(record);
      valid &= clientID == state->clientID;
      if (record.type == KVMFR_CLIPBOARD_MESSAGE_RELEASE &&
          releaseGeneration)
        *releaseGeneration = record.generation;
      if (lgmpHostAckData(state->queue) != LGMP_OK)
        valid = false;
    }
    usleep(1000);
  }
  valid &= pthread_join(thread, NULL) == 0;
  valid &= atomic_load(&task.done);
  return valid;
}

static bool recreateClipboard(TestState * state)
{
  state->ops->setStatusListener(state->clipboard, NULL, NULL);
  if (atomic_load(&state->events.attached))
  {
    state->ops->detach(state->clipboard);
    atomic_store(&state->events.attached, false);
  }
  CHECK(disconnectAndDrain(state, NULL));
  lgmpClipboard_destroy(&state->clipboard);

  CHECK(lgmpClipboard_create(state->client, &state->clipboard));
  CHECK(lgmpClipboard_connect(state->clipboard, state->clientID));
  state->ops = lgmpClipboard_getOps();
  CHECK(state->ops);
  state->ops->setStatusListener(state->clipboard, statusChanged, state);
  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (lgmpHostQueueHasSubs(state->queue))
      return true;
    usleep(1000);
  }
  return false;
}

static bool testProcessRestart(TestState * state)
{
  KVMFRClipboardMessage oldClaim;
  CHECK(claim(state, 76, false, &oldClaim));
  CHECK(own(state, 76, oldClaim.generation));
  const LG_ClipboardData formats[] = { LG_CLIPBOARD_DATA_TEXT };
  CHECK(state->ops->notifyTypes(state->clipboard, formats, 1));
  KVMFRClipboardMessage oldLocalOffer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &oldLocalOffer));
  KVMFRClipboardMessage oldRequest;
  CHECK(offerAndRequest(state, oldClaim.generation,
        760, 7600, &oldRequest));
  const uint64_t oldFileDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x76000001);
  const uint64_t oldFileAcquisition = UINT64_C(0x76010001);
  KVMFRClipboardMessage fileOffer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, oldClaim.generation);
  fileOffer.clipboardGeneration = oldFileDataset;
  fileOffer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  const unsigned oldFileOffers = atomic_load(&state->events.fileOffer);
  CHECK(postRecord(state, &fileOffer,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileOffer, oldFileOffers + 1));
  CHECK(state->ops->fileAcquire(state->clipboard,
        oldFileDataset, oldFileAcquisition));
  KVMFRClipboardMessage fileWire;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE, &fileWire));

  CHECK(recreateClipboard(state));
  KVMFRClipboardMessage newClaim;
  CHECK(claim(state, 77, false, &newClaim));
  CHECK(own(state, 77, newClaim.generation));
  CHECK(state->ops->notifyTypes(state->clipboard, formats, 1));
  KVMFRClipboardMessage newLocalOffer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &newLocalOffer));
  CHECK(newLocalOffer.clipboardGeneration !=
      oldLocalOffer.clipboardGeneration);
  KVMFRClipboardMessage newRequest;
  CHECK(offerAndRequest(state, newClaim.generation,
        770, 7700, &newRequest));
  CHECK(newRequest.transfer != oldRequest.transfer);

  const unsigned cancelCount = atomic_load(&state->events.dataCancel);
  KVMFRClipboardMessage staleCancel =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_CANCEL, newClaim.generation);
  staleCancel.clipboardGeneration = 760;
  staleCancel.transfer            = oldRequest.transfer;
  staleCancel.format              = KVMFR_CLIPBOARD_FORMAT_TEXT;
  staleCancel.token               = LG_CLIPBOARD_CANCEL_ABORTED;
  CHECK(postRecord(state, &staleCancel,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.dataCancel) == cancelCount);

  const unsigned beginCount = atomic_load(&state->events.dataBegin);
  KVMFRClipboardMessage staleData =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, newClaim.generation);
  staleData.clipboardGeneration = 760;
  staleData.transfer            = oldRequest.transfer;
  staleData.format              = KVMFR_CLIPBOARD_FORMAT_TEXT;
  staleData.flags               = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  CHECK(postRecord(state, &staleData, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.dataBegin) == beginCount);

  KVMFRClipboardMessage data =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, newClaim.generation);
  data.clipboardGeneration = 770;
  data.transfer            = newRequest.transfer;
  data.format              = KVMFR_CLIPBOARD_FORMAT_TEXT;
  data.flags               = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  const unsigned endCount = atomic_load(&state->events.dataEnd);
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitAtomic(state, &state->events.dataEnd, endCount + 1));
  CHECK(atomic_load(&state->events.dataBegin) == beginCount + 1);

  const uint64_t newFileDataset = KVMFR_CLIPBOARD_TRANSFER_HELPER |
    UINT64_C(0x77000001);
  const uint64_t newFileAcquisition = UINT64_C(0x77010001);
  fileOffer = hostRecord(
      KVMFR_CLIPBOARD_MESSAGE_OFFER, newClaim.generation);
  fileOffer.clipboardGeneration = newFileDataset;
  fileOffer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  const unsigned newFileOffers = atomic_load(&state->events.fileOffer);
  CHECK(postRecord(state, &fileOffer,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileOffer, newFileOffers + 1));
  CHECK(state->ops->fileAcquire(state->clipboard,
        newFileDataset, newFileAcquisition));
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRE, &fileWire));

  const unsigned acquiredCount =
    atomic_load(&state->events.fileAcquired);
  KVMFRClipboardMessage staleAcquired =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED,
      newClaim.generation);
  staleAcquired.clipboardGeneration = oldFileDataset;
  staleAcquired.transfer            = oldFileAcquisition;
  staleAcquired.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &staleAcquired,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.fileAcquired) == acquiredCount);

  KVMFRClipboardMessage staleFileCancel =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_CANCEL,
      newClaim.generation);
  staleFileCancel.clipboardGeneration = oldFileDataset;
  staleFileCancel.transfer            = oldFileAcquisition;
  staleFileCancel.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  staleFileCancel.token               = KVMFR_CLIPBOARD_FILE_ERROR_STALE;
  CHECK(postRecord(state, &staleFileCancel,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.fileCancel) == 0);

  KVMFRClipboardMessage acquired =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_FILE_ACQUIRED,
      newClaim.generation);
  acquired.clipboardGeneration = newFileDataset;
  acquired.transfer            = newFileAcquisition;
  acquired.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &acquired,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.fileAcquired,
        acquiredCount + 1));
  CHECK(state->events.fileDataset == newFileDataset);
  CHECK(state->events.fileAcquisition == newFileAcquisition);
  return true;
}

static bool testDisconnect(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 80, false, &claimRecord));
  CHECK(own(state, 80, claimRecord.generation));
  uint32_t releaseGeneration = 0;
  CHECK(disconnectAndDrain(state, &releaseGeneration));
  CHECK(releaseGeneration == claimRecord.generation);
  return true;
}

static bool testMalformed(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 90, false, &claimRecord));
  CHECK(own(state, 90, claimRecord.generation));

  const unsigned statusCount = atomic_load(&state->statusCount);
  PLGMPMemory badStatusMemory;
  CHECK(allocMemory(state, sizeof(KVMFRClipboardStatus),
        &badStatusMemory));
  KVMFRClipboardStatus * badStatus = lgmpHostMemPtr(badStatusMemory);
  *badStatus = (KVMFRClipboardStatus)
  {
    .version    = KVMFR_CLIPBOARD_VERSION,
    .flags      = KVMFR_CLIPBOARD_STATUS_AVAILABLE,
    .generation = 91,
    .lease      = 1000,
  };
  CHECK(postMemory(state, badStatusMemory,
        KVMFR_CLIPBOARD_QUEUE_STATUS, nextSerial(state)));
  CHECK(waitMemory(state, badStatusMemory));
  CHECK(atomic_load(&state->statusCount) == statusCount);

  KVMFRClipboardMessage malformed =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, claimRecord.generation + 1);
  malformed.clipboardGeneration = 900;
  malformed.token = KVMFR_CLIPBOARD_FORMAT_MASK_TEXT;
  CHECK(postRecord(state, &malformed,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.notice) == 0);

  malformed.generation = claimRecord.generation;
  CHECK(postRecord(state, &malformed, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.notice) == 0);

  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));
  CHECK(postGrant(state, claimRecord.generation, 1));
  CHECK(waitMemory(state, state->grantMemory[0]));

  KVMFRClipboardMessage requestRecord;
  CHECK(offerAndRequest(state, claimRecord.generation,
        901, 9001, &requestRecord));
  KVMFRClipboardMessage data =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, claimRecord.generation);
  data.clipboardGeneration = 901;
  data.transfer = requestRecord.transfer;
  data.offset   = 1;
  data.size     = 1;
  data.format   = KVMFR_CLIPBOARD_FORMAT_TEXT;
  data.flags    = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.dataBegin) == 0);

  const uint8_t byte = 42;
  data.offset = 0;
  data.length = 1;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, &byte));
  CHECK(waitAtomic(state, &state->events.dataEnd, 1));
  CHECK(atomic_load(&state->events.dataBegin) == 1);
  CHECK(atomic_load(&state->events.dataChunk) == 1);
  CHECK(state->events.streamID == 9001);
  CHECK(state->events.chunkData[0] == byte);
  return true;
}

static bool testFileFormatIsolation(TestState * state)
{
  KVMFRClipboardMessage claimRecord;
  CHECK(claim(state, 91, false, &claimRecord));
  CHECK(own(state, 91, claimRecord.generation));

  const LG_ClipboardData files[] = { LG_CLIPBOARD_DATA_FILES };
  CHECK(!state->ops->notifyTypes(state->clipboard, files, 1));
  CHECK(noClientData(state));

  const uint64_t localDataset = UINT64_C(0x123456789abcdef);
  CHECK(state->ops->offerFiles(state->clipboard, localDataset));
  KVMFRClipboardMessage localOffer;
  CHECK(readType(state, KVMFR_CLIPBOARD_MESSAGE_OFFER, &localOffer));
  CHECK(localOffer.clipboardGeneration == localDataset);

  KVMFRClipboardMessage request =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_REQUEST, claimRecord.generation);
  request.clipboardGeneration = localDataset;
  request.transfer            = KVMFR_CLIPBOARD_TRANSFER_HELPER | UINT64_C(1);
  request.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  CHECK(postRecord(state, &request, KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.request) == 0);
  CHECK(state->ops->dataBegin(state->clipboard, request.transfer,
        LG_CLIPBOARD_DATA_FILES, 0) == LG_CLIPBOARD_RESULT_FAILED);

  KVMFRClipboardMessage remoteOffer =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_OFFER, claimRecord.generation);
  remoteOffer.clipboardGeneration = UINT64_C(0x2222333344445555);
  remoteOffer.token = KVMFR_CLIPBOARD_FORMAT_MASK_FILES;
  CHECK(postRecord(state, &remoteOffer,
        KVMFR_CLIPBOARD_QUEUE_MESSAGE, NULL));
  CHECK(waitAtomic(state, &state->events.notice, 1));
  CHECK(state->events.noticeCount == 1);
  CHECK(state->events.noticeTypes[0] == LG_CLIPBOARD_DATA_FILES);
  CHECK(!state->ops->request(state->clipboard,
        100, LG_CLIPBOARD_DATA_FILES));
  CHECK(noClientData(state));

  KVMFRClipboardMessage data =
    hostRecord(KVMFR_CLIPBOARD_MESSAGE_DATA, claimRecord.generation);
  data.clipboardGeneration = remoteOffer.clipboardGeneration;
  data.transfer            = UINT64_C(0x2345);
  data.format              = KVMFR_CLIPBOARD_FORMAT_FILES;
  data.flags               = KVMFR_CLIPBOARD_FLAG_BEGIN |
    KVMFR_CLIPBOARD_FLAG_END;
  CHECK(postRecord(state, &data, KVMFR_CLIPBOARD_QUEUE_DATA, NULL));
  CHECK(waitMemory(state, state->memories[state->memoryCount - 1]));
  CHECK(atomic_load(&state->events.dataBegin) == 0);
  return true;
}

static bool stateInit(TestState * state)
{
  memset(state, 0, sizeof(*state));
  state->memory = MAP_FAILED;
  atomic_init(&state->statusCount, 0);
  atomic_init(&state->statusAvailable, false);
  atomic_init(&state->statusGeneration, 0);
  atomic_init(&state->autoAttach, false);
  atomic_init(&state->events.notice, 0);
  atomic_init(&state->events.release, 0);
  atomic_init(&state->events.request, 0);
  atomic_init(&state->events.requestCancel, 0);
  atomic_init(&state->events.dataBegin, 0);
  atomic_init(&state->events.dataChunk, 0);
  atomic_init(&state->events.dataEnd, 0);
  atomic_init(&state->events.dataCancel, 0);
  atomic_init(&state->events.dataReady, 0);
  atomic_init(&state->events.fileOffer, 0);
  atomic_init(&state->events.fileAcquire, 0);
  atomic_init(&state->events.fileAcquired, 0);
  atomic_init(&state->events.fileRequest, 0);
  atomic_init(&state->events.fileCancel, 0);
  atomic_init(&state->events.acceptRequest, true);
  atomic_init(&state->events.blockChunkOnce, false);
  atomic_init(&state->events.attached, false);
  atomic_init(&state->events.fileOfferBeforeNotice, false);

  state->memory = mmap(NULL, TEST_SHM_SIZE, PROT_READ | PROT_WRITE,
    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  CHECK(state->memory != MAP_FAILED);
  const uint32_t sessionData = UINT32_C(0x12345678);
  CHECK(lgmpHostInit(state->memory, TEST_SHM_SIZE, &state->host,
        sizeof(sessionData), (uint8_t *)&sessionData) == LGMP_OK);
  const struct LGMPQueueConfig config =
  {
    .queueID     = LGMP_Q_CLIPBOARD,
    .numMessages = LGMP_Q_CLIPBOARD_LEN,
    .subTimeout  = 1000,
  };
  CHECK(lgmpHostQueueNew(state->host, config, &state->queue) == LGMP_OK);
  CHECK(lgmpClientInit(state->memory, TEST_SHM_SIZE, &state->client) ==
      LGMP_OK);
  usleep(300000);
  CHECK(hostProcess(state));
  uint32_t dataSize;
  uint32_t remoteVersion;
  uint8_t * data;
  CHECK(lgmpClientSessionInit(state->client, &dataSize, &data,
        &state->clientID, &remoteVersion) == LGMP_OK);
  CHECK(state->clientID != 0);
  CHECK(dataSize == sizeof(sessionData));
  CHECK(memcmp(data, &sessionData, sizeof(sessionData)) == 0);

  CHECK(lgmpClipboard_create(state->client, &state->clipboard));
  CHECK(lgmpClipboard_connect(state->clipboard, state->clientID));
  state->ops = lgmpClipboard_getOps();
  CHECK(state->ops);
  state->ops->setStatusListener(
    state->clipboard, statusChanged, state);
  CHECK(atomic_load(&state->statusCount) == 1);
  CHECK(!atomic_load(&state->statusAvailable));

  for (unsigned i = 0; i < TEST_WAIT_MS; ++i)
  {
    CHECK(hostProcess(state));
    if (lgmpHostQueueHasSubs(state->queue))
      return true;
    usleep(1000);
  }
  return false;
}

static void stateFree(TestState * state)
{
  if (state->clipboard)
  {
    state->ops->setStatusListener(state->clipboard, NULL, NULL);
    if (atomic_load(&state->events.attached))
      state->ops->detach(state->clipboard);
    disconnectAndDrain(state, NULL);
    lgmpClipboard_destroy(&state->clipboard);
  }
  lgmpClientFree(&state->client);
  for (unsigned i = 0; i < state->memoryCount; ++i)
    lgmpHostMemFree(&state->memories[i]);
  lgmpHostFree(&state->host);
  if (state->memory != MAP_FAILED)
    munmap(state->memory, TEST_SHM_SIZE);
}

typedef bool (*TestFn)(TestState * state);

static const struct
{
  const char * name;
  TestFn run;
}
TESTS[] =
{
  { "claim"           , testClaim               },
  { "offer-request"   , testOfferRequest        },
  { "inbound-stream"  , testInboundStream       },
  { "outbound-grant"  , testOutboundGrant       },
  { "blocked"         , testBlocked             },
  { "cancel"          , testCancel              },
  { "restart"         , testRestart             },
  { "process-restart" , testProcessRestart      },
  { "file-stream"     , testFileStream          },
  { "file-limits"     , testFileLimits          },
  { "file-owner-loss" , testFileOwnerLoss       },
  { "file-malformed"  , testMalformedFileData   },
  { "disconnect"      , testDisconnect          },
  { "malformed"       , testMalformed           },
  { "file-format"     , testFileFormatIsolation },
};

int main(int argc, char * argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: %s <test-case>\n", argv[0]);
    return 2;
  }
  TestFn test = NULL;
  for (unsigned i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); ++i)
    if (strcmp(argv[1], TESTS[i].name) == 0)
    {
      test = TESTS[i].run;
      break;
    }
  if (!test)
  {
    fprintf(stderr, "unknown test case: %s\n", argv[1]);
    return 2;
  }

  debug_init();
  TestState state = { 0 };
  state.memory = MAP_FAILED;
  const bool initialized = stateInit(&state);
  const bool passed = initialized && test(&state);
  stateFree(&state);
  return passed ? 0 : 1;
}
