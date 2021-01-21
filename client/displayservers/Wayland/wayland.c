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

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/input.h>
#include <poll.h>

#include <SDL2/SDL.h>
#include <wayland-client.h>

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
# include <wayland-egl.h>
# include "egl_dynprocs.h"
# include <EGL/eglext.h>
#endif

#include "app.h"
#include "common/debug.h"

#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-xdg-decoration-unstable-v1-client-protocol.h"
#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"
#include "wayland-idle-inhibit-unstable-v1-client-protocol.h"

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

  int32_t width, height;
  bool fullscreen;
  uint32_t resizeSerial;
  bool configured;

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
  struct wl_data_device * dataDevice;

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
};

struct WCBTransfer
{
  void * data;
  size_t size;
  const char ** mimetypes;
};

struct WCBState
{
  enum LG_ClipboardData stashedType;
  char * stashedMimetype;
  uint8_t * stashedContents;
  ssize_t stashedSize;
  bool isReceiving;
  bool isSelfCopy;

  bool haveRequest;
  LG_ClipboardData      type;
};

static struct WaylandDSState wm;
static struct WCBState       wcb;

static const uint32_t cursorBitmap[] = {
  0x000000, 0x000000, 0x000000, 0x000000,
  0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000,
  0x000000, 0xFFFFFF, 0xFFFFFF, 0x000000,
  0x000000, 0x000000, 0x000000, 0x000000,
};

static struct wl_buffer * createCursorBuffer(void)
{
  int fd = memfd_create("lg-cursor", 0);
  if (fd < 0)
  {
    DEBUG_ERROR("Failed to create cursor shared memory: %d", errno);
    return NULL;
  }

  struct wl_buffer * result = NULL;

  if (ftruncate(fd, sizeof cursorBitmap) < 0)
  {
    DEBUG_ERROR("Failed to ftruncate cursor shared memory: %d", errno);
    goto fail;
  }

  void * shm_data = mmap(NULL, sizeof cursorBitmap, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm_data == MAP_FAILED)
  {
    DEBUG_ERROR("Failed to map memory for cursor: %d", errno);
    goto fail;
  }

  struct wl_shm_pool * pool = wl_shm_create_pool(wm.shm, fd, sizeof cursorBitmap);
  result = wl_shm_pool_create_buffer(pool, 0, 4, 4, 16, WL_SHM_FORMAT_XRGB8888);
  wl_shm_pool_destroy(pool);

  memcpy(shm_data, cursorBitmap, sizeof cursorBitmap);
  munmap(shm_data, sizeof cursorBitmap);

fail:
  close(fd);
  return result;
}

// XDG WM base listeners.

static void xdgWmBasePing(void * data, struct xdg_wm_base * xdgWmBase, uint32_t serial)
{
  xdg_wm_base_pong(xdgWmBase, serial);
}

static const struct xdg_wm_base_listener xdgWmBaseListener = {
  .ping = xdgWmBasePing,
};

// Registry-handling listeners.

static void registryGlobalHandler(void * data, struct wl_registry * registry,
    uint32_t name, const char * interface, uint32_t version)
{
  if (!strcmp(interface, wl_seat_interface.name) && !wm.seat)
    wm.seat = wl_registry_bind(wm.registry, name, &wl_seat_interface, 1);
  else if (!strcmp(interface, wl_shm_interface.name))
    wm.shm = wl_registry_bind(wm.registry, name, &wl_shm_interface, 1);
  else if (!strcmp(interface, wl_compositor_interface.name))
    wm.compositor = wl_registry_bind(wm.registry, name, &wl_compositor_interface, 4);
  else if (!strcmp(interface, xdg_wm_base_interface.name))
    wm.xdgWmBase = wl_registry_bind(wm.registry, name, &xdg_wm_base_interface, 1);
  else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
    wm.xdgDecorationManager = wl_registry_bind(wm.registry, name,
        &zxdg_decoration_manager_v1_interface, 1);
  else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name))
    wm.relativePointerManager = wl_registry_bind(wm.registry, name,
        &zwp_relative_pointer_manager_v1_interface, 1);
  else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name))
    wm.pointerConstraints = wl_registry_bind(wm.registry, name,
        &zwp_pointer_constraints_v1_interface, 1);
  else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
    wm.keyboardInhibitManager = wl_registry_bind(wm.registry, name,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
  else if (!strcmp(interface, wl_data_device_manager_interface.name))
    wm.dataDeviceManager = wl_registry_bind(wm.registry, name,
        &wl_data_device_manager_interface, 3);
  else if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name))
    wm.idleInhibitManager = wl_registry_bind(wm.registry, name,
        &zwp_idle_inhibit_manager_v1_interface, 1);
}

static void registryGlobalRemoveHandler(void * data,
    struct wl_registry * registry, uint32_t name)
{
  // Do nothing.
}

static const struct wl_registry_listener registryListener = {
  .global = registryGlobalHandler,
  .global_remove = registryGlobalRemoveHandler,
};

// Mouse-handling listeners.

static void pointerMotionHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, wl_fixed_t sxW, wl_fixed_t syW)
{
  int sx = wl_fixed_to_int(sxW);
  int sy = wl_fixed_to_int(syW);
  app_updateCursorPos(sx, sy);
  if (!wm.relativePointer)
    app_handleMouseBasic();
}

static void pointerEnterHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface, wl_fixed_t sxW,
    wl_fixed_t syW)
{
  app_handleEnterEvent(true);

  wl_pointer_set_cursor(pointer, serial, wm.showPointer ? wm.cursor : NULL, 0, 0);
  wm.pointerEnterSerial = serial;

  if (wm.relativePointer)
    return;

  int sx = wl_fixed_to_int(sxW);
  int sy = wl_fixed_to_int(syW);
  app_resyncMouseBasic();
  app_updateCursorPos(sx, sy);
  if (!wm.relativePointer)
    app_handleMouseBasic();
}

static void pointerLeaveHandler(void * data, struct wl_pointer * pointer,
    uint32_t serial, struct wl_surface * surface)
{
  app_handleEnterEvent(false);
}

static void pointerAxisHandler(void * data, struct wl_pointer * pointer,
  uint32_t serial, uint32_t axis, wl_fixed_t value)
{
  int button = value > 0 ?
    5 /* SPICE_MOUSE_BUTTON_DOWN */ :
    4 /* SPICE_MOUSE_BUTTON_UP */;
  app_handleButtonPress(button);
  app_handleButtonRelease(button);
}

static int mapWaylandToSpiceButton(uint32_t button)
{
  switch (button)
  {
    case BTN_LEFT:
      return 1;  // SPICE_MOUSE_BUTTON_LEFT
    case BTN_MIDDLE:
      return 2;  // SPICE_MOUSE_BUTTON_MIDDLE
    case BTN_RIGHT:
      return 3;  // SPICE_MOUSE_BUTTON_RIGHT
    case BTN_SIDE:
      return 6;  // SPICE_MOUSE_BUTTON_SIDE
    case BTN_EXTRA:
      return 7;  // SPICE_MOUSE_BUTTON_EXTRA
  }

  return 0;  // SPICE_MOUSE_BUTTON_INVALID
}

static void pointerButtonHandler(void *data, struct wl_pointer *pointer,
    uint32_t serial, uint32_t time, uint32_t button, uint32_t stateW)
{
  button = mapWaylandToSpiceButton(button);

  if (stateW == WL_POINTER_BUTTON_STATE_PRESSED)
    app_handleButtonPress(button);
  else
    app_handleButtonRelease(button);
}

static const struct wl_pointer_listener pointerListener = {
  .enter = pointerEnterHandler,
  .leave = pointerLeaveHandler,
  .motion = pointerMotionHandler,
  .button = pointerButtonHandler,
  .axis = pointerAxisHandler,
};

static void waylandInhibitIdle(void)
{
  if (wm.idleInhibitManager && !wm.idleInhibitor)
    wm.idleInhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(
        wm.idleInhibitManager, wm.surface);
}

static void waylandUninhibitIdle(void)
{
  if (wm.idleInhibitor)
  {
    zwp_idle_inhibitor_v1_destroy(wm.idleInhibitor);
    wm.idleInhibitor = NULL;
  }
}

// Keyboard-handling listeners.

static void keyboardKeymapHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t format, int fd, uint32_t size)
{
  close(fd);
}

static void keyboardEnterHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface, struct wl_array * keys)
{
  app_handleFocusEvent(true);
  wm.keyboardEnterSerial = serial;

  uint32_t * key;
  wl_array_for_each(key, keys)
    app_handleKeyPress(*key);
}

static void keyboardLeaveHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, struct wl_surface * surface)
{
  app_handleFocusEvent(false);
}

static void keyboardKeyHandler(void * data, struct wl_keyboard * keyboard,
    uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    app_handleKeyPress(key);
  else
    app_handleKeyRelease(key);
}

static void keyboardModifiersHandler(void * data,
    struct wl_keyboard * keyboard, uint32_t serial, uint32_t modsDepressed,
    uint32_t modsLatched, uint32_t modsLocked, uint32_t group)
{
  // Do nothing.
}

static const struct wl_keyboard_listener keyboardListener = {
  .keymap = keyboardKeymapHandler,
  .enter = keyboardEnterHandler,
  .leave = keyboardLeaveHandler,
  .key = keyboardKeyHandler,
  .modifiers = keyboardModifiersHandler,
};

// Seat-handling listeners.

static void handlePointerCapability(uint32_t capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (!hasPointer && wm.pointer)
  {
    wl_pointer_destroy(wm.pointer);
    wm.pointer = NULL;
  }
  else if (hasPointer && !wm.pointer)
  {
    wm.pointer = wl_seat_get_pointer(wm.seat);
    wl_pointer_add_listener(wm.pointer, &pointerListener, NULL);
  }
}

static void handleKeyboardCapability(uint32_t capabilities)
{
  bool hasKeyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;
  if (!hasKeyboard && wm.keyboard)
  {
    wl_keyboard_destroy(wm.keyboard);
    wm.keyboard = NULL;
  }
  else if (hasKeyboard && !wm.keyboard)
  {
    wm.keyboard = wl_seat_get_keyboard(wm.seat);
    wl_keyboard_add_listener(wm.keyboard, &keyboardListener, NULL);
  }
}

static void seatCapabilitiesHandler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
  wm.capabilities = capabilities;
  handlePointerCapability(capabilities);
  handleKeyboardCapability(capabilities);
}

static void seatNameHandler(void * data, struct wl_seat * seat,
    const char * name)
{
  // Do nothing.
}

static const struct wl_seat_listener seatListener = {
    .capabilities = seatCapabilitiesHandler,
    .name = seatNameHandler,
};

// Surface-handling listeners.

static void xdgSurfaceConfigure(void * data, struct xdg_surface * xdgSurface,
    uint32_t serial)
{
  if (wm.configured)
    wm.resizeSerial = serial;
  else
  {
    xdg_surface_ack_configure(xdgSurface, serial);
    wm.configured = true;
  }
}

static const struct xdg_surface_listener xdgSurfaceListener = {
  .configure = xdgSurfaceConfigure,
};

static void xdgToplevelConfigure(void * data, struct xdg_toplevel * xdgToplevel,
    int32_t width, int32_t height, struct wl_array * states)
{
  wm.width = width;
  wm.height = height;
  wm.fullscreen = false;

  enum xdg_toplevel_state * state;
  wl_array_for_each(state, states)
  {
    if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
      wm.fullscreen = true;
  }
}

static void xdgToplevelClose(void * data, struct xdg_toplevel * xdgToplevel)
{
  app_handleCloseEvent();
}

static const struct xdg_toplevel_listener xdgToplevelListener = {
  .configure = xdgToplevelConfigure,
  .close     = xdgToplevelClose,
};

static bool waylandEarlyInit(void)
{
  // Request to receive EPIPE instead of SIGPIPE when one end of a pipe
  // disconnects while a write is pending. This is useful to the Wayland
  // clipboard backend, where an arbitrary application is on the other end of
  // that pipe.
  signal(SIGPIPE, SIG_IGN);

  return true;
}

static bool waylandProbe(void)
{
  return getenv("WAYLAND_DISPLAY") != NULL;
}

static EGLDisplay waylandGetEGLDisplay(void);

static bool waylandInit(const LG_DSInitParams params)
{
  memset(&wm, 0, sizeof(wm));

  wm.display = wl_display_connect(NULL);
  wm.registry = wl_display_get_registry(wm.display);

  wl_registry_add_listener(wm.registry, &registryListener, NULL);
  wl_display_roundtrip(wm.display);

  if (!wm.seat || !wm.dataDeviceManager || !wm.compositor || !wm.xdgWmBase)
  {
    DEBUG_ERROR("Compositor missing a required interface, will not proceed");
    return false;
  }

  if (!wm.relativePointerManager)
    DEBUG_WARN("zwp_relative_pointer_manager_v1 not exported by compositor, "
               "mouse will not be captured");
  if (!wm.pointerConstraints)
    DEBUG_WARN("zwp_pointer_constraints_v1 not exported by compositor, mouse "
               "will not be captured");
  if (!wm.keyboardInhibitManager)
    DEBUG_WARN("zwp_keyboard_shortcuts_inhibit_manager_v1 not exported by "
               "compositor, keyboard will not be grabbed");
  if (!wm.idleInhibitManager)
    DEBUG_WARN("zwp_idle_inhibit_manager_v1 not exported by compositor, will "
               "not be able to suppress idle states");

  wl_seat_add_listener(wm.seat, &seatListener, NULL);
  xdg_wm_base_add_listener(wm.xdgWmBase, &xdgWmBaseListener, NULL);
  wl_display_roundtrip(wm.display);

  wm.dataDevice = wl_data_device_manager_get_data_device(
      wm.dataDeviceManager, wm.seat);

  wm.surface = wl_compositor_create_surface(wm.compositor);
  wm.eglWindow = wl_egl_window_create(wm.surface, params.w, params.h);
  wm.xdgSurface = xdg_wm_base_get_xdg_surface(wm.xdgWmBase, wm.surface);
  xdg_surface_add_listener(wm.xdgSurface, &xdgSurfaceListener, NULL);

  wm.xdgToplevel = xdg_surface_get_toplevel(wm.xdgSurface);
  xdg_toplevel_add_listener(wm.xdgToplevel, &xdgToplevelListener, NULL);
  xdg_toplevel_set_title(wm.xdgToplevel, params.title);
  xdg_toplevel_set_app_id(wm.xdgToplevel, "looking-glass-client");

  if (params.fullscreen)
    xdg_toplevel_set_fullscreen(wm.xdgToplevel, NULL);

  if (params.maximize)
    xdg_toplevel_set_maximized(wm.xdgToplevel);

  wl_surface_commit(wm.surface);

  if (wm.xdgDecorationManager)
  {
    wm.xdgToplevelDecoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
        wm.xdgDecorationManager, wm.xdgToplevel);
    if (wm.xdgToplevelDecoration)
    {
      zxdg_toplevel_decoration_v1_set_mode(wm.xdgToplevelDecoration,
          params.borderless ?
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE :
            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
  }

  struct wl_buffer * cursorBuffer = createCursorBuffer();
  if (cursorBuffer)
  {
    wm.cursor = wl_compositor_create_surface(wm.compositor);
    wl_surface_attach(wm.cursor, cursorBuffer, 0, 0);
    wl_surface_commit(wm.cursor);
  }

#ifdef ENABLE_OPENGL
  if (params.opengl)
  {
    EGLint attr[] =
    {
      EGL_BUFFER_SIZE      , 24,
      EGL_CONFORMANT       , EGL_OPENGL_BIT,
      EGL_RENDERABLE_TYPE  , EGL_OPENGL_BIT,
      EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
      EGL_RED_SIZE         , 8,
      EGL_GREEN_SIZE       , 8,
      EGL_BLUE_SIZE        , 8,
      EGL_SAMPLE_BUFFERS   , 0,
      EGL_SAMPLES          , 0,
      EGL_NONE
    };

    wm.glDisplay = waylandGetEGLDisplay();

    int maj, min;
    if (!eglInitialize(wm.glDisplay, &maj, &min))
    {
      DEBUG_ERROR("Unable to initialize EGL");
      return false;
    }

    if (wm.glDisplay == EGL_NO_DISPLAY)
    {
      DEBUG_ERROR("Failed to get EGL display (eglError: 0x%x)", eglGetError());
      return false;
    }

    EGLint num_config;
    if (!eglChooseConfig(wm.glDisplay, attr, &wm.glConfig, 1, &num_config))
    {
      DEBUG_ERROR("Failed to choose config (eglError: 0x%x)", eglGetError());
      return false;
    }

    wm.glSurface = eglCreateWindowSurface(wm.glDisplay, wm.glConfig, wm.eglWindow, NULL);
    if (wm.glSurface == EGL_NO_SURFACE)
    {
      DEBUG_ERROR("Failed to create EGL surface (eglError: 0x%x)", eglGetError());
      return false;
    }
  }
#endif

  wm.width = params.w;
  wm.height = params.h;

  return true;
}

static void waylandStartup(void)
{
}

static void waylandShutdown(void)
{
}

#ifdef ENABLE_EGL
static EGLNativeWindowType waylandGetEGLNativeWindow(void)
{
  return (EGLNativeWindowType) wm.eglWindow;
}
#endif

#if defined(ENABLE_EGL) || defined(ENABLE_OPENGL)
static EGLDisplay waylandGetEGLDisplay(void)
{
  EGLNativeDisplayType native = (EGLNativeDisplayType) wm.display;

  const char *early_exts = eglQueryString(NULL, EGL_EXTENSIONS);

  if (strstr(early_exts, "EGL_KHR_platform_wayland") != NULL &&
      g_egl_dynProcs.eglGetPlatformDisplay)
  {
    DEBUG_INFO("Using eglGetPlatformDisplay");
    return g_egl_dynProcs.eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native, NULL);
  }

  if (strstr(early_exts, "EGL_EXT_platform_wayland") != NULL &&
      g_egl_dynProcs.eglGetPlatformDisplayEXT)
  {
    DEBUG_INFO("Using eglGetPlatformDisplayEXT");
    return g_egl_dynProcs.eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, native, NULL);
  }

  DEBUG_INFO("Using eglGetDisplay");
  return eglGetDisplay(native);
}

static void waylandEGLSwapBuffers(EGLDisplay display, EGLSurface surface)
{
  eglSwapBuffers(display, surface);

  if (wm.resizeSerial)
  {
    wl_egl_window_resize(wm.eglWindow, wm.width, wm.height, 0, 0);

    struct wl_region * region = wl_compositor_create_region(wm.compositor);
    wl_region_add(region, 0, 0, wm.width, wm.height);
    wl_surface_set_opaque_region(wm.surface, region);
    wl_region_destroy(region);

    app_handleResizeEvent(wm.width, wm.height, (struct Border) {0, 0, 0, 0});
    xdg_surface_ack_configure(wm.xdgSurface, wm.resizeSerial);
    wm.resizeSerial = 0;
  }
}
#endif

#ifdef ENABLE_OPENGL
static LG_DSGLContext waylandGLCreateContext(void)
{
  eglBindAPI(EGL_OPENGL_API);
  return eglCreateContext(wm.glDisplay, wm.glConfig, EGL_NO_CONTEXT, NULL);
}

static void waylandGLDeleteContext(LG_DSGLContext context)
{
  eglDestroyContext(wm.glDisplay, context);
}

static void waylandGLMakeCurrent(LG_DSGLContext context)
{
  eglMakeCurrent(wm.glDisplay, wm.glSurface, wm.glSurface, context);
}

static void waylandGLSetSwapInterval(int interval)
{
  eglSwapInterval(wm.glDisplay, interval);
}

static void waylandGLSwapBuffers(void)
{
  waylandEGLSwapBuffers(wm.glDisplay, wm.glSurface);
}
#endif

static void waylandShowPointer(bool show)
{
  wm.showPointer = show;
  wl_pointer_set_cursor(wm.pointer, wm.pointerEnterSerial, show ? wm.cursor : NULL, 0, 0);
}

static void waylandWait(unsigned int time)
{
  while (wl_display_prepare_read(wm.display))
    wl_display_dispatch_pending(wm.display);
  wl_display_flush(wm.display);

  struct pollfd pollfd = {
    .fd     = wl_display_get_fd(wm.display),
    .events = POLLIN,
  };

  if (poll(&pollfd, 1, time) == -1 || pollfd.revents & POLLERR)
  {
    if (errno != EINTR)
      DEBUG_INFO("Poll failed: %d\n", errno);
    wl_display_cancel_read(wm.display);
  }
  else
    wl_display_read_events(wm.display);

  wl_display_dispatch_pending(wm.display);
}

static void waylandSetWindowSize(int x, int y)
{
  // FIXME: implement.
}

static void waylandSetFullscreen(bool fs)
{
  if (fs)
    xdg_toplevel_set_fullscreen(wm.xdgToplevel, NULL);
  else
    xdg_toplevel_unset_fullscreen(wm.xdgToplevel);
}

static bool waylandGetFullscreen(void)
{
  return wm.fullscreen;
}

static void relativePointerMotionHandler(void * data,
    struct zwp_relative_pointer_v1 *pointer, uint32_t timeHi, uint32_t timeLo,
    wl_fixed_t dxW, wl_fixed_t dyW, wl_fixed_t dxUnaccelW,
    wl_fixed_t dyUnaccelW)
{
  double dx, dy;
  if (app_cursorWantsRaw())
  {
    dx = wl_fixed_to_double(dxUnaccelW);
    dy = wl_fixed_to_double(dyUnaccelW);
  }
  else
  {
    dx = wl_fixed_to_double(dxW);
    dy = wl_fixed_to_double(dyW);
  }
  app_handleMouseGrabbed(dx, dy);
}

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = relativePointerMotionHandler,
};

static void waylandGrabPointer(void)
{
  if (!wm.relativePointerManager || !wm.pointerConstraints)
    return;

  if (!wm.relativePointer)
  {
    wm.relativePointer =
      zwp_relative_pointer_manager_v1_get_relative_pointer(
        wm.relativePointerManager, wm.pointer);
    zwp_relative_pointer_v1_add_listener(wm.relativePointer,
      &relativePointerListener, NULL);
  }

  if (!wm.confinedPointer)
  {
    wm.confinedPointer = zwp_pointer_constraints_v1_confine_pointer(
        wm.pointerConstraints, wm.surface, wm.pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  }
}

static void waylandUngrabPointer(void)
{
  if (wm.relativePointer)
  {
    zwp_relative_pointer_v1_destroy(wm.relativePointer);
    wm.relativePointer = NULL;
  }

  if (wm.confinedPointer)
  {
    zwp_confined_pointer_v1_destroy(wm.confinedPointer);
    wm.confinedPointer = NULL;
  }

  app_resyncMouseBasic();
  app_handleMouseBasic();
}

static void waylandGrabKeyboard(void)
{
  if (wm.keyboardInhibitManager && !wm.keyboardInhibitor)
  {
    wm.keyboardInhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
        wm.keyboardInhibitManager, wm.surface, wm.seat);
  }
}

static void waylandUngrabKeyboard(void)
{
  if (wm.keyboardInhibitor)
  {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(wm.keyboardInhibitor);
    wm.keyboardInhibitor = NULL;
  }
}

static void waylandWarpPointer(int x, int y, bool exiting)
{
  // This is an unsupported operation on Wayland.
}

static void waylandRealignPointer(void)
{
  app_resyncMouseBasic();
}

static bool waylandIsValidPointerPos(int x, int y)
{
  return x >= 0 && x < wm.width && y >= 0 && y < wm.height;
}

static void waylandFree(void)
{
  waylandUngrabPointer();

  if (wm.idleInhibitManager)
  {
    waylandUninhibitIdle();
    zwp_idle_inhibit_manager_v1_destroy(wm.idleInhibitManager);
  }

  // TODO: these also need to be freed, but are currently owned by SDL.
  // wl_display_destroy(wm.display);
  // wl_surface_destroy(wm.surface);
  wl_pointer_destroy(wm.pointer);
  wl_seat_destroy(wm.seat);
  wl_registry_destroy(wm.registry);
}

static bool waylandGetProp(LG_DSProperty prop, void * ret)
{
  if (prop == LG_DS_WARP_SUPPORT)
  {
    *(bool*)ret = false;
    return true;
  }

  return false;
}

static const char * textMimetypes[] =
{
  "text/plain",
  "text/plain;charset=utf-8",
  "TEXT",
  "STRING",
  "UTF8_STRING",
  NULL,
};

static const char * pngMimetypes[] =
{
  "image/png",
  NULL,
};

static const char * bmpMimetypes[] =
{
  "image/bmp",
  "image/x-bmp",
  "image/x-MS-bmp",
  "image/x-win-bitmap",
  NULL,
};

static const char * tiffMimetypes[] =
{
  "image/tiff",
  NULL,
};

static const char * jpegMimetypes[] =
{
  "image/jpeg",
  NULL,
};

static const char ** cbTypeToMimetypes(enum LG_ClipboardData type)
{
  switch (type)
  {
    case LG_CLIPBOARD_DATA_TEXT:
      return textMimetypes;
    case LG_CLIPBOARD_DATA_PNG:
      return pngMimetypes;
    case LG_CLIPBOARD_DATA_BMP:
      return bmpMimetypes;
    case LG_CLIPBOARD_DATA_TIFF:
      return tiffMimetypes;
    case LG_CLIPBOARD_DATA_JPEG:
      return jpegMimetypes;
    default:
      DEBUG_ERROR("invalid clipboard type");
      abort();
  }
}

static bool containsMimetype(const char ** mimetypes,
    const char * needle)
{
  for (const char ** mimetype = mimetypes; *mimetype; mimetype++)
    if (!strcmp(needle, *mimetype))
      return true;

  return false;
}

static bool mimetypeEndswith(const char * mimetype, const char * what)
{
  size_t mimetypeLen = strlen(mimetype);
  size_t whatLen = strlen(what);

  if (mimetypeLen < whatLen)
    return false;

  return !strcmp(mimetype + mimetypeLen - whatLen, what);
}

static bool isTextMimetype(const char * mimetype)
{
  if (containsMimetype(textMimetypes, mimetype))
    return true;

  char * text = "text/";
  if (!strncmp(mimetype, text, strlen(text)))
    return true;

  if (mimetypeEndswith(mimetype, "script") ||
      mimetypeEndswith(mimetype, "xml") ||
      mimetypeEndswith(mimetype, "yaml"))
    return true;

  if (strstr(mimetype, "json"))
    return true;

  return false;
}

static enum LG_ClipboardData mimetypeToCbType(const char * mimetype)
{
  if (isTextMimetype(mimetype))
    return LG_CLIPBOARD_DATA_TEXT;

  if (containsMimetype(pngMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_PNG;

  if (containsMimetype(bmpMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_BMP;

  if (containsMimetype(tiffMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_TIFF;

  if (containsMimetype(jpegMimetypes, mimetype))
    return LG_CLIPBOARD_DATA_JPEG;

  return LG_CLIPBOARD_DATA_NONE;
}

// Destination client handlers.

static void dataOfferHandleOffer(void * data, struct wl_data_offer * offer,
    const char * mimetype)
{
  enum LG_ClipboardData type = mimetypeToCbType(mimetype);
  // Oftentimes we'll get text/html alongside text/png, but would prefer to send
  // image/png. In general, prefer images over text content.
  if (type != LG_CLIPBOARD_DATA_NONE &&
      (wcb.stashedType == LG_CLIPBOARD_DATA_NONE ||
       wcb.stashedType == LG_CLIPBOARD_DATA_TEXT))
  {
    wcb.stashedType = type;
    if (wcb.stashedMimetype)
      free(wcb.stashedMimetype);
    wcb.stashedMimetype = strdup(mimetype);
  }
}

static void dataOfferHandleSourceActions(void * data,
    struct wl_data_offer * offer, uint32_t sourceActions)
{
  // Do nothing.
}

static void dataOfferHandleAction(void * data, struct wl_data_offer * offer,
    uint32_t dndAction)
{
  // Do nothing.
}

static const struct wl_data_offer_listener dataOfferListener = {
  .offer = dataOfferHandleOffer,
  .source_actions = dataOfferHandleSourceActions,
  .action = dataOfferHandleAction,
};

static void dataDeviceHandleDataOffer(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  wcb.stashedType = LG_CLIPBOARD_DATA_NONE;
  wl_data_offer_add_listener(offer, &dataOfferListener, NULL);
}

static void dataDeviceHandleSelection(void * data,
    struct wl_data_device * dataDevice, struct wl_data_offer * offer)
{
  if (wcb.stashedType == LG_CLIPBOARD_DATA_NONE || !offer)
    return;

  int fds[2];
  if (pipe(fds) < 0)
  {
    DEBUG_ERROR("Failed to get a clipboard pipe: %s", strerror(errno));
    abort();
  }

  wcb.isReceiving = true;
  wcb.isSelfCopy = false;
  wl_data_offer_receive(offer, wcb.stashedMimetype, fds[1]);
  close(fds[1]);
  free(wcb.stashedMimetype);
  wcb.stashedMimetype = NULL;

  wl_display_roundtrip(wm.display);

  if (wcb.stashedContents)
  {
    free(wcb.stashedContents);
    wcb.stashedContents = NULL;
  }

  size_t size = 4096, numRead = 0;
  uint8_t * buf = (uint8_t *) malloc(size);
  while (true)
  {
    ssize_t result = read(fds[0], buf + numRead, size - numRead);
    if (result < 0)
    {
      DEBUG_ERROR("Failed to read from clipboard: %s", strerror(errno));
      abort();
    }

    if (result == 0)
    {
      buf[numRead] = 0;
      break;
    }

    numRead += result;
    if (numRead >= size)
    {
      size *= 2;
      void * nbuf = realloc(buf, size);
      if (!nbuf) {
        DEBUG_ERROR("Failed to realloc clipboard buffer: %s", strerror(errno));
        abort();
      }

      buf = nbuf;
    }
  }

  wcb.stashedSize = numRead;
  wcb.stashedContents = buf;
  wcb.isReceiving = false;

  close(fds[0]);
  wl_data_offer_destroy(offer);

  if (!wcb.isSelfCopy)
    app_clipboardNotify(wcb.stashedType, 0);
}

static const struct wl_data_device_listener dataDeviceListener = {
  .data_offer = dataDeviceHandleDataOffer,
  .selection = dataDeviceHandleSelection,
};

static void waylandCBRequest(LG_ClipboardData type)
{
  // We only notified once, so it must be this.
  assert(type == wcb.stashedType);
  app_clipboardData(wcb.stashedType, wcb.stashedContents, wcb.stashedSize);
}

static bool waylandCBInit(void)
{
  memset(&wcb, 0, sizeof(wcb));

  wcb.stashedType = LG_CLIPBOARD_DATA_NONE;
  wl_data_device_add_listener(wm.dataDevice, &dataDeviceListener, NULL);

  return true;
}

static void dataSourceHandleSend(void * data, struct wl_data_source * source,
    const char * mimetype, int fd)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  if (wcb.isReceiving)
    wcb.isSelfCopy = true;
  else if (containsMimetype(transfer->mimetypes, mimetype))
  {
    // Consider making this do non-blocking sends to not stall the Wayland
    // event loop if it becomes a problem. This is "fine" in the sense that
    // wl-copy also stalls like this, but it's not necessary.
    fcntl(fd, F_SETFL, 0);

    size_t pos = 0;
    while (pos < transfer->size)
    {
      ssize_t written = write(fd, transfer->data + pos, transfer->size - pos);
      if (written < 0)
      {
        if (errno != EPIPE)
          DEBUG_ERROR("Failed to write clipboard data: %s", strerror(errno));
        goto error;
      }

      pos += written;
    }
  }

error:
  close(fd);
}

static void dataSourceHandleCancelled(void * data,
    struct wl_data_source * source)
{
  struct WCBTransfer * transfer = (struct WCBTransfer *) data;
  free(transfer->data);
  free(transfer);
  wl_data_source_destroy(source);
}

static const struct wl_data_source_listener dataSourceListener = {
  .send = dataSourceHandleSend,
  .cancelled = dataSourceHandleCancelled,
};

static void waylandCBReplyFn(void * opaque, LG_ClipboardData type,
   	uint8_t * data, uint32_t size)
{
  struct WCBTransfer * transfer = malloc(sizeof(struct WCBTransfer));
  void * dataCopy = malloc(size);
  memcpy(dataCopy, data, size);
  *transfer = (struct WCBTransfer) {
    .data = dataCopy,
    .size = size,
    .mimetypes = cbTypeToMimetypes(type),
  };

  struct wl_data_source * source =
    wl_data_device_manager_create_data_source(wm.dataDeviceManager);
  wl_data_source_add_listener(source, &dataSourceListener, transfer);
  for (const char ** mimetype = transfer->mimetypes; *mimetype; mimetype++)
    wl_data_source_offer(source, *mimetype);

  wl_data_device_set_selection(wm.dataDevice, source,
    wm.keyboardEnterSerial);
}

static void waylandCBNotice(LG_ClipboardData type)
{
  wcb.haveRequest = true;
  wcb.type        = type;
  app_clipboardRequest(waylandCBReplyFn, NULL);
}

static void waylandCBRelease(void)
{
  wcb.haveRequest = false;
}

struct LG_DisplayServerOps LGDS_Wayland =
{
  .probe              = waylandProbe,
  .earlyInit          = waylandEarlyInit,
  .init               = waylandInit,
  .startup            = waylandStartup,
  .shutdown           = waylandShutdown,
  .free               = waylandFree,
  .getProp            = waylandGetProp,

#ifdef ENABLE_EGL
  .getEGLDisplay      = waylandGetEGLDisplay,
  .getEGLNativeWindow = waylandGetEGLNativeWindow,
  .eglSwapBuffers     = waylandEGLSwapBuffers,
#endif

#ifdef ENABLE_OPENGL
  .glCreateContext    = waylandGLCreateContext,
  .glDeleteContext    = waylandGLDeleteContext,
  .glMakeCurrent      = waylandGLMakeCurrent,
  .glSetSwapInterval  = waylandGLSetSwapInterval,
  .glSwapBuffers      = waylandGLSwapBuffers,
#endif

  .showPointer        = waylandShowPointer,
  .grabPointer        = waylandGrabPointer,
  .ungrabPointer      = waylandUngrabPointer,
  .grabKeyboard       = waylandGrabKeyboard,
  .ungrabKeyboard     = waylandUngrabKeyboard,
  .warpPointer        = waylandWarpPointer,
  .realignPointer     = waylandRealignPointer,
  .isValidPointerPos  = waylandIsValidPointerPos,
  .inhibitIdle        = waylandInhibitIdle,
  .uninhibitIdle      = waylandUninhibitIdle,
  .wait               = waylandWait,
  .setWindowSize      = waylandSetWindowSize,
  .setFullscreen      = waylandSetFullscreen,
  .getFullscreen      = waylandGetFullscreen,

  .cbInit    = waylandCBInit,
  .cbNotice  = waylandCBNotice,
  .cbRelease = waylandCBRelease,
  .cbRequest = waylandCBRequest
};
