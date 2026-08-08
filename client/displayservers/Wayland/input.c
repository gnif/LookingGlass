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

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "app.h"
#include "common/debug.h"

const double WL_SCROLL_STEP = 15.0;
const double WL_HALF_SCROLL_STEP = WL_SCROLL_STEP / 2.0;

#define MTRACE(fmt, ...) \
  do \
  { \
    const uint64_t seq = app_mouseSeq(); \
    if (seq) \
      app_mouseTrace(__FILE__, __LINE__, __FUNCTION__, seq, \
          "wl." fmt, ##__VA_ARGS__); \
  } \
  while (0)

#define MLOG(seq, fmt, ...) \
  do \
  { \
    if (seq) \
      app_mouseTrace(__FILE__, __LINE__, __FUNCTION__, seq, \
          "wl." fmt, ##__VA_ARGS__); \
  } \
  while (0)

static uint32_t proxyId(void * proxy)
{
  return proxy ? wl_proxy_get_id(proxy) : 0;
}

// Mouse-handling listeners.

static void pointerMotionHandler(void * data, struct wl_pointer * pointer,
    uint32_t time, wl_fixed_t sxW, wl_fixed_t syW)
{
  wlMotionAbs(&wlWm.motion, wl_fixed_to_double(sxW),
      wl_fixed_to_double(syW));
  MTRACE("abs time=%u pos=%.3f,%.3f", time, wlWm.motion.x,
      wlWm.motion.y);
  app_updateCursorPos(wlWm.motion.x, wlWm.motion.y);
}

static void pointerEnterHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface, wl_fixed_t sxW,
    wl_fixed_t syW)
{
  MTRACE("enter main=%d surface=%p serial=%u pos=%.3f,%.3f",
      surface == wlWm.surface, (void *)surface, serial,
      wl_fixed_to_double(sxW), wl_fixed_to_double(syW));

  if (surface != wlWm.surface)
    return;

  wlWm.pointerInSurface = true;
  app_handleEnterEvent(true);

  wl_pointer_set_cursor(pointer, serial, wlWm.cursor, wlWm.cursorHotX, wlWm.cursorHotY);
  wlWm.pointerEnterSerial = serial;

  wlMotionAbs(&wlWm.motion, wl_fixed_to_double(sxW),
      wl_fixed_to_double(syW));
  app_updateCursorPos(wlWm.motion.x, wlWm.motion.y);
}

static void pointerLeaveHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface)
{
  MTRACE("leave main=%d surface=%p serial=%u",
      surface == wlWm.surface, (void *)surface, serial);

  if (surface != wlWm.surface)
    return;

  wlWm.pointerInSurface = false;
  app_handleEnterEvent(false);
}

static void pointerAxisHandler(void * data, struct wl_pointer * pointer,
  uint32_t serial, uint32_t axis, wl_fixed_t value)
{
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
    return;

  double delta = wl_fixed_to_double(value);

  wlWm.scrollState += delta;

  while (wlWm.scrollState > WL_HALF_SCROLL_STEP)
  {
    app_handleButtonPress(5 /* SPICE_MOUSE_BUTTON_DOWN */);
    app_handleButtonRelease(5 /* SPICE_MOUSE_BUTTON_DOWN */);
    wlWm.scrollState -= WL_SCROLL_STEP;
  }

  while (wlWm.scrollState < -WL_HALF_SCROLL_STEP)
  {
    app_handleButtonPress(4 /* SPICE_MOUSE_BUTTON_UP */);
    app_handleButtonRelease(4 /* SPICE_MOUSE_BUTTON_UP */);
    wlWm.scrollState += WL_SCROLL_STEP;
  }

  app_handleWheelMotion(delta / WL_SCROLL_STEP);
}

static int mapWaylandButton(uint32_t button)
{
  switch (button)
  {
    case BTN_LEFT:
      return 1;
    case BTN_MIDDLE:
      return 2;
    case BTN_RIGHT:
      return 3;
    case BTN_SIDE:
      return 6;
    case BTN_EXTRA:
      return 7;
    case BTN_FORWARD:
      return 8;
    case BTN_BACK:
      return 9;
    case BTN_TASK:
      return 10;
  }

  return 0;
}

static void pointerButtonHandler(void *data, struct wl_pointer *pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t stateW)
{
  button = mapWaylandButton(button);
  if (!button)
    return;

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

static void lockedHandler(void * data,
    struct zwp_locked_pointer_v1 * pointer)
{
  bool valid;
  LG_LOCK(wlWm.surfaceLock);
  valid = pointer == wlWm.lockedPointer;
  if (valid)
    atomic_store_explicit(&wlWm.lockActive, true, memory_order_release);
  LG_UNLOCK(wlWm.surfaceLock);
  MTRACE("lock active=1 id=%u valid=%d", proxyId(pointer), valid);
}

static void unlockedHandler(void * data,
    struct zwp_locked_pointer_v1 * pointer)
{
  bool valid;
  LG_LOCK(wlWm.surfaceLock);
  valid = pointer == wlWm.lockedPointer;
  if (valid)
    atomic_store_explicit(&wlWm.lockActive, false, memory_order_release);
  LG_UNLOCK(wlWm.surfaceLock);
  MTRACE("lock active=0 id=%u valid=%d", proxyId(pointer), valid);
}

static const struct zwp_locked_pointer_v1_listener lockedListener = {
  .locked   = lockedHandler,
  .unlocked = unlockedHandler,
};

static struct zwp_locked_pointer_v1 * createLock(void)
{
  struct zwp_locked_pointer_v1 * pointer =
    zwp_pointer_constraints_v1_lock_pointer(
        wlWm.pointerConstraints, wlWm.surface, wlWm.pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  zwp_locked_pointer_v1_add_listener(pointer, &lockedListener, NULL);
  return pointer;
}

static void relativePointerMotionHandler(void * data,
    struct zwp_relative_pointer_v1 *pointer, uint32_t timeHi, uint32_t timeLo,
    wl_fixed_t dxW, wl_fixed_t dyW, wl_fixed_t dxUnaccelW,
    wl_fixed_t dyUnaccelW)
{
  if (!atomic_load_explicit(&wlWm.lockActive, memory_order_acquire))
    return;

  const double dx = wl_fixed_to_double(dxW);
  const double dy = wl_fixed_to_double(dyW);
  MTRACE("rel time=%u:%u delta=%.3f,%.3f raw=%.3f,%.3f "
      "pos=%.3f,%.3f", timeHi, timeLo, dx, dy,
      wl_fixed_to_double(dxUnaccelW), wl_fixed_to_double(dyUnaccelW),
      wlWm.motion.x, wlWm.motion.y);

  app_handleMouseRelative(
      dx,
      dy,
      wl_fixed_to_double(dxUnaccelW),
      wl_fixed_to_double(dyUnaccelW));
}

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = relativePointerMotionHandler,
};

// Keyboard-handling listeners.

static void keyboardKeymapHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t format, int fd, uint32_t size)
{
  if (!wlWm.xkb)
    goto done;

  if (wlWm.keymap)
  {
    xkb_keymap_unref(wlWm.keymap);
    wlWm.keymap = NULL;
  }

  if (wlWm.xkbState)
  {
    xkb_state_unref(wlWm.xkbState);
    wlWm.xkbState = NULL;
  }

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
  {
    DEBUG_WARN("Unsupported keymap format, keyboard input will not work: %d", format);
    goto done;
  }

  char * map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to mmap keymap: %s", strerror(errno));
    goto done;
  }

  wlWm.keymap = xkb_keymap_new_from_string(wlWm.xkb, map,
    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

  if (!wlWm.keymap)
    DEBUG_WARN("Failed to load keymap, keyboard input will not work");

  munmap(map, size);

  if (wlWm.keymap)
  {
    wlWm.xkbState = xkb_state_new(wlWm.keymap);
    if (!wlWm.xkbState)
      DEBUG_WARN("Failed to create xkb_state");
  }

done:
  close(fd);
}

bool waylandGetKeyLabel(int key, char * label, size_t size)
{
  if (!wlWm.xkbState)
    return false;

  key += 8; // xkb scancode is evdev scancode + 8
  xkb_keysym_t sym = xkb_state_key_get_one_sym(wlWm.xkbState, key);
  sym = xkb_keysym_to_upper(sym);

  const uint32_t codepoint = xkb_keysym_to_utf32(sym);
  return codepoint > 0x20 && codepoint != 0x7F &&
    xkb_keysym_to_utf8(sym, label, size) > 0;
}

static void keyboardEnterHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface, struct wl_array * keys)
{
  if (surface != wlWm.surface)
    return;

  wlWm.focusedOnSurface = true;
  app_handleFocusEvent(true);
  wlWm.keyboardEnterSerial = serial;

  uint32_t * key;
  wl_array_for_each(key, keys)
    app_handleKeyPress(*key);
}

static void keyboardLeaveHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface)
{
  if (surface != wlWm.surface)
    return;

  wlWm.focusedOnSurface = false;
  waylandCBInvalidate();
  app_handleFocusEvent(false);
}

static void keyboardKeyHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  if (!wlWm.focusedOnSurface)
    return;

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    app_handleKeyPress(key);
  else
    app_handleKeyRelease(key);

  if (!wlWm.xkbState || !app_isOverlayMode() || state != WL_KEYBOARD_KEY_STATE_PRESSED)
    return;

  key += 8; // xkb scancode is evdev scancode + 8
  int size = xkb_state_key_get_utf8(wlWm.xkbState, key, NULL, 0);

  if (size <= 0)
    return;

  char buffer[size + 1];
  xkb_state_key_get_utf8(wlWm.xkbState, key, buffer, size + 1);
  app_handleKeyboardTyped(buffer);
}

static void keyboardModifiersHandler(void * data,
    struct wl_keyboard * keyboard, uint32_t serial, uint32_t modsDepressed,
    uint32_t modsLatched, uint32_t modsLocked, uint32_t group)
{
  if (!wlWm.xkbState)
    return;

  xkb_state_update_mask(wlWm.xkbState, modsDepressed, modsLatched, modsLocked, 0, 0, group);
  app_handleKeyboardModifiers(
    xkb_state_mod_name_is_active(wlWm.xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0,
    xkb_state_mod_name_is_active(wlWm.xkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0,
    xkb_state_mod_name_is_active(wlWm.xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0,
    xkb_state_mod_name_is_active(wlWm.xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0
  );

  app_handleKeyboardLEDs(
    xkb_state_led_name_is_active(wlWm.xkbState, XKB_LED_NAME_NUM) > 0,
    xkb_state_led_name_is_active(wlWm.xkbState, XKB_LED_NAME_CAPS) > 0,
    xkb_state_led_name_is_active(wlWm.xkbState, XKB_LED_NAME_SCROLL) > 0
  );
}

static const struct wl_keyboard_listener keyboardListener = {
  .keymap = keyboardKeymapHandler,
  .enter = keyboardEnterHandler,
  .leave = keyboardLeaveHandler,
  .key = keyboardKeyHandler,
  .modifiers = keyboardModifiersHandler,
};

static void ensureCapturePointer(bool requestCapture)
{
  uint32_t relativeId         = 0;
  uint32_t lockId             = 0;
  uint64_t lockSeq            = 0;
  bool     captureRequested    = false;
  bool     havePointer         = false;
  bool     haveRelativeManager = false;
  bool     haveConstraints     = false;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    if (requestCapture)
      wlWm.captureRequested = true;

    captureRequested    = wlWm.captureRequested;
    havePointer         = !!wlWm.pointer;
    haveRelativeManager = !!wlWm.relativePointerManager;
    haveConstraints     = !!wlWm.pointerConstraints;

    if (captureRequested && havePointer && haveRelativeManager &&
        haveConstraints)
    {
      if (!wlWm.relativePointer)
      {
        wlWm.relativePointer =
          zwp_relative_pointer_manager_v1_get_relative_pointer(
            wlWm.relativePointerManager, wlWm.pointer);
        zwp_relative_pointer_v1_add_listener(wlWm.relativePointer,
            &relativePointerListener, NULL);
        relativeId = proxyId(wlWm.relativePointer);
      }

      if (!wlWm.lockedPointer)
      {
        atomic_store_explicit(&wlWm.lockActive, false,
            memory_order_release);
        wlWm.lockedPointer = createLock();
        lockId = proxyId(wlWm.lockedPointer);
        lockSeq = app_mouseSeq();
      }
    }
  });

  if (relativeId)
    MTRACE("rel req id=%u why=capture", relativeId);

  MLOG(lockSeq, "lock req id=%u why=capture", lockId);
  if (captureRequested &&
      (!havePointer || !haveRelativeManager || !haveConstraints))
    MTRACE("capture skip pointer=%d relMgr=%d constraints=%d",
        havePointer, haveRelativeManager, haveConstraints);
}

static void waylandCleanUpPointer(bool notify)
{
  uint32_t lockId = 0;
  uint64_t lockSeq = 0;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    atomic_store_explicit(&wlWm.lockActive, false, memory_order_release);

    if (wlWm.lockedPointer)
    {
      lockId = proxyId(wlWm.lockedPointer);
      zwp_locked_pointer_v1_destroy(wlWm.lockedPointer);
      wlWm.lockedPointer = NULL;
      lockSeq = app_mouseSeq();
    }

    if (wlWm.relativePointer)
    {
      zwp_relative_pointer_v1_destroy(wlWm.relativePointer);
      wlWm.relativePointer = NULL;
    }

    wl_pointer_destroy(wlWm.pointer);
    wlWm.pointer = NULL;
    wlWm.pointerInSurface = false;
  });

  MLOG(lockSeq, "lock destroy id=%u why=pointer", lockId);

  if (notify)
    app_handleEnterEvent(false);
}

// Seat-handling listeners.

static void handlePointerCapability(uint32_t capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (!hasPointer && wlWm.pointer)
    waylandCleanUpPointer(true);
  else if (hasPointer && !wlWm.pointer)
  {
    LG_LOCK(wlWm.surfaceLock);
    wlWm.pointer = wl_seat_get_pointer(wlWm.seat);
    wl_pointer_add_listener(wlWm.pointer, &pointerListener, NULL);
    LG_UNLOCK(wlWm.surfaceLock);
    waylandSetPointer(wlWm.cursorId);
    ensureCapturePointer(false);
  }
}

static void handleKeyboardCapability(uint32_t capabilities)
{
  bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
  if (!hasKeyboard && wlWm.keyboard)
  {
    wl_keyboard_destroy(wlWm.keyboard);
    wlWm.keyboard = NULL;
  }
  else if (hasKeyboard && !wlWm.keyboard)
  {
    wlWm.keyboard = wl_seat_get_keyboard(wlWm.seat);
    wl_keyboard_add_listener(wlWm.keyboard, &keyboardListener, NULL);
  }
}

static void seatCapabilitiesHandler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
  wlWm.capabilities = capabilities;
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

bool waylandInputInit(bool allowNoInput)
{
  if (!wlWm.seat)
  {
    MTRACE("input seat=0 allowNone=%d", allowNoInput);
    if (allowNoInput)
    {
      DEBUG_WARN("Compositor missing wl_seat, input will be disabled");
      return true;
    }

    DEBUG_ERROR("Compositor missing wl_seat, will not proceed");
    return false;
  }

  if (!wlWm.relativePointerManager)
    DEBUG_WARN("zwp_relative_pointer_manager_v1 not exported by compositor, "
               "mouse will not be captured");

  if (!wlWm.pointerConstraints)
    DEBUG_WARN("zwp_pointer_constraints_v1 not exported by compositor, mouse "
               "will not be captured");

  if (!wlWm.keyboardInhibitManager)
    DEBUG_WARN("zwp_keyboard_shortcuts_inhibit_manager_v1 not exported by "
               "compositor, keyboard will not be grabbed");

  MTRACE("input seat=1 relMgr=%d constraints=%d",
      !!wlWm.relativePointerManager,
      !!wlWm.pointerConstraints);

  wlWm.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!wlWm.xkb)
    DEBUG_WARN("Failed to initialize xkb, keyboard input will not work");

  wl_seat_add_listener(wlWm.seat, &seatListener, NULL);
  wl_display_roundtrip(wlWm.display);

  return true;
}

void waylandInputFree(void)
{
  if (!wlWm.seat)
    return;

  if (wlWm.pointer)
    waylandCleanUpPointer(false);

  // The only legal way the keyboard can be null is if it never existed.
  // When unplugged, the compositor must have an inert object.
  if (wlWm.keyboard)
    wl_keyboard_destroy(wlWm.keyboard);

  wl_seat_destroy(wlWm.seat);

  if (wlWm.xkbState)
    xkb_state_unref(wlWm.xkbState);

  if (wlWm.keymap)
    xkb_keymap_unref(wlWm.keymap);

  if (wlWm.xkb)
    xkb_context_unref(wlWm.xkb);
}

void waylandGrabPointer(void)
{
}

void waylandUngrabPointer(void)
{
}

void waylandCapturePointer(void)
{
  ensureCapturePointer(true);
}

void waylandUncapturePointer(void)
{
  uint32_t lockId     = 0;
  uint32_t relativeId = 0;
  uint64_t lockSeq    = 0;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    wlWm.captureRequested = false;
    atomic_store_explicit(&wlWm.lockActive, false, memory_order_release);

    if (wlWm.lockedPointer)
    {
      lockId = proxyId(wlWm.lockedPointer);
      zwp_locked_pointer_v1_destroy(wlWm.lockedPointer);
      wlWm.lockedPointer = NULL;
      lockSeq = app_mouseSeq();
    }

    if (wlWm.relativePointer)
    {
      relativeId = proxyId(wlWm.relativePointer);
      zwp_relative_pointer_v1_destroy(wlWm.relativePointer);
      wlWm.relativePointer = NULL;
    }
  });

  MLOG(lockSeq, "lock destroy id=%u why=uncapture", lockId);
  if (relativeId)
    MTRACE("rel destroy id=%u why=uncapture", relativeId);
}

bool waylandIsPointerGrabbed(void)
{
  return false;
}

bool waylandIsPointerCaptured(void)
{
  return atomic_load_explicit(&wlWm.lockActive, memory_order_acquire);
}

void waylandGrabKeyboard(void)
{
  if (wlWm.seat &&
      wlWm.keyboardInhibitManager && !wlWm.keyboardInhibitor)
  {
    wlWm.keyboardInhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
        wlWm.keyboardInhibitManager, wlWm.surface, wlWm.seat);
  }
}

void waylandUngrabKeyboard(void)
{
  if (wlWm.keyboardInhibitor)
  {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(wlWm.keyboardInhibitor);
    wlWm.keyboardInhibitor = NULL;
  }
}

void waylandWarpPointer(int x, int y, bool exiting)
{
  (void)x;
  (void)y;
  (void)exiting;
}

void waylandRealignPointer(void)
{
  if (wlWm.pointerInSurface)
    app_updateCursorPos(wlWm.motion.x, wlWm.motion.y);
}

void waylandGuestPointerUpdated(double x, double y, double localX,
    double localY)
{
  (void)x;
  (void)y;
  (void)localX;
  (void)localY;
}
