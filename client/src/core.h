/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#ifndef _H_LG_CORE_
#define _H_LG_CORE_

#include <stdbool.h>

bool core_inputEnabled(void);
void core_setCursorInView(bool enable);
void core_setGrab(bool enable);
void core_setGrabQuiet(bool enable);
bool core_warpPointer(int x, int y, bool exiting);
void core_updatePositionInfo(void);
void core_alignToGuest(void);
bool core_isValidPointerPos(int x, int y);
bool core_startCursorThread(void);
void core_stopCursorThread(void);
bool core_startFrameThread(void);
void core_stopFrameThread(void);
void core_handleGuestMouseUpdate(void);
void core_handleMouseGrabbed(double ex, double ey);
void core_handleMouseNormal(double ex, double ey);
void core_resetOverlayInputState(void);

#endif
