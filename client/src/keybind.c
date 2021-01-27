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

#include "keybind.h"

#include "main.h"
#include "app.h"
#include "core.h"
#include "kb.h"

#include "spice/spice.h"

#include <stdio.h>

static void bind_fullscreen(int sc, void * opaque)
{
  app_setFullscreen(!app_getFullscreen());
}

static void bind_video(int sc, void * opaque)
{
  g_state.stopVideo = !g_state.stopVideo;
  app_alert(
    LG_ALERT_INFO,
    g_state.stopVideo ? "Video Stream Disabled" : "Video Stream Enabled"
  );

  if (g_state.stopVideo)
    core_stopFrameThread();
  else
    core_startFrameThread();
}

static void bind_rotate(int sc, void * opaque)
{
  if (g_params.winRotate == LG_ROTATE_MAX-1)
    g_params.winRotate = 0;
  else
    ++g_params.winRotate;
  core_updatePositionInfo();
}

static void bind_input(int sc, void * opaque)
{
  g_state.ignoreInput = !g_state.ignoreInput;

  if (g_state.ignoreInput)
    core_setCursorInView(false);
  else
    g_state.ds->realignPointer();

  app_alert(
    LG_ALERT_INFO,
    g_state.ignoreInput ? "Input Disabled" : "Input Enabled"
  );
}

static void bind_quit(int sc, void * opaque)
{
  g_state.state = APP_STATE_SHUTDOWN;
}

static void bind_mouseSens(int sc, void * opaque)
{
  bool inc = (bool)opaque;

  if (inc)
  {
    if (g_cursor.sens < 9)
      ++g_cursor.sens;
  }
  else
  {
    if (g_cursor.sens > -9)
      --g_cursor.sens;
  }

  char msg[20];
  snprintf(msg, sizeof(msg), "Sensitivity: %s%d",
      g_cursor.sens > 0 ? "+" : "", g_cursor.sens);

  app_alert(
    LG_ALERT_INFO,
    msg
  );
}

static void bind_ctrlAltFn(int sc, void * opaque)
{
  const uint32_t ctrl = xfree86_to_ps2[KEY_LEFTCTRL];
  const uint32_t alt  = xfree86_to_ps2[KEY_LEFTALT ];
  const uint32_t fn   = xfree86_to_ps2[sc];
  spice_key_down(ctrl);
  spice_key_down(alt );
  spice_key_down(fn  );

  spice_key_up(ctrl);
  spice_key_up(alt );
  spice_key_up(fn  );
}

static void bind_passthrough(int sc, void * opaque)
{
  sc = xfree86_to_ps2[sc];
  spice_key_down(sc);
  spice_key_up  (sc);
}

void keybind_register(void)
{
  app_registerKeybind(KEY_F, bind_fullscreen, NULL);
  app_registerKeybind(KEY_V, bind_video     , NULL);
  app_registerKeybind(KEY_R, bind_rotate    , NULL);
  app_registerKeybind(KEY_Q, bind_quit      , NULL);

  if (g_params.useSpiceInput)
  {
    app_registerKeybind(KEY_I     , bind_input    , NULL);
    app_registerKeybind(KEY_INSERT, bind_mouseSens, (void*)true );
    app_registerKeybind(KEY_DELETE, bind_mouseSens, (void*)false);

    app_registerKeybind(KEY_F1 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F2 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F3 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F4 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F5 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F6 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F7 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F8 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F9 , bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F10, bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F11, bind_ctrlAltFn, NULL);
    app_registerKeybind(KEY_F12, bind_ctrlAltFn, NULL);

    app_registerKeybind(KEY_LEFTMETA , bind_passthrough, NULL);
    app_registerKeybind(KEY_RIGHTMETA, bind_passthrough, NULL);
  }
}
