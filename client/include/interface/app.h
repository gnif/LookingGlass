/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
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

#pragma once

#include <SDL2/SDL.h>

typedef enum LG_MsgAlert
{
  LG_ALERT_INFO   ,
  LG_ALERT_SUCCESS,
  LG_ALERT_WARNING,
  LG_ALERT_ERROR
}
LG_MsgAlert;

typedef struct KeybindHandle * KeybindHandle;
typedef void (*SuperEventFn)(SDL_Scancode key, void * opaque);

/**
 * Show an alert on screen
 * @param type The alert type
 * param  fmt  The alert message format
 @ param  ...  formatted message values
 */
void app_alert(LG_MsgAlert type, const char * fmt, ...);

/**
 * Register a handler for the <super>+<key> combination
 * @param key      The scancode to register
 * @param callback The function to be called when the combination is pressed
 * @param opaque   A pointer to be passed to the callback, may be NULL
 * @retval A handle for the binding or NULL on failure.
 *         The caller is required to release the handle via `app_release_keybind` when it is no longer required
 */
KeybindHandle app_register_keybind(SDL_Scancode key, SuperEventFn callback, void * opaque);

/**
 * Release an existing key binding
 * @param handle A pointer to the keybind handle to release, may be NULL
 */
void app_release_keybind(KeybindHandle * handle);