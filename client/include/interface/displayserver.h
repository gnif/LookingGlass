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

#ifndef _H_I_DISPLAYSERVER_
#define _H_I_DISPLAYSERVER_

#include <stdbool.h>
#include <EGL/egl.h>
#include "common/types.h"
#include "common/debug.h"

typedef enum LG_ClipboardData
{
  LG_CLIPBOARD_DATA_TEXT = 0,
  LG_CLIPBOARD_DATA_PNG,
  LG_CLIPBOARD_DATA_BMP,
  LG_CLIPBOARD_DATA_TIFF,
  LG_CLIPBOARD_DATA_JPEG,

  LG_CLIPBOARD_DATA_NONE // enum max, not a data type
}
LG_ClipboardData;

typedef enum LG_DSProperty
{
  /**
   * returns the maximum number of samples supported
   * if not implemented LG assumes no multisample support
   * return data type: int
   */
  LG_DS_MAX_MULTISAMPLE,

  /**
   * returns if the platform is warp capable
   * if not implemented LG assumes that the platform is warp capable
   * return data type: bool
   */
  LG_DS_WARP_SUPPORT,
}
LG_DSProperty;

enum LG_DSWarpSupport
{
  LG_DS_WARP_NONE,
  LG_DS_WARP_SURFACE,
  LG_DS_WARP_SCREEN,
};

typedef enum LG_DSPointer
{
  LG_POINTER_NONE = 0,
  LG_POINTER_SQUARE,
  LG_POINTER_ARROW,
  LG_POINTER_INPUT,
  LG_POINTER_MOVE,
  LG_POINTER_RESIZE_NS,
  LG_POINTER_RESIZE_EW,
  LG_POINTER_RESIZE_NESW,
  LG_POINTER_RESIZE_NWSE,
  LG_POINTER_HAND,
  LG_POINTER_NOT_ALLOWED,
}
LG_DSPointer;

#define LG_POINTER_COUNT (LG_POINTER_NOT_ALLOWED + 1)

typedef struct LG_DSInitParams
{
  const char * title;
  int  x, y, w, h;
  bool center;
  bool fullscreen;
  bool resizable;
  bool borderless;
  bool maximize;

  // if true the renderer requires an OpenGL context
  bool opengl;

  // x11 needs to know if this is in use so we can decide to setup for
  // presentation times
  bool jitRender;
}
LG_DSInitParams;

typedef void (* LG_ClipboardReplyFn)(void * opaque, const LG_ClipboardData type,
    uint8_t * data, uint32_t size);

typedef struct LG_DSGLContext
  * LG_DSGLContext;

typedef struct LGEvent LGEvent;

struct LG_DisplayServerOps
{
  const char * name;

  /* called before options are parsed, useful for registering options */
  void (*setup)(void);

  /* return true if the selected ds is valid for the current platform */
  bool (*probe)(void);

  /* called before anything has been initialized */
  bool (*earlyInit)(void);

  /* called when it's time to create and show the application window */
  bool (*init)(const LG_DSInitParams params);

  /* called at startup after window creation, renderer and SPICE is ready */
  void (*startup)(void);

  /* called just before final window destruction, before final free */
  void (*shutdown)(void);

  /* final free */
  void (*free)(void);

  /*
   * return a system specific property, returns false if unsupported or failure
   * if the platform does not support/implement the requested property the value
   * of `ret` must not be altered.
   */
  bool (*getProp)(LG_DSProperty prop, void * ret);

#ifdef ENABLE_EGL
  /* EGL support */
  EGLDisplay (*getEGLDisplay)(void);
  EGLNativeWindowType (*getEGLNativeWindow)(void);
  void (*eglSwapBuffers)(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count);
#endif

#ifdef ENABLE_OPENGL
  /* opengl platform specific methods */
  LG_DSGLContext (*glCreateContext)(void);
  void (*glDeleteContext)(LG_DSGLContext context);
  void (*glMakeCurrent)(LG_DSGLContext context);
  void (*glSetSwapInterval)(int interval);
  void (*glSwapBuffers)(void);
#endif

  /* Waits for a good time to render the next frame in time for the next vblank.
   * This is optional and a display server may choose to not implement it.
   *
   * return true to force the frame to be rendered, this is used by X11 for
   * calibration */
  bool (*waitFrame)(void);

  /* This must be called when waitFrame returns, but no frame is actually rendered. */
  void (*skipFrame)(void);

  /* This is used to interrupt waitFrame. */
  void (*stopWaitFrame)(void);

  /* dm specific cursor implementations */
  void (*guestPointerUpdated)(double x, double y, double localX, double localY);
  void (*setPointer)(LG_DSPointer pointer);
  void (*grabKeyboard)(void);
  void (*ungrabKeyboard)(void);
  /* (un)grabPointer is used to toggle cursor tracking/confine in normal mode */
  void (*grabPointer)(void);
  void (*ungrabPointer)(void);
  /* (un)capturePointer is used do toggle special cursor tracking in capture mode */
  void (*capturePointer)(void);
  void (*uncapturePointer)(void);

  /* exiting = true if the warp is to leave the window */
  void (*warpPointer)(int x, int y, bool exiting);

  /* called when the client needs to realign the pointer. This should simply
   * call the appropriate app_handleMouse* method for the platform with zero
   * deltas */
  void (*realignPointer)(void);

  /* returns true if the position specified is actually valid */
  bool (*isValidPointerPos)(int x, int y);

  /* called to disable/enable the screensaver */
  void (*inhibitIdle)(void);
  void (*uninhibitIdle)(void);

  /* called to request activation */
  void (*requestActivation)(void);

  /* wait for the specified time without blocking UI processing/event loops */
  void (*wait)(unsigned int time);

  /* get/set the window dimensions & state */
  void (*setWindowSize)(int x, int y);
  bool (*getFullscreen)(void);
  void (*setFullscreen)(bool fs);
  void (*minimize)(void);

  /* clipboard support, optional, if not supported set to NULL */
  bool (*cbInit)(void);
  void (*cbNotice)(LG_ClipboardData type);
  void (*cbRelease)(void);
  void (*cbRequest)(LG_ClipboardData type);
};

#ifdef ENABLE_EGL
  #define ASSERT_EGL_FN(x) DEBUG_ASSERT(x)
#else
  #define ASSERT_EGL_FN(x)
#endif

#ifdef ENABLE_OPENGL
  #define ASSERT_OPENGL_FN(x) DEBUG_ASSERT(x)
#else
  #define ASSERT_OPENGL_FN(x)
#endif

#define ASSERT_LG_DS_VALID(x) \
  DEBUG_ASSERT((x)->setup    ); \
  DEBUG_ASSERT((x)->probe    ); \
  DEBUG_ASSERT((x)->earlyInit); \
  DEBUG_ASSERT((x)->init     ); \
  DEBUG_ASSERT((x)->startup  ); \
  DEBUG_ASSERT((x)->shutdown ); \
  DEBUG_ASSERT((x)->free     ); \
  DEBUG_ASSERT((x)->getProp  ); \
  ASSERT_EGL_FN((x)->getEGLDisplay     ); \
  ASSERT_EGL_FN((x)->getEGLNativeWindow); \
  ASSERT_EGL_FN((x)->eglSwapBuffers    ); \
  ASSERT_OPENGL_FN((x)->glCreateContext  ); \
  ASSERT_OPENGL_FN((x)->glDeleteContext  ); \
  ASSERT_OPENGL_FN((x)->glMakeCurrent    ); \
  ASSERT_OPENGL_FN((x)->glSetSwapInterval); \
  ASSERT_OPENGL_FN((x)->glSwapBuffers    ); \
  DEBUG_ASSERT(!(x)->waitFrame == !(x)->stopWaitFrame); \
  DEBUG_ASSERT((x)->guestPointerUpdated); \
  DEBUG_ASSERT((x)->setPointer         ); \
  DEBUG_ASSERT((x)->grabPointer        ); \
  DEBUG_ASSERT((x)->ungrabPointer      ); \
  DEBUG_ASSERT((x)->capturePointer     ); \
  DEBUG_ASSERT((x)->uncapturePointer   ); \
  DEBUG_ASSERT((x)->warpPointer        ); \
  DEBUG_ASSERT((x)->realignPointer     ); \
  DEBUG_ASSERT((x)->isValidPointerPos  ); \
  DEBUG_ASSERT((x)->inhibitIdle        ); \
  DEBUG_ASSERT((x)->uninhibitIdle      ); \
  DEBUG_ASSERT((x)->wait               ); \
  DEBUG_ASSERT((x)->setWindowSize      ); \
  DEBUG_ASSERT((x)->setFullscreen      ); \
  DEBUG_ASSERT((x)->getFullscreen      ); \
  DEBUG_ASSERT((x)->minimize           );

#endif
