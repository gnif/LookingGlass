/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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
#include "input.h"
#include "kb.h"
#include "message.h"

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
  app_setState(APP_STATE_SHUTDOWN);
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
  lgInput_keyDown(KEY_LEFTCTRL);
  lgInput_keyDown(KEY_LEFTALT);
  lgInput_keyDown(sc);

  lgInput_keyUp(sc);
  lgInput_keyUp(KEY_LEFTALT);
  lgInput_keyUp(KEY_LEFTCTRL);
}

static void bind_passthrough(int sc, void * opaque)
{
  lgInput_keyDown(sc);
  lgInput_keyUp  (sc);
}

static void bind_toggleOverlay(int sc, void * opaque)
{
  app_setOverlay(!g_state.overlayInput);
}

static void bind_toggleKey(int sc, void * opaque)
{
  const int key = (uintptr_t) opaque;
  lgInput_keyDown(key);
  lgInput_keyUp(key);
}

static void bind_setGuestRes(int sc, void * opaque)
{
  if (!(g_state.transportFeatures & LG_TRANSPORT_FEATURE_WINDOW_SIZE))
  {
    app_alert(LG_ALERT_INFO, "The guest doesn't support this feature");
    return;
  }

  LGMsg msg =
  {
    .type = LG_MSG_WINDOWSIZE,
    .windowSize =
    {
      .width  = g_state.windowW,
      .height = g_state.windowH
    }
  };
  lgMessage_post(&msg);
}

void keybind_commonRegister(void)
{
  app_registerKeybind(KEY_F, bind_fullscreen   , NULL,
      "Full screen toggle");
  app_registerKeybind(KEY_V, bind_video        , NULL,
      "Video stream toggle");
  app_registerKeybind(KEY_R, bind_rotate       , NULL,
      "Rotate the output clockwise by 90° increments");
  app_registerKeybind(KEY_Q, bind_quit         , NULL,
      "Quit");
  app_registerKeybind(KEY_O, bind_toggleOverlay, NULL,
      "Toggle overlay");
  app_registerKeybind(KEY_EQUAL, bind_setGuestRes, NULL,
      "Set guest resolution to match window size");
}

#if ENABLE_AUDIO
static void bind_toggleMicDefault(int sc, void * opaque)
{
  g_state.micDefaultState = (g_state.micDefaultState + 1) % MIC_DEFAULT_MAX;

  switch (g_state.micDefaultState)
  {
    case MIC_DEFAULT_PROMPT:
      app_alert(LG_ALERT_INFO, "Microphone access will prompt");
      break;

    case MIC_DEFAULT_ALLOW:
      app_alert(LG_ALERT_INFO, "Microphone access allowed by default");
      break;

    case MIC_DEFAULT_DENY:
      app_alert(LG_ALERT_INFO, "Microphone access denied by default");
  }
}
#endif

void keybind_inputRegister(void)
{
  /* register the common input keybinds */
  static bool firstTime = true;
  if (firstTime)
  {
    app_registerKeybind(KEY_I, bind_input, NULL,
        "Keyboard & mouse toggle");

    app_registerKeybind(KEY_INSERT, bind_mouseSens, (void *) true,
        "Increase mouse sensitivity in capture mode");
    app_registerKeybind(KEY_DELETE, bind_mouseSens, (void *) false,
        "Decrease mouse sensitivity in capture mode");

    app_registerKeybind(KEY_UP, bind_toggleKey, (void *) KEY_VOLUMEUP,
        "Send volume up to the guest");
    app_registerKeybind(KEY_DOWN, bind_toggleKey, (void *) KEY_VOLUMEDOWN,
        "Send volume down to the guest");
    app_registerKeybind(KEY_M, bind_toggleKey, (void *) KEY_MUTE,
        "Send mute to the guest");

    app_registerKeybind(KEY_LEFTMETA, bind_passthrough, NULL,
        "Send LWin to the guest");
    app_registerKeybind(KEY_RIGHTMETA, bind_passthrough, NULL,
        "Send RWin to the guest");

#if ENABLE_AUDIO
    if (lgAudio_supportsRecord())
    {
      app_registerKeybind(KEY_E, lgAudio_recordToggleKeybind, NULL,
          "Toggle audio recording");
      app_registerKeybind(KEY_C, bind_toggleMicDefault, NULL,
          "Cycle audio recording default");
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
    handles[handleCount++] = app_registerKeybind(KEY_F1, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F1 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F2, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F2 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F3, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F3 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F4, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F4 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F5, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F5 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F6, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F6 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F7, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F7 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F8, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F8 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F9, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F9 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F10, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F10 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F11, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F11 to the guest");
    handles[handleCount++] = app_registerKeybind(KEY_F12, bind_ctrlAltFn, NULL,
        "Send Ctrl+Alt+F12 to the guest");
  }
}
