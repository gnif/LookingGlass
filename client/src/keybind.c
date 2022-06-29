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

#include "keybind.h"

#include "main.h"
#include "app.h"
#include "audio.h"
#include "core.h"
#include "kb.h"

#include <purespice.h>
#include <stdio.h>

static void bind_fullscreen(int sc, void * opaque)
{
  app_setFullscreen(!app_getFullscreen());
}

static void bind_video(int sc, void * opaque)
{
  app_stopVideo(!g_state.stopVideo);
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
  const uint32_t ctrl = linux_to_ps2[KEY_LEFTCTRL];
  const uint32_t alt  = linux_to_ps2[KEY_LEFTALT ];
  const uint32_t fn   = linux_to_ps2[sc];
  purespice_keyDown(ctrl);
  purespice_keyDown(alt );
  purespice_keyDown(fn  );

  purespice_keyUp(ctrl);
  purespice_keyUp(alt );
  purespice_keyUp(fn  );
}

static void bind_passthrough(int sc, void * opaque)
{
  sc = linux_to_ps2[sc];
  purespice_keyDown(sc);
  purespice_keyUp  (sc);
}

static void bind_toggleOverlay(int sc, void * opaque)
{
  app_setOverlay(!g_state.overlayInput);
}

static void bind_toggleKey(int sc, void * opaque)
{
  purespice_keyDown((uintptr_t) opaque);
  purespice_keyUp((uintptr_t) opaque);
}

void keybind_commonRegister(void)
{
  app_registerKeybind(0, 'F', bind_fullscreen   , NULL,
      "Full screen toggle");
  app_registerKeybind(0, 'V', bind_video        , NULL,
      "Video stream toggle");
  app_registerKeybind(0, 'R', bind_rotate       , NULL,
      "Rotate the output clockwise by 90° increments");
  app_registerKeybind(0, 'Q', bind_quit         , NULL,
      "Quit");
  app_registerKeybind(0, 'O', bind_toggleOverlay, NULL,
      "Toggle overlay");
}

void keybind_spiceRegister(void)
{
  /* register the common keybinds for spice */
  static bool firstTime = true;
  if (firstTime)
  {
    app_registerKeybind(0, 'I', bind_input, NULL,
        "Spice keyboard & mouse toggle");

    app_registerKeybind(KEY_INSERT, 0, bind_mouseSens, (void *) true ,
        "Increase mouse sensitivity 0, in capture mode");
    app_registerKeybind(KEY_DELETE, 0, bind_mouseSens, (void *) false,
        "Descrease mouse sensitivity in capture mode");

    app_registerKeybind(KEY_UP  , 0 , bind_toggleKey, (void *) PS2_VOLUME_UP  ,
        "Send volume up to the guest");
    app_registerKeybind(KEY_DOWN, 0 , bind_toggleKey, (void *) PS2_VOLUME_DOWN,
        "Send volume down to the guest");
    app_registerKeybind(0      , 'M', bind_toggleKey, (void *) PS2_MUTE       ,
        "Send mute to the guest");

    app_registerKeybind(KEY_LEFTMETA , 0, bind_passthrough, NULL,
        "Send LWin to the guest");
    app_registerKeybind(KEY_RIGHTMETA, 0, bind_passthrough, NULL,
        "Send RWin to the guest");

#if ENABLE_AUDIO
    if (audio_supportsRecord())
    {
      app_registerKeybind(0, 'E', audio_recordToggleKeybind, NULL,
          "Toggle audio recording");
    }
#endif

    firstTime = false;
  }

  /* release any OS based keybinds that have been bound */
  static KeybindHandle handles[32] = { 0 }; // increase size as needed
  static int handleCount = 0;
  for(int i = 0; i < handleCount; ++i)
    app_releaseKeybind(&handles[i]);
  handleCount = 0;

  /* register OS based keybinds */
  if (app_guestIsLinux())
  {
    handles[handleCount++] = app_registerKeybind(KEY_F1 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F1 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F2 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F2 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F3 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F3 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F4 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F4 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F5 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F5 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F6 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F6 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F7 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F7 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F8 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F8 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F9 , 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F9 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F10, 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F10 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F11, 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F11 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F12, 0, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F12 to the guest");
  }
}
