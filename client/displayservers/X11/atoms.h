/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#ifndef _H_X11DS_ATOMS_
#define _H_X11DS_ATOMS_

#define DEF_ATOMS() \
  DEF_ATOM(_NET_SUPPORTING_WM_CHECK, True) \
  DEF_ATOM(_NET_SUPPORTED, True) \
  DEF_ATOM(_NET_WM_NAME, True) \
  DEF_ATOM(_NET_REQUEST_FRAME_EXTENTS, True) \
  DEF_ATOM(_NET_FRAME_EXTENTS, True) \
  DEF_ATOM(_NET_WM_BYPASS_COMPOSITOR, False) \
  DEF_ATOM(_NET_WM_ICON, True) \
  DEF_ATOM(_NET_WM_STATE, True) \
  DEF_ATOM(_NET_WM_STATE_FULLSCREEN, True) \
  DEF_ATOM(_NET_WM_STATE_FOCUSED, True) \
  DEF_ATOM(_NET_WM_STATE_MAXIMIZED_HORZ, True) \
  DEF_ATOM(_NET_WM_STATE_MAXIMIZED_VERT, True) \
  DEF_ATOM(_NET_WM_STATE_DEMANDS_ATTENTION, True) \
  DEF_ATOM(_NET_WM_WINDOW_TYPE, True) \
  DEF_ATOM(_NET_WM_WINDOW_TYPE_NORMAL, True) \
  DEF_ATOM(_NET_WM_WINDOW_TYPE_UTILITY, True) \
  DEF_ATOM(_NET_WM_PID, True) \
  DEF_ATOM(WM_DELETE_WINDOW, True) \
  DEF_ATOM(_MOTIF_WM_HINTS, True) \
  \
  DEF_ATOM(CLIPBOARD, False) \
  DEF_ATOM(TARGETS, False) \
  DEF_ATOM(SEL_DATA, False) \
  DEF_ATOM(INCR, False)

#include <X11/Xlib.h>

#define DEF_ATOM(x, onlyIfExists) Atom x;
struct X11DSAtoms
{
  DEF_ATOMS()
};
#undef DEF_ATOM

extern struct X11DSAtoms x11atoms;

void X11AtomsInit(void);

#endif
