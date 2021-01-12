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

#include <stdbool.h>
#include <stdint.h>

struct WMState
{
  bool pointerGrabbed;
  bool keyboardGrabbed;

  void * opaque;
};

struct WMDataWayland
{
  struct wl_display * display;
  struct wl_surface * surface;
  struct wl_registry * registry;
  struct wl_seat * seat;

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
};

struct WMState g_wmState;

void wmInit();
void wmFree();
void wmGrabPointer();
void wmUngrabPointer();
void wmGrabKeyboard();
void wmUngrabKeyboard();
void wmGrabAll();
void wmUngrabAll();
void wmWarpMouse(int x, int y);
