/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2021 Guanzhong Chen (quantum2048@gmail.com)
Copyright (C) 2021 Tudor Brindus (contact@tbrindus.ca)
https://looking-glass.io

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "wayland.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

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

// Destination client handlers.

static void dataOfferHandleOffer(void * data, struct wl_data_offer * offer,
    const char * mimetype)
{
  enum LG_ClipboardData type = mimetypeToCbType(mimetype);
  // We almost never prefer text/html, as that's used to represent rich text.
  // Since we can't copy or paste rich text, we should instead prefer actual
  // images or plain text.
  if (type != LG_CLIPBOARD_DATA_NONE &&
      (wlCb.pendingType == LG_CLIPBOARD_DATA_NONE ||
       strstr(wlCb.pendingMimetype, "html")))
  {
    wlCb.pendingType = type;
    if (wlCb.pendingMimetype)
      free(wlCb.pendingMimetype);
    wlCb.pendingMimetype = strdup(mimetype);
  }

  if (!strcmp(mimetype, wlCb.lgMimetype))
    wlCb.isSelfCopy = true;
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
  wlCb.pendingType = LG_CLIPBOARD_DATA_NONE;
  wlCb.isSelfCopy  = false;
  wl_data_offer_add_listener(offer, &dataOfferListener, NULL);
}

static void clipboardReadCancel(struct ClipboardRead * data, bool freeBuf)
{
  waylandPollUnregister(data->fd);
  close(data->fd);
  wl_data_offer_destroy(data->offer);
  if (freeBuf)
    free(data->buf);
  free(data);
  wlCb.currentRead = NULL;
}

static void clipboardReadCallback(uint32_t events, void * opaque)
{
  struct ClipboardRead * data = opaque;
  if (events & EPOLLERR) 
  {
    clipboardReadCancel(data, true);
    return;
  }

  ssize_t result = read(data->fd, data->buf + data->numRead, data->size - data->numRead);
  if (result < 0)
  {
    DEBUG_ERROR("Failed to read from clipboard: %s", strerror(errno));
    clipboardReadCancel(data, true);
    return;
  }

  if (result == 0)
  {
    data->buf[data->numRead] = 0;
    wlCb.stashedType = data->type;
    wlCb.stashedSize = data->numRead;
    wlCb.stashedContents = data->buf;

    clipboardReadCancel(data, false);
    app_clipboardNotifyTypes(&wlCb.stashedType, 1);
    app_clipboardNotifySize(wlCb.stashedType, 0);
    return;
  }

  data->numRead += result;
  if (data->numRead >= data->size)
  {
    data->size *= 2;
    void * nbuf = realloc(data->buf, data->size);
    if (!nbuf) {
      DEBUG_ERROR("Failed to realloc clipboard buffer: %s", strerror(errno));
      clipboardReadCancel(data, true);
      return;
    }

    data->buf = nbuf;
  }
}

static void dataDeviceHandleSelection(void * opaque,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  if (wlCb.pendingType == LG_CLIPBOARD_DATA_NONE || wlCb.isSelfCopy || !offer)
    return;

  if (wlCb.currentRead)
    clipboardReadCancel(wlCb.currentRead, true);

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    abort();
  }

  wl_data_offer_receive(offer, wlCb.pendingMimetype, fds[1]);
  close(fds[1]);
  free(wlCb.pendingMimetype);
  wlCb.pendingMimetype = NULL;

  wl_display_roundtrip(wlWm.display);

  if (wlCb.stashedContents)
  {
    free(wlCb.stashedContents);
    wlCb.stashedContents = NULL;
  }

  struct ClipboardRead * data = malloc(sizeof(struct ClipboardRead));
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
  data->offer   = offer;
  data->type    = wlCb.pendingType;

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

static void dataDeviceHandleEnter(void * data, struct wl_data_device * device,
    uint32_t serial, struct wl_surface * surface, wl_fixed_t sxW, wl_fixed_t syW,
    struct wl_data_offer * offer)
{
  // Do nothing.
}

static void dataDeviceHandleMotion(void * data, struct wl_data_device * device,
    uint32_t time, wl_fixed_t sxW, wl_fixed_t syW)
{
  // Do nothing.
}

static void dataDeviceHandleLeave(void * data, struct wl_data_device * device)
{
  // Do nothing.
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = dataDeviceHandleDataOffer,
  .selection = dataDeviceHandleSelection,
  .enter = dataDeviceHandleEnter,
  .motion = dataDeviceHandleMotion,
  .leave = dataDeviceHandleLeave,
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

  wlCb.stashedType = LG_CLIPBOARD_DATA_NONE;
  wl_data_device_add_listener(wlCb.dataDevice, &dataDeviceListener, NULL);

  snprintf(wlCb.lgMimetype, sizeof(wlCb.lgMimetype),
      "application/x-looking-glass-copy;pid=%d", getpid());

  return true;
}

void waylandCBRequest(LG_ClipboardData type)
{
  // We only notified once, so it must be this.
  assert(type == wlCb.stashedType);
  app_clipboardData(wlCb.stashedType, wlCb.stashedContents, wlCb.stashedSize);
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

static void dataSourceHandleSend(void * data, struct wl_data_source * source,
    const char * mimetype, int fd)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  if (containsMimetype(transfer->mimetypes, mimetype))
  {
    // Consider making this do non-blocking sends to not stall the Wayland
    // event loop if it becomes a problem. This is "fine" in the sense that
    // wl-copy also stalls like this, but it's not necessary.
    fcntl(fd, F_SETFL, 0);

    struct ClipboardWrite * data = malloc(sizeof(struct ClipboardWrite));
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
  .send = dataSourceHandleSend,
  .cancelled = dataSourceHandleCancelled,
};

static void waylandCBReplyFn(void * opaque, LG_ClipboardData type,
   	uint8_t * data, uint32_t size)
{
  struct WCBTransfer * transfer = malloc(sizeof(struct WCBTransfer));
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
