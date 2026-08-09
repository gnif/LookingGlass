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

#include "clipboard_spice.h"

#include "common/debug.h"
#include "common/locking.h"

#include <stdatomic.h>

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

static struct
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
}
l_spice =
{
  .stateLock      = ATOMIC_FLAG_INIT,
  .eventDispatch  = ATOMIC_FLAG_INIT,
  .statusDispatch = ATOMIC_FLAG_INIT,
  .remoteType     = LG_CLIPBOARD_DATA_NONE,
};

static uint32_t nextGeneration(uint32_t generation)
{
  if (++generation == 0)
    ++generation;
  return generation;
}

static LG_ClipboardRequest nextRequestNL(void)
{
  if (++l_spice.requestSerial == LG_CLIPBOARD_REQUEST_INVALID)
    ++l_spice.requestSerial;
  return l_spice.requestSerial;
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
    case LG_CLIPBOARD_DATA_NONE : *type = SPICE_DATA_NONE; return true;
  }

  return false;
}

static void spiceSetStatusListener(void * opaque,
    LG_ClipboardStatusFn callback, void * callbackOpaque)
{
  (void)opaque;
  LG_LOCK(l_spice.statusDispatch);

  LG_LOCK(l_spice.stateLock);
  l_spice.statusCallback = callback;
  l_spice.statusOpaque   = callbackOpaque;
  const LG_ClipboardStatus status =
  {
    .available  = l_spice.available,
    .generation = l_spice.statusGeneration,
  };
  LG_UNLOCK(l_spice.stateLock);

  if (callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(l_spice.statusDispatch);
}

static bool spiceAttach(void * opaque,
    const LG_ClipboardEventOps * events, void * eventOpaque)
{
  (void)opaque;
  if (!events)
    return false;

  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  if (!l_spice.available)
  {
    LG_UNLOCK(l_spice.stateLock);
    LG_UNLOCK(l_spice.eventDispatch);
    return false;
  }

  l_spice.events      = events;
  l_spice.eventOpaque = eventOpaque;
  const bool             notice = l_spice.remoteNotice;
  const LG_ClipboardData type   = l_spice.remoteType;
  LG_UNLOCK(l_spice.stateLock);

  if (notice && events->notice)
    events->notice(eventOpaque, &type, 1);
  LG_UNLOCK(l_spice.eventDispatch);
  return true;
}

static void spiceDetach(void * opaque)
{
  (void)opaque;
  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  const bool failWrite = l_spice.available && l_spice.write.pending;
  l_spice.events      = NULL;
  l_spice.eventOpaque = NULL;
  l_spice.read        = (PendingRequest) { 0 };
  l_spice.write       = (PendingRequest) { 0 };
  LG_UNLOCK(l_spice.stateLock);
  if (failWrite)
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  LG_UNLOCK(l_spice.eventDispatch);
}

static bool spiceRelease(void * opaque)
{
  (void)opaque;
  LG_LOCK(l_spice.stateLock);
  const bool available  = l_spice.available;
  const bool failWrite  = l_spice.write.pending;
  l_spice.write = (PendingRequest) { 0 };
  LG_UNLOCK(l_spice.stateLock);

  if (!available)
    return false;

  const bool failed = !failWrite ||
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  return purespice_clipboardRelease() && failed;
}

static bool spiceNotifyTypes(void * opaque,
    const LG_ClipboardData types[], size_t count)
{
  (void)opaque;
  if (count == 0)
    return spiceRelease(NULL);
  if (!types || count > LG_CLIPBOARD_DATA_NONE)
    return false;

  PSDataType converted[LG_CLIPBOARD_DATA_NONE];
  for (size_t i = 0; i < count; ++i)
    if (types[i] == LG_CLIPBOARD_DATA_NONE ||
        !lgType(types[i], &converted[i]))
      return false;

  LG_LOCK(l_spice.stateLock);
  const bool available = l_spice.available;
  const bool failWrite = l_spice.write.pending;
  l_spice.write = (PendingRequest) { 0 };
  LG_UNLOCK(l_spice.stateLock);

  if (!available)
    return false;

  const bool failed = !failWrite ||
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  return purespice_clipboardGrab(converted, (int)count) && failed;
}

static bool spiceData(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type, const void * data, size_t size)
{
  (void)opaque;
  PSDataType converted;
  if (request == LG_CLIPBOARD_REQUEST_INVALID || !lgType(type, &converted) ||
      (type == LG_CLIPBOARD_DATA_NONE && size != 0) ||
      (size && !data))
    return false;

  LG_LOCK(l_spice.stateLock);
  const bool valid = l_spice.available && l_spice.write.pending &&
    l_spice.write.request == request &&
    (type == LG_CLIPBOARD_DATA_NONE || l_spice.write.type == type);
  if (valid)
    l_spice.write = (PendingRequest) { 0 };
  LG_UNLOCK(l_spice.stateLock);

  if (!valid || !purespice_clipboardDataStart(converted, size))
    return false;

  return !size || purespice_clipboardData(
      converted, (uint8_t *)data, size);
}

static bool spiceRequest(void * opaque, LG_ClipboardRequest request,
    LG_ClipboardData type)
{
  (void)opaque;
  PSDataType converted;
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      type == LG_CLIPBOARD_DATA_NONE || !lgType(type, &converted))
    return false;

  LG_LOCK(l_spice.stateLock);
  const bool valid = l_spice.available && l_spice.events &&
    !l_spice.read.pending;
  if (valid)
    l_spice.read = (PendingRequest)
    {
      .pending    = true,
      .request    = request,
      .type       = type,
    };
  LG_UNLOCK(l_spice.stateLock);

  if (!valid)
    return false;

  if (purespice_clipboardRequest(converted))
    return true;

  LG_LOCK(l_spice.stateLock);
  if (!l_spice.read.pending || l_spice.read.request != request)
  {
    LG_UNLOCK(l_spice.stateLock);
    return true;
  }
  l_spice.read = (PendingRequest) { 0 };
  LG_UNLOCK(l_spice.stateLock);
  return false;
}

const LG_ClipboardOps LGC_Spice =
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

void lgcSpice_setAvailable(bool available)
{
  LG_LOCK(l_spice.statusDispatch);
  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);

  const bool changed = l_spice.available != available;
  if (changed)
  {
    l_spice.available        = available;
    l_spice.statusGeneration = nextGeneration(
        l_spice.statusGeneration);
  }
  if (!available)
  {
    l_spice.remoteNotice = false;
    l_spice.remoteType   = LG_CLIPBOARD_DATA_NONE;
    l_spice.read         = (PendingRequest) { 0 };
    l_spice.write        = (PendingRequest) { 0 };
  }

  const LG_ClipboardStatusFn callback       = l_spice.statusCallback;
  void                     * callbackOpaque = l_spice.statusOpaque;
  const LG_ClipboardStatus status =
  {
    .available  = available,
    .generation = l_spice.statusGeneration,
  };
  LG_UNLOCK(l_spice.stateLock);
  LG_UNLOCK(l_spice.eventDispatch);

  if (changed && callback)
    callback(callbackOpaque, &status);
  LG_UNLOCK(l_spice.statusDispatch);
}

void lgcSpice_notice(PSDataType source)
{
  LG_ClipboardData type;
  if (!spiceType(source, &type))
  {
    if (source != SPICE_DATA_NONE)
      DEBUG_ERROR("Invalid SPICE clipboard notice type: %d", source);
    lgcSpice_release();
    return;
  }

  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  const bool failWrite = l_spice.write.pending;
  l_spice.remoteNotice = true;
  l_spice.remoteType   = type;
  l_spice.read         = (PendingRequest) { 0 };
  l_spice.write        = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = l_spice.available ? l_spice.events : NULL,
    .opaque = l_spice.eventOpaque,
  };
  LG_UNLOCK(l_spice.stateLock);

  if (failWrite)
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
  if (target.events && target.events->notice)
    target.events->notice(target.opaque, &type, 1);
  LG_UNLOCK(l_spice.eventDispatch);
}

void lgcSpice_data(PSDataType source, uint8_t * buffer, uint32_t size)
{
  LG_ClipboardData type      = LG_CLIPBOARD_DATA_NONE;
  const bool       converted = spiceType(source, &type);
  const bool       validData = source == SPICE_DATA_NONE ||
    (converted && (!size || buffer));

  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  if (!l_spice.read.pending)
  {
    LG_UNLOCK(l_spice.stateLock);
    LG_UNLOCK(l_spice.eventDispatch);
    DEBUG_WARN("Ignoring unsolicited SPICE clipboard data");
    return;
  }

  const PendingRequest request = l_spice.read;
  l_spice.read = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = l_spice.available ? l_spice.events : NULL,
    .opaque = l_spice.eventOpaque,
  };
  LG_UNLOCK(l_spice.stateLock);

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
  LG_UNLOCK(l_spice.eventDispatch);
}

void lgcSpice_release(void)
{
  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  l_spice.remoteNotice = false;
  l_spice.remoteType   = LG_CLIPBOARD_DATA_NONE;
  l_spice.read         = (PendingRequest) { 0 };
  const ClipboardEventTarget target =
  {
    .events = l_spice.available ? l_spice.events : NULL,
    .opaque = l_spice.eventOpaque,
  };
  LG_UNLOCK(l_spice.stateLock);

  if (target.events && target.events->release)
    target.events->release(target.opaque);
  LG_UNLOCK(l_spice.eventDispatch);
}

void lgcSpice_request(PSDataType source)
{
  LG_ClipboardData type;
  if (!spiceType(source, &type))
  {
    DEBUG_ERROR("Invalid SPICE clipboard request type: %d", source);
    purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
    return;
  }

  LG_LOCK(l_spice.eventDispatch);
  LG_LOCK(l_spice.stateLock);
  const bool busy = l_spice.write.pending;
  const ClipboardEventTarget target =
  {
    .events = l_spice.available && !busy ?
      l_spice.events : NULL,
    .opaque = l_spice.eventOpaque,
  };
  LG_ClipboardRequest request = LG_CLIPBOARD_REQUEST_INVALID;
  if (target.events)
  {
    request = nextRequestNL();
    l_spice.write = (PendingRequest)
    {
      .pending    = true,
      .request    = request,
      .type       = type,
    };
  }
  LG_UNLOCK(l_spice.stateLock);

  const bool accepted = target.events && target.events->request &&
    target.events->request(target.opaque, request, type);
  if (!accepted)
  {
    bool fail = !busy && request == LG_CLIPBOARD_REQUEST_INVALID;
    LG_LOCK(l_spice.stateLock);
    if (l_spice.write.pending && l_spice.write.request == request)
    {
      l_spice.write = (PendingRequest) { 0 };
      fail = true;
    }
    LG_UNLOCK(l_spice.stateLock);
    if (fail)
      purespice_clipboardDataStart(SPICE_DATA_NONE, 0);
    else if (busy)
      DEBUG_WARN("Ignoring overlapping SPICE clipboard request");
  }
  LG_UNLOCK(l_spice.eventDispatch);
}
