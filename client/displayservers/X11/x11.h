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

#ifndef _H_X11DS_X11_
#define _H_X11DS_X11_

#include <stdatomic.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>

#include <GL/glx.h>

#include "interface/displayserver.h"
#include "common/thread.h"
#include "common/types.h"

struct X11DSState
{
  Display *     display;
  Window        window;
  XVisualInfo * visual;

  _Atomic(uint64_t) lastWMEvent;
  bool              invalidateAll;

  int               xpresentOp;
  bool              jitRender;
  _Atomic(uint64_t) presentMsc, presentUst;
  uint32_t          presentSerial;
  Pixmap            presentPixmap;
  XserverRegion     presentRegion;
  LGEvent *         frameEvent;

  LGThread * eventThread;

  int xinputOp;
  int pointerDev;
  int keyboardDev;
  int xValuator;
  int yValuator;

  bool pointerGrabbed;
  bool keyboardGrabbed;
  bool entered;
  bool focused;
  bool fullscreen;

  struct Rect   rect;
  struct Border border;

  Cursor cursors[LG_POINTER_COUNT];

  XIM xim;
  XIC xic;

  // XFixes vars
  int eventBase;
  int errorBase;
};

extern struct X11DSState x11;

#endif
