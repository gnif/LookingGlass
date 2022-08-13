/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include "app.h"
#include "common/debug.h"

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
    waylandCBInvalidate();
    return;
  }

  struct DataOffer * extra = wl_data_offer_get_user_data(offer);
  if (!hasAnyMimetype(extra->mimetypes) || extra->isSelfCopy)
  {
    waylandCBInvalidate();
    wl_data_offer_destroy(offer);
    return;
  }

  wlCb.offer = offer;

  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    free(wlCb.mimetypes[i]);
  memcpy(wlCb.mimetypes, extra->mimetypes, sizeof(wlCb.mimetypes));

  wl_data_offer_set_user_data(offer, NULL);
  free(extra);

  int idx = 0;
  enum LG_ClipboardData types[LG_CLIPBOARD_DATA_NONE];
  for (enum LG_ClipboardData i = 0; i < LG_CLIPBOARD_DATA_NONE; ++i)
    if (wlCb.mimetypes[i])
      types[idx++] = i;

  app_clipboardNotifyTypes(types, idx);
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
  memset(&wlCb, 0, sizeof(wlCb));

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

static void clipboardReadCancel(struct ClipboardRead * data)
{
  waylandPollUnregister(data->fd);
  close(data->fd);
  free(data->buf);
  free(data);
  wlCb.currentRead = NULL;
}

static void clipboardReadCallback(uint32_t events, void * opaque)
{
  struct ClipboardRead * data = opaque;
  if (events & EPOLLERR) 
  {
    clipboardReadCancel(data);
    return;
  }

  ssize_t result = read(data->fd, data->buf + data->numRead, data->size - data->numRead);
  if (result < 0)
  {
    DEBUG_ERROR("Failed to read from clipboard: %s", strerror(errno));
    clipboardReadCancel(data);
    return;
  }

  if (result == 0)
  {
    app_clipboardNotifySize(data->type, data->numRead);
    app_clipboardData(data->type, data->buf, data->numRead);
    clipboardReadCancel(data);
    return;
  }

  data->numRead += result;
  if (data->numRead >= data->size)
  {
    data->size *= 2;
    void * nbuf = realloc(data->buf, data->size);
    if (!nbuf) {
      DEBUG_ERROR("Failed to realloc clipboard buffer: %s", strerror(errno));
      clipboardReadCancel(data);
      return;
    }

    data->buf = nbuf;
  }
}

void waylandCBInvalidate(void)
{
  if (wlCb.currentRead)
    clipboardReadCancel(wlCb.currentRead);

  app_clipboardRelease();

  if (wlCb.offer)
    wl_data_offer_destroy(wlCb.offer);
  wlCb.offer = NULL;
}

void waylandCBRequest(LG_ClipboardData type)
{
  if (!wlCb.offer || !wlCb.mimetypes[type])
  {
    app_clipboardRelease();
    return;
  }

  if (wlCb.currentRead)
    clipboardReadCancel(wlCb.currentRead);

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    abort();
  }

  wl_data_offer_receive(wlCb.offer, wlCb.mimetypes[type], fds[1]);
  close(fds[1]);

  struct ClipboardRead * data = malloc(sizeof(*data));
  if (!data)
  {
    DEBUG_ERROR("Failed to allocate memory to read clipboard");
    close(fds[0]);
    return;
  }

  data->fd      = fds[0];
  data->size    = 4096;
  data->numRead = 0;
  data->buf     = malloc(data->size);
  data->offer   = wlCb.offer;
  data->type    = type;

  if (!data->buf)
  {
    DEBUG_ERROR("Failed to allocate memory to receive clipboard data");
    close(data->fd);
    free(data);
    return;
  }

  if (!waylandPollRegister(data->fd, clipboardReadCallback, data, EPOLLIN))
  {
    DEBUG_ERROR("Failed to register clipboard read into epoll: %s", strerror(errno));
    close(data->fd);
    free(data->buf);
    free(data);
  }

  wlCb.currentRead = data;
}

struct ClipboardWrite
{
  int fd;
  size_t pos;
  struct CountedBuffer * buffer;
};

static void clipboardWriteCallback(uint32_t events, void * opaque)
{
  struct ClipboardWrite * data = opaque;
  if (events & EPOLLERR)
    goto error;

  ssize_t written = write(data->fd, data->buffer->data + data->pos, data->buffer->size - data->pos);
  if (written < 0)
  {
    if (errno != EPIPE)
      DEBUG_ERROR("Failed to write clipboard data: %s", strerror(errno));
    goto error;
  }

  data->pos += written;
  if (data->pos < data->buffer->size)
    return;

error:
  waylandPollUnregister(data->fd);
  close(data->fd);
  countedBufferRelease(&data->buffer);
  free(data);
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
    struct ClipboardWrite * data = malloc(sizeof(*data));
    if (!data)
    {
      DEBUG_ERROR("Out of memory trying to allocate ClipboardWrite");
      goto error;
    }

    data->fd     = fd;
    data->pos    = 0;
    data->buffer = transfer->data;
    countedBufferAddRef(transfer->data);
    waylandPollRegister(fd, clipboardWriteCallback, data, EPOLLOUT);
    return;
  }

error:
  close(fd);
}

static void dataSourceHandleCancelled(void * data,
    struct wl_data_source * source)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  countedBufferRelease(&transfer->data);
  free(transfer);
  wl_data_source_destroy(source);
}

static const struct wl_data_source_listener dataSourceListener = {
  .target    = dataSourceHandleTarget,
  .send      = dataSourceHandleSend,
  .cancelled = dataSourceHandleCancelled,
};

static void waylandCBReplyFn(void * opaque, LG_ClipboardData type,
   	uint8_t * data, uint32_t size)
{
  struct WCBTransfer * transfer = malloc(sizeof(*transfer));
  if (!transfer)
  {
    DEBUG_ERROR("Out of memory when allocating WCBTransfer");
    return;
  }

  transfer->mimetypes = cbTypeToMimetypes(type);
  transfer->data = countedBufferNew(size);
  if (!transfer->data)
  {
    DEBUG_ERROR("Out of memory when allocating clipboard buffer");
    free(transfer);
    return;
  }
  memcpy(transfer->data->data, data, size);

  struct wl_data_source * source =
    wl_data_device_manager_create_data_source(wlWm.dataDeviceManager);
  wl_data_source_add_listener(source, &dataSourceListener, transfer);
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);
  wl_data_source_offer(source, wlCb.lgMimetype);

  wl_data_device_set_selection(wlCb.dataDevice, source,
    wlWm.keyboardEnterSerial);
}

void waylandCBNotice(LG_ClipboardData type)
{
  wlCb.haveRequest = true;
  wlCb.type        = type;
  app_clipboardRequest(waylandCBReplyFn, NULL);
}

void waylandCBRelease(void)
{
  wlCb.haveRequest = false;
}
