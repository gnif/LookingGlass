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

#include <stdbool.h>
#include <sys/types.h>

#include <wayland-client.h>
#include <wayland-cursor.h>

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
# include <wayland-egl.h>
# include <EGL/egl.h>
# include <EGL/eglext.h>
# include "eglutil.h"
#endif

#include "app.h"
#include "egl_dynprocs.h"
#include "common/locking.h"
#include "common/countedbuffer.h"
#include "common/ringbuffer.h"
#include "interface/displayserver.h"

#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-presentation-time-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"
#include "wayland-xdg-decoration-unstable-v1-client-protocol.h"
#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"
#include "wayland-xdg-output-unstable-v1-client-protocol.h"

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
  wl_fixed_t scale;
  int32_t scaleInt;
  int32_t logicalWidth;
  int32_t logicalHeight;
  int32_t modeWidth;
  int32_t modeHeight;
  bool    modeRotate;
  struct wl_output * output;
  struct zxdg_output_v1 * xdgOutput;
  uint32_t version;
  struct wl_list link;
};

struct SurfaceOutput
{
  struct wl_output * output;
  struct wl_list link;
};

enum EGLSwapWithDamageState {
  SWAP_WITH_DAMAGE_UNKNOWN,
  SWAP_WITH_DAMAGE_UNSUPPORTED,
  SWAP_WITH_DAMAGE_KHR,
  SWAP_WITH_DAMAGE_EXT,
};

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

struct WaylandDSState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;
  bool pointerInSurface;
  bool focusedOnSurface;

  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;
  struct wl_shm * shm;
  struct wl_compositor * compositor;

  int32_t width, height;
  wl_fixed_t scale;
  bool fractionalScale;
  bool needsResize;
  bool fullscreen;
  uint32_t resizeSerial;
  bool configured;
  bool warpSupport;
  double cursorX, cursorY;

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
  struct wl_egl_window * eglWindow;
  struct SwapWithDamageData swapWithDamage;
#endif

#ifdef ENABLE_OPENGL
  EGLDisplay glDisplay;
  EGLConfig glConfig;
  EGLSurface glSurface;
#endif

  struct wp_presentation * presentation;
  clockid_t clkId;
  RingBuffer photonTimings;
  GraphHandle photonGraph;

#ifdef ENABLE_LIBDECOR
  struct libdecor * libdecor;
  struct libdecor_frame * libdecorFrame;
#else
  struct xdg_wm_base * xdgWmBase;
  struct xdg_surface * xdgSurface;
  struct xdg_toplevel * xdgToplevel;
  struct zxdg_decoration_manager_v1 * xdgDecorationManager;
  struct zxdg_toplevel_decoration_v1 * xdgToplevelDecoration;
#endif

  const char             * cursorThemeName;
  int                      cursorSize;
  int                      cursorScale;
  struct wl_cursor_theme * cursorTheme;
  struct wl_buffer       * cursorSquareBuffer;
  struct wl_surface      * cursors[LG_POINTER_COUNT];
  struct Point             cursorHot[LG_POINTER_COUNT];
  LG_DSPointer             cursorId;
  struct wl_surface      * cursor;
  int                      cursorHotX;
  int                      cursorHotY;

  struct wl_data_device_manager * dataDeviceManager;

  uint32_t capabilities;

  struct wl_keyboard * keyboard;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 * keyboardInhibitManager;
  struct zwp_keyboard_shortcuts_inhibitor_v1 * keyboardInhibitor;
  uint32_t keyboardEnterSerial;
  struct xkb_context * xkb;
  struct xkb_state * xkbState;
  struct xkb_keymap * keymap;

  struct wl_pointer * pointer;
  struct zwp_relative_pointer_manager_v1 * relativePointerManager;
  struct zwp_pointer_constraints_v1 * pointerConstraints;
  struct zwp_relative_pointer_v1 * relativePointer;
  struct zwp_confined_pointer_v1 * confinedPointer;
  struct zwp_locked_pointer_v1 * lockedPointer;
  bool showPointer;
  uint32_t pointerEnterSerial;
  LG_Lock confineLock;

  struct zwp_idle_inhibit_manager_v1 * idleInhibitManager;
  struct zwp_idle_inhibitor_v1 * idleInhibitor;

  struct wp_viewporter * viewporter;
  struct wp_viewport * viewport;
  struct zxdg_output_manager_v1 * xdgOutputManager;
  struct wl_list outputs; // WaylandOutput::link
  struct wl_list surfaceOutputs; // SurfaceOutput::link
  bool useFractionalScale;

  LGEvent * frameEvent;

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

  char * mimetypes[LG_CLIPBOARD_DATA_NONE];
  struct wl_data_offer * offer;
  struct wl_data_offer * dndOffer;

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
void waylandCBInvalidate(void);

// cursor module
bool waylandCursorInit(void);
void waylandCursorFree(void);
void waylandSetPointer(LG_DSPointer pointer);
void waylandCursorScaleChange(void);

// gl module
#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
bool waylandEGLInit(int w, int h);
EGLDisplay waylandGetEGLDisplay(void);
void waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count);
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
void waylandCapturePointer(void);
void waylandUncapturePointer(void);
void waylandRealignPointer(void);
void waylandWarpPointer(int x, int y, bool exiting);
void waylandGuestPointerUpdated(double x, double y, double localX, double localY);

// output module
bool waylandOutputInit(void);
void waylandOutputFree(void);
void waylandOutputBind(uint32_t name, uint32_t version);
void waylandOutputTryUnbind(uint32_t name);
wl_fixed_t waylandOutputGetScale(struct wl_output * output);

// poll module
bool waylandPollInit(void);
void waylandWait(unsigned int time);
bool waylandPollRegister(int fd, WaylandPollCallback callback, void * opaque, uint32_t events);
bool waylandPollUnregister(int fd);

// presentation module
bool waylandPresentationInit(void);
void waylandPresentationFrame(void);
void waylandPresentationFree(void);

// registry module
bool waylandRegistryInit(void);
void waylandRegistryFree(void);

// shell module
bool waylandShellInit(const char * title, bool fullscreen, bool maximize, bool borderless, bool resizable);
void waylandShellAckConfigureIfNeeded(void);
void waylandSetFullscreen(bool fs);
bool waylandGetFullscreen(void);
void waylandMinimize(void);
void waylandShellResize(int w, int h);

// window module
bool waylandWindowInit(const char * title, bool fullscreen, bool maximize, bool borderless, bool resizable);
void waylandWindowFree(void);
void waylandWindowUpdateScale(void);
void waylandSetWindowSize(int x, int y);
bool waylandIsValidPointerPos(int x, int y);
bool waylandWaitFrame(void);
void waylandSkipFrame(void);
void waylandStopWaitFrame(void);
