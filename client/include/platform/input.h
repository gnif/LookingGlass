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

#ifndef _H_LG_PLATFORM_INPUT_
#define _H_LG_PLATFORM_INPUT_

#include "interface/event_source.h"

#include <stdbool.h>

void inputPlatformEarlyInit(void);
bool inputPlatformStart(void);
void inputPlatformStop(void);
void inputPlatformFree(void);
void inputPlatformSetGrab(bool keyboard, bool pointer);
bool inputPlatformIsKeyboardExclusive(void);
bool inputPlatformIsPointerExclusive(void);
LG_DSEventSource inputPlatformGetEventSource(void);

#endif
