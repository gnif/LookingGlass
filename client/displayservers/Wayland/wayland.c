/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
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

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input.h>

#include <SDL2/SDL.h>
#include <wayland-client.h>

#include "app.h"
#include "common/debug.h"

#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"

struct WaylandDSState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;

  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;

  struct wl_data_device_manager * dataDeviceManager;
  struct wl_data_device * dataDevice;

  uint32_t capabilities;

  struct wl_keyboard * keyboard;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 * keyboardInhibitManager;
  struct zwp_keyboard_shortcuts_inhibitor_v1 * keyboardInhibitor;
  uint32_t keyboardEnterSerial;

  struct wl_pointer * pointer;
  struct zwp_relative_pointer_manager_v1 * relativePointerManager;
  struct zwp_pointer_constraints_v1 * pointerConstraints;
  struct zwp_relative_pointer_v1 * relativePointer;
  struct zwp_confined_pointer_v1 * confinedPointer;

  struct zwp_idle_inhibit_manager_v1 * idleInhibitManager;
  struct zwp_idle_inhibitor_v1 * idleInhibitor;
};

struct WCBTransfer
{
  void * data;
  size_t size;
  const char ** mimetypes;
};

struct WCBState
{
  enum LG_ClipboardData stashedType;
  char * stashedMimetype;
  uint8_t * stashedContents;
  ssize_t stashedSize;
  bool isReceiving;
  bool isSelfCopy;

  bool haveRequest;
  LG_ClipboardData      type;
};

static struct WaylandDSState wm;
static struct WCBState       wcb;

// Registry-handling listeners.

static void registryGlobalHandler(void * data, struct wl_registry * registry,
    uint32_t name, const char * interface, uint32_t version)
{
  if (!strcmp(interface, wl_seat_interface.name) && !wm.seat)
    wm.seat = wl_registry_bind(wm.registry, name, &wl_seat_interface, 1);
  else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name))
    wm.relativePointerManager = wl_registry_bind(wm.registry, name,
        &zwp_relative_pointer_manager_v1_interface, 1);
  else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name))
    wm.pointerConstraints = wl_registry_bind(wm.registry, name,
        &zwp_pointer_constraints_v1_interface, 1);
  else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
    wm.keyboardInhibitManager = wl_registry_bind(wm.registry, name,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
  else if (!strcmp(interface, wl_data_device_manager_interface.name))
    wm.dataDeviceManager = wl_registry_bind(wm.registry, name,
        &wl_data_device_manager_interface, 3);
  else if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name))
    wm.idleInhibitManager = wl_registry_bind(wm.registry, name,
        &zwp_idle_inhibit_manager_v1_interface, 1);
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

// Mouse-handling listeners.

static void pointerMotionHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, wl_fixed_t sxW, wl_fixed_t syW)
{
  if (wm.relativePointer)
    return;

  int sx = wl_fixed_to_int(sxW);
  int sy = wl_fixed_to_int(syW);
  app_updateCursorPos(sx, sy);
  app_handleMouseBasic();
}

static void pointerEnterHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface, wl_fixed_t sxW,
    wl_fixed_t syW)
{
  if (wm.relativePointer)
    return;

  int sx = wl_fixed_to_int(sxW);
  int sy = wl_fixed_to_int(syW);
  app_updateCursorPos(sx, sy);
  app_handleMouseBasic();
}

static void pointerLeaveHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface)
{
  // Do nothing.
}

static void pointerAxisHandler(void * data, struct wl_pointer * pointer,
  uint32_t serial, uint32_t axis, wl_fixed_t value)
{
  int button = value > 0 ?
    5 /* SPICE_MOUSE_BUTTON_DOWN */ :
    4 /* SPICE_MOUSE_BUTTON_UP */;
  app_handleButtonPress(button);
  app_handleButtonRelease(button);
}

static int mapWaylandToSpiceButton(uint32_t button)
{
  switch (button)
  {
    case BTN_LEFT:
      return 1;  // SPICE_MOUSE_BUTTON_LEFT
    case BTN_MIDDLE:
      return 2;  // SPICE_MOUSE_BUTTON_MIDDLE
    case BTN_RIGHT:
      return 3;  // SPICE_MOUSE_BUTTON_RIGHT
    case BTN_SIDE:
      return 6;  // SPICE_MOUSE_BUTTON_SIDE
    case BTN_EXTRA:
      return 7;  // SPICE_MOUSE_BUTTON_EXTRA
  }

  return 0;  // SPICE_MOUSE_BUTTON_INVALID
}

static void pointerButtonHandler(void *data, struct wl_pointer *pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t stateW)
{
  button = mapWaylandToSpiceButton(button);

  if (stateW == WL_POINTER_BUTTON_STATE_PRESSED)
    app_handleButtonPress(button);
  else
    app_handleButtonRelease(button);
}

static const struct wl_pointer_listener pointerListener = {
  .enter = pointerEnterHandler,
  .leave = pointerLeaveHandler,
  .motion = pointerMotionHandler,
  .button = pointerButtonHandler,
  .axis = pointerAxisHandler,
};

static void waylandInhibitIdle(void)
{
  if (wm.idleInhibitManager && !wm.idleInhibitor)
    wm.idleInhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
        wm.idleInhibitManager, wm.surface);
}

static void waylandUninhibitIdle(void)
{
  if (wm.idleInhibitor)
  {
    zwp_idle_inhibitor_v1_destroy(wm.idleInhibitor);
    wm.idleInhibitor = NULL;
  }
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
  wm.keyboardEnterSerial = serial;

  uint32_t * key;
  wl_array_for_each(key, keys)
    app_handleKeyPress(*key);
}

static void keyboardLeaveHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface)
{
  // Do nothing.
}

static void keyboardKeyHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    app_handleKeyPress(key);
  else
    app_handleKeyRelease(key);
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

static void handlePointerCapability(uint32_t capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (!hasPointer && wm.pointer)
  {
    wl_pointer_destroy(wm.pointer);
    wm.pointer = NULL;
  }
  else if (hasPointer && !wm.pointer)
  {
    wm.pointer = wl_seat_get_pointer(wm.seat);
    wl_pointer_add_listener(wm.pointer, &pointerListener, NULL);
  }
}

static void handleKeyboardCapability(uint32_t capabilities)
{
  bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
  if (!hasKeyboard && wm.keyboard)
  {
    wl_keyboard_destroy(wm.keyboard);
    wm.keyboard = NULL;
  }
  else if (hasKeyboard && !wm.keyboard)
  {
    wm.keyboard = wl_seat_get_keyboard(wm.seat);
    wl_keyboard_add_listener(wm.keyboard, &keyboardListener, NULL);
  }
}

static void seatCapabilitiesHandler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
  wm.capabilities = capabilities;
  handlePointerCapability(capabilities);
  handleKeyboardCapability(capabilities);
}

static void seatNameHandler(void * data, struct wl_seat * seat,
    const char * name)
{
  // Do nothing.
}

static const struct wl_seat_listener seatListener = {
    .capabilities = seatCapabilitiesHandler,
    .name = seatNameHandler,
};

static bool waylandEarlyInit(void)
{
  if (!getenv("SDL_VIDEODRIVER"))
  {
    int err = setenv("SDL_VIDEODRIVER", "wayland", 1);
    if (err < 0)
    {
      DEBUG_ERROR("Unable to set the env variable SDL_VIDEODRIVER: %d", err);
      return false;
    }
    DEBUG_INFO("SDL_VIDEODRIVER has been set to wayland");
  }

  // Request to receive EPIPE instead of SIGPIPE when one end of a pipe
  // disconnects while a write is pending. This is useful to the Wayland
  // clipboard backend, where an arbitrary application is on the other end of
  // that pipe.
  signal(SIGPIPE, SIG_IGN);

  return true;
}

static bool waylandInit(SDL_SysWMinfo * info)
{
  memset(&wm, 0, sizeof(wm));

  wm.display = info->info.wl.display;
  wm.surface = info->info.wl.surface;
  wm.registry = wl_display_get_registry(wm.display);

  wl_registry_add_listener(wm.registry, &registryListener, NULL);
  wl_display_roundtrip(wm.display);

  if (!wm.seat || !wm.dataDeviceManager)
  {
    DEBUG_ERROR("Compositor missing a required interface, will not proceed");
    return false;
  }

  if (!wm.relativePointerManager)
    DEBUG_WARN("zwp_relative_pointer_manager_v1 not exported by compositor, "
               "mouse will not be captured");
  if (!wm.pointerConstraints)
    DEBUG_WARN("zwp_pointer_constraints_v1 not exported by compositor, mouse "
               "will not be captured");
  if (!wm.keyboardInhibitManager)
    DEBUG_WARN("zwp_keyboard_shortcuts_inhibit_manager_v1 not exported by "
               "compositor, keyboard will not be grabbed");
  if (!wm.idleInhibitManager)
    DEBUG_WARN("zwp_idle_inhibit_manager_v1 not exported by compositor, will "
               "not be able to suppress idle states");

  wl_seat_add_listener(wm.seat, &seatListener, NULL);
  wl_display_roundtrip(wm.display);

  wm.dataDevice = wl_data_device_manager_get_data_device(
      wm.dataDeviceManager, wm.seat);

  return true;
}

static void waylandStartup(void)
{
}

static void relativePointerMotionHandler(void * data,
    struct zwp_relative_pointer_v1 *pointer, uint32_t timeHi, uint32_t timeLo,
    wl_fixed_t dxW, wl_fixed_t dyW, wl_fixed_t dxUnaccelW,
    wl_fixed_t dyUnaccelW)
{
  double dx, dy;
  if (app_cursorWantsRaw())
  {
    dx = wl_fixed_to_double(dxUnaccelW);
    dy = wl_fixed_to_double(dyUnaccelW);
  }
  else
  {
    dx = wl_fixed_to_double(dxW);
    dy = wl_fixed_to_double(dyW);
  }
  app_handleMouseGrabbed(dx, dy);
}

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = relativePointerMotionHandler,
};

static void waylandGrabPointer(void)
{
  if (!wm.relativePointerManager || !wm.pointerConstraints)
    return;

  if (!wm.relativePointer)
  {
    wm.relativePointer =
      zwp_relative_pointer_manager_v1_get_relative_pointer(
        wm.relativePointerManager, wm.pointer);
    zwp_relative_pointer_v1_add_listener(wm.relativePointer,
      &relativePointerListener, NULL);
  }

  if (!wm.confinedPointer)
  {
    wm.confinedPointer = zwp_pointer_constraints_v1_confine_pointer(
        wm.pointerConstraints, wm.surface, wm.pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  }
}

static void waylandUngrabPointer(void)
{
  if (wm.relativePointer)
  {
    zwp_relative_pointer_v1_destroy(wm.relativePointer);
    wm.relativePointer = NULL;
  }

  if (wm.confinedPointer)
  {
    zwp_confined_pointer_v1_destroy(wm.confinedPointer);
    wm.confinedPointer = NULL;
  }
}

static void waylandGrabKeyboard(void)
{
  if (wm.keyboardInhibitManager && !wm.keyboardInhibitor)
  {
    wm.keyboardInhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
        wm.keyboardInhibitManager, wm.surface, wm.seat);
  }
}

static void waylandUngrabKeyboard(void)
{
  if (wm.keyboardInhibitor)
  {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(wm.keyboardInhibitor);
    wm.keyboardInhibitor = NULL;
  }
}

static void waylandWarpPointer(int x, int y, bool exiting)
{
  // This is an unsupported operation on Wayland.
}

static void waylandRealignPointer(void)
{
  app_handleMouseBasic();
}

static void waylandFree(void)
{
  waylandUngrabPointer();

  if (wm.idleInhibitManager)
  {
    waylandUninhibitIdle();
    zwp_idle_inhibit_manager_v1_destroy(wm.idleInhibitManager);
  }

  // TODO: these also need to be freed, but are currently owned by SDL.
  // wl_display_destroy(wm.display);
  // wl_surface_destroy(wm.surface);
  wl_pointer_destroy(wm.pointer);
  wl_seat_destroy(wm.seat);
  wl_registry_destroy(wm.registry);
}

static bool waylandGetProp(LG_DSProperty prop, void * ret)
{
  if (prop == LG_DS_WARP_SUPPORT)
  {
    *(bool*)ret = false;
    return true;
  }

  return false;
}

static bool waylandEventFilter(SDL_Event * event)
{
  /* prevent the default processing for the following events */
  switch(event->type)
  {
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      return true;
  }

  return false;
}

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
      (wcb.stashedType == LG_CLIPBOARD_DATA_NONE ||
       strstr(wcb.stashedMimetype, "html")))
  {
    wcb.stashedType = type;
    if (wcb.stashedMimetype)
      free(wcb.stashedMimetype);
    wcb.stashedMimetype = strdup(mimetype);
  }
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
  wcb.stashedType = LG_CLIPBOARD_DATA_NONE;
  wl_data_offer_add_listener(offer, &dataOfferListener, NULL);
}

static void dataDeviceHandleSelection(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  if (wcb.stashedType == LG_CLIPBOARD_DATA_NONE || !offer)
    return;

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    abort();
  }

  wcb.isReceiving = true;
  wcb.isSelfCopy = false;
  wl_data_offer_receive(offer, wcb.stashedMimetype, fds[1]);
  close(fds[1]);
  free(wcb.stashedMimetype);
  wcb.stashedMimetype = NULL;

  wl_display_roundtrip(wm.display);

  if (wcb.stashedContents)
  {
    free(wcb.stashedContents);
    wcb.stashedContents = NULL;
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

  wcb.stashedSize = numRead;
  wcb.stashedContents = buf;
  wcb.isReceiving = false;

  close(fds[0]);
  wl_data_offer_destroy(offer);

  if (!wcb.isSelfCopy)
    app_clipboardNotify(wcb.stashedType, 0);
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = dataDeviceHandleDataOffer,
  .selection = dataDeviceHandleSelection,
};

static void waylandCBRequest(LG_ClipboardData type)
{
  // We only notified once, so it must be this.
  assert(type == wcb.stashedType);
  app_clipboardData(wcb.stashedType, wcb.stashedContents, wcb.stashedSize);
}

static bool waylandCBInit(void)
{
  memset(&wcb, 0, sizeof(wcb));

  wcb.stashedType = LG_CLIPBOARD_DATA_NONE;
  wl_data_device_add_listener(wm.dataDevice, &dataDeviceListener, NULL);

  return true;
}

static void dataSourceHandleSend(void * data, struct wl_data_source * source,
    const char * mimetype, int fd)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  if (wcb.isReceiving)
    wcb.isSelfCopy = true;
  else if (containsMimetype(transfer->mimetypes, mimetype))
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

static void waylandCBReplyFn(void * opaque, LG_ClipboardData type,
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
    wl_data_device_manager_create_data_source(wm.dataDeviceManager);
  wl_data_source_add_listener(source, &dataSourceListener, transfer);
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);

  wl_data_device_set_selection(wm.dataDevice, source,
    wm.keyboardEnterSerial);
}

static void waylandCBNotice(LG_ClipboardData type)
{
  wcb.haveRequest = true;
  wcb.type        = type;
  app_clipboardRequest(waylandCBReplyFn, NULL);
}

static void waylandCBRelease(void)
{
  wcb.haveRequest = false;
}

struct LG_DisplayServerOps LGDS_Wayland =
{
  .subsystem      = SDL_SYSWM_WAYLAND,
  .earlyInit      = waylandEarlyInit,
  .init           = waylandInit,
  .startup        = waylandStartup,
  .free           = waylandFree,
  .getProp        = waylandGetProp,
  .eventFilter    = waylandEventFilter,
  .grabPointer    = waylandGrabPointer,
  .ungrabPointer  = waylandUngrabPointer,
  .grabKeyboard   = waylandGrabKeyboard,
  .ungrabKeyboard = waylandUngrabKeyboard,
  .warpPointer    = waylandWarpPointer,
  .realignPointer = waylandRealignPointer,
  .inhibitIdle    = waylandInhibitIdle,
  .uninhibitIdle  = waylandUninhibitIdle,

  .cbInit    = waylandCBInit,
  .cbNotice  = waylandCBNotice,
  .cbRelease = waylandCBRelease,
  .cbRequest = waylandCBRequest
};
