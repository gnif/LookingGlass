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
#include <math.h>
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

  if (!wlWm.warpSupport && !wlWm.relativePointer)
    app_handleMouseBasic();
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

  if (wlWm.warpSupport)
  {
    app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
    return;
  }

  if (wlWm.relativePointer)
    return;

  app_resyncMouseBasic();
  app_handleMouseBasic();
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

static void confinedHandler(void * data,
    struct zwp_confined_pointer_v1 * pointer)
{
  bool valid;
  LG_LOCK(wlWm.surfaceLock);
  valid = pointer == wlWm.confinedPointer;
  if (valid)
    atomic_store_explicit(&wlWm.confActive, true, memory_order_release);
  LG_UNLOCK(wlWm.surfaceLock);
  MTRACE("conf active=1 id=%u valid=%d", proxyId(pointer), valid);

  if (valid)
    app_handleGrabEvent(true);
}

static void unconfinedHandler(void * data,
    struct zwp_confined_pointer_v1 * pointer)
{
  bool valid;
  LG_LOCK(wlWm.surfaceLock);
  valid = pointer == wlWm.confinedPointer;
  if (valid)
    atomic_store_explicit(&wlWm.confActive, false, memory_order_release);
  LG_UNLOCK(wlWm.surfaceLock);
  MTRACE("conf active=0 id=%u valid=%d", proxyId(pointer), valid);

  if (valid)
    app_handleGrabEvent(false);
}

static const struct zwp_confined_pointer_v1_listener confinedListener = {
  .confined   = confinedHandler,
  .unconfined = unconfinedHandler,
};

static struct zwp_confined_pointer_v1 * createConfine(
    struct wl_region * region);
static bool queueConfSync(void);

static void confSyncHandler(void * data, struct wl_callback * callback,
    uint32_t serial)
{
  bool notify = false;
  bool valid = false;
  const uint32_t id = proxyId(callback);
  uint32_t confId = 0;
  uint64_t confSeq = 0;

  LG_LOCK(wlWm.surfaceLock);
  if (callback == wlWm.confSync)
  {
    valid = true;
    wl_callback_destroy(callback);
    wlWm.confSync = NULL;
    atomic_store_explicit(&wlWm.confActive, false,
        memory_order_release);
    notify = wlWm.inputLive;

    if (wlWm.inputLive && wlWm.confReq && wlWm.pointer &&
        wlWm.pointerConstraints && !wlWm.confinedPointer &&
        !wlWm.lockedPointer)
    {
      wlWm.confinedPointer = createConfine(NULL);
      confId = proxyId(wlWm.confinedPointer);
      confSeq = app_mouseSeq();
    }
  }
  LG_UNLOCK(wlWm.surfaceLock);

  MTRACE("conf sync id=%u serial=%u valid=%d notify=%d", id, serial,
      valid, notify);
  MLOG(confSeq, "conf req id=%u why=sync", confId);

  if (notify)
    app_handleGrabEvent(false);
}

static const struct wl_callback_listener confSyncListener = {
  .done = confSyncHandler,
};

static bool queueConfSync(void)
{
  if (wlWm.confSync)
    return true;

  wlWm.confSync = wl_display_sync(wlWm.display);
  if (!wlWm.confSync)
    return false;

  wl_callback_add_listener(wlWm.confSync, &confSyncListener, NULL);
  return true;
}

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

static struct zwp_confined_pointer_v1 * createConfine(
    struct wl_region * region)
{
  struct zwp_confined_pointer_v1 * pointer =
    zwp_pointer_constraints_v1_confine_pointer(
        wlWm.pointerConstraints, wlWm.surface, wlWm.pointer, region,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  zwp_confined_pointer_v1_add_listener(pointer, &confinedListener, NULL);
  return pointer;
}

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

static void waylandCleanUpPointer(bool notify)
{
  bool event = false;
  uint32_t lockId = 0;
  uint32_t confId = 0;
  uint64_t lockSeq = 0;
  uint64_t confSeq = 0;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    if (wlWm.confSync)
    {
      wl_callback_destroy(wlWm.confSync);
      wlWm.confSync = NULL;
    }

    atomic_store_explicit(&wlWm.lockActive, false, memory_order_release);
    atomic_store_explicit(&wlWm.confActive, false, memory_order_release);

    if (wlWm.lockedPointer)
    {
      lockId = proxyId(wlWm.lockedPointer);
      zwp_locked_pointer_v1_destroy(wlWm.lockedPointer);
      wlWm.lockedPointer = NULL;
      lockSeq = app_mouseSeq();
    }

    if (wlWm.confinedPointer)
    {
      confId = proxyId(wlWm.confinedPointer);
      zwp_confined_pointer_v1_destroy(wlWm.confinedPointer);
      wlWm.confinedPointer = NULL;
      confSeq = app_mouseSeq();
    }

    if (wlWm.relativePointer)
    {
      zwp_relative_pointer_v1_destroy(wlWm.relativePointer);
      wlWm.relativePointer = NULL;
    }

    wl_pointer_destroy(wlWm.pointer);
    wlWm.pointer = NULL;
    wlWm.pointerInSurface = false;

    event = notify && wlWm.inputLive;
  });

  MLOG(lockSeq, "lock destroy id=%u why=pointer", lockId);
  MLOG(confSeq, "conf destroy id=%u why=pointer", confId);

  if (event)
  {
    app_handleEnterEvent(false);
    app_handleGrabEvent(false);
  }
}

// Seat-handling listeners.

static void handlePointerCapability(uint32_t capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (!hasPointer && wlWm.pointer)
    waylandCleanUpPointer(true);
  else if (hasPointer && !wlWm.pointer)
  {
    wlWm.pointer = wl_seat_get_pointer(wlWm.seat);
    wl_pointer_add_listener(wlWm.pointer, &pointerListener, NULL);
    waylandSetPointer(wlWm.cursorId);

    if (wlWm.warpSupport)
    {
      wlWm.relativePointer =
        zwp_relative_pointer_manager_v1_get_relative_pointer(
          wlWm.relativePointerManager, wlWm.pointer);
      zwp_relative_pointer_v1_add_listener(wlWm.relativePointer,
        &relativePointerListener, NULL);
    }

    if (app_isCaptureMode())
      waylandCapturePointer();
    else
    {
      bool confReq;
      INTERLOCKED_SECTION(wlWm.surfaceLock,
      {
        confReq = wlWm.confReq;
      });
      if (confReq)
        waylandGrabPointer();
    }
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
      wlWm.warpSupport = false;
      return true;
    }

    DEBUG_ERROR("Compositor missing wl_seat, will not proceed");
    return false;
  }

  if (wlWm.warpSupport && (!wlWm.relativePointerManager || !wlWm.pointerConstraints))
  {
    DEBUG_WARN("Cursor warp is requested, but cannot be honoured due to lack "
               "of zwp_relative_pointer_manager_v1 or zwp_pointer_constraints_v1");
    wlWm.warpSupport = false;
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

  MTRACE("input seat=1 warp=%d relMgr=%d constraints=%d",
      wlWm.warpSupport, !!wlWm.relativePointerManager,
      !!wlWm.pointerConstraints);

  LG_LOCK(wlWm.surfaceLock);
  wlWm.inputLive = true;
  LG_UNLOCK(wlWm.surfaceLock);

  wlWm.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!wlWm.xkb)
    DEBUG_WARN("Failed to initialize xkb, keyboard input will not work");

  wl_seat_add_listener(wlWm.seat, &seatListener, NULL);
  wl_display_roundtrip(wlWm.display);

  return true;
}

void waylandInputFree(void)
{
  LG_LOCK(wlWm.surfaceLock);
  wlWm.inputLive = false;
  LG_UNLOCK(wlWm.surfaceLock);

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
  uint32_t relativeId     = 0;
  uint32_t confId         = 0;
  uint64_t confSeq        = 0;
  bool haveConf           = false;
  bool haveLock           = false;
  bool haveSync           = false;
  bool havePointer        = false;
  bool haveConstraints    = false;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    wlWm.confReq = true;

    if (wlWm.pointer && !wlWm.warpSupport && !wlWm.relativePointer &&
        wlWm.relativePointerManager)
    {
      wlWm.relativePointer =
        zwp_relative_pointer_manager_v1_get_relative_pointer(
          wlWm.relativePointerManager, wlWm.pointer);
      zwp_relative_pointer_v1_add_listener(wlWm.relativePointer,
        &relativePointerListener, NULL);
      relativeId = proxyId(wlWm.relativePointer);
    }

    if (wlWm.pointer && wlWm.pointerConstraints &&
        !wlWm.confinedPointer && !wlWm.lockedPointer && !wlWm.confSync)
    {
      wlWm.confinedPointer = createConfine(NULL);
      confId = proxyId(wlWm.confinedPointer);
      confSeq = app_mouseSeq();
    }
    haveConf        = !!wlWm.confinedPointer;
    haveLock        = !!wlWm.lockedPointer;
    haveSync        = !!wlWm.confSync;
    havePointer     = !!wlWm.pointer;
    haveConstraints = !!wlWm.pointerConstraints;
  });

  if (relativeId)
    MTRACE("rel req id=%u why=grab", relativeId);

  if (confId)
    MLOG(confSeq, "conf req id=%u why=grab", confId);
  else
    MTRACE("conf skip pointer=%d constraints=%d conf=%d lock=%d sync=%d",
        havePointer, haveConstraints, haveConf, haveLock, haveSync);
}

inline static uint32_t destroyConfine(uint64_t * traceSeq)
{
  *traceSeq = 0;

  uint32_t confId = 0;
  if (wlWm.confinedPointer)
  {
    confId = proxyId(wlWm.confinedPointer);
    zwp_confined_pointer_v1_destroy(wlWm.confinedPointer);
    wlWm.confinedPointer = NULL;
    atomic_store_explicit(&wlWm.confActive, false, memory_order_release);
    *traceSeq = app_mouseSeq();

    if (!queueConfSync())
      DEBUG_ERROR("Failed to queue pointer release sync");
  }

  return confId;
}

static void ungrabBasic(void)
{
  if (wlWm.warpSupport)
    return;

  uint32_t relativeId = 0;
  LG_LOCK(wlWm.surfaceLock);
  if (wlWm.pointer && !wlWm.relativePointer &&
      wlWm.relativePointerManager)
  {
    wlWm.relativePointer =
      zwp_relative_pointer_manager_v1_get_relative_pointer(
        wlWm.relativePointerManager, wlWm.pointer);
    zwp_relative_pointer_v1_add_listener(wlWm.relativePointer,
      &relativePointerListener, NULL);
    relativeId = proxyId(wlWm.relativePointer);
  }
  LG_UNLOCK(wlWm.surfaceLock);

  if (relativeId)
    MTRACE("rel req id=%u why=ungrab", relativeId);

  app_resyncMouseBasic();
  app_handleMouseBasic();
}

void waylandUngrabPointer(void)
{
  bool notify;
  uint64_t confSeq;
  uint32_t confId;

  LG_LOCK(wlWm.surfaceLock);
  wlWm.confReq = false;
  confId = destroyConfine(&confSeq);
  notify = !wlWm.confSync && wlWm.inputLive;
  LG_UNLOCK(wlWm.surfaceLock);

  MLOG(confSeq, "conf destroy id=%u why=ungrab", confId);
  ungrabBasic();

  if (notify)
    app_handleGrabEvent(false);
}

void waylandCapturePointer(void)
{
  if (!wlWm.warpSupport)
  {
    MTRACE("capture fallback=confine");
    waylandGrabPointer();
    return;
  }

  uint32_t confId = 0;
  uint32_t lockId = 0;
  uint64_t confSeq = 0;
  uint64_t lockSeq = 0;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    wlWm.confReq = true;
    confId = destroyConfine(&confSeq);

    if (wlWm.pointer && !wlWm.lockedPointer)
    {
      atomic_store_explicit(&wlWm.lockActive, false,
          memory_order_release);
      wlWm.lockedPointer = createLock();
      lockId = proxyId(wlWm.lockedPointer);
      lockSeq = app_mouseSeq();
    }
  });

  MLOG(confSeq, "conf destroy id=%u why=capture", confId);
  MLOG(lockSeq, "lock req id=%u why=capture", lockId);
}

void waylandUncapturePointer(void)
{
  uint32_t lockId = 0;
  uint32_t confDropId = 0;
  uint32_t confReqId = 0;
  uint64_t lockSeq = 0;
  uint64_t confDropSeq = 0;
  uint64_t confReqSeq = 0;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    if (wlWm.lockedPointer)
    {
      lockId = proxyId(wlWm.lockedPointer);
      zwp_locked_pointer_v1_destroy(wlWm.lockedPointer);
      wlWm.lockedPointer = NULL;
      atomic_store_explicit(&wlWm.lockActive, false,
          memory_order_release);
      lockSeq = app_mouseSeq();
    }

    /* we need to ungrab the pointer on the following conditions when exiting capture mode:
     *   - if warp is not supported, exit via window edge detection will never work
     *     as the cursor can not be warped out of the window when we release it.
     *   - if the format is invalid as we do not know where the guest cursor is,
     *     which also breaks edge detection.
     *   - if the user has opted to use captureInputOnly mode.
     */
    if (!wlWm.warpSupport || !app_isFormatValid() || app_isCaptureOnlyMode())
    {
      wlWm.confReq = false;
      confDropId = destroyConfine(&confDropSeq);
    }
    else
    {
      wlWm.confReq = true;
      if (wlWm.pointer && !wlWm.confSync && !wlWm.confinedPointer)
      {
        wlWm.confinedPointer = createConfine(NULL);
        confReqId = proxyId(wlWm.confinedPointer);
        confReqSeq = app_mouseSeq();
      }
    }
  });

  MLOG(lockSeq, "lock destroy id=%u why=uncapture", lockId);
  MLOG(confDropSeq, "conf destroy id=%u why=uncapture", confDropId);
  MLOG(confReqSeq, "conf req id=%u why=uncapture", confReqId);
  ungrabBasic();
}

bool waylandIsPointerGrabbed(void)
{
  return atomic_load_explicit(&wlWm.confActive, memory_order_acquire);
}

bool waylandIsPointerCaptured(void)
{
  const atomic_bool * active = wlWm.warpSupport ?
    &wlWm.lockActive : &wlWm.confActive;
  return atomic_load_explicit(active, memory_order_acquire);
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
  const int reqX = x;
  const int reqY = y;
  if (!wlWm.pointerInSurface)
  {
    MTRACE("warp drop=surface target=%d,%d exit=%d", x, y, exiting);
    return;
  }

  if (wlWm.lockedPointer)
  {
    MTRACE("warp drop=lock target=%d,%d exit=%d", x, y, exiting);
    return;
  }

  LG_LOCK(wlWm.surfaceLock);
  if (wlWm.lockedPointer)
  {
    LG_UNLOCK(wlWm.surfaceLock);
    MTRACE("warp drop=lock-race target=%d,%d exit=%d", x, y, exiting);
    return;
  }

  int width, height;
  wlWm.desktop->getSize(&width, &height);

  if (x < 0) x = 0;
  else if (x >= width) x = width - 1;
  if (y < 0) y = 0;
  else if (y >= height) y = height - 1;

  struct wl_region * region = wl_compositor_create_region(wlWm.compositor);
  wl_region_add(region, x, y, 1, 1);

  const uint32_t confId = proxyId(wlWm.confinedPointer);
  uint32_t tempId = 0;
  uint64_t warpSeq = 0;
  if (wlWm.confinedPointer)
  {
    zwp_confined_pointer_v1_set_region(wlWm.confinedPointer, region);
    wl_surface_commit(wlWm.surface);
    zwp_confined_pointer_v1_set_region(wlWm.confinedPointer, NULL);
  }
  else
  {
    struct zwp_confined_pointer_v1 * confine = createConfine(region);
    tempId = proxyId(confine);
    wl_surface_commit(wlWm.surface);
    zwp_confined_pointer_v1_destroy(confine);
  }

  wl_surface_commit(wlWm.surface);
  wl_region_destroy(region);
  warpSeq = app_mouseSeq();
  LG_UNLOCK(wlWm.surfaceLock);

  MLOG(warpSeq, "warp req=%d,%d target=%d,%d exit=%d conf=%u temp=%u",
      reqX, reqY, x, y, exiting, confId, tempId);
}

void waylandRealignPointer(void)
{
  if (!wlWm.warpSupport)
    app_resyncMouseBasic();
}

void waylandGuestPointerUpdated(double x, double y, double localX, double localY)
{
  const char * drop = NULL;
  if (!wlWm.pointer)
    drop = "pointer";
  else if (!wlWm.warpSupport)
    drop = "support";
  else if (!wlWm.pointerInSurface)
    drop = "surface";
  else if (wlWm.lockedPointer)
    drop = "lock";

  if (drop)
  {
    MTRACE("guest drop=%s guest=%.3f,%.3f local=%.3f,%.3f",
        drop, x, y, localX, localY);
    return;
  }

  MTRACE("guest warp guest=%.3f,%.3f local=%.3f,%.3f",
      x, y, localX, localY);
  waylandWarpPointer((int) round(localX), (int) round(localY), false);
}
