/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

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

#include "interface/clipboard.h"
#include "common/debug.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

struct WCBTransfer
{
  void * data;
  size_t size;
  const char ** mimetypes;
};

struct WCBState
{
  struct wl_display * display;
  struct wl_registry * registry;
  struct wl_data_device_manager * dataDeviceManager;
  struct wl_seat * seat;
  struct wl_data_device * dataDevice;

  enum LG_ClipboardData stashedType;
  char * stashedMimetype;
  uint8_t * stashedContents;
  ssize_t stashedSize;

  struct wl_keyboard * keyboard;
  uint32_t keyboardEnterSerial;
  uint32_t capabilities;

  LG_ClipboardReleaseFn releaseFn;
  LG_ClipboardRequestFn requestFn;
  LG_ClipboardNotifyFn  notifyFn;
  LG_ClipboardDataFn    dataFn;
  LG_ClipboardData      type;
};

static struct WCBState * this = NULL;

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

static const char * wayland_cb_getName()
{
  return "Wayland";
}

// Keyboard-handling listeners.

static void keyboardKeymapHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t format, int fd, uint32_t size)
{
  close(fd);
}

static void keyboardEnterHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface, struct wl_array * keys)
{
  this->keyboardEnterSerial = serial;
}

static void keyboardLeaveHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface)
{
  // Do nothing.
}

static void keyboardKeyHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  // Do nothing.
}

static void keyboardModifiersHandler(void * data,
    struct wl_keyboard * keyboard, uint32_t serial, uint32_t modsDepressed,
    uint32_t modsLatched, uint32_t modsLocked, uint32_t group)
{
  // Do nothing.
}

static const struct wl_keyboard_listener keyboardListener = {
  .keymap = keyboardKeymapHandler,
  .enter = keyboardEnterHandler,
  .leave = keyboardLeaveHandler,
  .key = keyboardKeyHandler,
  .modifiers = keyboardModifiersHandler,
};

// Seat-handling listeners.

static void seatCapabilitiesHandler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
  this->capabilities = capabilities;

  bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
  if (!hasKeyboard && this->keyboard)
  {
    wl_keyboard_destroy(this->keyboard);
    this->keyboard = NULL;
  }
  else if (hasKeyboard && !this->keyboard)
  {
    this->keyboard = wl_seat_get_keyboard(this->seat);
    wl_keyboard_add_listener(this->keyboard, &keyboardListener, NULL);
  }
}

static void seatNameHandler(void * data, struct wl_seat * seat,
    const char * name)
{
  // Do nothing.
}

static const struct wl_seat_listener seatListener = {
    .capabilities = seatCapabilitiesHandler,
    .name = seatNameHandler
};

// Registry-handling listeners.

static void registryGlobalHandler(void * data,
    struct wl_registry * registry, uint32_t name, const char * interface,
    uint32_t version)
{
  if (!strcmp(interface, wl_data_device_manager_interface.name))
    this->dataDeviceManager = wl_registry_bind(this->registry, name,
        &wl_data_device_manager_interface, 3);
  else if (!strcmp(interface, wl_seat_interface.name) && !this->seat)
    // TODO: multi-seat support.
    this->seat = wl_registry_bind(this->registry, name,
        &wl_seat_interface, 1);
}

static void registryGlobalRemoveHandler(void * data,
    struct wl_registry * registry, uint32_t name)
{
  // Do nothing.
}

static const struct wl_registry_listener registryListener = {
  .global = registryGlobalHandler,
  .global_remove = registryGlobalRemoveHandler,
};

// Destination client handlers.

static void dataHandleOffer(void * data, struct wl_data_offer * offer,
    const char * mimetype)
{
  enum LG_ClipboardData type = mimetypeToCbType(mimetype);
  // Oftentimes we'll get text/html alongside text/png, but would prefer to send
  // image/png. In general, prefer images over text content.
  if (type != LG_CLIPBOARD_DATA_NONE &&
      (this->stashedType == LG_CLIPBOARD_DATA_NONE ||
       this->stashedType == LG_CLIPBOARD_DATA_TEXT))
  {
    this->stashedType = type;
    if (this->stashedMimetype)
      free(this->stashedMimetype);
    this->stashedMimetype = strdup(mimetype);
  }
}

static const struct wl_data_offer_listener dataOfferListener = {
  .offer = dataHandleOffer,
};

static void dataDeviceHandleDataOffer(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  wl_data_offer_add_listener(offer, &dataOfferListener, NULL);
}

static void dataDeviceHandleSelection(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  if (this->stashedType == LG_CLIPBOARD_DATA_NONE || !offer)
    return;

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    abort();
  }

  wl_data_offer_receive(offer, this->stashedMimetype, fds[1]);
  close(fds[1]);
  free(this->stashedMimetype);
  this->stashedMimetype = NULL;

  wl_display_roundtrip(this->display);

  if (this->stashedContents)
  {
    free(this->stashedContents);
    this->stashedContents = NULL;
  }

  size_t size = 4096, numRead = 0;
  uint8_t * buf = (uint8_t *) malloc(size);
  while (true)
  {
    ssize_t result = read(fds[0], buf + numRead, size - numRead);
    if (result < 0)
    {
      DEBUG_ERROR("Failed to read from clipboard: %s", strerror(errno));
      abort();
    }

    if (result == 0)
    {
      buf[numRead] = 0;
      break;
    }

    numRead += result;
    if (numRead >= size)
    {
      size *= 2;
      void * nbuf = realloc(buf, size);
      if (!nbuf) {
        DEBUG_ERROR("Failed to realloc clipboard buffer: %s", strerror(errno));
        abort();
      }

      buf = nbuf;
    }
  }

  this->stashedSize = numRead;
  this->stashedContents = buf;

  close(fds[0]);
  wl_data_offer_destroy(offer);

  this->notifyFn(this->stashedType, 0);
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = dataDeviceHandleDataOffer,
  .selection = dataDeviceHandleSelection,
};

static void wayland_cb_request(LG_ClipboardData type)
{
  // We only notified once, so it must be this.
  assert(type == this->stashedType);
  this->dataFn(this->stashedType, this->stashedContents, this->stashedSize);
}

// End of Wayland handlers.

static bool wayland_cb_init(
    SDL_SysWMinfo         * wminfo,
    LG_ClipboardReleaseFn   releaseFn,
    LG_ClipboardNotifyFn    notifyFn,
    LG_ClipboardDataFn      dataFn)
{
  if (wminfo->subsystem != SDL_SYSWM_WAYLAND)
    return false;

  this = (struct WCBState *)malloc(sizeof(struct WCBState));
  memset(this, 0, sizeof(struct WCBState));

  this->releaseFn = releaseFn;
  this->notifyFn  = notifyFn;
  this->dataFn    = dataFn;
  this->display   = wminfo->info.wl.display;
  this->registry  = wl_display_get_registry(this->display);
  this->stashedType = LG_CLIPBOARD_DATA_NONE;

  // Wait for the initial set of globals to appear.
  wl_registry_add_listener(this->registry, &registryListener, NULL);
  wl_display_roundtrip(this->display);

  this->dataDevice = wl_data_device_manager_get_data_device(
      this->dataDeviceManager, this->seat);
  wl_data_device_add_listener(this->dataDevice, &dataDeviceListener, NULL);

  // Wait for the compositor to let us know of capabilities.
  wl_seat_add_listener(this->seat, &seatListener, NULL);
  wl_display_roundtrip(this->display);
  return true;
}

static void wayland_cb_free()
{
  free(this);
  this = NULL;
}

static void wayland_cb_wmevent(SDL_SysWMmsg * msg)
{
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

    size_t pos = 0;
    while (pos < transfer->size)
    {
      ssize_t written = write(fd, transfer->data + pos, transfer->size - pos);
      if (written < 0)
      {
        if (errno != EPIPE)
          DEBUG_ERROR("Failed to write clipboard data: %s", strerror(errno));
        goto error;
      }

      pos += written;
    }
  }

error:
  close(fd);
}

static void dataSourceHandleCancelled(void * data,
    struct wl_data_source * source)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  free(transfer->data);
  free(transfer);
  wl_data_source_destroy(source);
}

static const struct wl_data_source_listener dataSourceListener = {
  .send = dataSourceHandleSend,
  .cancelled = dataSourceHandleCancelled,
};

static void wayland_cb_reply_fn(void * opaque, LG_ClipboardData type,
   	uint8_t * data, uint32_t size)
{
  struct WCBTransfer * transfer = malloc(sizeof(struct WCBTransfer));
  void * dataCopy = malloc(size);
  memcpy(dataCopy, data, size);
  *transfer = (struct WCBTransfer) {
    .data = dataCopy,
    .size = size,
    .mimetypes = cbTypeToMimetypes(type),
  };

  struct wl_data_source * source =
    wl_data_device_manager_create_data_source(this->dataDeviceManager);
  wl_data_source_add_listener(source, &dataSourceListener, transfer);
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);

  wl_data_device_set_selection(this->dataDevice, source,
    this->keyboardEnterSerial);
}

static void wayland_cb_notice(LG_ClipboardRequestFn requestFn,
    LG_ClipboardData type)
{
  this->requestFn = requestFn;
  this->type      = type;

  if (!this->requestFn)
    return;

  // Won't have a keyboard enter serial if we don't have the keyboard
  // capability.
  if (!this->keyboard)
    return;

  this->requestFn(wayland_cb_reply_fn, NULL);
}

static void wayland_cb_release()
{
  this->requestFn = NULL;
}

const LG_Clipboard LGC_Wayland =
{
  .getName = wayland_cb_getName,
  .init    = wayland_cb_init,
  .free    = wayland_cb_free,
  .wmevent = wayland_cb_wmevent,
  .notice  = wayland_cb_notice,
  .release = wayland_cb_release,
  .request = wayland_cb_request,
};
