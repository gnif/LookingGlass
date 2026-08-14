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

#include <stdatomic.h>
#include <stdlib.h>

typedef struct ClipboardEventTarget
{
  const LG_ClipboardEventOps * events;
  void                       * opaque;
}
ClipboardEventTarget;

typedef struct PendingRequest
{
  bool                pending;
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
}
PendingRequest;

struct SpiceClipboard
{
  LG_Lock stateLock;
  LG_Lock eventDispatch;
  LG_Lock statusDispatch;

  bool                 available;
  uint32_t             statusGeneration;
  LG_ClipboardStatusFn statusCallback;
  void               * statusOpaque;

  const LG_ClipboardEventOps * events;
  void                        * eventOpaque;

  bool             remoteNotice;
  LG_ClipboardData remoteType;
  PendingRequest   read;
  PendingRequest   write;

  LG_ClipboardRequest requestSerial;
};

static _Atomic(SpiceClipboard *) l_callbackTarget;

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static LG_ClipboardRequest nextRequestNL(SpiceClipboard * clipboard)
{
  if (++clipboard->requestSerial == LG_CLIPBOARD_REQUEST_INVALID)
    ++clipboard->requestSerial;
  return clipboard->requestSerial;
}

static bool spiceType(PSDataType source, LG_ClipboardData * type)
{
  switch (source)
  {
    case SPICE_DATA_TEXT : *type = LG_CLIPBOARD_DATA_TEXT; return true;
    case SPICE_DATA_PNG  : *type = LG_CLIPBOARD_DATA_PNG ; return true;
    case SPICE_DATA_BMP  : *type = LG_CLIPBOARD_DATA_BMP ; return true;
    case SPICE_DATA_TIFF : *type = LG_CLIPBOARD_DATA_TIFF; return true;
    case SPICE_DATA_JPEG : *type = LG_CLIPBOARD_DATA_JPEG; return true;
    case SPICE_DATA_NONE : break;
  }

  return false;
}

static bool lgType(LG_ClipboardData source, PSDataType * type)
{
  switch (source)
  {
    case LG_CLIPBOARD_DATA_TEXT : *type = SPICE_DATA_TEXT; return true;
    case LG_CLIPBOARD_DATA_PNG  : *type = SPICE_DATA_PNG ; return true;
    case LG_CLIPBOARD_DATA_BMP  : *type = SPICE_DATA_BMP ; return true;
    case LG_CLIPBOARD_DATA_TIFF : *type = SPICE_DATA_TIFF; return true;
    case LG_CLIPBOARD_DATA_JPEG : *type = SPICE_DATA_JPEG; return true;
    case LG_CLIPBOARD_DATA_FILES: return false;
    case LG_CLIPBOARD_DATA_NONE : *type = SPICE_DATA_NONE; return true;
  }

  return false;
}

static void spiceSetStatusListener(void * opaque,
    LG_ClipboardStatusFn callback, void * callbackOpaque)
{
  SpiceClipboard * clipboard = opaque;
  LG_LOCK(clipboard->statusDispatch);

  LG_LOCK(clipboard->stateLock);
  clipboard->statusCallback = callback;
  clipboard->statusOpaque   = callbackOpaque;
  const LG_ClipboardStatus status =
  {
    .available  = clipboard->available,
    .generation = clipboard->statusGeneration,
  };
  LG_UNLOCK(clipboard->stateLock);

  if (callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(clipboard->statusDispatch);
}

static bool spiceAttach(void * opaque,
    const LG_ClipboardEventOps * events, void * eventOpaque)
{
  SpiceClipboard * clipboard = opaque;
  if (!events)
    return false;

  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  if (!clipboard->available)
  {
    LG_UNLOCK(clipboard->stateLock);
    LG_UNLOCK(clipboard->eventDispatch);
    return false;
  }

  clipboard->events      = events;
  clipboard->eventOpaque = eventOpaque;
  const bool             notice = clipboard->remoteNotice;
  const LG_ClipboardData type   = clipboard->remoteType;
  LG_UNLOCK(clipboard->stateLock);

  if (notice && events->notice)
    events->notice(eventOpaque, &type, 1);
  LG_UNLOCK(clipboard->eventDispatch);
  return true;
}

static void spiceDetach(void * opaque)
{
  SpiceClipboard * clipboard = opaque;
  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  const bool failWrite = clipboard->available && clipboard->write.pending;
  clipboard->events      = NULL;
  clipboard->eventOpaque = NULL;
  clipboard->read        = (PendingRequest) { 0 };
  clipboard->write       = (PendingRequest) { 0 };
  LG_UNLOCK(clipboard->stateLock);
  if (failWrite)
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  LG_UNLOCK(clipboard->eventDispatch);
}

static bool spiceRelease(void * opaque)
{
  SpiceClipboard * clipboard = opaque;
  LG_LOCK(clipboard->stateLock);
  const bool available = clipboard->available;
  const bool failWrite = clipboard->write.pending;
  clipboard->write = (PendingRequest) { 0 };
  LG_UNLOCK(clipboard->stateLock);

  if (!available)
    return false;

  const bool failed = !failWrite ||
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  return purespice_clipboardRelease() && failed;
}

static bool spiceNotifyTypes(void * opaque,
    const LG_ClipboardData types[], size_t count)
{
  SpiceClipboard * clipboard = opaque;
  if (count == 0)
    return spiceRelease(clipboard);
  if (!types || count > LG_CLIPBOARD_DATA_NONE)
    return false;

  PSDataType converted[LG_CLIPBOARD_DATA_NONE];
  for (size_t i = 0; i < count; ++i)
    if (types[i] == LG_CLIPBOARD_DATA_NONE ||
        !lgType(types[i], &converted[i]))
      return false;

  LG_LOCK(clipboard->stateLock);
  const bool available = clipboard->available;
  const bool failWrite = clipboard->write.pending;
  clipboard->write = (PendingRequest) { 0 };
  LG_UNLOCK(clipboard->stateLock);

  if (!available)
    return false;

  const bool failed = !failWrite ||
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  return purespice_clipboardGrab(converted, (int)count) && failed;
}

static bool spiceData(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  SpiceClipboard * clipboard = opaque;
  PSDataType converted;
  if (request == LG_CLIPBOARD_REQUEST_INVALID || !lgType(type, &converted) ||
      (type == LG_CLIPBOARD_DATA_NONE && size != 0) ||
      (size && !data))
    return false;

  LG_LOCK(clipboard->stateLock);
  const bool valid = clipboard->available && clipboard->write.pending &&
    clipboard->write.request == request &&
    (type == LG_CLIPBOARD_DATA_NONE || clipboard->write.type == type);
  if (valid)
    clipboard->write = (PendingRequest) { 0 };
  LG_UNLOCK(clipboard->stateLock);

  if (!valid || !purespice_clipboardDataStart(converted, size))
    return false;

  return !size || purespice_clipboardData(
      converted, (uint8_t *)data, size);
}

static bool spiceRequest(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type)
{
  SpiceClipboard * clipboard = opaque;
  PSDataType converted;
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      type == LG_CLIPBOARD_DATA_NONE || !lgType(type, &converted))
    return false;

  LG_LOCK(clipboard->stateLock);
  const bool valid = clipboard->available && clipboard->events &&
    !clipboard->read.pending;
  if (valid)
    clipboard->read = (PendingRequest)
    {
      .pending = true,
      .request = request,
      .type    = type,
    };
  LG_UNLOCK(clipboard->stateLock);

  if (!valid)
    return false;

  if (purespice_clipboardRequest(converted))
    return true;

  LG_LOCK(clipboard->stateLock);
  if (!clipboard->read.pending || clipboard->read.request != request)
  {
    LG_UNLOCK(clipboard->stateLock);
    return true;
  }
  clipboard->read = (PendingRequest) { 0 };
  LG_UNLOCK(clipboard->stateLock);
  return false;
}

static const LG_ClipboardOps l_clipboardOps =
{
  .name              = "SPICE",
  .setStatusListener = spiceSetStatusListener,
  .attach            = spiceAttach,
  .detach            = spiceDetach,
  .release           = spiceRelease,
  .notifyTypes       = spiceNotifyTypes,
  .data              = spiceData,
  .request           = spiceRequest,
};

bool spiceClipboard_init(SpiceClipboard ** clipboard)
{
  *clipboard = calloc(1, sizeof(**clipboard));
  if (!*clipboard)
    return false;

  LG_LOCK_INIT((*clipboard)->stateLock);
  LG_LOCK_INIT((*clipboard)->eventDispatch);
  LG_LOCK_INIT((*clipboard)->statusDispatch);
  (*clipboard)->remoteType = LG_CLIPBOARD_DATA_NONE;
  return true;
}

void spiceClipboard_free(SpiceClipboard ** clipboard)
{
  if (!clipboard || !*clipboard)
    return;

  SpiceClipboard * expected = *clipboard;
  atomic_compare_exchange_strong_explicit(&l_callbackTarget,
      &expected, NULL, memory_order_acq_rel, memory_order_acquire);

  LG_LOCK_FREE((*clipboard)->statusDispatch);
  LG_LOCK_FREE((*clipboard)->eventDispatch);
  LG_LOCK_FREE((*clipboard)->stateLock);
  free(*clipboard);
  *clipboard = NULL;
}

const LG_ClipboardOps * spiceClipboard_getOps(void)
{
  return &l_clipboardOps;
}

void spiceClipboard_setAvailable(SpiceClipboard * clipboard, bool available)
{
  LG_LOCK(clipboard->statusDispatch);
  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);

  const bool changed = clipboard->available != available;
  if (changed)
  {
    clipboard->available        = available;
    clipboard->statusGeneration = nextGeneration(
        clipboard->statusGeneration);
  }
  if (!available)
  {
    clipboard->remoteNotice = false;
    clipboard->remoteType   = LG_CLIPBOARD_DATA_NONE;
    clipboard->read         = (PendingRequest) { 0 };
    clipboard->write        = (PendingRequest) { 0 };
  }

  const LG_ClipboardStatusFn callback       = clipboard->statusCallback;
  void                     * callbackOpaque = clipboard->statusOpaque;
  const LG_ClipboardStatus status =
  {
    .available  = available,
    .generation = clipboard->statusGeneration,
  };
  LG_UNLOCK(clipboard->stateLock);
  LG_UNLOCK(clipboard->eventDispatch);

  if (changed && callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(clipboard->statusDispatch);
}

void spiceClipboard_setCallbackTarget(SpiceClipboard * clipboard)
{
  atomic_store_explicit(
      &l_callbackTarget, clipboard, memory_order_release);
}

static SpiceClipboard * callbackTarget(void)
{
  return atomic_load_explicit(&l_callbackTarget, memory_order_acquire);
}

void spiceClipboard_notice(PSDataType source)
{
  SpiceClipboard * clipboard = callbackTarget();
  if (!clipboard)
    return;

  LG_ClipboardData type;
  if (!spiceType(source, &type))
  {
    if (source != SPICE_DATA_NONE)
      DEBUG_ERROR("Invalid SPICE clipboard notice type: %d", source);
    spiceClipboard_release();
    return;
  }

  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  const bool failWrite = clipboard->write.pending;
  clipboard->remoteNotice = true;
  clipboard->remoteType   = type;
  clipboard->read         = (PendingRequest) { 0 };
  clipboard->write        = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = clipboard->available ? clipboard->events : NULL,
    .opaque = clipboard->eventOpaque,
  };
  LG_UNLOCK(clipboard->stateLock);

  if (failWrite)
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  if (target.events && target.events->notice)
    target.events->notice(target.opaque, &type, 1);
  LG_UNLOCK(clipboard->eventDispatch);
}

void spiceClipboard_data(PSDataType source, uint8_t * buffer, uint32_t size)
{
  SpiceClipboard * clipboard = callbackTarget();
  if (!clipboard)
    return;

  LG_ClipboardData type      = LG_CLIPBOARD_DATA_NONE;
  const bool       converted = spiceType(source, &type);
  const bool       validData = source == SPICE_DATA_NONE ||
    (converted && (!size || buffer));

  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  if (!clipboard->read.pending)
  {
    LG_UNLOCK(clipboard->stateLock);
    LG_UNLOCK(clipboard->eventDispatch);
    DEBUG_WARN("Ignoring unsolicited SPICE clipboard data");
    return;
  }

  const PendingRequest request = clipboard->read;
  clipboard->read = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = clipboard->available ? clipboard->events : NULL,
    .opaque = clipboard->eventOpaque,
  };
  LG_UNLOCK(clipboard->stateLock);

  if (target.events && target.events->data)
  {
    if (!validData || (source != SPICE_DATA_NONE && type != request.type))
    {
      DEBUG_ERROR("Invalid SPICE clipboard response");
      target.events->data(target.opaque, request.request,
          LG_CLIPBOARD_DATA_NONE, NULL, 0);
    }
    else
    {
      if (type == LG_CLIPBOARD_DATA_TEXT && size)
      {
        uint8_t * output = buffer;
        for (uint32_t i = 0; i < size; ++i)
          if (buffer[i] != '\r')
            *output++ = buffer[i];
        size = (uint32_t)(output - buffer);
      }

      target.events->data(target.opaque, request.request,
          type, buffer, size);
    }
  }
  LG_UNLOCK(clipboard->eventDispatch);
}

void spiceClipboard_release(void)
{
  SpiceClipboard * clipboard = callbackTarget();
  if (!clipboard)
    return;

  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  clipboard->remoteNotice = false;
  clipboard->remoteType   = LG_CLIPBOARD_DATA_NONE;
  clipboard->read         = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = clipboard->available ? clipboard->events : NULL,
    .opaque = clipboard->eventOpaque,
  };
  LG_UNLOCK(clipboard->stateLock);

  if (target.events && target.events->release)
    target.events->release(target.opaque);
  LG_UNLOCK(clipboard->eventDispatch);
}

void spiceClipboard_request(PSDataType source)
{
  SpiceClipboard * clipboard = callbackTarget();
  if (!clipboard)
  {
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
    return;
  }

  LG_ClipboardData type;
  if (!spiceType(source, &type))
  {
    DEBUG_ERROR("Invalid SPICE clipboard request type: %d", source);
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
    return;
  }

  LG_LOCK(clipboard->eventDispatch);
  LG_LOCK(clipboard->stateLock);
  const bool busy = clipboard->write.pending;
  const ClipboardEventTarget target =
  {
    .events = clipboard->available && !busy ?
      clipboard->events : NULL,
    .opaque = clipboard->eventOpaque,
  };
  LG_ClipboardRequest request = LG_CLIPBOARD_REQUEST_INVALID;
  if (target.events)
  {
    request = nextRequestNL(clipboard);
    clipboard->write = (PendingRequest)
    {
      .pending = true,
      .request = request,
      .type    = type,
    };
  }
  LG_UNLOCK(clipboard->stateLock);

  const bool accepted = target.events && target.events->request &&
    target.events->request(target.opaque, request, type);
  if (!accepted)
  {
    bool fail = !busy && request == LG_CLIPBOARD_REQUEST_INVALID;
    LG_LOCK(clipboard->stateLock);
    if (clipboard->write.pending && clipboard->write.request == request)
    {
      clipboard->write = (PendingRequest) { 0 };
      fail = true;
    }
    LG_UNLOCK(clipboard->stateLock);
    if (fail)
      purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
    else if (busy)
      DEBUG_WARN("Ignoring overlapping SPICE clipboard request");
  }
  LG_UNLOCK(clipboard->eventDispatch);
}

void spiceClipboard_status(bool available)
{
  SpiceClipboard * clipboard = callbackTarget();
  if (clipboard)
    spiceClipboard_setAvailable(clipboard, available);
}
