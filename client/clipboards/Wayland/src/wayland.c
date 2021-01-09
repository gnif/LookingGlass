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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-client.h>

struct transfer {
  void * data;
  size_t size;
  const char ** mimetypes;
};

struct state
{
  struct wl_display * display;
  struct wl_registry * registry;
  struct wl_data_device_manager * data_device_manager;
  struct wl_seat * seat;
  struct wl_data_device * data_device;

  struct wl_keyboard * keyboard;
  uint32_t keyboard_enter_serial;
  uint32_t capabilities;

  LG_ClipboardReleaseFn releaseFn;
  LG_ClipboardRequestFn requestFn;
  LG_ClipboardNotifyFn  notifyFn;
  LG_ClipboardDataFn    dataFn;
  LG_ClipboardData      type;
};

static struct state * this = NULL;

static const char * text_mimetypes[] =
{
  "text/plain",
  "text/plain;charset=utf-8",
  "TEXT",
  "STRING",
  "UTF8_STRING",
  NULL,
};

static const char * png_mimetypes[] =
{
  "image/png",
  NULL,
};

static const char * bmp_mimetypes[] =
{
  "image/bmp",
  "image/x-bmp",
  "image/x-MS-bmp",
  "image/x-win-bitmap",
  NULL,
};

static const char * tiff_mimetypes[] =
{
  "image/tiff",
  NULL,
};

static const char * jpeg_mimetypes[] =
{
  "image/jpeg",
  NULL,
};

static const char ** cb_type_to_mimetypes(enum LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return text_mimetypes;
    case LG_CLIPBOARD_DATA_PNG:
      return png_mimetypes;
    case LG_CLIPBOARD_DATA_BMP:
      return bmp_mimetypes;
    case LG_CLIPBOARD_DATA_TIFF:
      return tiff_mimetypes;
    case LG_CLIPBOARD_DATA_JPEG:
      return jpeg_mimetypes;
    default:
      DEBUG_ERROR("invalid clipboard type");
      abort();
  }
}

static const char * wayland_cb_getName()
{
  return "Wayland";
}

// Keyboard-handling listeners.

static void keyboard_keymap_handler(
    void * data,
    struct wl_keyboard * keyboard,
    uint32_t format,
    int fd,
    uint32_t size
) {
    close(fd);
}

static void keyboard_enter_handler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface, struct wl_array * keys)
{
  this->keyboard_enter_serial = serial;
}

static void keyboard_leave_handler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface)
{
  // Do nothing.
}

static void keyboard_key_handler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  // Do nothing.
}

static void keyboard_modifiers_handler(void * data,
    struct wl_keyboard * keyboard, uint32_t serial, uint32_t mods_depressed,
    uint32_t mods_latched, uint32_t mods_locked, uint32_t group)
{
  // Do nothing.
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap_handler,
    .enter = keyboard_enter_handler,
    .leave = keyboard_leave_handler,
    .key = keyboard_key_handler,
    .modifiers = keyboard_modifiers_handler,
};

// Seat-handling listeners.

static void seat_capabilities_handler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
    this->capabilities = capabilities;
}

static void seat_name_handler(void * data, struct wl_seat * seat,
    const char * name)
{
  // Do nothing.
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities_handler,
    .name = seat_name_handler
};

// Registry-handling listeners.

static void registry_global_handler(void * data,
    struct wl_registry * wl_registry, uint32_t name, const char * interface,
    uint32_t version) {
  if (!strcmp(interface, wl_data_device_manager_interface.name))
    this->data_device_manager = wl_registry_bind(this->registry, name,
        &wl_data_device_manager_interface, 3);
  else if (!strcmp(interface, wl_seat_interface.name) && !this->seat)
    // TODO: multi-seat support.
    this->seat = wl_registry_bind(this->registry, name,
        &wl_seat_interface, 1);
}

static void registry_global_remove_handler(void * data,
    struct wl_registry * wl_registry, uint32_t name) {
  // Do nothing.
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global_handler,
  .global_remove = registry_global_remove_handler,
};

// End of Wayland handlers.

static bool wayland_cb_init(
    SDL_SysWMinfo         * wminfo,
    LG_ClipboardReleaseFn   releaseFn,
    LG_ClipboardNotifyFn    notifyFn,
    LG_ClipboardDataFn      dataFn)
{
  if (wminfo->subsystem != SDL_SYSWM_WAYLAND)
  {
    DEBUG_ERROR("wrong subsystem");
    return false;
  }

  this = (struct state *)malloc(sizeof(struct state));
  memset(this, 0, sizeof(struct state));

  this->releaseFn = releaseFn;
  this->notifyFn  = notifyFn;
  this->dataFn    = dataFn;
  this->display   = wminfo->info.wl.display;
  this->registry  = wl_display_get_registry(this->display);

  // Wait for the initial set of globals to appear.
  wl_registry_add_listener(this->registry, &registry_listener, NULL);
  wl_display_roundtrip(this->display);

  this->data_device = wl_data_device_manager_get_data_device(
      this->data_device_manager, this->seat);

  // Wait for the compositor to let us know of capabilities.
  wl_seat_add_listener(this->seat, &seat_listener, NULL);
  wl_display_roundtrip(this->display);
  if (!(this->capabilities & WL_SEAT_CAPABILITY_KEYBOARD))
  {
    // TODO: keyboard hotplug support.
    DEBUG_ERROR("no keyboard");
    return false;
  }

  this->keyboard = wl_seat_get_keyboard(this->seat);
  wl_keyboard_add_listener(this->keyboard, &keyboard_listener, NULL);
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

static bool is_valid_mimetype(struct transfer * transfer,
    const char * needle)
{
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    if (!strcmp(needle, *mimetype))
      return true;

  return false;
}

static void data_source_handle_send(void * data, struct wl_data_source * source,
    const char * mimetype, int fd) {
  struct transfer *transfer = (struct transfer *) data;
  if (is_valid_mimetype(transfer, mimetype))
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
        DEBUG_ERROR("Failed to write clipboard data: %s", strerror(errno));
        goto error;
      }

      pos += written;
    }
  }

error:
  close(fd);
}

static void data_source_handle_cancelled(void * data,
    struct wl_data_source * source) {
  struct transfer * transfer = (struct transfer *) data;
  free(transfer->data);
  free(transfer);
  wl_data_source_destroy(source);
}

static const struct wl_data_source_listener data_source_listener = {
  .send = data_source_handle_send,
  .cancelled = data_source_handle_cancelled,
};

static void wayland_cb_reply_fn(void * opaque, LG_ClipboardData type, uint8_t * data, uint32_t size)
{
  struct transfer * transfer = malloc(sizeof(struct transfer));
  void * data_copy = malloc(size);
  memcpy(data_copy, data, size);
  *transfer = (struct transfer) {
    .data = data_copy,
    .size = size,
    .mimetypes = cb_type_to_mimetypes(type),
  };

  struct wl_data_source * source =
    wl_data_device_manager_create_data_source(this->data_device_manager);
  wl_data_source_add_listener(source, &data_source_listener, transfer);
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);

  wl_data_device_set_selection(this->data_device, source,
    this->keyboard_enter_serial);
}

static void wayland_cb_notice(LG_ClipboardRequestFn requestFn, LG_ClipboardData type)
{
  this->requestFn = requestFn;
  this->type      = type;

  if (!this->requestFn)
    return;

  this->requestFn(wayland_cb_reply_fn, NULL);
}

static void wayland_cb_release()
{
  this->requestFn = NULL;
}

static void wayland_cb_request(LG_ClipboardData type)
{
}

const LG_Clipboard LGC_Wayland =
{
  .getName = wayland_cb_getName,
  .init    = wayland_cb_init,
  .free    = wayland_cb_free,
  .wmevent = wayland_cb_wmevent,
  .notice  = wayland_cb_notice,
  .release = wayland_cb_release,
  .request = wayland_cb_request
};
