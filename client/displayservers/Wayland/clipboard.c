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

#include "wayland.h"

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <wayland-client.h>

#include "../../src/clipboard.h"
#include "common/debug.h"
#include "common/KVMFRClipboard.h"

struct DataOffer {
  bool isSelfCopy;
  char * mimetypes[LG_CLIPBOARD_DATA_NONE];
};

static const char * textMimetypes[] =
{
  "text/plain",
  "text/plain;charset=utf-8",
  "TEXT",
  "STRING",
  "UTF8_STRING",
  NULL,
};

static const char * pngMimetypes[] =
{
  "image/png",
  NULL,
};

static const char * bmpMimetypes[] =
{
  "image/bmp",
  "image/x-bmp",
  "image/x-MS-bmp",
  "image/x-win-bitmap",
  NULL,
};

static const char * tiffMimetypes[] =
{
  "image/tiff",
  NULL,
};

static const char * jpegMimetypes[] =
{
  "image/jpeg",
  NULL,
};

static const char ** cbTypeToMimetypes(enum LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return textMimetypes;
    case LG_CLIPBOARD_DATA_PNG:
      return pngMimetypes;
    case LG_CLIPBOARD_DATA_BMP:
      return bmpMimetypes;
    case LG_CLIPBOARD_DATA_TIFF:
      return tiffMimetypes;
    case LG_CLIPBOARD_DATA_JPEG:
      return jpegMimetypes;
    default:
      DEBUG_ERROR("invalid clipboard type");
      abort();
  }
}

static bool containsMimetype(const char ** mimetypes,
    const char * needle)
{
  for (const char ** mimetype = mimetypes; *mimetype; mimetype++)
    if (!strcmp(needle, *mimetype))
      return true;

  return false;
}

static bool mimetypeEndswith(const char * mimetype, const char * what)
{
  size_t mimetypeLen = strlen(mimetype);
  size_t whatLen = strlen(what);

  if (mimetypeLen < whatLen)
    return false;

  return !strcmp(mimetype + mimetypeLen - whatLen, what);
}

static bool isTextMimetype(const char * mimetype)
{
  if (containsMimetype(textMimetypes, mimetype))
    return true;

  if (!strcmp(mimetype, "text/ico"))
    return false;

  char * text = "text/";
  if (!strncmp(mimetype, text, strlen(text)))
    return true;

  if (mimetypeEndswith(mimetype, "script") ||
      mimetypeEndswith(mimetype, "xml") ||
      mimetypeEndswith(mimetype, "yaml"))
    return true;

  if (strstr(mimetype, "json"))
    return true;

  return false;
}

static enum LG_ClipboardData mimetypeToCbType(const char * mimetype)
{
  if (isTextMimetype(mimetype))
    return LG_CLIPBOARD_DATA_TEXT;

  if (containsMimetype(pngMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_PNG;

  if (containsMimetype(bmpMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_BMP;

  if (containsMimetype(tiffMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_TIFF;

  if (containsMimetype(jpegMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_JPEG;

  return LG_CLIPBOARD_DATA_NONE;
}

static bool isImageCbtype(enum LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return false;
    case LG_CLIPBOARD_DATA_PNG:
    case LG_CLIPBOARD_DATA_BMP:
    case LG_CLIPBOARD_DATA_TIFF:
    case LG_CLIPBOARD_DATA_JPEG:
      return true;
    default:
      DEBUG_ERROR("invalid clipboard type");
      abort();
  }
}

static bool hasAnyMimetype(char ** mimetypes)
{
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (mimetypes[i])
      return true;
  return false;
}

static bool hasImageMimetype(char ** mimetypes)
{
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (isImageCbtype(i) && mimetypes[i])
      return true;
  return false;
}

/* wlCb.lock must be held. */
static struct ClipboardRead * clipboardReadTakeCurrentNL(void)
{
  struct ClipboardRead * data = wlCb.currentRead;
  if (!data)
    return NULL;

  wlCb.currentRead = NULL;
  return data;
}

static void clipboardReadRetire(struct ClipboardRead * data)
{
  if (data)
    waylandPollUnregister(data->fd);
}

static bool clipboardReadClaim(struct ClipboardRead * data)
{
  LG_LOCK(wlCb.lock);
  const bool current = wlCb.currentRead == data;
  if (current)
    clipboardReadTakeCurrentNL();
  LG_UNLOCK(wlCb.lock);

  if (current)
    clipboardReadRetire(data);
  return current;
}

static void clipboardReadRelease(struct ClipboardRead * data)
{
  if (atomic_fetch_sub_explicit(
      &data->references, 1, memory_order_acq_rel) == 1)
    free(data);
}

static void clipboardReadCleanup(void * opaque)
{
  struct ClipboardRead * data = opaque;
  close(data->fd);
  clipboardReadRelease(data);
}

// Destination client handlers.

static void dataOfferHandleOffer(void * opaque, struct wl_data_offer * offer,
    const char * mimetype)
{
  struct DataOffer * data = opaque;

  if (!strcmp(mimetype, wlCb.lgMimetype))
  {
    data->isSelfCopy = true;
    return;
  }

  enum LG_ClipboardData type = mimetypeToCbType(mimetype);

  if (type == LG_CLIPBOARD_DATA_NONE)
    return;

  // text/html represents rich text format, which is almost never desirable when
  // and should not be used when a plain text or image format is available.
  if ((isImageCbtype(type) || containsMimetype(textMimetypes, mimetype)) &&
      data->mimetypes[LG_CLIPBOARD_DATA_TEXT] &&
      strstr(data->mimetypes[LG_CLIPBOARD_DATA_TEXT], "html"))
  {
    free(data->mimetypes[LG_CLIPBOARD_DATA_TEXT]);
    data->mimetypes[LG_CLIPBOARD_DATA_TEXT] = NULL;
  }

  if (strstr(mimetype, "html") && hasImageMimetype(data->mimetypes))
    return;

  if (data->mimetypes[type])
    return;

  data->mimetypes[type] = strdup(mimetype);
}

static void dataOfferHandleSourceActions(void * data,
    struct wl_data_offer * offer, uint32_t sourceActions)
{
  // Do nothing.
}

static void dataOfferHandleAction(void * data, struct wl_data_offer * offer,
    uint32_t dndAction)
{
  // Do nothing.
}

static const struct wl_data_offer_listener dataOfferListener = {
  .offer = dataOfferHandleOffer,
  .source_actions = dataOfferHandleSourceActions,
  .action = dataOfferHandleAction,
};

static void dataDeviceHandleDataOffer(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  struct DataOffer * extra = calloc(1, sizeof(struct DataOffer));
  if (!extra)
  {
    DEBUG_ERROR("Out of memory while handling clipboard");
    abort();
  }
  wl_data_offer_set_user_data(offer, extra);
  wl_data_offer_add_listener(offer, &dataOfferListener, extra);
}

static void dataDeviceHandleSelection(void * opaque,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  if (!offer)
  {
    LG_LOCK(wlCb.lock);
    wlCb.selectionSource = NULL;
    LG_UNLOCK(wlCb.lock);
    waylandCBInvalidate();
    return;
  }

  struct DataOffer * extra = wl_data_offer_get_user_data(offer);
  if (!hasAnyMimetype(extra->mimetypes) || extra->isSelfCopy)
  {
    if (!extra->isSelfCopy)
    {
      LG_LOCK(wlCb.lock);
      wlCb.selectionSource = NULL;
      LG_UNLOCK(wlCb.lock);
    }
    wl_data_offer_set_user_data(offer, NULL);
    for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
      free(extra->mimetypes[i]);
    free(extra);
    waylandCBInvalidate();
    wl_data_offer_destroy(offer);
    return;
  }

  size_t idx = 0;
  enum LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (extra->mimetypes[i])
      types[idx++] = i;

  char * oldMimetypes[LG_CLIPBOARD_DATA_NONE];
  LG_LOCK(wlCb.lock);
  struct ClipboardRead * read = clipboardReadTakeCurrentNL();
  struct wl_data_offer * oldOffer = wlCb.offer;
  memcpy(oldMimetypes, wlCb.mimetypes, sizeof(oldMimetypes));
  wlCb.selectionSource = NULL;
  wlCb.offer = offer;
  memcpy(wlCb.mimetypes, extra->mimetypes, sizeof(wlCb.mimetypes));
  LG_UNLOCK(wlCb.lock);

  wl_data_offer_set_user_data(offer, NULL);
  memset(extra->mimetypes, 0, sizeof(extra->mimetypes));
  free(extra);

  if (read)
  {
    const LG_ClipboardRequest request = read->request;
    clipboardReadRetire(read);
    lgClipboard_abort(request);
  }
  if (oldOffer && oldOffer != offer)
    wl_data_offer_destroy(oldOffer);
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    free(oldMimetypes[i]);

  lgClipboard_notifyTypes(types, idx);
}

static void dataDeviceHandleEnter(void * data, struct wl_data_device * device,
    uint32_t serial, struct wl_surface * surface, wl_fixed_t sxW, wl_fixed_t syW,
    struct wl_data_offer * offer)
{
  DEBUG_ASSERT(wlCb.dndOffer == NULL);
  wlCb.dndOffer = offer;

  struct DataOffer * extra = wl_data_offer_get_user_data(offer);
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    free(extra->mimetypes[i]);
  free(extra);

  wl_data_offer_set_user_data(offer, NULL);
  wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE,
      WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
}

static void dataDeviceHandleMotion(void * data, struct wl_data_device * device,
    uint32_t time, wl_fixed_t sxW, wl_fixed_t syW)
{
  // Do nothing.
}

static void dataDeviceHandleLeave(void * data, struct wl_data_device * device)
{
  wl_data_offer_destroy(wlCb.dndOffer);
  wlCb.dndOffer = NULL;
}

static void dataDeviceHandleDrop(void * data, struct wl_data_device * device)
{
  wl_data_offer_destroy(wlCb.dndOffer);
  wlCb.dndOffer = NULL;
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = dataDeviceHandleDataOffer,
  .selection = dataDeviceHandleSelection,
  .enter = dataDeviceHandleEnter,
  .motion = dataDeviceHandleMotion,
  .leave = dataDeviceHandleLeave,
  .drop = dataDeviceHandleDrop,
};

bool waylandCBInit(void)
{
  if (!wlWm.seat)
  {
    DEBUG_WARN("Clipboard unavailable without wl_seat");
    return false;
  }

  if (!wlWm.dataDeviceManager)
  {
    DEBUG_ERROR("Missing wl_data_device_manager interface (version 3+)");
    return false;
  }

  wlCb.dataDevice = wl_data_device_manager_get_data_device(
      wlWm.dataDeviceManager, wlWm.seat);
  if (!wlCb.dataDevice)
  {
    DEBUG_ERROR("Failed to get data device");
    return false;
  }

  wl_data_device_add_listener(wlCb.dataDevice, &dataDeviceListener, NULL);

  snprintf(wlCb.lgMimetype, sizeof(wlCb.lgMimetype),
      "application/x-looking-glass-copy;pid=%d", getpid());

  return true;
}

enum ClipboardReadOperation
{
  CLIPBOARD_READ_BEGIN,
  CLIPBOARD_READ_CHUNK,
  CLIPBOARD_READ_END,
};

enum ClipboardReadCallResult
{
  CLIPBOARD_READ_CALL_STALE,
  CLIPBOARD_READ_CALL_ACCEPTED,
  CLIPBOARD_READ_CALL_BLOCKED,
  CLIPBOARD_READ_CALL_RETRY,
  CLIPBOARD_READ_CALL_FAILED,
};

/* The provider's ready callback can run after its operation has returned but
 * before this thread records BLOCKED. Keep the call in progress until the
 * result is committed under wlCb.lock so that edge is retained. */
static enum ClipboardReadCallResult clipboardReadCall(
    struct ClipboardRead * data, enum ClipboardReadOperation operation)
{
  LG_ClipboardRequest request;
  LG_ClipboardData type;
  uint64_t offset;
  size_t pending;

  LG_LOCK(wlCb.lock);
  if (wlCb.currentRead != data || data->blocked || data->calling)
  {
    LG_UNLOCK(wlCb.lock);
    return CLIPBOARD_READ_CALL_STALE;
  }

  data->calling      = true;
  data->readyPending = false;
  request = data->request;
  type    = data->type;
  offset  = data->offset;
  pending = data->pending;
  LG_UNLOCK(wlCb.lock);

  LG_ClipboardResult result;
  switch (operation)
  {
    case CLIPBOARD_READ_BEGIN:
      result = lgClipboard_dataBegin(
          request, type, LG_CLIPBOARD_SIZE_UNKNOWN);
      break;

    case CLIPBOARD_READ_CHUNK:
      result = lgClipboard_dataChunk(
          request, offset, data->buffer, pending);
      break;

    case CLIPBOARD_READ_END:
      result = lgClipboard_dataEnd(request, offset);
      break;

    default:
      result = LG_CLIPBOARD_RESULT_FAILED;
      break;
  }

  LG_LOCK(wlCb.lock);
  if (wlCb.currentRead != data)
  {
    LG_UNLOCK(wlCb.lock);
    return CLIPBOARD_READ_CALL_STALE;
  }

  data->calling = false;
  if (result == LG_CLIPBOARD_RESULT_BLOCKED)
  {
    if (data->readyPending)
    {
      data->readyPending = false;
      LG_UNLOCK(wlCb.lock);
      return CLIPBOARD_READ_CALL_RETRY;
    }

    data->blocked = true;
    waylandPollUpdate(data->fd, 0);
    LG_UNLOCK(wlCb.lock);
    return CLIPBOARD_READ_CALL_BLOCKED;
  }

  data->readyPending = false;
  if (result != LG_CLIPBOARD_RESULT_ACCEPTED)
  {
    LG_UNLOCK(wlCb.lock);
    return CLIPBOARD_READ_CALL_FAILED;
  }

  switch (operation)
  {
    case CLIPBOARD_READ_BEGIN:
      data->begun = true;
      break;

    case CLIPBOARD_READ_CHUNK:
      data->offset += data->pending;
      data->pending = 0;
      break;

    case CLIPBOARD_READ_END:
      break;
  }
  LG_UNLOCK(wlCb.lock);
  return CLIPBOARD_READ_CALL_ACCEPTED;
}

static void clipboardReadCallback(uint32_t events, void * opaque)
{
  struct ClipboardRead * data = opaque;
  LG_LOCK(wlCb.lock);
  const bool active = wlCb.currentRead == data;
  const bool busy = active && (data->blocked || data->calling);
  LG_UNLOCK(wlCb.lock);
  if (!active || busy)
    return;

  if (events & EPOLLERR)
  {
    if (clipboardReadClaim(data))
      lgClipboard_abort(data->request);
    return;
  }

  bool readAttempted = false;
  for (;;)
  {
    enum ClipboardReadOperation operation;
    bool haveOperation = true;
    int readError = 0;

    LG_LOCK(wlCb.lock);
    if (wlCb.currentRead != data || data->blocked || data->calling)
    {
      LG_UNLOCK(wlCb.lock);
      return;
    }

    if (!data->begun)
      operation = CLIPBOARD_READ_BEGIN;
    else if (data->pending)
      operation = CLIPBOARD_READ_CHUNK;
    else if (data->eof)
      operation = CLIPBOARD_READ_END;
    else if (!readAttempted)
    {
      const ssize_t result = read(
          data->fd, data->buffer, sizeof(data->buffer));
      readAttempted = true;
      if (result < 0)
      {
        readError = errno;
        haveOperation = false;
      }
      else if (result == 0)
      {
        data->eof = true;
        operation = CLIPBOARD_READ_END;
      }
      else
      {
        data->pending = (size_t)result;
        operation = CLIPBOARD_READ_CHUNK;
      }
    }
    else
      haveOperation = false;
    LG_UNLOCK(wlCb.lock);

    if (readError)
    {
      if (readError == EAGAIN || readError == EWOULDBLOCK)
        return;
      DEBUG_ERROR("Failed to read from clipboard: %s", strerror(readError));
      if (clipboardReadClaim(data))
        lgClipboard_abort(data->request);
      return;
    }
    if (!haveOperation)
      return;

    const enum ClipboardReadCallResult result =
      clipboardReadCall(data, operation);
    if (result == CLIPBOARD_READ_CALL_RETRY ||
        result == CLIPBOARD_READ_CALL_ACCEPTED)
    {
      if (operation == CLIPBOARD_READ_END &&
          result == CLIPBOARD_READ_CALL_ACCEPTED)
      {
        clipboardReadClaim(data);
        return;
      }
      continue;
    }
    if (result == CLIPBOARD_READ_CALL_FAILED)
      clipboardReadClaim(data);
    return;
  }
}

void waylandCBInvalidate(void)
{
  char * mimetypes[LG_CLIPBOARD_DATA_NONE];
  LG_LOCK(wlCb.lock);
  struct ClipboardRead * read = clipboardReadTakeCurrentNL();
  struct wl_data_offer * offer = wlCb.offer;
  wlCb.offer = NULL;
  memcpy(mimetypes, wlCb.mimetypes, sizeof(mimetypes));
  memset(wlCb.mimetypes, 0, sizeof(wlCb.mimetypes));
  LG_UNLOCK(wlCb.lock);

  if (read)
  {
    const LG_ClipboardRequest request = read->request;
    clipboardReadRetire(read);
    lgClipboard_abort(request);
  }

  lgClipboard_release();

  if (offer)
    wl_data_offer_destroy(offer);
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    free(mimetypes[i]);
}

void waylandCBRequest(LG_ClipboardRequest request, LG_ClipboardData type)
{
  if (request == LG_CLIPBOARD_REQUEST_INVALID ||
      type < LG_CLIPBOARD_DATA_TEXT || type >= LG_CLIPBOARD_DATA_NONE)
    return;

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    lgClipboard_abort(request);
    return;
  }
  const int readFlags   = fcntl(fds[0], F_GETFL);
  const int readFdFlags = fcntl(fds[0], F_GETFD);
  const int writeFdFlags = fcntl(fds[1], F_GETFD);
  if (readFlags < 0 || readFdFlags < 0 || writeFdFlags < 0 ||
      fcntl(fds[0], F_SETFD, readFdFlags | FD_CLOEXEC) < 0 ||
      fcntl(fds[1], F_SETFD, writeFdFlags | FD_CLOEXEC) < 0 ||
      fcntl(fds[0], F_SETFL, readFlags | O_NONBLOCK) < 0)
  {
    DEBUG_ERROR("Failed to configure clipboard pipe: %s", strerror(errno));
    close(fds[0]);
    close(fds[1]);
    lgClipboard_abort(request);
    return;
  }

  struct ClipboardRead * data = malloc(sizeof(*data));
  if (!data)
  {
    DEBUG_ERROR("Failed to allocate memory to read clipboard");
    close(fds[0]);
    close(fds[1]);
    lgClipboard_abort(request);
    return;
  }

  *data = (struct ClipboardRead)
  {
    .fd      = fds[0],
    .request = request,
    .type    = type,
  };
  atomic_init(&data->references, 1);

  LG_LOCK(wlCb.lock);
  struct ClipboardRead * old = clipboardReadTakeCurrentNL();
  const bool available = wlCb.offer && wlCb.mimetypes[type];
  if (available)
  {
    wl_data_offer_receive(
        wlCb.offer, wlCb.mimetypes[type], fds[1]);
  }
  close(fds[1]);

  const bool registered = available &&
    waylandPollRegisterWithCleanup(data->fd,
        clipboardReadCallback, data, clipboardReadCleanup, EPOLLIN);
  if (registered)
    wlCb.currentRead = data;
  LG_UNLOCK(wlCb.lock);

  if (old)
  {
    const LG_ClipboardRequest oldRequest = old->request;
    clipboardReadRetire(old);
    lgClipboard_abort(oldRequest);
  }
  if (!registered)
  {
    if (available)
      DEBUG_ERROR("Failed to register clipboard read into epoll: %s",
          strerror(errno));
    clipboardReadCleanup(data);
    lgClipboard_abort(request);
    return;
  }
}

void waylandCBRequestReady(LG_ClipboardRequest request)
{
  struct ClipboardRead * failed = NULL;
  struct ClipboardRead * retry = NULL;
  LG_LOCK(wlCb.lock);
  struct ClipboardRead * data = wlCb.currentRead;
  if (data && data->request == request && data->calling)
    data->readyPending = true;
  else if (data && data->request == request && data->blocked)
  {
    data->blocked = false;
    if (!waylandPollUpdate(data->fd, EPOLLIN))
      failed = clipboardReadTakeCurrentNL();
    else
    {
      atomic_fetch_add_explicit(
          &data->references, 1, memory_order_relaxed);
      retry = data;
    }
  }
  LG_UNLOCK(wlCb.lock);

  if (retry)
  {
    /* A blocked operation may already own bytes in data->buffer. Retry it
     * directly: an empty pipe with an open writer has no EPOLLIN state with
     * which to wake the pending operation. The extra reference covers
     * cancellation while this non-poll dispatch is in progress. */
    clipboardReadCallback(0, retry);
    clipboardReadRelease(retry);
  }

  if (failed)
  {
    const LG_ClipboardRequest id = failed->request;
    clipboardReadRetire(failed);
    lgClipboard_abort(id);
  }
}

void waylandCBRequestCancel(LG_ClipboardRequest request,
    LG_ClipboardCancelReason reason)
{
  (void)reason;
  LG_LOCK(wlCb.lock);
  struct ClipboardRead * data = wlCb.currentRead;
  if (!data || data->request != request)
    data = NULL;
  else
    clipboardReadTakeCurrentNL();
  LG_UNLOCK(wlCb.lock);
  clipboardReadRetire(data);
}

struct ClipboardWrite
{
  LG_Lock             lock;
  int                 fd;
  LG_ClipboardRequest request;
  LG_ClipboardData    type;
  uint64_t            offset;
  size_t              pos;
  size_t              pending;
  bool                begun;
  bool                ended;
  bool                blocked;
  bool                discard;
  bool                pollOwned;
  bool                pollRegistered;
  bool                streamActive;
  bool                requesting;
  uint8_t             buffer[KVMFR_CLIPBOARD_DATA_BYTES];
};

static void clipboardWriteDestroy(struct ClipboardWrite * data)
{
  LG_LOCK_FREE(data->lock);
  free(data);
}

static void clipboardWriteRetireStream(struct ClipboardWrite * data)
{
  int fd = -1;
  bool destroy;
  LG_LOCK(data->lock);
  data->streamActive = false;
  if (data->pollRegistered)
  {
    data->pollRegistered = false;
    fd = data->fd;
  }
  destroy = !data->pollOwned && !data->requesting;
  LG_UNLOCK(data->lock);

  if (fd >= 0)
    waylandPollUnregister(fd);
  else if (destroy)
    clipboardWriteDestroy(data);
}

static LG_ClipboardResult clipboardWriteBegin(void * opaque,
    LG_ClipboardData type, uint64_t sizeHint)
{
  (void)sizeHint;
  struct ClipboardWrite * data = opaque;
  LG_LOCK(data->lock);
  if (data->begun || data->ended || !data->streamActive ||
      type != data->type)
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  data->begun = true;
  LG_UNLOCK(data->lock);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult clipboardWriteChunk(void * opaque,
    uint64_t offset, const void * buffer, size_t size)
{
  struct ClipboardWrite * data = opaque;
  LG_LOCK(data->lock);
  if (!data->begun || data->ended || !data->streamActive ||
      !buffer || !size)
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (data->pending)
  {
    data->blocked = true;
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (offset != data->offset)
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (data->discard)
  {
    data->offset += size;
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_ACCEPTED;
  }
  if (size > sizeof(data->buffer))
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  memcpy(data->buffer, buffer, size);
  data->pending = size;
  data->pos     = 0;
  if (!waylandPollUpdate(data->fd, EPOLLOUT))
  {
    data->discard = true;
    data->pending = 0;
    data->offset += size;
  }
  LG_UNLOCK(data->lock);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static LG_ClipboardResult clipboardWriteEnd(void * opaque,
    uint64_t finalSize)
{
  struct ClipboardWrite * data = opaque;
  LG_LOCK(data->lock);
  if (!data->begun || data->ended || !data->streamActive)
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }
  if (data->pending)
  {
    data->blocked = true;
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_BLOCKED;
  }
  if (finalSize != data->offset)
  {
    LG_UNLOCK(data->lock);
    return LG_CLIPBOARD_RESULT_FAILED;
  }

  data->ended = true;
  LG_UNLOCK(data->lock);
  clipboardWriteRetireStream(data);
  return LG_CLIPBOARD_RESULT_ACCEPTED;
}

static void clipboardWriteCancel(void * opaque,
    LG_ClipboardCancelReason reason)
{
  (void)reason;
  struct ClipboardWrite * data = opaque;
  clipboardWriteRetireStream(data);
}

static const LG_ClipboardStreamOps clipboardWriteStream =
{
  .begin  = clipboardWriteBegin,
  .chunk  = clipboardWriteChunk,
  .end    = clipboardWriteEnd,
  .cancel = clipboardWriteCancel,
};

static void clipboardWriteCleanup(void * opaque)
{
  struct ClipboardWrite * data = opaque;
  LG_LOCK(data->lock);
  data->pollOwned      = false;
  data->pollRegistered = false;
  close(data->fd);
  data->fd = -1;
  const bool destroy = !data->streamActive && !data->requesting;
  LG_UNLOCK(data->lock);
  if (destroy)
    clipboardWriteDestroy(data);
}

static void clipboardWriteCallback(uint32_t events, void * opaque)
{
  struct ClipboardWrite * data = opaque;
  if (events & (EPOLLERR | EPOLLHUP))
    goto error;

  LG_LOCK(data->lock);
  if (!data->pending)
  {
    waylandPollUpdate(data->fd, 0);
    LG_UNLOCK(data->lock);
    return;
  }

  const ssize_t written = write(data->fd, data->buffer + data->pos,
      data->pending - data->pos);
  if (written < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      LG_UNLOCK(data->lock);
      return;
    }
    if (errno != EPIPE)
      DEBUG_ERROR("Failed to write clipboard data: %s", strerror(errno));
    LG_UNLOCK(data->lock);
    goto error;
  }

  data->pos += written;
  if (data->pos < data->pending)
  {
    LG_UNLOCK(data->lock);
    return;
  }

  data->offset += data->pending;
  data->pending = 0;
  data->pos     = 0;
  waylandPollUpdate(data->fd, 0);
  const bool ready = data->blocked;
  data->blocked = false;
  const LG_ClipboardRequest request = data->request;
  LG_UNLOCK(data->lock);
  if (ready)
    lgClipboard_requestReady(request);
  return;

error:
  LG_LOCK(data->lock);
  data->discard = true;
  data->offset += data->pending;
  data->pending = 0;
  data->pos     = 0;
  int fd = -1;
  if (data->pollRegistered)
  {
    data->pollRegistered = false;
    fd = data->fd;
  }
  const bool errorReady = data->blocked;
  data->blocked = false;
  const LG_ClipboardRequest errorRequest = data->request;
  LG_UNLOCK(data->lock);
  if (fd >= 0)
    waylandPollUnregister(fd);
  if (errorReady)
    lgClipboard_requestReady(errorRequest);
}

static void dataSourceHandleTarget(void * data, struct wl_data_source * source,
    const char * mimetype)
{
  // Certain Wayland clients send this for copy-paste operations even though
  // it only makes sense for drag-and-drop. We just do nothing.
}

static void dataSourceHandleSend(void * data, struct wl_data_source * source,
    const char * mimetype, int fd)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  if (containsMimetype(transfer->mimetypes, mimetype))
  {
    struct ClipboardWrite * data = calloc(1, sizeof(*data));
    if (!data)
    {
      DEBUG_ERROR("Out of memory trying to allocate ClipboardWrite");
      goto error;
    }

    data->fd      = fd;
    data->request = LG_CLIPBOARD_REQUEST_INVALID;
    data->type    = transfer->type;
    LG_LOCK_INIT(data->lock);
    const int flags   = fcntl(fd, F_GETFL);
    const int fdFlags = fcntl(fd, F_GETFD);
    if (flags < 0 || fdFlags < 0 ||
        fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC) < 0 ||
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      DEBUG_ERROR("Failed to make clipboard pipe nonblocking: %s",
          strerror(errno));
      clipboardWriteDestroy(data);
      goto error;
    }
    data->pollOwned      = true;
    data->pollRegistered = true;
    data->streamActive   = true;
    if (waylandPollRegisterWithCleanup(fd,
          clipboardWriteCallback, data, clipboardWriteCleanup, 0))
    {
      LG_LOCK(data->lock);
      data->requesting = true;
      LG_UNLOCK(data->lock);
      const bool result = lgClipboard_requestStream(transfer->type,
          &clipboardWriteStream, data, &data->request);

      int retireFd = -1;
      bool destroy = false;
      LG_LOCK(data->lock);
      data->requesting = false;
      if (!result || data->request == LG_CLIPBOARD_REQUEST_INVALID ||
          !data->streamActive)
      {
        data->streamActive = false;
        if (data->pollRegistered)
        {
          data->pollRegistered = false;
          retireFd = data->fd;
        }
        destroy = !data->pollOwned;
      }
      LG_UNLOCK(data->lock);

      if (retireFd >= 0)
        waylandPollUnregister(retireFd);
      else if (destroy)
        clipboardWriteDestroy(data);
      return;
    }

    data->pollOwned      = false;
    data->pollRegistered = false;
    data->streamActive   = false;
    clipboardWriteDestroy(data);
  }

error:
  close(fd);
}

static void dataSourceHandleCancelled(void * data,
    struct wl_data_source * source)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  DEBUG_ASSERT(transfer->source == source);

  LG_LOCK(wlCb.lock);
  struct WCBTransfer ** link = &wlCb.sources;
  while (*link && *link != transfer)
    link = &(*link)->next;
  if (*link)
    *link = transfer->next;
  if (wlCb.selectionSource == source)
    wlCb.selectionSource = NULL;
  LG_UNLOCK(wlCb.lock);

  free(transfer);
  wl_data_source_destroy(source);
}

static const struct wl_data_source_listener dataSourceListener = {
  .target    = dataSourceHandleTarget,
  .send      = dataSourceHandleSend,
  .cancelled = dataSourceHandleCancelled,
};

static void waylandCBPublish(LG_ClipboardData type)
{
  struct WCBTransfer * transfer = malloc(sizeof(*transfer));
  if (!transfer)
  {
    DEBUG_ERROR("Out of memory when allocating WCBTransfer");
    return;
  }

  transfer->mimetypes = cbTypeToMimetypes(type);
  transfer->type      = type;
  transfer->next      = NULL;

  struct wl_data_source * source =
    wl_data_device_manager_create_data_source(wlWm.dataDeviceManager);
  if (!source)
  {
    DEBUG_ERROR("Failed to create clipboard data source");
    free(transfer);
    return;
  }
  transfer->source = source;
  if (wl_data_source_add_listener(source, &dataSourceListener, transfer) < 0)
  {
    DEBUG_ERROR("Failed to listen to clipboard data source");
    wl_data_source_destroy(source);
    free(transfer);
    return;
  }
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);
  wl_data_source_offer(source, wlCb.lgMimetype);

  LG_LOCK(wlCb.lock);
  if (wlCb.dataDevice)
  {
    transfer->next         = wlCb.sources;
    wlCb.sources           = transfer;
    wlCb.selectionSource   = source;
    wl_data_device_set_selection(wlCb.dataDevice, source,
      wlWm.keyboardEnterSerial);
  }
  else
  {
    wl_data_source_destroy(source);
    free(transfer);
  }
  LG_UNLOCK(wlCb.lock);
}

void waylandCBNotice(LG_ClipboardData type)
{
  waylandCBPublish(type);
}

void waylandCBRelease(void)
{
  LG_LOCK(wlCb.lock);
  if (wlCb.dataDevice && wlCb.selectionSource)
  {
    wlCb.selectionSource = NULL;
    wl_data_device_set_selection(wlCb.dataDevice, NULL,
      wlWm.keyboardEnterSerial);
  }
  LG_UNLOCK(wlCb.lock);
}

void waylandCBFree(void)
{
  char * mimetypes[LG_CLIPBOARD_DATA_NONE];
  LG_LOCK(wlCb.lock);
  struct ClipboardRead * read = clipboardReadTakeCurrentNL();
  struct wl_data_offer * offer = wlCb.offer;
  struct wl_data_offer * dndOffer = wlCb.dndOffer;
  struct wl_data_device * dataDevice = wlCb.dataDevice;
  struct WCBTransfer * sources = wlCb.sources;
  if (dataDevice && wlCb.selectionSource)
    wl_data_device_set_selection(dataDevice, NULL,
      wlWm.keyboardEnterSerial);
  wlCb.offer      = NULL;
  wlCb.dndOffer   = NULL;
  wlCb.dataDevice = NULL;
  wlCb.selectionSource = NULL;
  wlCb.sources         = NULL;
  memcpy(mimetypes, wlCb.mimetypes, sizeof(mimetypes));
  memset(wlCb.mimetypes, 0, sizeof(wlCb.mimetypes));
  LG_UNLOCK(wlCb.lock);

  clipboardReadRetire(read);
  if (offer)
    wl_data_offer_destroy(offer);
  if (dndOffer && dndOffer != offer)
    wl_data_offer_destroy(dndOffer);
  if (dataDevice)
    wl_data_device_release(dataDevice);
  while (sources)
  {
    struct WCBTransfer * next = sources->next;
    wl_data_source_destroy(sources->source);
    free(sources);
    sources = next;
  }
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    free(mimetypes[i]);

  LG_LOCK_FREE(wlCb.lock);
}
