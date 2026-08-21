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

#include <stdbool.h>

/**
 * initialize configuration options
 */
void evdev_earlyInit(void);

/**
 * start the evdev layer
 */
bool evdev_start(void);

/**
 * join the evdev thread; device state stays valid for callbacks
 */
void evdev_stop(void);

/**
 * free the device state; only call once display server callbacks have stopped
 */
void evdev_free(void);

/**
 * update the keyboard and pointer capture requirements
 */
void evdev_setGrab(bool keyboard, bool pointer);

/**
 * return the event fd used to dispatch captured input
 */
int evdev_getEventFD(void);

/**
 * dispatch captured input on the display server's event thread
 */
void evdev_dispatch(void * opaque);

/**
 * return true if keyboard input should only be processed by evdev
 */
bool evdev_isExclusive(void);

/**
 * return true if pointer input should only be processed by evdev
 */
bool evdev_isPointerExclusive(void);
