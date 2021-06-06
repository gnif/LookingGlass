/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "interface/displayserver.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#ifdef ENABLE_EGL
#include <EGL/eglext.h>
#endif

#if defined(SDL_VIDEO_DRIVER_WAYLAND)
#include <wayland-egl.h>
#endif

#include "app.h"
#include "kb.h"
#include "egl_dynprocs.h"
#include "common/types.h"
#include "common/debug.h"
#include "util.h"

struct SDLDSState
{
  SDL_Window * window;
  SDL_Cursor * cursor;

  EGLNativeWindowType wlDisplay;

  bool keyboardGrabbed;
  bool pointerGrabbed;
  bool exiting;
};

static struct SDLDSState sdl;

/* forwards */
static int sdlEventFilter(void * userdata, SDL_Event * event);

static void sdlSetup(void)
{
}

static bool sdlProbe(void)
{
  return true;
}

static bool sdlEarlyInit(void)
{
  return true;
}

static bool sdlInit(const LG_DSInitParams params)
{
  memset(&sdl, 0, sizeof(sdl));

  // Allow screensavers for now: we will enable and disable as needed.
  SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");

  if (SDL_Init(SDL_INIT_VIDEO) < 0)
  {
    DEBUG_ERROR("SDL_Init Failed");
    return false;
  }

#ifdef ENABLE_OPENGL
  if (params.opengl)
  {
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER      , 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE          , 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE        , 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE         , 8);
  }
#endif

  sdl.window = SDL_CreateWindow(
    params.title,
    params.center ? SDL_WINDOWPOS_CENTERED : params.x,
    params.center ? SDL_WINDOWPOS_CENTERED : params.y,
    params.w,
    params.h,
    (
      SDL_WINDOW_HIDDEN |
      (params.resizable  ? SDL_WINDOW_RESIZABLE  : 0) |
      (params.borderless ? SDL_WINDOW_BORDERLESS : 0) |
      (params.maximize   ? SDL_WINDOW_MAXIMIZED  : 0) |
      (params.opengl     ? SDL_WINDOW_OPENGL     : 0)
    )
  );

  if (sdl.window == NULL)
  {
    DEBUG_ERROR("Could not create an SDL window: %s\n", SDL_GetError());
    goto fail_init;
  }

  const uint8_t data[4] = {0xf, 0x9, 0x9, 0xf};
  const uint8_t mask[4] = {0xf, 0xf, 0xf, 0xf};
  sdl.cursor = SDL_CreateCursor(data, mask, 8, 4, 4, 0);
  SDL_SetCursor(sdl.cursor);

  SDL_ShowWindow(sdl.window);

  SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

  if (params.fullscreen)
    SDL_SetWindowFullscreen(sdl.window, SDL_WINDOW_FULLSCREEN_DESKTOP);

  if (!params.center)
    SDL_SetWindowPosition(sdl.window, params.x, params.y);

  // ensure mouse acceleration is identical in server mode
  SDL_SetHintWithPriority(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "1", SDL_HINT_OVERRIDE);

  SDL_SetEventFilter(sdlEventFilter, NULL);
  return true;

fail_init:
  SDL_Quit();
  return false;
}

static void sdlStartup(void)
{
}

static void sdlShutdown(void)
{
}

static void sdlFree(void)
{
  SDL_DestroyWindow(sdl.window);

  if (sdl.cursor)
    SDL_FreeCursor(sdl.cursor);

  if (sdl.window)
    SDL_DestroyWindow(sdl.window);
  SDL_Quit();
}

static bool sdlGetProp(LG_DSProperty prop, void * ret)
{
  return false;
}

#ifdef ENABLE_EGL
static EGLDisplay sdlGetEGLDisplay(void)
{
  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);
  if (!SDL_GetWindowWMInfo(sdl.window, &wminfo))
  {
    DEBUG_ERROR("SDL_GetWindowWMInfo failed");
    return EGL_NO_DISPLAY;
  }

  EGLNativeDisplayType native;
  EGLenum platform;

  switch(wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      native   = (EGLNativeDisplayType)wminfo.info.x11.display;
      platform = EGL_PLATFORM_X11_KHR;
      break;

#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
      native   = (EGLNativeDisplayType)wminfo.info.wl.display;
      platform = EGL_PLATFORM_WAYLAND_KHR;
      break;
#endif

    default:
      DEBUG_ERROR("Unsupported subsystem");
      return EGL_NO_DISPLAY;
  }

  const char *early_exts = eglQueryString(NULL, EGL_EXTENSIONS);

  if (util_hasGLExt(early_exts, "EGL_KHR_platform_base") &&
      g_egl_dynProcs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    return g_egl_dynProcs.eglGetPlatformDisplay(platform, native, NULL);
  }

  if (util_hasGLExt(early_exts, "EGL_EXT_platform_base") &&
      g_egl_dynProcs.eglGetPlatformDisplayEXT)
  {
    DEBUG_INFO("Using eglGetPlatformDisplayEXT");
    return g_egl_dynProcs.eglGetPlatformDisplayEXT(platform, native, NULL);
  }

  DEBUG_INFO("Using eglGetDisplay");
  return eglGetDisplay(native);
}

static EGLNativeWindowType sdlGetEGLNativeWindow(void)
{
  SDL_SysWMinfo wminfo;
  SDL_VERSION(&wminfo.version);
  if (!SDL_GetWindowWMInfo(sdl.window, &wminfo))
  {
    DEBUG_ERROR("SDL_GetWindowWMInfo failed");
    return 0;
  }

  switch(wminfo.subsystem)
  {
    case SDL_SYSWM_X11:
      return (EGLNativeWindowType)wminfo.info.x11.window;

#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
    {
      if (sdl.wlDisplay)
        return sdl.wlDisplay;

      int width, height;
      SDL_GetWindowSize(sdl.window, &width, &height);
      sdl.wlDisplay = (EGLNativeWindowType)wl_egl_window_create(
          wminfo.info.wl.surface, width, height);

      return sdl.wlDisplay;
    }
#endif

    default:
      DEBUG_ERROR("Unsupported subsystem");
      return 0;
  }
}

static void sdlEGLSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count)
{
  eglSwapBuffers(display, surface);
}
#endif //ENABLE_EGL

#ifdef ENABLE_OPENGL
static LG_DSGLContext sdlGLCreateContext(void)
{
  return (LG_DSGLContext)SDL_GL_CreateContext(sdl.window);
}

static void sdlGLDeleteContext(LG_DSGLContext context)
{
  SDL_GL_DeleteContext((SDL_GLContext)context);
}

static void sdlGLMakeCurrent(LG_DSGLContext context)
{
  SDL_GL_MakeCurrent(sdl.window, (SDL_GLContext)context);
}

static void sdlGLSetSwapInterval(int interval)
{
  SDL_GL_SetSwapInterval(interval);
}

static void sdlGLSwapBuffers(void)
{
  SDL_GL_SwapWindow(sdl.window);
}
#endif //ENABLE_OPENGL

static int sdlEventFilter(void * userdata, SDL_Event * event)
{
  switch(event->type)
  {
    case SDL_QUIT:
      app_handleCloseEvent();
      break;

    case SDL_MOUSEMOTION:
      // stop motion events during the warp out of the window
      if (sdl.exiting)
        break;

      app_updateCursorPos(event->motion.x, event->motion.y);
      app_handleMouseRelative(event->motion.xrel, event->motion.yrel,
          event->motion.xrel, event->motion.yrel);
      break;

    case SDL_MOUSEBUTTONDOWN:
    {
      int button = event->button.button;
      if (button > 3)
        button += 2;

      app_handleButtonPress(button);
      break;
    }

    case SDL_MOUSEBUTTONUP:
    {
      int button = event->button.button;
      if (button > 3)
        button += 2;

      app_handleButtonRelease(button);
      break;
    }

    case SDL_MOUSEWHEEL:
    {
      int button = event->wheel.y > 0 ? 4 : 5;
      app_handleButtonPress(button);
      app_handleButtonRelease(button);
      break;
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
          app_handleEnterEvent(true);
          break;

        case SDL_WINDOWEVENT_LEAVE:
          sdl.exiting = false;
          app_handleEnterEvent(false);
          break;

        case SDL_WINDOWEVENT_FOCUS_GAINED:
          app_handleFocusEvent(true);
          break;

        case SDL_WINDOWEVENT_FOCUS_LOST:
          app_handleFocusEvent(false);
          break;

        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
        {
          struct Border border;
          SDL_GetWindowBordersSize(
            sdl.window,
            &border.top,
            &border.left,
            &border.bottom,
            &border.right
          );
          app_handleResizeEvent(
              event->window.data1,
              event->window.data2,
              1,
              border);
          break;
        }

        case SDL_WINDOWEVENT_MOVED:
          app_updateWindowPos(event->window.data1, event->window.data2);
          break;

        case SDL_WINDOWEVENT_CLOSE:
          app_handleCloseEvent();
          break;
      }
      break;
  }

  return 0;
}

static void sdlShowPointer(bool show)
{
  SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE);
}

static void sdlGrabPointer(void)
{
  SDL_SetWindowGrab(sdl.window, SDL_TRUE);
  SDL_SetRelativeMouseMode(SDL_TRUE);
  sdl.pointerGrabbed = true;
}

static void sdlUngrabPointer(void)
{
  SDL_SetWindowGrab(sdl.window, SDL_FALSE);
  SDL_SetRelativeMouseMode(SDL_FALSE);
  sdl.pointerGrabbed = false;
}

static void sdlGrabKeyboard(void)
{
  if (sdl.pointerGrabbed)
    SDL_SetWindowGrab(sdl.window, SDL_FALSE);
  else
  {
    DEBUG_WARN("SDL does not support grabbing only the keyboard, grabbing all");
    sdl.pointerGrabbed = true;
  }

  SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "1");
  SDL_SetWindowGrab(sdl.window, SDL_TRUE);
  sdl.keyboardGrabbed = true;
}

static void sdlUngrabKeyboard(void)
{
  SDL_SetHint(SDL_HINT_GRAB_KEYBOARD, "0");
  SDL_SetWindowGrab(sdl.window, SDL_FALSE);
  if (sdl.pointerGrabbed)
    SDL_SetWindowGrab(sdl.window, SDL_TRUE);
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
  SDL_WarpMouseInWindow(sdl.window, x, y);
}

static void sdlRealignPointer(void)
{
  app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
}

static bool sdlIsValidPointerPos(int x, int y)
{
  const int displays = SDL_GetNumVideoDisplays();
  for(int i = 0; i < displays; ++i)
  {
    SDL_Rect r;
    SDL_GetDisplayBounds(i, &r);
    if ((x >= r.x && x < r.x + r.w) &&
        (y >= r.y && y < r.y + r.h))
      return true;
  }
  return false;
}

static void sdlInhibitIdle(void)
{
  SDL_DisableScreenSaver();
}

static void sdlUninhibitIdle(void)
{
  SDL_EnableScreenSaver();
}

static void sdlWait(unsigned int time)
{
  SDL_WaitEventTimeout(NULL, time);
}

static void sdlSetWindowSize(int x, int y)
{
  SDL_SetWindowSize(sdl.window, x, y);
}

static void sdlSetFullscreen(bool fs)
{
  SDL_SetWindowFullscreen(sdl.window, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static bool sdlGetFullscreen(void)
{
  return (SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
}

static void sdlMinimize(void)
{
  SDL_MinimizeWindow(sdl.window);
}

struct LG_DisplayServerOps LGDS_SDL =
{
  .setup               = sdlSetup,
  .probe               = sdlProbe,
  .earlyInit           = sdlEarlyInit,
  .init                = sdlInit,
  .startup             = sdlStartup,
  .shutdown            = sdlShutdown,
  .free                = sdlFree,
  .getProp             = sdlGetProp,

#ifdef ENABLE_EGL
  .getEGLDisplay       = sdlGetEGLDisplay,
  .getEGLNativeWindow  = sdlGetEGLNativeWindow,
  .eglSwapBuffers      = sdlEGLSwapBuffers,
#endif

#ifdef ENABLE_OPENGL
  .glCreateContext     = sdlGLCreateContext,
  .glDeleteContext     = sdlGLDeleteContext,
  .glMakeCurrent       = sdlGLMakeCurrent,
  .glSetSwapInterval   = sdlGLSetSwapInterval,
  .glSwapBuffers       = sdlGLSwapBuffers,
#endif

  .showPointer         = sdlShowPointer,
  .grabPointer         = sdlGrabPointer,
  .ungrabPointer       = sdlUngrabPointer,
  .capturePointer      = sdlGrabPointer,
  .uncapturePointer    = sdlUngrabPointer,
  .grabKeyboard        = sdlGrabKeyboard,
  .ungrabKeyboard      = sdlUngrabKeyboard,
  .warpPointer         = sdlWarpPointer,
  .realignPointer      = sdlRealignPointer,
  .isValidPointerPos   = sdlIsValidPointerPos,
  .inhibitIdle         = sdlInhibitIdle,
  .uninhibitIdle       = sdlUninhibitIdle,
  .wait                = sdlWait,
  .setWindowSize       = sdlSetWindowSize,
  .setFullscreen       = sdlSetFullscreen,
  .getFullscreen       = sdlGetFullscreen,
  .minimize            = sdlMinimize,

  /* SDL does not have clipboard support */
  .cbInit    = NULL,
};
