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

#include "interface/displayserver.h"

#include <SDL2/SDL.h>

#include "app.h"
#include "common/debug.h"

struct SDLDSState
{
  bool keyboardGrabbed;
  bool pointerGrabbed;
  bool exiting;
};

static struct SDLDSState sdl;

static void sdlInit(SDL_SysWMinfo * info)
{
  memset(&sdl, 0, sizeof(sdl));
}

static void sdlStartup(void)
{
}

static void sdlShutdown(void)
{
}

static void sdlFree(void)
{
}

static bool sdlGetProp(LG_DSProperty prop, void * ret)
{
  return false;
}

static bool sdlEventFilter(SDL_Event * event)
{
  switch(event->type)
  {
    case SDL_MOUSEMOTION:
      // stop motion events during the warp out of the window
      if (sdl.exiting)
        return true;
      break;

    case SDL_WINDOWEVENT:
    {
      switch(event->window.event)
      {
        /* after leave re-enable warp and cursor processing */
        case SDL_WINDOWEVENT_LEAVE:
          sdl.exiting = false;
          return false;
      }
      break;
    }
  }

  return false;
}

static void sdlGrabPointer(void)
{
  SDL_SetWindowGrab(app_getWindow(), SDL_TRUE);
  SDL_SetRelativeMouseMode(SDL_TRUE);
  sdl.pointerGrabbed = true;
}

static void sdlUngrabPointer(void)
{
  SDL_SetWindowGrab(app_getWindow(), SDL_FALSE);
  SDL_SetRelativeMouseMode(SDL_FALSE);
  sdl.pointerGrabbed = false;
}

static void sdlGrabKeyboard(void)
{
  if (sdl.pointerGrabbed)
    SDL_SetWindowGrab(app_getWindow(), SDL_FALSE);
  else
  {
    DEBUG_WARN("SDL does not support grabbing only the keyboard, grabbing all");
    sdl.pointerGrabbed = true;
  }

  SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
  SDL_SetWindowGrab(app_getWindow(), SDL_TRUE);
  sdl.keyboardGrabbed = true;
}

static void sdlUngrabKeyboard(void)
{
  SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "0");
  SDL_SetWindowGrab(app_getWindow(), SDL_FALSE);
  if (sdl.pointerGrabbed)
    SDL_SetWindowGrab(app_getWindow(), SDL_TRUE);
  sdl.keyboardGrabbed = false;
}

static void sdlWarpMouse(int x, int y, bool exiting)
{
  if (sdl.exiting)
    return;

  sdl.exiting = exiting;

  // if exiting turn off relative mode
  if (exiting)
    SDL_SetRelativeMouseMode(SDL_FALSE);

  // issue the warp
  SDL_WarpMouseInWindow(app_getWindow(), x, y);
}

struct LG_DisplayServerOps LGDS_SDL =
{
  .subsystem      = SDL_SYSWM_UNKNOWN,
  .init           = sdlInit,
  .startup        = sdlStartup,
  .shutdown       = sdlShutdown,
  .free           = sdlFree,
  .getProp        = sdlGetProp,
  .eventFilter    = sdlEventFilter,
  .grabPointer    = sdlGrabPointer,
  .ungrabPointer  = sdlUngrabPointer,
  .grabKeyboard   = sdlGrabKeyboard,
  .ungrabKeyboard = sdlUngrabKeyboard,
  .warpMouse      = sdlWarpMouse,

  /* SDL does not have clipboard support */
  .cbInit    = NULL,
};
