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

#ifndef _H_LG_CLIENT_INPUT_
#define _H_LG_CLIENT_INPUT_

#include "interface/input.h"

typedef void (*LGInputKeyboardLEDsFn)(void * opaque,
    bool valid, uint8_t leds);

void lgInput_init(void);
void lgInput_free(void);

void lgInput_setFallback(const LG_InputOps * ops, void * opaque);
/* Drop functions remove a dead endpoint without invoking it. */
void lgInput_dropFallback(void);
void lgInput_setTransport(const LG_InputOps * ops, void * opaque);
void lgInput_dropTransport(void);
void lgInput_useTransport(bool enable);

bool lgInput_available(void);
bool lgInput_supports(LG_InputSupport support);
/* The listener is called synchronously with the current guest LED state and
 * later from transport threads whenever it changes. The callback must not
 * call back into the input layer. */
void lgInput_setKeyboardLEDsListener(
    LGInputKeyboardLEDsFn callback, void * opaque);

bool lgInput_keyDown(int key);
bool lgInput_keyUp(int key);
bool lgInput_keyboardLEDs(bool numLock, bool capsLock, bool scrollLock);
void lgInput_releaseKeys(void);

bool lgInput_mouseMotion(int32_t x, int32_t y);
bool lgInput_mousePosition(uint32_t x, uint32_t y, uint32_t width,
    uint32_t height);
bool lgInput_mousePress(unsigned int button);
bool lgInput_mouseRelease(unsigned int button);

#endif
