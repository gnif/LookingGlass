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

#ifndef _H_I_DISPLAYSERVER_
#define _H_I_DISPLAYSERVER_

#include <stdbool.h>
#include <EGL/egl.h>

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

typedef struct LG_DSInitParams
{
  const char * title;
  int  x, y, w, h;
  bool center;
  bool fullscreen;
  bool resizable;
  bool borderless;
  bool maximize;
  bool minimizeOnFocusLoss;
}
LG_DSInitParams;

typedef void (* LG_ClipboardReplyFn)(void * opaque, const LG_ClipboardData type,
    uint8_t * data, uint32_t size);

struct LG_DisplayServerOps
{
  /* return true if the selected ds is valid for the current platform */
  bool (*probe)(void);

  /* called before anything has been initialized */
  bool (*earlyInit)(void);

  /* called when it's time to create and show the application window */
  bool (*init)(const LG_DSInitParams params);

  /* called at startup after window creation, renderer and SPICE is ready */
  void (*startup)();

  /* called just before final window destruction, before final free */
  void (*shutdown)();

  /* final free */
  void (*free)();

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
#endif

  /* opengl platform specific methods */
  void (*glSwapBuffers)(void);

  /* dm specific cursor implementations */
  void (*showPointer)(bool show);
  void (*grabPointer)();
  void (*ungrabPointer)();
  void (*grabKeyboard)();
  void (*ungrabKeyboard)();

  /* exiting = true if the warp is to leave the window */
  void (*warpPointer)(int x, int y, bool exiting);

  /* called when the client needs to realign the pointer. This should simply
   * call the appropriate app_handleMouse* method for the platform with zero
   * deltas */
  void (*realignPointer)();

  /* returns true if the position specified is actually valid */
  bool (*isValidPointerPos)(int x, int y);

  /* called to disable/enable the screensaver */
  void (*inhibitIdle)();
  void (*uninhibitIdle)();

  /* wait for the specified time without blocking UI processing/event loops */
  void (*wait)(unsigned int time);

  /* set the window dimensions */
  void (*setWindowSize)(int x, int y);
  void (*setFullscreen)(bool fs);

  /* clipboard support */
  bool (*cbInit)(void);
  void (*cbNotice)(LG_ClipboardData type);
  void (*cbRelease)(void);
  void (*cbRequest)(LG_ClipboardData type);
};

#define IS_LG_DS_VALID(x) \
  ((x)->probe              && \
   (x)->earlyInit          && \
   (x)->init               && \
   (x)->startup            && \
   (x)->shutdown           && \
   (x)->free               && \
   (x)->getProp            && \
   (x)->getEGLDisplay      && \
   (x)->getEGLNativeWindow && \
   (x)->glSwapBuffers      && \
   (x)->showPointer        && \
   (x)->grabPointer        && \
   (x)->ungrabPointer      && \
   (x)->warpPointer        && \
   (x)->realignPointer     && \
   (x)->isValidPointerPos  && \
   (x)->inhibitIdle        && \
   (x)->uninhibitIdle      && \
   (x)->wait               && \
   (x)->setWindowSize      && \
   (x)->setFullscreen)
#endif
