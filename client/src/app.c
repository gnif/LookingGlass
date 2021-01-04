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

#include "main.h"
#include "common/debug.h"
#include <stdarg.h>

void app_alert(LG_MsgAlert type, const char * fmt, ...)
{
  if (!g_state.lgr || !params.showAlerts)
    return;

  va_list args;
  va_start(args, fmt);
  const int length = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  char *buffer = malloc(length + 1);
  va_start(args, fmt);
  vsnprintf(buffer, length + 1, fmt, args);
  va_end(args);

  g_state.lgr->on_alert(
    g_state.lgrData,
    type,
    buffer,
    NULL
  );

  free(buffer);
}

KeybindHandle app_register_keybind(SDL_Scancode key, SuperEventFn callback, void * opaque)
{
  // don't allow duplicate binds
  if (g_state.bindings[key])
  {
    DEBUG_INFO("Key already bound");
    return NULL;
  }

  KeybindHandle handle = (KeybindHandle)malloc(sizeof(struct KeybindHandle));
  handle->key      = key;
  handle->callback = callback;
  handle->opaque   = opaque;

  g_state.bindings[key] = handle;
  return handle;
}

void app_release_keybind(KeybindHandle * handle)
{
  if (!*handle)
    return;

  g_state.bindings[(*handle)->key] = NULL;
  free(*handle);
  *handle = NULL;
}
