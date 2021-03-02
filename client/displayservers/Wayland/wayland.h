/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2021 Guanzhong Chen (quantum2048@gmail.com)
Copyright (C) 2021 Tudor Brindus (contact@tbrindus.ca)
https://looking-glass.io

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

#include <stdbool.h>
#include <sys/types.h>

#include <wayland-client.h>

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
# include <wayland-egl.h>
# include <EGL/egl.h>
# include <EGL/eglext.h>
#endif

#include "common/locking.h"
#include "common/countedbuffer.h"
#include "interface/displayserver.h"

#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-xdg-decoration-unstable-v1-client-protocol.h"
#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"

typedef void (*WaylandPollCallback)(uint32_t events, void * opaque);

struct WaylandPoll
{
  int fd;
  bool removed;
  WaylandPollCallback callback;
  void * opaque;
  struct wl_list link;
};

struct WaylandOutput
{
  uint32_t name;
  int32_t scale;
  struct wl_output * output;
  uint32_t version;
  struct wl_list link;
};

struct SurfaceOutput
{
  struct wl_output * output;
  struct wl_list link;
};

struct WaylandDSState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;

  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;
  struct wl_shm * shm;
  struct wl_compositor * compositor;

  int32_t width, height, scale;
  bool needsResize;
  bool fullscreen;
  uint32_t resizeSerial;
  bool configured;
  bool warpSupport;
  double cursorX, cursorY;

#ifdef ENABLE_EGL
  struct wl_egl_window * eglWindow;
#endif

#ifdef ENABLE_OPENGL
  EGLDisplay glDisplay;
  EGLConfig glConfig;
  EGLSurface glSurface;
#endif

  struct xdg_wm_base * xdgWmBase;
  struct xdg_surface * xdgSurface;
  struct xdg_toplevel * xdgToplevel;
  struct zxdg_decoration_manager_v1 * xdgDecorationManager;
  struct zxdg_toplevel_decoration_v1 * xdgToplevelDecoration;

  struct wl_surface * cursor;

  struct wl_data_device_manager * dataDeviceManager;

  uint32_t capabilities;

  struct wl_keyboard * keyboard;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 * keyboardInhibitManager;
  struct zwp_keyboard_shortcuts_inhibitor_v1 * keyboardInhibitor;
  uint32_t keyboardEnterSerial;

  struct wl_pointer * pointer;
  struct zwp_relative_pointer_manager_v1 * relativePointerManager;
  struct zwp_pointer_constraints_v1 * pointerConstraints;
  struct zwp_relative_pointer_v1 * relativePointer;
  struct zwp_confined_pointer_v1 * confinedPointer;
  bool showPointer;
  uint32_t pointerEnterSerial;

  struct zwp_idle_inhibit_manager_v1 * idleInhibitManager;
  struct zwp_idle_inhibitor_v1 * idleInhibitor;

  struct wl_list outputs; // WaylandOutput::link
  struct wl_list surfaceOutputs; // SurfaceOutput::link

  struct wl_list poll; // WaylandPoll::link
  struct wl_list pollFree; // WaylandPoll::link
  LG_Lock pollLock;
  LG_Lock pollFreeLock;
  int epollFd;
  int displayFd;
};

struct WCBTransfer
{
  struct CountedBuffer * data;
  const char ** mimetypes;
};

struct ClipboardRead
{
  int fd;
  size_t size;
  size_t numRead;
  uint8_t * buf;
  enum LG_ClipboardData type;
  struct wl_data_offer * offer;
};

struct WCBState
{
  struct wl_data_device * dataDevice;
  char lgMimetype[64];

  enum LG_ClipboardData pendingType;
  char * pendingMimetype;
  bool isSelfCopy;

  enum LG_ClipboardData stashedType;
  uint8_t * stashedContents;
  ssize_t stashedSize;

  bool haveRequest;
  LG_ClipboardData type;

  struct ClipboardRead * currentRead;
};

extern struct WaylandDSState wlWm;
extern struct WCBState       wlCb;

// clipboard module
bool waylandCBInit(void);
void waylandCBRequest(LG_ClipboardData type);
void waylandCBNotice(LG_ClipboardData type);
void waylandCBRelease(void);

// cursor module
bool waylandCursorInit(void);
void waylandShowPointer(bool show);

// gl module
#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
bool waylandEGLInit(int w, int h);
EGLDisplay waylandGetEGLDisplay(void);
void waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface);
#endif

#ifdef ENABLE_EGL
EGLNativeWindowType waylandGetEGLNativeWindow(void);
#endif

#ifdef ENABLE_OPENGL
bool waylandOpenGLInit(void);
LG_DSGLContext waylandGLCreateContext(void);
void waylandGLDeleteContext(LG_DSGLContext context);
void waylandGLMakeCurrent(LG_DSGLContext context);
void waylandGLSetSwapInterval(int interval);
void waylandGLSwapBuffers(void);
#endif

// idle module
bool waylandIdleInit(void);
void waylandIdleFree(void);
void waylandInhibitIdle(void);
void waylandUninhibitIdle(void);

// input module
bool waylandInputInit(void);
void waylandInputFree(void);
void waylandGrabKeyboard(void);
void waylandGrabPointer(void);
void waylandUngrabKeyboard(void);
void waylandUngrabPointer(void);
void waylandRealignPointer(void);
void waylandWarpPointer(int x, int y, bool exiting);

// output module
bool waylandOutputInit(void);
void waylandOutputFree(void);
void waylandOutputBind(uint32_t name, uint32_t version);
void waylandOutputTryUnbind(uint32_t name);
int32_t waylandOutputGetScale(struct wl_output * output);

// poll module
bool waylandPollInit(void);
void waylandWait(unsigned int time);
bool waylandPollRegister(int fd, WaylandPollCallback callback, void * opaque, uint32_t events);
bool waylandPollUnregister(int fd);

// registry module
bool waylandRegistryInit(void);
void waylandRegistryFree(void);

// window module
bool waylandWindowInit(const char * title, bool fullscreen, bool maximize, bool borderless);
void waylandWindowFree(void);
void waylandWindowUpdateScale(void);
void waylandSetWindowSize(int x, int y);
void waylandSetFullscreen(bool fs);
bool waylandGetFullscreen(void);
bool waylandIsValidPointerPos(int x, int y);
