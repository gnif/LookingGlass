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

#ifndef _H_LG_X11_INPUT_EVENT_
#define _H_LG_X11_INPUT_EVENT_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../input.h"

typedef struct X11Input
{
  const LG_DSInputSink * sink;
  void                 * opaque;
  uint32_t               buttons;
  bool                   entered;
  bool                   focused;
}
X11Input;

void x11InputInit(X11Input * input, const LG_DSInputSink * sink,
    void * opaque);

bool x11InputFocus(X11Input * input, bool focused, double x, double y,
    const uint32_t * keys, size_t count);
bool x11InputPointerEnter(X11Input * input, bool mainWindow,
    bool normal, double x, double y);
bool x11InputPointerLeave(X11Input * input, bool mainWindow,
    bool normal, bool captureMode);
void x11InputPointerMotion(X11Input * input, double x, double y);
void x11InputPointerButton(X11Input * input, unsigned int button,
    bool pressed, bool raw);
void x11InputRelativeMotion(X11Input * input,
    double x, double y, double rawX, double rawY);
void x11InputKeyboardKey(X11Input * input, unsigned int keycode,
    int minKeycode, bool pressed, bool raw, bool inputActive,
    const char * text);
void x11InputKeyboardState(X11Input * input,
    bool ctrl, bool shift, bool alt, bool super,
    bool numLock, bool capsLock, bool scrollLock);

#endif
