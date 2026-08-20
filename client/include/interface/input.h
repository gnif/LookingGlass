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

#ifndef _H_LG_CLIENT_INPUT_INTERFACE_
#define _H_LG_CLIENT_INPUT_INTERFACE_

#include <stdbool.h>
#include <stdint.h>

typedef enum LG_InputSupport
{
  LG_INPUT_SUPPORT_MOUSE_ABSOLUTE,
}
LG_InputSupport;

enum
{
  LG_KEYBOARD_LED_NUM_LOCK    = 0x1,
  LG_KEYBOARD_LED_CAPS_LOCK   = 0x2,
  LG_KEYBOARD_LED_SCROLL_LOCK = 0x4,
  LG_KEYBOARD_LED_COMPOSE     = 0x8,
  LG_KEYBOARD_LED_KANA        = 0x10,
};

typedef struct LG_InputStatus
{
  bool     available;
  uint32_t generation;
  /* USB HID keyboard LED bits reported by the guest. */
  bool     keyboardLEDsValid;
  uint8_t  keyboardLEDs;
}
LG_InputStatus;

typedef void (*LG_InputStatusFn)(void * opaque,
    const LG_InputStatus * status);

typedef struct LG_InputOps
{
  const char * name;

  /* Operations must fail safely if a remote endpoint disappears. */
  bool (*supports)(void * opaque, LG_InputSupport support);

  /* Registration must synchronously report the current status after releasing
   * any backend locks. Passing NULL unregisters the listener. */
  void (*setStatusListener)(void * opaque, LG_InputStatusFn callback,
      void * callbackOpaque);

  /* Keyboard codes use the Linux input-event KEY_* values. */
  bool (*keyDown)(void * opaque, int key);
  bool (*keyUp)(void * opaque, int key);
  bool (*keyboardLEDs)(void * opaque, bool numLock, bool capsLock,
      bool scrollLock);

  bool (*mouseMotion)(void * opaque, int32_t x, int32_t y);
  /* Position is in guest pixels and must be within the supplied dimensions. */
  bool (*mousePosition)(void * opaque, uint32_t x, uint32_t y,
      uint32_t width, uint32_t height);
  /* Button order: left, middle, right, wheel up/down, then HID buttons 4+. */
  bool (*mousePress)(void * opaque, unsigned int button);
  bool (*mouseRelease)(void * opaque, unsigned int button);

  /* Restore all devices owned by this implementation to a neutral state.
   * This must safely tolerate the remote input endpoint becoming unavailable. */
  void (*reset)(void * opaque);
}
LG_InputOps;

#endif
