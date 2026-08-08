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

#include "input.h"
#include "kb.h"

#include <purespice.h>
#include <stddef.h>

static bool spiceSupports(void * opaque, LG_InputSupport support)
{
  return false;
}

static bool spiceKeyDown(void * opaque, int key)
{
  const uint32_t ps2 = linux_to_ps2[key];
  return !ps2 || purespice_keyDown(ps2);
}

static bool spiceKeyUp(void * opaque, int key)
{
  const uint32_t ps2 = linux_to_ps2[key];
  return !ps2 || purespice_keyUp(ps2);
}

static bool spiceKeyboardLEDs(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  const uint32_t modifiers =
    (scrollLock ? 1 /* SPICE_SCROLL_LOCK_MODIFIER */ : 0) |
    (numLock    ? 2 /* SPICE_NUM_LOCK_MODIFIER    */ : 0) |
    (capsLock   ? 4 /* SPICE_CAPS_LOCK_MODIFIER   */ : 0);

  return purespice_keyModifiers(modifiers);
}

static bool spiceMouseMotion(void * opaque, int32_t x, int32_t y)
{
  return purespice_mouseMotion(x, y);
}

static bool spiceMousePress(void * opaque, unsigned int button)
{
  if (button > 7)
    return true;

  return purespice_mousePress(button);
}

static bool spiceMouseRelease(void * opaque, unsigned int button)
{
  if (button > 7)
    return true;

  return purespice_mouseRelease(button);
}

const LG_InputOps LGI_Spice =
{
  .name          = "SPICE",
  .supports      = spiceSupports,
  .keyDown       = spiceKeyDown,
  .keyUp         = spiceKeyUp,
  .keyboardLEDs  = spiceKeyboardLEDs,
  .mouseMotion   = spiceMouseMotion,
  .mousePosition = NULL,
  .mousePress    = spiceMousePress,
  .mouseRelease  = spiceMouseRelease,
  .reset         = NULL,
};
