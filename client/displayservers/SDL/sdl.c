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
#include "kb.h"
#include "common/debug.h"

struct SDLDSState
{
  bool keyboardGrabbed;
  bool pointerGrabbed;
  bool exiting;
};

static struct SDLDSState sdl;

static bool sdlEarlyInit(void)
{
  return true;
}

static bool sdlInit(SDL_SysWMinfo * info)
{
  memset(&sdl, 0, sizeof(sdl));
  return true;
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
    case SDL_QUIT:
      app_handleCloseEvent();
      return true;

    case SDL_MOUSEMOTION:
      // stop motion events during the warp out of the window
      if (sdl.exiting)
        return true;

      app_updateCursorPos(event->motion.x, event->motion.y);
      if (app_cursorIsGrabbed())
        app_handleMouseGrabbed(event->motion.xrel, event->motion.yrel);
      else
        app_handleMouseNormal(event->motion.xrel, event->motion.yrel);
      return true;

    case SDL_MOUSEBUTTONDOWN:
    {
      int button = event->button.button;
      if (button > 3)
        button += 2;

      app_handleButtonPress(button);
      return true;
    }

    case SDL_MOUSEBUTTONUP:
    {
      int button = event->button.button;
      if (button > 3)
        button += 2;

      app_handleButtonRelease(button);
      return true;
    }

    case SDL_MOUSEWHEEL:
    {
      int button = event->wheel.y > 0 ? 4 : 5;
      app_handleButtonPress(button);
      app_handleButtonRelease(button);
      return true;
    }

    case SDL_KEYDOWN:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      app_handleKeyPress(sdl_to_xfree86[sc]);
      break;
    }

    case SDL_KEYUP:
    {
      SDL_Scancode sc = event->key.keysym.scancode;
      app_handleKeyRelease(sdl_to_xfree86[sc]);
      break;
    }

    case SDL_WINDOWEVENT:
      switch(event->window.event)
      {
        case SDL_WINDOWEVENT_ENTER:
          app_handleWindowEnter();
          return true;

        case SDL_WINDOWEVENT_LEAVE:
          sdl.exiting = false;
          app_handleWindowLeave();
          return true;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
          app_handleFocusEvent(true);
          return true;

        case SDL_WINDOWEVENT_FOCUS_LOST:
          app_handleFocusEvent(false);
          return true;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
          app_handleResizeEvent(event->window.data1, event->window.data2);
          return true;

        case SDL_WINDOWEVENT_MOVED:
          app_updateWindowPos(event->window.data1, event->window.data2);
          return true;

        case SDL_WINDOWEVENT_CLOSE:
          app_handleCloseEvent();
          return true;
      }
      break;
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

static void sdlWarpPointer(int x, int y, bool exiting)
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

static void sdlRealignPointer(void)
{
  // no need to care about grab, realign only happens in normal mode
  app_handleMouseNormal(0, 0);
}

static void sdlInhibitIdle(void)
{
  SDL_DisableScreenSaver();
}

static void sdlUninhibitIdle(void)
{
  SDL_EnableScreenSaver();
}

struct LG_DisplayServerOps LGDS_SDL =
{
  .subsystem      = SDL_SYSWM_UNKNOWN,
  .earlyInit      = sdlEarlyInit,
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
  .warpPointer    = sdlWarpPointer,
  .realignPointer = sdlRealignPointer,
  .inhibitIdle    = sdlInhibitIdle,
  .uninhibitIdle  = sdlUninhibitIdle,

  /* SDL does not have clipboard support */
  .cbInit    = NULL,
};
