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
  void              * opaque;
}
ClipboardRequest;

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

  LG_ClipboardRequest requestSerial;
  LG_ClipboardRequest transferSerial;
  struct ll         * requests;
}
clipboard;

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

static void clearRemoteRequestNL(void)
{
  clipboard.remoteRequest        = false;
  clipboard.remoteRequestBinding = (ClipboardBinding) { 0 };
  clipboard.remoteRequestId      = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard.remoteTransferId     = LG_CLIPBOARD_REQUEST_INVALID;
  clipboard.remoteRequestType    = LG_CLIPBOARD_DATA_NONE;
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
    uint32_t generation, bool all)
{
  ClipboardRequest * request;
  while ((request = takeCancelableRequest(binding, generation, all)))
  {
    request->replyFn(
        request->opaque, LG_CLIPBOARD_DATA_NONE, NULL, 0);
    free(request);
  }
}

static void resetRemote(void)
{
  bool release;

  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  release = clipboard.remoteNotice && clipboard.localAvailable &&
    g_params.clipboardToLocal;
  clipboard.remoteNotice     = false;
  clipboard.remoteType       = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(NULL, 0, true);
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
    ops->release && ops->notifyTypes && ops->data && ops->request;
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

  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  clipboard.remoteNotice     = true;
  clipboard.remoteType       = type;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  generation = clipboard.remoteGeneration;
  clearRemoteRequestNL();
  notice = clipboard.localAvailable && g_params.clipboardToLocal;
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(&current, generation, false);
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

static void eventData(void * opaque, LG_ClipboardRequest id,
    LG_ClipboardData type, const void * data, size_t size)
{
  ClipboardBinding * binding = opaque;
  if (id == LG_CLIPBOARD_REQUEST_INVALID)
    return;

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!bindingActiveNL(binding))
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    return;
  }

  const ClipboardBinding current = *binding;
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.callbackLock);
  ClipboardRequest * request = takeRequest(id);
  if (!request)
  {
    LG_UNLOCK(clipboard.callbackLock);
    DEBUG_WARN("Ignoring stale clipboard data from %s",
        current.ops->name);
    return;
  }

  LG_LOCK(clipboard.stateLock);
  const bool deliver = clipboard.localAvailable &&
    g_params.clipboardToLocal && clipboard.remoteNotice &&
    request->remoteGeneration == clipboard.remoteGeneration;
  LG_UNLOCK(clipboard.stateLock);

  LG_LOCK_SHARED(clipboard.activeLock);
  const bool active = bindingActiveNL(&current);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  if (!deliver || !active || !bindingEqual(&request->binding, &current) ||
      type != request->type || !validType(type) ||
      size > UINT32_MAX || (size && !data))
  {
    DEBUG_ERROR("Invalid clipboard response from %s", current.ops->name);
    request->replyFn(
        request->opaque, LG_CLIPBOARD_DATA_NONE, NULL, 0);
  }
  else
    request->replyFn(
        request->opaque, type, data, (uint32_t)size);

  LG_UNLOCK(clipboard.callbackLock);
  free(request);
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
  LG_LOCK(clipboard.requestLock);
  LG_LOCK(clipboard.stateLock);
  release = clipboard.remoteNotice && clipboard.localAvailable &&
    g_params.clipboardToLocal;
  clipboard.remoteNotice     = false;
  clipboard.remoteType       = LG_CLIPBOARD_DATA_NONE;
  clipboard.remoteGeneration = nextGeneration(
      clipboard.remoteGeneration);
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  LG_LOCK(clipboard.callbackLock);
  cancelRequestsNL(NULL, 0, true);
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

static const LG_ClipboardEventOps eventOps =
{
  .notice  = eventNotice,
  .data    = eventData,
  .release = eventRelease,
  .request = eventRequest,
};

/* providerLock must be held. dropActive suppresses remote release when the
 * endpoint has already disappeared; detach must always quiesce local events. */
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

    if (old.ops)
    {
      if (!dropActive && !old.ops->release(old.opaque))
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
  cancelRequestsNL(NULL, 0, true);
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
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);
  LG_UNLOCK(clipboard.requestLock);

  if (notice)
    g_state.ds->cbNotice(type);
  if (!available)
  {
    cancelRequestsNL(NULL, 0, true);
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

void lgClipboard_setTransport(const LG_ClipboardOps * ops, void * opaque)
{
  setBinding(&clipboard.transport,
      ops, opaque, transportStatusChanged);
}

void lgClipboard_dropTransport(void)
{
  LG_LOCK(clipboard.registrationLock);
  LG_LOCK(clipboard.providerLock);
  const ClipboardBinding old = clipboard.transport;
  LG_UNLOCK(clipboard.providerLock);

  if (old.ops && old.ops->setStatusListener)
    old.ops->setStatusListener(old.opaque, NULL, NULL);

  LG_LOCK(clipboard.providerLock);
  LG_LOCK_EXCLUSIVE(clipboard.activeLock);
  const bool wasActive = bindingActiveNL(&clipboard.transport);
  clipboard.transport = (ClipboardBinding) { 0 };
  LG_UNLOCK_EXCLUSIVE(clipboard.activeLock);
  updateActive(wasActive);
  LG_UNLOCK(clipboard.providerLock);
  LG_UNLOCK(clipboard.registrationLock);
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
  if (!matches)
  {
    LG_UNLOCK(clipboard.stateLock);
    LG_UNLOCK_SHARED(clipboard.activeLock);
    LG_UNLOCK(clipboard.writeLock);
    return;
  }

  const LG_ClipboardRequest request = clipboard.remoteRequestId;
  const bool provider = clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  const bool valid = provider && clipboard.remoteRequestType == type &&
    validType(type) && (!size || data);
  clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);

  const bool result = provider && clipboard.active.ops->data(
      clipboard.active.opaque, request,
      valid ? type : LG_CLIPBOARD_DATA_NONE,
      valid ? data : NULL, valid ? size : 0);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);

  if (!valid)
    DEBUG_WARN("Ignoring unexpected local clipboard data");
  else if (!result)
    DEBUG_WARN("Failed to send remote clipboard data");
}

void lgClipboard_abort(LG_ClipboardRequest transfer)
{
  if (!g_params.clipboardToVM ||
      transfer == LG_CLIPBOARD_REQUEST_INVALID)
    return;

  LG_LOCK(clipboard.writeLock);
  LG_LOCK_SHARED(clipboard.activeLock);
  LG_LOCK(clipboard.stateLock);
  const bool matches = clipboard.remoteRequest &&
    clipboard.remoteTransferId == transfer;
  const bool valid = matches && clipboard.active.ops &&
    bindingEqual(&clipboard.remoteRequestBinding, &clipboard.active);
  const LG_ClipboardRequest request = clipboard.remoteRequestId;
  if (matches)
    clearRemoteRequestNL();
  LG_UNLOCK(clipboard.stateLock);

  const bool result = !valid || clipboard.active.ops->data(
      clipboard.active.opaque, request,
      LG_CLIPBOARD_DATA_NONE, NULL, 0);
  LG_UNLOCK_SHARED(clipboard.activeLock);
  LG_UNLOCK(clipboard.writeLock);

  if (!result)
    DEBUG_WARN("Failed to abort remote clipboard data");
}

bool lgClipboard_request(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!g_params.clipboardToLocal || !validType(type) ||
      !replyFn || !clipboard.requests)
    return false;

  ClipboardRequest * request = malloc(sizeof(*request));
  if (!request)
  {
    DEBUG_ERROR("Out of memory");
    return false;
  }

  LG_LOCK_SHARED(clipboard.activeLock);
  if (!clipboard.active.ops)
  {
    LG_UNLOCK_SHARED(clipboard.activeLock);
    free(request);
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
      .opaque           = opaque,
    };
  }
  LG_UNLOCK(clipboard.stateLock);

  const bool queued = available &&
    ll_push(clipboard.requests, request);
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
    free(failed);
    return false;
  }

  free(request);
  return false;
}
