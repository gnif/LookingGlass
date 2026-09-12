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

#ifndef _H_LG_KB_
#define _H_LG_KB_

#include <stdint.h>

#include "input/keycodes.h"

#define PS2_MUTE        0xE020
#define PS2_VOLUME_UP   0xE030
#define PS2_VOLUME_DOWN 0xE02E

extern const uint32_t lg_keycode_to_ps2    [LG_KEY_MAX];
extern const uint8_t  lg_keycode_to_hid    [LG_KEY_MAX];
extern const char *   lg_keycode_to_name   [LG_KEY_MAX];
extern const char *   lg_keycode_to_display[LG_KEY_MAX];
extern const int      lg_keycode_to_imgui  [LG_KEY_MAX];

#endif
