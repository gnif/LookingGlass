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

#ifndef _H_LG_WAYLAND_INPUT_EVENT_
#define _H_LG_WAYLAND_INPUT_EVENT_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../input.h"

typedef struct WaylandInput
{
  const LG_DSInputSink * sink;
  void                 * opaque;
  double                 scrollState;
  bool                   pointerInSurface;
  bool                   focused;
}
WaylandInput;

void wlInputInit(WaylandInput * input, const LG_DSInputSink * sink,
    void * opaque);

bool wlInputPointerEnter(WaylandInput * input, bool mainSurface);
bool wlInputPointerLeave(WaylandInput * input, bool mainSurface);
void wlInputPointerDisconnect(WaylandInput * input, bool notify);
void wlInputPointerMotion(WaylandInput * input, double x, double y);
void wlInputPointerButton(WaylandInput * input, uint32_t button,
    bool pressed);
void wlInputPointerAxis(WaylandInput * input, bool vertical, double delta);
void wlInputRelativeMotion(WaylandInput * input, bool active,
    double x, double y, double rawX, double rawY);
void wlInputRealignPointer(WaylandInput * input, double x, double y);

bool wlInputKeyboardEnter(WaylandInput * input, bool mainSurface,
    const uint32_t * keys, size_t count);
bool wlInputKeyboardLeave(WaylandInput * input, bool mainSurface);
void wlInputKeyboardKey(WaylandInput * input, uint32_t key, bool pressed,
    const char * text);
void wlInputKeyboardState(WaylandInput * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock);

#endif
