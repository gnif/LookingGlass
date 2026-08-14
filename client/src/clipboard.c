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
#include "main.h"

#include "common/debug.h"
#include "common/ll.h"
#include "common/locking.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define COMPAT_CHUNK_SIZE (64U * 1024U)

typedef struct ClipboardBinding
{
  const LG_ClipboardOps * ops;
  void                  * opaque;
  bool                    available;
  uint32_t                generation;
}
ClipboardBinding;

typedef struct ClipboardRequest
{
  LG_ClipboardRequest request;
  ClipboardBinding    binding;
  uint32_t            remoteGeneration;
  LG_ClipboardData    type;
  LG_ClipboardReplyFn replyFn;
  const LG_ClipboardStreamOps * stream;
  void                        * opaque;

  bool     started;
  bool     blocked;
  uint64_t sizeHint;
  uint64_t offset;

  uint8_t * buffer;
  size_t    bufferSize;
  size_t    bufferCapacity;

  bool             compat;
  unsigned int     compatPhase;
  LG_ClipboardData compatType;
  uint8_t        * compatData;
  size_t           compatSize;
}
ClipboardRequest;

enum
{
  COMPAT_BEGIN,
  COMPAT_CHUNK,
  COMPAT_END,
};

static struct
{
  LG_Lock   registrationLock;
  LG_Lock   providerLock;
  LG_RWLock activeLock;
  LG_Lock   stateLock;
  LG_Lock   requestLock;
  LG_Lock   writeLock;
  LG_Lock   callbackLock;

  ClipboardBinding fallback;
  ClipboardBinding transport;
  ClipboardBinding active;

  bool             localAvailable;
  LG_ClipboardData localTypes[LG_CLIPBOARD_DATA_NONE];
  size_t           localTypeCount;

  bool             remoteNotice;
  LG_ClipboardData remoteType;
  uint32_t         remoteGeneration;

  bool                remoteRequest;
  ClipboardBinding    remoteRequestBinding;
  LG_ClipboardRequest remoteRequestId;
  LG_ClipboardRequest remoteTransferId;
  LG_ClipboardData    remoteRequestType;
  bool                remoteStarted;
  bool                remoteBlocked;
  uint64_t            remoteSizeHint;
  uint64_t            remoteOffset;

  uint8_t * remoteBuffer;
  size_t    remoteBufferSize;
  size_t    remoteBufferCapacity;

  bool         remoteCompat;
  unsigned int remoteCompatPhase;

  LG_ClipboardRequest requestSerial;
  LG_ClipboardRequest transferSerial;
  struct ll         * requests;
}
clipboard;

static LG_ClipboardResult pumpRemoteCompat(void);

static bool validType(LG_ClipboardData type)
{
  return type >= LG_CLIPBOARD_DATA_TEXT &&
    type < LG_CLIPBOARD_DATA_NONE;
}

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static LG_ClipboardRequest nextRequestNL(void)
{
  if (++clipboard.requestSerial == LG_CLIPBOARD_REQUEST_INVALID)
    ++clipboard.requestSerial;
  return clipboard.requestSerial;
}

static LG_ClipboardRequest nextTransferNL(void)
{
  if (++clipboard.transferSerial == LG_CLIPBOARD_REQUEST_INVALID)
    ++clipboard.transferSerial;
  return clipboard.transferSerial;
}

static bool bindingEqual(const ClipboardBinding * a,
    const ClipboardBinding * b)
{
  return a->ops        == b->ops &&
    a->opaque          == b->opaque &&
    a->generation      == b->generation;
}

static bool bindingActiveNL(const ClipboardBinding * binding)
{
  return binding->ops && bindingEqual(binding, &clipboard.active);
}

static bool localHasTypeNL(LG_ClipboardData type)
{
  for (size_t i = 0; i < clipboard.localTypeCount; ++i)
    if (clipboard.localTypes[i] == type)
      return true;

  return false;
}

static bool validStream(const LG_ClipboardStreamOps * stream)
{
  return stream && stream->begin && stream->chunk &&
    stream->end && stream->cancel;
}

static bool streamProvider(const LG_ClipboardOps * ops)
{
  return ops && ops->dataBegin && ops->dataChunk && ops->dataEnd &&
    ops->dataCancel && ops->dataReady;
}

static bool growBuffer(uint8_t ** buffer, size_t * capacity, size_t size)
{
  if (size <= *capacity)
    return true;

  size_t next = *capacity ? *capacity : 4096;
  while (next < size)
  {
    if (next > SIZE_MAX / 2)
    {
      next = size;
      break;
    }
    next *= 2;
  }

  uint8_t * resized = realloc(*buffer, next);
  if (!resized)
    return false;

  *buffer   = resized;
  *capacity = next;
  return true;
}

static LG_ClipboardResult legacyBegin(void * opaque,
    LG_ClipboardData type, uint64_t sizeHint)
{
  ClipboardRequest * request = opaque;
  if (type != request->type ||
      (sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN && sizeHint > UINT32_MAX))
    return LG_CLIPBOARD_RESULT_FAILED;

  free(request->buffer);
  request->buffer         = NULL;
  request->bufferSize     = 0;
  request->bufferCapacity = 0;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult legacyChunk(void * opaque, uint64_t offset,
    const void * data, size_t size)
{
  ClipboardRequest * request = opaque;
  if (offset != request->bufferSize || !size || !data ||
      offset > UINT32_MAX || size > UINT32_MAX - offset)
    return LG_CLIPBOARD_RESULT_FAILED;

  const size_t required = request->bufferSize + size;
  if (!growBuffer(&request->buffer, &request->bufferCapacity, required))
    return LG_CLIPBOARD_RESULT_FAILED;

  memcpy(request->buffer + request->bufferSize, data, size);
  request->bufferSize = required;
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult legacyEnd(void * opaque, uint64_t finalSize)
{
  ClipboardRequest * request = opaque;
  if (finalSize != request->bufferSize || finalSize > UINT32_MAX)
    return LG_CLIPBOARD_RESULT_FAILED;

  request->replyFn(request->opaque, request->type,
      request->bufferSize ? request->buffer : NULL,
      (uint32_t)request->bufferSize);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void legacyCancel(void * opaque, LG_ClipboardCancelReason reason)
{
  (void)reason;
  ClipboardRequest * request = opaque;
  request->replyFn(
      request->opaque, LG_CLIPBOARD_DATA_NONE, NULL, 0);
}

static const LG_ClipboardStreamOps legacyStream =
{
  .begin  = legacyBegin,
  .chunk  = legacyChunk,
  .end    = legacyEnd,
  .cancel = legacyCancel,
};

static LG_ClipboardRequest clearRemoteRequestNL(void)
{
  const LG_ClipboardRequest transfer = clipboard.remoteRequest ?
    clipboard.remoteTransferId : LG_CLIPBOARD_REQUEST_INVALID;
  free(clipboard.remoteBuffer);
  clipboard.remoteRequest        = false;
  clipboard.remoteRequestBinding = (ClipboardBinding) { 0 };
  clipboard.remoteRequestId      = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard.remoteTransferId     = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard.remoteRequestType    = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteStarted        = false;
  clipboard.remoteBlocked        = false;
  clipboard.remoteSizeHint       = LG_CLIPBOARD_SIZE_UNKNOWN;
  clipboard.remoteOffset         = 0;
  clipboard.remoteBuffer         = NULL;
  clipboard.remoteBufferSize     = 0;
  clipboard.remoteBufferCapacity = 0;
  clipboard.remoteCompat         = false;
  clipboard.remoteCompatPhase    = COMPAT_BEGIN;
  return transfer;
}

static void freeRequest(ClipboardRequest * request)
{
  if (!request)
    return;

  free(request->compatData);
  free(request->buffer);
  free(request);
}

static void * requestStreamOpaque(ClipboardRequest * request)
{
  return request->replyFn ? request : request->opaque;
}

static void cancelRequest(ClipboardRequest * request,
    LG_ClipboardCancelReason reason)
{
  request->stream->cancel(requestStreamOpaque(request), reason);
}

static ClipboardRequest * takeCancelableRequest(
    const ClipboardBinding * binding, uint32_t generation, bool all)
{
  if (!clipboard.requests)
    return NULL;

  ClipboardRequest * request;
  ll_lock(clipboard.requests);
  ll_forEachNL(clipboard.requests, item, request)
  {
    if (!all && bindingEqual(&request->binding, binding) &&
        request->remoteGeneration == generation)
      continue;

    ll_removeNL(clipboard.requests, item);
    free(item);
    ll_unlock(clipboard.requests);
    return request;
  }
  ll_unlock(clipboard.requests);
  return NULL;
}

static ClipboardRequest * takeRequest(LG_ClipboardRequest id)
{
  if (!clipboard.requests)
    return NULL;

  ClipboardRequest * request;
  ll_lock(clipboard.requests);
  ll_forEachNL(clipboard.requests, item, request)
  {
    if (request->request != id)
      continue;

    ll_removeNL(clipboard.requests, item);
    free(item);
    ll_unlock(clipboard.requests);
    return request;
  }
  ll_unlock(clipboard.requests);
  return NULL;
}

/* callbackLock must be held while completing canceled requests. */
static void cancelRequestsNL(const ClipboardBinding * binding,
    uint32_t generation, bool all, LG_ClipboardCancelReason reason)
{
  ClipboardRequest * request;
  while ((request = takeCancelableRequest(binding, generation, all)))
  {
    cancelRequest(request, reason);
    freeRequest(request);
  }
}

static void resetRemote(void)
{
  bool release;
  LG_ClipboardRequest transfer;

  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  release = clipboard.remoteNotice && clipboard.localAvailable &&
    g_params.clipboardToLocal;
  clipboard.remoteNotice     = false;
  clipboard.remoteType       = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  transfer = clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(NULL, 0, true, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
  if (transfer != LG_CLIPBOARD_REQUEST_INVALID &&
      g_state.ds->cbRequestCancel)
    g_state.ds->cbRequestCancel(
        transfer, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
  LG_LOCK(clipboard.stateLock);
  release = release && clipboard.localAvailable &&
    !clipboard.remoteNotice;
  LG_UNLOCK(clipboard.stateLock);
  if (release)
    g_state.ds->cbRelease();
  LG_UNLOCK(clipboard.callbackLock);
}

static bool validOps(const LG_ClipboardOps * ops)
{
  return ops && ops->name && ops->attach && ops->detach &&
    ops->release && ops->notifyTypes && ops->request &&
    (ops->data || streamProvider(ops));
}

static ClipboardBinding makeBinding(
    const LG_ClipboardOps * ops, void * opaque)
{
  return (ClipboardBinding)
  {
    .ops        = ops,
    .opaque     = opaque,
    .available  = ops && !ops->setStatusListener,
    .generation = 0,
  };
}

static ClipboardBinding * nextBindingSlotNL(void)
{
  if (clipboard.transport.available)
    return &clipboard.transport;
  if (clipboard.fallback.available)
    return &clipboard.fallback;
  return NULL;
}

static void publishLocal(const ClipboardBinding * binding)
{
  LG_LOCK(clipboard.writeLock);
  LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];
  size_t count;

  LG_LOCK(clipboard.stateLock);
  count = g_params.clipboardToVM ? clipboard.localTypeCount : 0;
  memcpy(types, clipboard.localTypes, count * sizeof(*types));
  LG_UNLOCK(clipboard.stateLock);

  const bool result = count ?
    binding->ops->notifyTypes(binding->opaque, types, count) :
    binding->ops->release(binding->opaque);
  if (!result)
    DEBUG_WARN("Failed to publish the local clipboard to %s",
        binding->ops->name);
  LG_UNLOCK(clipboard.writeLock);
}

static void eventNotice(void * opaque,
    const LG_ClipboardData types[], size_t count)
{
  ClipboardBinding * binding = opaque;
  if (!types || count == 0)
    return;

  LG_ClipboardData type = LG_CLIPBOARD_DATA_NONE;
  for (size_t i = 0; i < count; ++i)
    if (validType(types[i]))
    {
      type = types[i];
      break;
    }
  if (type == LG_CLIPBOARD_DATA_NONE)
    return;

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!bindingActiveNL(binding))
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    return;
  }

  const ClipboardBinding current = *binding;
  LG_UNLOCK_SHARED(clipboard.activeLock);
  bool notice;
  uint32_t generation;
  LG_ClipboardRequest transfer;

  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  clipboard.remoteNotice     = true;
  clipboard.remoteType       = type;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  generation = clipboard.remoteGeneration;
  transfer = clearRemoteRequestNL();
  notice = clipboard.localAvailable && g_params.clipboardToLocal;
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(&current, generation, false,
      LG_CLIPBOARD_CANCEL_REPLACED);
  if (transfer != LG_CLIPBOARD_REQUEST_INVALID &&
      g_state.ds->cbRequestCancel)
    g_state.ds->cbRequestCancel(transfer, LG_CLIPBOARD_CANCEL_REPLACED);
  LG_LOCK(clipboard.stateLock);
  notice = notice && clipboard.localAvailable &&
    clipboard.remoteNotice &&
    clipboard.remoteGeneration == generation;
  LG_UNLOCK(clipboard.stateLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  const bool active = bindingActiveNL(&current);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  if (notice && active)
    g_state.ds->cbNotice(type);
  LG_UNLOCK(clipboard.callbackLock);
}

static bool returnRequest(ClipboardRequest * request)
{
  if (ll_push(clipboard.requests, request))
    return true;

  cancelRequest(request, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
  freeRequest(request);
  return false;
}

/* callbackLock must be held. The returned request has been removed from the
 * request list and must either be returned to it or freed. */
static ClipboardRequest * takeIncoming(ClipboardBinding * binding,
    LG_ClipboardRequest id, ClipboardBinding * current)
{
  if (id == LG_CLIPBOARD_REQUEST_INVALID)
    return NULL;

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!bindingActiveNL(binding))
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    return NULL;
  }
  *current = *binding;
  LG_UNLOCK_SHARED(clipboard.activeLock);

  ClipboardRequest * request = takeRequest(id);
  if (!request)
    return NULL;

  LG_LOCK(clipboard.stateLock);
  const bool deliver = clipboard.localAvailable &&
    g_params.clipboardToLocal && clipboard.remoteNotice &&
    request->remoteGeneration == clipboard.remoteGeneration;
  LG_UNLOCK(clipboard.stateLock);

  LG_LOCK_SHARED(clipboard.activeLock);
  const bool active = bindingActiveNL(current);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  if (deliver && active && bindingEqual(&request->binding, current))
    return request;

  cancelRequest(request, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
  freeRequest(request);
  return NULL;
}

static LG_ClipboardResult incomingBegin(ClipboardRequest * request,
    LG_ClipboardData type, uint64_t sizeHint, bool * terminal)
{
  if (request->started || request->blocked || type != request->type ||
      !validType(type))
    goto invalid;

  const LG_ClipboardResult result = request->stream->begin(
      requestStreamOpaque(request), type, sizeHint);
  switch (result)
  {
    case LG_CLIPBOARD_RESULT_ACCEPTED:
      request->started  = true;
      request->sizeHint = sizeHint;
      request->offset   = 0;
      return result;

    case LG_CLIPBOARD_RESULT_BLOCKED:
      request->blocked = true;
      return result;

    case LG_CLIPBOARD_RESULT_FAILED:
      break;
  }

invalid:
  cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
  *terminal = true;
  return LG_CLIPBOARD_RESULT_FAILED;
}

static LG_ClipboardResult incomingChunk(ClipboardRequest * request,
    uint64_t offset, const void * data, size_t size, bool * terminal)
{
  if (!request->started || request->blocked || offset != request->offset ||
      !data || !size || size > UINT64_MAX - offset)
    goto invalid;

  const LG_ClipboardResult result = request->stream->chunk(
      requestStreamOpaque(request), offset, data, size);
  switch (result)
  {
    case LG_CLIPBOARD_RESULT_ACCEPTED:
      request->offset += size;
      return result;

    case LG_CLIPBOARD_RESULT_BLOCKED:
      request->blocked = true;
      return result;

    case LG_CLIPBOARD_RESULT_FAILED:
      break;
  }

invalid:
  cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
  *terminal = true;
  return LG_CLIPBOARD_RESULT_FAILED;
}

static LG_ClipboardResult incomingEnd(ClipboardRequest * request,
    uint64_t finalSize, bool * terminal)
{
  if (!request->started || request->blocked || finalSize != request->offset)
    goto invalid;

  const LG_ClipboardResult result = request->stream->end(
      requestStreamOpaque(request), finalSize);
  switch (result)
  {
    case LG_CLIPBOARD_RESULT_ACCEPTED:
      *terminal = true;
      return result;

    case LG_CLIPBOARD_RESULT_BLOCKED:
      request->blocked = true;
      return result;

    case LG_CLIPBOARD_RESULT_FAILED:
      break;
  }

invalid:
  cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
  *terminal = true;
  return LG_CLIPBOARD_RESULT_FAILED;
}

static LG_ClipboardResult pumpIncomingCompat(ClipboardRequest * request,
    bool * terminal)
{
  for (;;)
  {
    LG_ClipboardResult result;
    switch (request->compatPhase)
    {
      case COMPAT_BEGIN:
        result = incomingBegin(request, request->compatType,
            request->compatSize, terminal);
        if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
          return result;
        request->compatPhase = COMPAT_CHUNK;
        break;

      case COMPAT_CHUNK:
        if (request->offset < request->compatSize)
        {
          const size_t remaining = request->compatSize - request->offset;
          const size_t size = remaining < COMPAT_CHUNK_SIZE ?
            remaining : COMPAT_CHUNK_SIZE;
          result = incomingChunk(request, request->offset,
              request->compatData + request->offset, size, terminal);
          if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
            return result;
          break;
        }
        request->compatPhase = COMPAT_END;
        break;

      case COMPAT_END:
        return incomingEnd(request, request->compatSize, terminal);

      default:
        cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
        *terminal = true;
        return LG_CLIPBOARD_RESULT_FAILED;
    }
  }
}

static LG_ClipboardResult eventDataBegin(void * opaque,
    LG_ClipboardRequest id, LG_ClipboardData type, uint64_t sizeHint)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  ClipboardBinding current;
  ClipboardRequest * request = takeIncoming(binding, id, &current);
  if (!request)
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  bool terminal = false;
  LG_ClipboardResult result;
  if (request->compat)
  {
    cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
    terminal = true;
    result = LG_CLIPBOARD_RESULT_FAILED;
  }
  else
    result = incomingBegin(request, type, sizeHint, &terminal);
  if (!terminal && !returnRequest(request))
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (terminal)
    freeRequest(request);
  LG_UNLOCK(clipboard.callbackLock);
  return result;
}

static LG_ClipboardResult eventDataChunk(void * opaque,
    LG_ClipboardRequest id, uint64_t offset, const void * data, size_t size)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  ClipboardBinding current;
  ClipboardRequest * request = takeIncoming(binding, id, &current);
  if (!request)
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  bool terminal = false;
  LG_ClipboardResult result;
  if (request->compat)
  {
    cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
    terminal = true;
    result = LG_CLIPBOARD_RESULT_FAILED;
  }
  else
    result = incomingChunk(request, offset, data, size, &terminal);
  if (!terminal && !returnRequest(request))
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (terminal)
    freeRequest(request);
  LG_UNLOCK(clipboard.callbackLock);
  return result;
}

static LG_ClipboardResult eventDataEnd(void * opaque,
    LG_ClipboardRequest id, uint64_t finalSize)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  ClipboardBinding current;
  ClipboardRequest * request = takeIncoming(binding, id, &current);
  if (!request)
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  bool terminal = false;
  LG_ClipboardResult result;
  if (request->compat)
  {
    cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
    terminal = true;
    result = LG_CLIPBOARD_RESULT_FAILED;
  }
  else
    result = incomingEnd(request, finalSize, &terminal);
  if (!terminal && !returnRequest(request))
  {
    LG_UNLOCK(clipboard.callbackLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (terminal)
    freeRequest(request);
  LG_UNLOCK(clipboard.callbackLock);
  return result;
}

static void eventDataCancel(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  ClipboardBinding current;
  ClipboardRequest * request = takeIncoming(binding, id, &current);
  if (request)
  {
    cancelRequest(request, reason);
    freeRequest(request);
  }
  LG_UNLOCK(clipboard.callbackLock);
}

static void eventData(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type, const void * data, size_t size)
{
  ClipboardBinding * binding = opaque;
  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    eventDataCancel(opaque, id, LG_CLIPBOARD_CANCEL_ABORTED);
    return;
  }

  LG_LOCK(clipboard.callbackLock);
  ClipboardBinding current;
  ClipboardRequest * request = takeIncoming(binding, id, &current);
  if (!request)
  {
    LG_UNLOCK(clipboard.callbackLock);
    DEBUG_WARN("Ignoring stale clipboard data");
    return;
  }

  bool terminal = false;
  LG_ClipboardResult result = LG_CLIPBOARD_RESULT_FAILED;
  if (type != request->type || !validType(type) || (size && !data) ||
      request->started || request->compat)
  {
    cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
    terminal = true;
  }
  else if (request->replyFn && size <= UINT32_MAX)
  {
    request->replyFn(request->opaque, type, data, (uint32_t)size);
    terminal = true;
    result = LG_CLIPBOARD_RESULT_ACCEPTED;
  }
  else if (!request->replyFn)
  {
    if (size)
    {
      request->compatData = malloc(size);
      if (request->compatData)
        memcpy(request->compatData, data, size);
    }
    if (!size || request->compatData)
    {
      request->compat      = true;
      request->compatPhase = COMPAT_BEGIN;
      request->compatType  = type;
      request->compatSize  = size;
      result = pumpIncomingCompat(request, &terminal);
    }
    else
    {
      cancelRequest(request, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
      terminal = true;
    }
  }
  else
  {
    cancelRequest(request, LG_CLIPBOARD_CANCEL_INVALID);
    terminal = true;
  }

  if (!terminal && !returnRequest(request))
    result = LG_CLIPBOARD_RESULT_FAILED;
  if (terminal)
    freeRequest(request);
  LG_UNLOCK(clipboard.callbackLock);

  if (result == LG_CLIPBOARD_RESULT_FAILED)
    DEBUG_ERROR("Invalid clipboard response from %s", current.ops->name);
}

static void eventRelease(void * opaque)
{
  ClipboardBinding * binding = opaque;

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!bindingActiveNL(binding))
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    return;
  }
  LG_UNLOCK_SHARED(clipboard.activeLock);

  bool release;
  LG_ClipboardRequest transfer;
  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  release = clipboard.remoteNotice && clipboard.localAvailable &&
    g_params.clipboardToLocal;
  clipboard.remoteNotice     = false;
  clipboard.remoteType       = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  transfer = clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(NULL, 0, true, LG_CLIPBOARD_CANCEL_REPLACED);
  if (transfer != LG_CLIPBOARD_REQUEST_INVALID &&
      g_state.ds->cbRequestCancel)
    g_state.ds->cbRequestCancel(transfer, LG_CLIPBOARD_CANCEL_REPLACED);
  LG_LOCK(clipboard.stateLock);
  release = release && clipboard.localAvailable &&
    !clipboard.remoteNotice;
  LG_UNLOCK(clipboard.stateLock);
  if (release)
    g_state.ds->cbRelease();
  LG_UNLOCK(clipboard.callbackLock);
}

static bool eventRequest(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type)
{
  ClipboardBinding * binding = opaque;
  if (id == LG_CLIPBOARD_REQUEST_INVALID || !validType(type))
    return false;

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!bindingActiveNL(binding))
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    return false;
  }

  const ClipboardBinding current = *binding;
  const char * name = current.ops->name;
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_ClipboardRequest transfer = LG_CLIPBOARD_REQUEST_INVALID;
  LG_LOCK(clipboard.stateLock);
  const bool request = clipboard.localAvailable &&
    g_params.clipboardToVM && localHasTypeNL(type) &&
    !clipboard.remoteRequest;
  if (request)
  {
    clipboard.remoteRequest        = true;
    clipboard.remoteRequestBinding = current;
    clipboard.remoteRequestId      = id;
    clipboard.remoteTransferId     = nextTransferNL();
    clipboard.remoteRequestType    = type;
    transfer = clipboard.remoteTransferId;
  }
  LG_UNLOCK(clipboard.stateLock);

  LG_LOCK(clipboard.callbackLock);
  LG_LOCK(clipboard.stateLock);
  const bool deliver = request && clipboard.localAvailable &&
    clipboard.remoteRequest &&
    clipboard.remoteRequestId == id &&
    clipboard.remoteTransferId == transfer;
  LG_UNLOCK(clipboard.stateLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  const bool active = bindingActiveNL(&current);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  if (deliver && active)
    g_state.ds->cbRequest(transfer, type);
  else
  {
    LG_LOCK(clipboard.stateLock);
    if (clipboard.remoteRequest &&
        clipboard.remoteRequestId == id &&
        clipboard.remoteTransferId == transfer)
      clearRemoteRequestNL();
    LG_UNLOCK(clipboard.stateLock);
    DEBUG_WARN("Ignoring invalid clipboard request from %s",
        name);
  }
  LG_UNLOCK(clipboard.callbackLock);
  return deliver && active;
}

static void eventDataReady(void * opaque, LG_ClipboardRequest id)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool valid = bindingActiveNL(binding) && clipboard.remoteRequest &&
    clipboard.remoteRequestId == id && clipboard.remoteBlocked &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  const bool compat = valid && clipboard.remoteCompat;
  const LG_ClipboardRequest transfer = valid ?
    clipboard.remoteTransferId : LG_CLIPBOARD_REQUEST_INVALID;
  if (valid)
    clipboard.remoteBlocked = false;
  LG_UNLOCK(clipboard.stateLock);

  if (compat)
    pumpRemoteCompat();
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
  if (!compat && valid && g_state.ds->cbRequestReady)
    g_state.ds->cbRequestReady(transfer);
  LG_UNLOCK(clipboard.callbackLock);
}

static void eventRequestCancel(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardCancelReason reason)
{
  ClipboardBinding * binding = opaque;
  LG_LOCK(clipboard.callbackLock);
  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool valid = bindingActiveNL(binding) && clipboard.remoteRequest &&
    clipboard.remoteRequestId == id &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  const LG_ClipboardRequest transfer = valid ?
    clipboard.remoteTransferId : LG_CLIPBOARD_REQUEST_INVALID;
  if (valid)
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);

  if (valid && g_state.ds->cbRequestCancel)
    g_state.ds->cbRequestCancel(transfer, reason);
  LG_UNLOCK(clipboard.callbackLock);
}

static const LG_ClipboardEventOps eventOps =
{
  .notice        = eventNotice,
  .data          = eventData,
  .dataBegin     = eventDataBegin,
  .dataChunk     = eventDataChunk,
  .dataEnd       = eventDataEnd,
  .dataCancel    = eventDataCancel,
  .dataReady     = eventDataReady,
  .requestCancel = eventRequestCancel,
  .release       = eventRelease,
  .request       = eventRequest,
};

/* providerLock must be held. dropActive suppresses all calls into an endpoint
 * which has already disappeared. */
static void updateActive(bool dropActive)
{
  for (;;)
  {
    LG_LOCK_EXCLUSIVE(clipboard.activeLock);
    ClipboardBinding * slot = nextBindingSlotNL();
    ClipboardBinding next = slot ? *slot : (ClipboardBinding) { 0 };
    const ClipboardBinding old = clipboard.active;
    if (bindingEqual(&old, &next))
    {
      clipboard.active = next;
      LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
      return;
    }
    clipboard.active = (ClipboardBinding) { 0 };
    LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);

    if (old.ops && !dropActive)
    {
      if (!old.ops->release(old.opaque))
        DEBUG_WARN("Failed to release Clipboard provider: %s",
            old.ops->name);
      old.ops->detach(old.opaque);
    }
    dropActive = false;
    resetRemote();

    LG_LOCK_EXCLUSIVE(clipboard.activeLock);
    slot = nextBindingSlotNL();
    if (!slot)
    {
      LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
      DEBUG_INFO("Clipboard is unavailable");
      return;
    }
    next = *slot;
    clipboard.active = next;
    LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);

    if (next.ops->attach(next.opaque, &eventOps, slot))
    {
      publishLocal(&next);
      DEBUG_INFO("Using Clipboard: %s", next.ops->name);
      return;
    }

    next.ops->detach(next.opaque);
    resetRemote();

    LG_LOCK_EXCLUSIVE(clipboard.activeLock);
    if (bindingEqual(&clipboard.active, &next))
      clipboard.active = (ClipboardBinding) { 0 };
    if (bindingEqual(slot, &next))
      slot->available = false;
    LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);

    DEBUG_WARN("Failed to attach Clipboard provider: %s", next.ops->name);
  }
}

static void statusChanged(ClipboardBinding * binding, void * opaque,
    const LG_ClipboardStatus * status)
{
  if (!status)
    return;

  LG_LOCK(clipboard.providerLock);
  LG_LOCK_EXCLUSIVE(clipboard.activeLock);
  const bool current = binding->ops && binding->opaque == opaque;
  const bool wasActive = current && bindingActiveNL(binding);
  const bool changed = current &&
    (binding->available != status->available ||
     binding->generation != status->generation);
  if (changed)
  {
    binding->available  = status->available;
    binding->generation = status->generation;
  }
  LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
  if (changed)
    updateActive(wasActive && !status->available);
  LG_UNLOCK(clipboard.providerLock);
}

static void fallbackStatusChanged(void * opaque,
    const LG_ClipboardStatus * status)
{
  statusChanged(&clipboard.fallback, opaque, status);
}

static void transportStatusChanged(void * opaque,
    const LG_ClipboardStatus * status)
{
  statusChanged(&clipboard.transport, opaque, status);
}

static void setBinding(ClipboardBinding * target,
    const LG_ClipboardOps * ops, void * opaque,
    LG_ClipboardStatusFn statusFn)
{
  if (ops && !validOps(ops))
  {
    DEBUG_ERROR("Invalid clipboard operations");
    ops    = NULL;
    opaque = NULL;
  }

  LG_LOCK(clipboard.registrationLock);
  LG_LOCK(clipboard.providerLock);
  const ClipboardBinding old = *target;
  LG_UNLOCK(clipboard.providerLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);

  const ClipboardBinding next = makeBinding(ops, opaque);
  LG_LOCK(clipboard.providerLock);
  LG_LOCK_EXCLUSIVE(clipboard.activeLock);
  *target = next;
  LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
  updateActive(false);
  LG_UNLOCK(clipboard.providerLock);

  if (next.ops && next.ops->setStatusListener)
    next.ops->setStatusListener(next.opaque, statusFn, next.opaque);
  LG_UNLOCK(clipboard.registrationLock);
}

void lgClipboard_init(void)
{
  memset(&clipboard, 0, sizeof(clipboard));
  LG_LOCK_INIT(clipboard.registrationLock);
  LG_LOCK_INIT(clipboard.providerLock);
  LG_RWLOCK_INIT(clipboard.activeLock);
  LG_LOCK_INIT(clipboard.stateLock);
  LG_LOCK_INIT(clipboard.requestLock);
  LG_LOCK_INIT(clipboard.writeLock);
  LG_LOCK_INIT(clipboard.callbackLock);
  clipboard.remoteType        = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteRequestType = LG_CLIPBOARD_DATA_NONE;
  clipboard.requests          = ll_new();
}

void lgClipboard_free(void)
{
  lgClipboard_setLocalAvailable(false);
  lgClipboard_setTransport(NULL, NULL);
  lgClipboard_setFallback(NULL, NULL);
  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(NULL, 0, true, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
  LG_UNLOCK(clipboard.callbackLock);
  if (clipboard.requests)
  {
    ll_free(clipboard.requests);
    clipboard.requests = NULL;
  }

  LG_LOCK_FREE(clipboard.writeLock);
  LG_LOCK_FREE(clipboard.requestLock);
  LG_LOCK_FREE(clipboard.stateLock);
  LG_LOCK_FREE(clipboard.callbackLock);
  LG_RWLOCK_FREE(clipboard.activeLock);
  LG_LOCK_FREE(clipboard.providerLock);
  LG_LOCK_FREE(clipboard.registrationLock);
}

void lgClipboard_setLocalAvailable(bool available)
{
  bool notice;
  bool release;
  LG_ClipboardData type;
  LG_ClipboardRequest transfer = LG_CLIPBOARD_REQUEST_INVALID;

  LG_LOCK(clipboard.callbackLock);
  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  release = !available && clipboard.localAvailable &&
    clipboard.remoteNotice && g_params.clipboardToLocal;
  clipboard.localAvailable = available;
  notice = available && clipboard.remoteNotice &&
    g_params.clipboardToLocal;
  type = clipboard.remoteType;
  if (!available)
    transfer = clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  if (notice)
    g_state.ds->cbNotice(type);
  if (!available)
  {
    cancelRequestsNL(NULL, 0, true, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
    if (transfer != LG_CLIPBOARD_REQUEST_INVALID &&
        g_state.ds->cbRequestCancel)
      g_state.ds->cbRequestCancel(
          transfer, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
    if (release)
      g_state.ds->cbRelease();
  }
  LG_UNLOCK(clipboard.callbackLock);
}

void lgClipboard_setFallback(const LG_ClipboardOps * ops, void * opaque)
{
  setBinding(&clipboard.fallback,
      ops, opaque, fallbackStatusChanged);
}

static void dropBinding(ClipboardBinding * target)
{
  LG_LOCK(clipboard.registrationLock);
  LG_LOCK(clipboard.providerLock);
  LG_LOCK_EXCLUSIVE(clipboard.activeLock);
  const bool wasActive = bindingActiveNL(target);
  *target = (ClipboardBinding) { 0 };
  LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
  updateActive(wasActive);
  LG_UNLOCK(clipboard.providerLock);
  LG_UNLOCK(clipboard.registrationLock);
}

void lgClipboard_dropFallback(void)
{
  dropBinding(&clipboard.fallback);
}

void lgClipboard_setTransport(const LG_ClipboardOps * ops, void * opaque)
{
  setBinding(&clipboard.transport,
      ops, opaque, transportStatusChanged);
}

void lgClipboard_dropTransport(void)
{
  dropBinding(&clipboard.transport);
}

void lgClipboard_release(void)
{
  if (!g_params.clipboardToVM)
    return;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK(clipboard.stateLock);
  clipboard.localTypeCount = 0;
  clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);

  LG_LOCK_SHARED(clipboard.activeLock);
  if (clipboard.active.ops &&
      !clipboard.active.ops->release(clipboard.active.opaque))
    DEBUG_WARN("Failed to release the remote clipboard");
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
}

void lgClipboard_notifyTypes(
    const LG_ClipboardData types[], size_t count)
{
  if (!g_params.clipboardToVM)
    return;
  if (count == 0)
  {
    lgClipboard_release();
    return;
  }
  if (!types || count > LG_CLIPBOARD_DATA_NONE)
    return;

  for (size_t i = 0; i < count; ++i)
    if (!validType(types[i]))
      return;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK(clipboard.stateLock);
  memcpy(clipboard.localTypes, types, count * sizeof(*types));
  clipboard.localTypeCount = count;
  clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);

  LG_LOCK_SHARED(clipboard.activeLock);
  if (clipboard.active.ops &&
      !clipboard.active.ops->notifyTypes(
        clipboard.active.opaque, types, count))
    DEBUG_WARN("Failed to publish the local clipboard types");
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
}

/* writeLock and activeLock must be held. */
static bool cancelRemoteNL(LG_ClipboardCancelReason reason)
{
  const bool result = streamProvider(clipboard.active.ops) ?
    clipboard.active.ops->dataCancel(clipboard.active.opaque,
        clipboard.remoteRequestId, reason) :
    clipboard.active.ops->data(clipboard.active.opaque,
        clipboard.remoteRequestId, LG_CLIPBOARD_DATA_NONE, NULL, 0);
  clearRemoteRequestNL();
  return result;
}

/* writeLock and activeLock must be held. */
static LG_ClipboardResult sendBegin(LG_ClipboardRequest transfer,
    LG_ClipboardData type, uint64_t sizeHint)
{
  LG_LOCK(clipboard.stateLock);
  const bool matches = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  if (!matches)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (clipboard.remoteBlocked)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (clipboard.remoteStarted || clipboard.remoteRequestType != type ||
      !validType(type))
  {
    cancelRemoteNL(LG_CLIPBOARD_CANCEL_INVALID);
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  LG_ClipboardResult result;
  if (streamProvider(clipboard.active.ops))
    result = clipboard.active.ops->dataBegin(clipboard.active.opaque,
        clipboard.remoteRequestId, type, sizeHint);
  else if (sizeHint != LG_CLIPBOARD_SIZE_UNKNOWN && sizeHint > SIZE_MAX)
    result = LG_CLIPBOARD_RESULT_FAILED;
  else
  {
    free(clipboard.remoteBuffer);
    clipboard.remoteBuffer         = NULL;
    clipboard.remoteBufferSize     = 0;
    clipboard.remoteBufferCapacity = 0;
    result = LG_CLIPBOARD_RESULT_ACCEPTED;
  }

  if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    clipboard.remoteStarted  = true;
    clipboard.remoteSizeHint = sizeHint;
    clipboard.remoteOffset   = 0;
  }
  else if (result == LG_CLIPBOARD_RESULT_BLOCKED)
    clipboard.remoteBlocked = true;
  else
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  return result;
}

/* writeLock and activeLock must be held. */
static LG_ClipboardResult sendChunk(LG_ClipboardRequest transfer,
    uint64_t offset, const void * data, size_t size)
{
  LG_LOCK(clipboard.stateLock);
  const bool matches = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  if (!matches)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (clipboard.remoteBlocked)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (!clipboard.remoteStarted || clipboard.remoteOffset != offset ||
      !data || !size || size > UINT64_MAX - offset)
  {
    cancelRemoteNL(LG_CLIPBOARD_CANCEL_INVALID);
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  LG_ClipboardResult result;
  if (streamProvider(clipboard.active.ops))
    result = clipboard.active.ops->dataChunk(clipboard.active.opaque,
        clipboard.remoteRequestId, offset, data, size);
  else if (offset > SIZE_MAX || size > SIZE_MAX - (size_t)offset ||
      !growBuffer(&clipboard.remoteBuffer,
        &clipboard.remoteBufferCapacity, (size_t)offset + size))
    result = LG_CLIPBOARD_RESULT_FAILED;
  else
  {
    memcpy(clipboard.remoteBuffer + offset, data, size);
    clipboard.remoteBufferSize = (size_t)offset + size;
    result = LG_CLIPBOARD_RESULT_ACCEPTED;
  }

  if (result == LG_CLIPBOARD_RESULT_ACCEPTED)
    clipboard.remoteOffset += size;
  else if (result == LG_CLIPBOARD_RESULT_BLOCKED)
    clipboard.remoteBlocked = true;
  else
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  return result;
}

/* writeLock and activeLock must be held. */
static LG_ClipboardResult sendEnd(LG_ClipboardRequest transfer,
    uint64_t finalSize)
{
  LG_LOCK(clipboard.stateLock);
  const bool matches = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  if (!matches)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (clipboard.remoteBlocked)
  {
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (!clipboard.remoteStarted || clipboard.remoteOffset != finalSize)
  {
    cancelRemoteNL(LG_CLIPBOARD_CANCEL_INVALID);
    LG_UNLOCK(clipboard.stateLock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  LG_ClipboardResult result;
  if (streamProvider(clipboard.active.ops))
    result = clipboard.active.ops->dataEnd(clipboard.active.opaque,
        clipboard.remoteRequestId, finalSize);
  else
    result = clipboard.active.ops->data(clipboard.active.opaque,
        clipboard.remoteRequestId, clipboard.remoteRequestType,
        clipboard.remoteBufferSize ? clipboard.remoteBuffer : NULL,
        clipboard.remoteBufferSize) ?
      LG_CLIPBOARD_RESULT_ACCEPTED : LG_CLIPBOARD_RESULT_FAILED;

  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
    clipboard.remoteBlocked = true;
  else
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  return result;
}

/* writeLock and activeLock must be held. */
static bool sendCancel(LG_ClipboardRequest transfer,
    LG_ClipboardCancelReason reason)
{
  LG_LOCK(clipboard.stateLock);
  const bool valid = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  if (!valid)
  {
    LG_UNLOCK(clipboard.stateLock);
    return false;
  }

  const bool result = cancelRemoteNL(reason);
  LG_UNLOCK(clipboard.stateLock);
  return result;
}

static LG_ClipboardResult pumpRemoteCompat(void)
{
  for (;;)
  {
    LG_LOCK(clipboard.stateLock);
    if (!clipboard.remoteRequest || !clipboard.remoteCompat)
    {
      LG_UNLOCK(clipboard.stateLock);
      return LG_CLIPBOARD_RESULT_FAILED;
    }
    const LG_ClipboardRequest transfer = clipboard.remoteTransferId;
    const LG_ClipboardData type = clipboard.remoteRequestType;
    const unsigned int phase = clipboard.remoteCompatPhase;
    const uint64_t offset = clipboard.remoteOffset;
    const size_t total = clipboard.remoteBufferSize;
    const uint8_t * data = clipboard.remoteBuffer;
    LG_UNLOCK(clipboard.stateLock);

    LG_ClipboardResult result;
    switch (phase)
    {
      case COMPAT_BEGIN:
        result = sendBegin(transfer, type, total);
        if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
          return result;
        LG_LOCK(clipboard.stateLock);
        if (clipboard.remoteRequest)
          clipboard.remoteCompatPhase = COMPAT_CHUNK;
        LG_UNLOCK(clipboard.stateLock);
        break;

      case COMPAT_CHUNK:
        if (offset < total)
        {
          const size_t remaining = total - offset;
          const size_t size = remaining < COMPAT_CHUNK_SIZE ?
            remaining : COMPAT_CHUNK_SIZE;
          result = sendChunk(transfer, offset, data + offset, size);
          if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
            return result;
          break;
        }
        LG_LOCK(clipboard.stateLock);
        if (clipboard.remoteRequest)
          clipboard.remoteCompatPhase = COMPAT_END;
        LG_UNLOCK(clipboard.stateLock);
        break;

      case COMPAT_END:
        return sendEnd(transfer, total);

      default:
        sendCancel(transfer, LG_CLIPBOARD_CANCEL_INVALID);
        return LG_CLIPBOARD_RESULT_FAILED;
    }
  }
}

LG_ClipboardResult lgClipboard_dataBegin(LG_ClipboardRequest transfer,
    LG_ClipboardData type, uint64_t sizeHint)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool compat = clipboard.remoteCompat;
  LG_UNLOCK(clipboard.stateLock);
  const LG_ClipboardResult result = compat ?
    LG_CLIPBOARD_RESULT_FAILED : sendBegin(transfer, type, sizeHint);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
  return result;
}

LG_ClipboardResult lgClipboard_dataChunk(LG_ClipboardRequest transfer,
    uint64_t offset, const void * data, size_t size)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool compat = clipboard.remoteCompat;
  LG_UNLOCK(clipboard.stateLock);
  const LG_ClipboardResult result = compat ?
    LG_CLIPBOARD_RESULT_FAILED : sendChunk(transfer, offset, data, size);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
  return result;
}

LG_ClipboardResult lgClipboard_dataEnd(LG_ClipboardRequest transfer,
    uint64_t finalSize)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return LG_CLIPBOARD_RESULT_FAILED;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool compat = clipboard.remoteCompat;
  LG_UNLOCK(clipboard.stateLock);
  const LG_ClipboardResult result = compat ?
    LG_CLIPBOARD_RESULT_FAILED : sendEnd(transfer, finalSize);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
  return result;
}

void lgClipboard_data(LG_ClipboardRequest transfer,
    LG_ClipboardData type, const void * data, size_t size)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool matches = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer;
  const bool provider = matches && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  const bool valid = provider && clipboard.remoteRequestType == type &&
    validType(type) && (!size || data) && !clipboard.remoteStarted &&
    !clipboard.remoteBlocked && !clipboard.remoteCompat;

  if (!valid)
  {
    LG_UNLOCK(clipboard.stateLock);
    if (provider)
      sendCancel(transfer, LG_CLIPBOARD_CANCEL_INVALID);
    LG_UNLOCK_SHARED(clipboard.activeLock);
    LG_UNLOCK(clipboard.writeLock);
    DEBUG_WARN("Ignoring unexpected local clipboard data");
    return;
  }

  if (!streamProvider(clipboard.active.ops))
  {
    const LG_ClipboardRequest request = clipboard.remoteRequestId;
    clearRemoteRequestNL();
    LG_UNLOCK(clipboard.stateLock);
    const bool result = clipboard.active.ops->data(clipboard.active.opaque,
        request, type, data, size);
    LG_UNLOCK_SHARED(clipboard.activeLock);
    LG_UNLOCK(clipboard.writeLock);
    if (!result)
      DEBUG_WARN("Failed to send remote clipboard data");
    return;
  }

  uint8_t * copy = size ? malloc(size) : NULL;
  if (size && !copy)
  {
    LG_UNLOCK(clipboard.stateLock);
    sendCancel(transfer, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
    LG_UNLOCK_SHARED(clipboard.activeLock);
    LG_UNLOCK(clipboard.writeLock);
    DEBUG_ERROR("Out of memory");
    return;
  }
  if (size)
    memcpy(copy, data, size);
  clipboard.remoteBuffer         = copy;
  clipboard.remoteBufferSize     = size;
  clipboard.remoteBufferCapacity = size;
  clipboard.remoteCompat         = true;
  clipboard.remoteCompatPhase    = COMPAT_BEGIN;
  LG_UNLOCK(clipboard.stateLock);

  const LG_ClipboardResult result = pumpRemoteCompat();
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);
  if (result == LG_CLIPBOARD_RESULT_FAILED)
    DEBUG_WARN("Failed to send remote clipboard data");
}

void lgClipboard_abort(LG_ClipboardRequest transfer)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  const bool result = sendCancel(
      transfer, LG_CLIPBOARD_CANCEL_ABORTED);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);

  if (!result)
    DEBUG_WARN("Failed to abort remote clipboard data");
}

static bool requestClipboard(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, const LG_ClipboardStreamOps * stream,
    void * opaque, LG_ClipboardRequest * resultId)
{
  if (resultId)
    *resultId = LG_CLIPBOARD_REQUEST_INVALID;
  if (!g_params.clipboardToLocal || !validType(type) ||
      (!replyFn && !validStream(stream)) || !clipboard.requests)
    return false;

  ClipboardRequest * request = calloc(1, sizeof(*request));
  if (!request)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!clipboard.active.ops)
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    freeRequest(request);
    return false;
  }

  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  const bool available = clipboard.localAvailable &&
    clipboard.remoteNotice && clipboard.remoteType == type;
  const uint32_t generation = clipboard.remoteGeneration;
  LG_ClipboardRequest id = LG_CLIPBOARD_REQUEST_INVALID;
  if (available)
  {
    id = nextRequestNL();
    *request = (ClipboardRequest)
    {
      .request          = id,
      .binding          = clipboard.active,
      .remoteGeneration = generation,
      .type             = type,
      .replyFn          = replyFn,
      .stream           = replyFn ? &legacyStream : stream,
      .opaque           = opaque,
      .sizeHint         = LG_CLIPBOARD_SIZE_UNKNOWN,
    };
  }
  LG_UNLOCK(clipboard.stateLock);

  const bool queued = available &&
    ll_push(clipboard.requests, request);
  /* Publish the request before the provider can make it visible to its
   * worker. A provider event may otherwise reach the stream sink before the
   * caller has learned which request it belongs to. */
  if (queued && resultId)
    *resultId = id;
  bool result = queued;
  if (queued)
    result = clipboard.active.ops->request(clipboard.active.opaque,
        request->request, type);
  LG_UNLOCK(clipboard.requestLock);
  LG_UNLOCK_SHARED(clipboard.activeLock);

  if (result)
    return true;
  if (queued)
  {
    ClipboardRequest * failed = takeRequest(id);
    if (!failed)
      return true;
    if (resultId)
      *resultId = LG_CLIPBOARD_REQUEST_INVALID;
    freeRequest(failed);
    return false;
  }

  freeRequest(request);
  return false;
}

bool lgClipboard_requestStream(LG_ClipboardData type,
    const LG_ClipboardStreamOps * stream, void * opaque,
    LG_ClipboardRequest * request)
{
  if (!request)
    return false;
  return requestClipboard(type, NULL, stream, opaque, request);
}

bool lgClipboard_requestReady(LG_ClipboardRequest id)
{
  if (id == LG_CLIPBOARD_REQUEST_INVALID || !clipboard.requests)
    return false;

  LG_LOCK(clipboard.callbackLock);
  ClipboardRequest * request = takeRequest(id);
  if (!request || !request->blocked)
  {
    if (request)
      returnRequest(request);
    LG_UNLOCK(clipboard.callbackLock);
    return false;
  }

  request->blocked = false;
  if (request->compat)
  {
    bool terminal = false;
    const LG_ClipboardResult result =
      pumpIncomingCompat(request, &terminal);
    if (!terminal && !returnRequest(request))
    {
      LG_UNLOCK(clipboard.callbackLock);
      return false;
    }
    if (terminal)
      freeRequest(request);
    LG_UNLOCK(clipboard.callbackLock);
    return result != LG_CLIPBOARD_RESULT_FAILED;
  }

  const ClipboardBinding binding = request->binding;
  if (!returnRequest(request))
  {
    LG_UNLOCK(clipboard.callbackLock);
    return false;
  }

  LG_LOCK_SHARED(clipboard.activeLock);
  const bool active = bindingActiveNL(&binding) &&
    streamProvider(binding.ops);
  const bool result = active && binding.ops->dataReady(
      binding.opaque, id);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  if (!result)
  {
    ClipboardRequest * failed = takeRequest(id);
    if (failed)
    {
      cancelRequest(failed, LG_CLIPBOARD_CANCEL_UNAVAILABLE);
      freeRequest(failed);
    }
  }
  LG_UNLOCK(clipboard.callbackLock);
  return result;
}

bool lgClipboard_request(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!replyFn)
    return false;
  return requestClipboard(type, replyFn, NULL, opaque, NULL);
}
