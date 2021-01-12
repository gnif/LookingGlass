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

#include <wayland-client.h>

#include "common/debug.h"

#include "wayland-keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#include "wayland-pointer-constraints-unstable-v1-client-protocol.h"
#include "wayland-relative-pointer-unstable-v1-client-protocol.h"

struct WMState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;

  void * opaque;
};

static struct WMState g_wmState;

static void wmWaylandInit();
static void wmWaylandFree();
static void wmWaylandGrabKeyboard();
static void wmWaylandUngrabKeyboard();
static void wmWaylandGrabPointer();
static void wmWaylandUngrabPointer();

void wmInit()
{
  switch (g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_WAYLAND:
      wmWaylandInit();
      break;

    default:
      break;
  }
}

void wmFree()
{
  switch (g_state.wminfo.subsystem)
  {
    case SDL_SYSWM_WAYLAND:
      wmWaylandFree();
      break;

    default:
      break;
  }
}

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

    case SDL_SYSWM_WAYLAND:
      wmWaylandGrabPointer();
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

    case SDL_SYSWM_WAYLAND:
      wmWaylandUngrabPointer();
      break;

    default:
      SDL_SetWindowGrab(g_state.window, SDL_FALSE);
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

    case SDL_SYSWM_WAYLAND:
      wmWaylandGrabKeyboard();
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

    case SDL_SYSWM_WAYLAND:
      wmWaylandUngrabKeyboard();
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

// Wayland support.

struct WMDataWayland
{
  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;

  uint32_t capabilities;

  struct wl_pointer * pointer;
  struct zwp_relative_pointer_manager_v1 * relativePointerManager;
  struct zwp_pointer_constraints_v1 * pointerConstraints;
  struct zwp_relative_pointer_v1 * relativePointer;
  struct zwp_confined_pointer_v1 * confinedPointer;
  struct zwp_keyboard_shortcuts_inhibit_manager_v1 * keyboardInhibitManager;
  struct zwp_keyboard_shortcuts_inhibitor_v1 * keyboardInhibitor;
};

// Registry-handling listeners.

static void registryGlobalHandler(void * data, struct wl_registry * registry,
    uint32_t name, const char * interface, uint32_t version)
{
  struct WMDataWayland * wm = data;

  if (!strcmp(interface, wl_seat_interface.name) && !wm->seat)
    wm->seat = wl_registry_bind(wm->registry, name, &wl_seat_interface, 1);
  else if (!strcmp(interface, zwp_relative_pointer_manager_v1_interface.name))
    wm->relativePointerManager = wl_registry_bind(wm->registry, name,
        &zwp_relative_pointer_manager_v1_interface, 1);
  else if (!strcmp(interface, zwp_pointer_constraints_v1_interface.name))
    wm->pointerConstraints = wl_registry_bind(wm->registry, name,
        &zwp_pointer_constraints_v1_interface, 1);
  else if (!strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name))
    wm->keyboardInhibitManager = wl_registry_bind(wm->registry, name,
        &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, 1);
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

// Seat-handling listeners.

static void handlePointerCapability(struct WMDataWayland * wm,
    uint32_t capabilities)
{
  bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
  if (!hasPointer && wm->pointer)
  {
    wl_pointer_destroy(wm->pointer);
    wm->pointer = NULL;
  }
  else if (hasPointer && !wm->pointer)
    wm->pointer = wl_seat_get_pointer(wm->seat);
}

static void seatCapabilitiesHandler(void * data, struct wl_seat * seat,
    uint32_t capabilities)
{
  struct WMDataWayland * wm = data;
  wm->capabilities = capabilities;
  handlePointerCapability(wm, capabilities);
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

void wmWaylandInit()
{
  struct WMDataWayland * wm = malloc(sizeof(struct WMDataWayland));
  memset(wm, 0, sizeof(struct WMDataWayland));

  wm->display = g_state.wminfo.info.wl.display;
  wm->surface = g_state.wminfo.info.wl.surface;
  wm->registry = wl_display_get_registry(wm->display);

  wl_registry_add_listener(wm->registry, &registryListener, wm);
  wl_display_roundtrip(wm->display);

  wl_seat_add_listener(wm->seat, &seatListener, wm);
  wl_display_roundtrip(wm->display);

  g_wmState.opaque = wm;
}

static void relativePointerMotionHandler(void * data,
    struct zwp_relative_pointer_v1 *pointer, uint32_t timeHi, uint32_t timeLo,
    wl_fixed_t dxW, wl_fixed_t dyW, wl_fixed_t dxUnaccelW,
    wl_fixed_t dyUnaccelW)
{
  double dxUnaccel = wl_fixed_to_double(dxUnaccelW);
  double dyUnaccel = wl_fixed_to_double(dyUnaccelW);
  handleMouseGrabbed(dxUnaccel, dyUnaccel);
}

static const struct zwp_relative_pointer_v1_listener relativePointerListener = {
    .relative_motion = relativePointerMotionHandler,
};

static void wmWaylandGrabPointer()
{
  struct WMDataWayland * wm = g_wmState.opaque;

  if (!wm->relativePointer)
  {
    wm->relativePointer =
      zwp_relative_pointer_manager_v1_get_relative_pointer(
        wm->relativePointerManager, wm->pointer);
    zwp_relative_pointer_v1_add_listener(wm->relativePointer,
      &relativePointerListener, wm);
  }

  if (!wm->confinedPointer)
  {
    wm->confinedPointer = zwp_pointer_constraints_v1_confine_pointer(
        wm->pointerConstraints, wm->surface, wm->pointer, NULL,
        ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  }
}

static void wmWaylandUngrabPointer()
{
  struct WMDataWayland * wm = g_wmState.opaque;

  if (wm->relativePointer)
  {
    zwp_relative_pointer_v1_destroy(wm->relativePointer);
    wm->relativePointer = NULL;
  }

  if (wm->confinedPointer)
  {
    zwp_confined_pointer_v1_destroy(wm->confinedPointer);
    wm->confinedPointer = NULL;
  }
}

static void wmWaylandGrabKeyboard()
{
  struct WMDataWayland * wm = g_wmState.opaque;

  if (wm->keyboardInhibitManager)
  {
    wm->keyboardInhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(
        wm->keyboardInhibitManager, wm->surface, wm->seat);
  }
}

static void wmWaylandUngrabKeyboard()
{
  struct WMDataWayland * wm = g_wmState.opaque;

  if (wm->keyboardInhibitor)
  {
    zwp_keyboard_shortcuts_inhibitor_v1_destroy(wm->keyboardInhibitor);
    wm->keyboardInhibitor = NULL;
  }
}

static void wmWaylandFree()
{
  struct WMDataWayland * wm = g_wmState.opaque;

  wmWaylandUngrabPointer();

  // TODO: these also need to be freed, but are currently owned by SDL.
  // wl_display_destroy(wm->display);
  // wl_surface_destroy(wm->surface);
  wl_pointer_destroy(wm->pointer);
  wl_seat_destroy(wm->seat);
  wl_registry_destroy(wm->registry);

  free(g_wmState.opaque);
}
