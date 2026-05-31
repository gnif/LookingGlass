/**
 * Looking Glass
 * Copyright © 2017-2025 The Looking Glass Authors
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

#ifndef _H_X11DS_WM_DEFAULT_
#define _H_X11DS_WM_DEFAULT_

#include "wm.h"
#include "x11.h"
#include "atoms.h"

static void wm_default_setup(void)
{
}

static bool wm_default_init(void)
{
  return true;
}

static void wm_default_deinit(void)
{
}

static void wm_default_setFullscreen(bool enable)
{
  XEvent e =
  {
    .xclient = {
      .type         = ClientMessage,
      .send_event   = true,
      .message_type = x11atoms._NET_WM_STATE,
      .format       = 32,
      .window       = x11.window,
      .data.l       = {
        enable ? _NET_WM_STATE_ADD : _NET_WM_STATE_REMOVE,
        x11atoms._NET_WM_STATE_FULLSCREEN,
        0
      }
    }
  };

  XSendEvent(x11.display, DefaultRootWindow(x11.display), False,
      SubstructureNotifyMask | SubstructureRedirectMask, &e);
};

X11WM X11WM_Default =
{
  .setup         = wm_default_setup,
  .init          = wm_default_init,
  .deinit        = wm_default_deinit,
  .setFullscreen = wm_default_setFullscreen
};

#endif
