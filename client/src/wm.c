/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2021 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "wm.h"
#include "main.h"

#include <stdbool.h>
#include <SDL2/SDL.h>

#include "common/debug.h"

struct WMState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;
};

static struct WMState g_wmState;

void wmGrabPointer()
{
  switch(g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      XGrabPointer(
        g_state.wminfo.info.x11.display,
        g_state.wminfo.info.x11.window,
        true,
        None,
        GrabModeAsync,
        GrabModeAsync,
        g_state.wminfo.info.x11.window,
        None,
        CurrentTime);
      break;

    default:
      SDL_SetWindowGrab(g_state.window, SDL_TRUE);
      break;
  }

  g_wmState.pointerGrabbed = true;
}

void wmUngrabPointer()
{
  switch(g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      XUngrabPointer(g_state.wminfo.info.x11.display, CurrentTime);
      break;

    default:
      SDL_SetWindowGrab(g_state.window, SDL_TRUE);
      break;
  }

  g_wmState.pointerGrabbed = false;
}

void wmGrabKeyboard()
{
  switch(g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      XGrabKeyboard(
        g_state.wminfo.info.x11.display,
        g_state.wminfo.info.x11.window,
        true,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime);
      break;

    default:
      if (g_wmState.pointerGrabbed)
        SDL_SetWindowGrab(g_state.window, SDL_FALSE);
      else
      {
        DEBUG_WARN("SDL does not support grabbing only the keyboard, grabbing all");
        g_wmState.pointerGrabbed = true;
      }

      SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
      SDL_SetWindowGrab(g_state.window, SDL_TRUE);
      break;
  }

  g_wmState.keyboardGrabbed = true;
}

void wmUngrabKeyboard()
{
  switch(g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      XUngrabKeyboard(g_state.wminfo.info.x11.display, CurrentTime);
      break;

    default:
      SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "0");
      SDL_SetWindowGrab(g_state.window, SDL_FALSE);
      if (g_wmState.pointerGrabbed)
        SDL_SetWindowGrab(g_state.window, SDL_TRUE);
      break;
  }

  g_wmState.keyboardGrabbed = false;
}

void wmGrabAll()
{
  wmGrabPointer();
  wmGrabKeyboard();
}

void wmUngrabAll()
{
  wmUngrabPointer();
  wmUngrabKeyboard();
}

void wmWarpMouse(int x, int y)
{
  switch(g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      XWarpPointer(
          g_state.wminfo.info.x11.display,
          None,
          g_state.wminfo.info.x11.window,
          0, 0, 0, 0,
          x, y);
      XSync(g_state.wminfo.info.x11.display, False);
      break;

    default:
      SDL_WarpMouseInWindow(g_state.window, x, y);
      break;
  }
}
