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

#ifndef _H_LG_DS_INPUT_
#define _H_LG_DS_INPUT_

#include <stdbool.h>

typedef struct LG_DSInputSink
{
  void (*position)(void * opaque, double x, double y);
  void (*relative)(void * opaque, double x, double y,
      double rawX, double rawY);
  void (*button)(void * opaque, unsigned int button, bool pressed);
  void (*wheel)(void * opaque, double motion);
  void (*key)(void * opaque, int scancode, bool pressed);
  void (*text)(void * opaque, const char * text);
  void (*modifiers)(void * opaque, bool ctrl, bool shift, bool alt,
      bool super);
  void (*leds)(void * opaque, bool numLock, bool capsLock,
      bool scrollLock);
  void (*enter)(void * opaque, bool entered);
  void (*focus)(void * opaque, bool focused);
}
LG_DSInputSink;

#endif
