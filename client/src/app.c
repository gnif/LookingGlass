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

#include "app.h"

#include "main.h"
#include "core.h"
#include "util.h"
#include "clipboard.h"

#include "ll.h"
#include "kb.h"

#include "common/debug.h"

#include <stdarg.h>
#include <math.h>
#include <string.h>

bool app_isRunning(void)
{
  return
    g_state.state == APP_STATE_RUNNING ||
    g_state.state == APP_STATE_RESTART;
}

void app_updateCursorPos(double x, double y)
{
  g_cursor.pos.x = x;
  g_cursor.pos.y = y;
  g_cursor.valid = true;
}

void app_handleFocusEvent(bool focused)
{
  g_state.focused = focused;
  if (!core_inputEnabled())
    return;

  if (!focused)
  {
    core_setGrabQuiet(false);
    core_setCursorInView(false);
  }

  g_cursor.realign = true;
  g_state.ds->realignPointer();
}

void app_handleEnterEvent(bool entered)
{
  if (entered)
  {
    g_cursor.inWindow = true;
    if (!core_inputEnabled())
      return;

    g_cursor.realign = true;
  }
  else
  {
    g_cursor.inWindow = false;
    core_setCursorInView(false);

    if (!core_inputEnabled())
      return;

    if (!g_params.alwaysShowCursor)
      g_cursor.draw = false;
    g_cursor.redraw = true;
  }
}

void app_clipboardRelease(void)
{
  if (!g_params.clipboardToVM)
    return;

  spice_clipboard_release();
}

void app_clipboardNotify(const LG_ClipboardData type, size_t size)
{
  if (!g_params.clipboardToVM)
    return;

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    spice_clipboard_release();
    return;
  }

  g_state.cbType    = cb_lgTypeToSpiceType(type);
  g_state.cbChunked = size > 0;
  g_state.cbXfer    = size;

  spice_clipboard_grab(g_state.cbType);

  if (size)
    spice_clipboard_data_start(g_state.cbType, size);
}

void app_clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size)
{
  if (!g_params.clipboardToVM)
    return;

  if (g_state.cbChunked && size > g_state.cbXfer)
  {
    DEBUG_ERROR("refusing to send more then cbXfer bytes for chunked xfer");
    size = g_state.cbXfer;
  }

  if (!g_state.cbChunked)
    spice_clipboard_data_start(g_state.cbType, size);

  spice_clipboard_data(g_state.cbType, data, (uint32_t)size);
  g_state.cbXfer -= size;
}

void app_clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!g_params.clipboardToLocal)
    return;

  struct CBRequest * cbr = (struct CBRequest *)malloc(sizeof(struct CBRequest));

  cbr->type    = g_state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(g_state.cbRequestList, cbr);

  spice_clipboard_request(g_state.cbType);
}

void spiceClipboardNotice(const SpiceDataType type)
{
  if (!g_params.clipboardToLocal)
    return;

  if (!g_state.cbAvailable)
    return;

  g_state.cbType = type;
  g_state.ds->cbNotice(cb_spiceTypeToLGType(type));
}

void app_handleButtonPress(int button)
{
  if (!core_inputEnabled() || !g_cursor.inView)
    return;

  g_cursor.buttons |= (1U << button);

  if (!spice_mouse_press(button))
    DEBUG_ERROR("app_handleButtonPress: failed to send message");
}

void app_handleButtonRelease(int button)
{
  if (!core_inputEnabled())
    return;

  g_cursor.buttons &= ~(1U << button);

  if (!spice_mouse_release(button))
    DEBUG_ERROR("app_handleButtonRelease: failed to send message");
}

void app_handleKeyPress(int sc)
{
  if (sc == g_params.escapeKey && !g_state.escapeActive)
  {
    g_state.escapeActive = true;
    g_state.escapeAction = -1;
    return;
  }

  if (g_state.escapeActive)
  {
    g_state.escapeAction = sc;
    return;
  }

  if (!core_inputEnabled())
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  if (!g_state.keyDown[sc])
  {
    uint32_t ps2 = xfree86_to_ps2[sc];
    if (!ps2)
      return;

    if (spice_key_down(ps2))
      g_state.keyDown[sc] = true;
    else
    {
      DEBUG_ERROR("app_handleKeyPress: failed to send message");
      return;
    }
  }
}

void app_handleKeyRelease(int sc)
{
  if (g_state.escapeActive)
  {
    if (g_state.escapeAction == -1)
    {
      if (g_params.useSpiceInput)
        core_setGrab(!g_cursor.grab);
    }
    else
    {
      KeybindHandle handle = g_state.bindings[sc];
      if (handle)
      {
        handle->callback(sc, handle->opaque);
        return;
      }
    }

    if (sc == g_params.escapeKey)
      g_state.escapeActive = false;
  }

  if (!core_inputEnabled())
    return;

  // avoid sending key up events when we didn't send a down
  if (!g_state.keyDown[sc])
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  uint32_t ps2 = xfree86_to_ps2[sc];
  if (!ps2)
    return;

  if (spice_key_up(ps2))
    g_state.keyDown[sc] = false;
  else
  {
    DEBUG_ERROR("app_handleKeyRelease: failed to send message");
    return;
  }
}

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy)
{
  if (g_cursor.grab)
  {
    if (g_params.rawMouse)
      core_handleMouseGrabbed(rawx, rawy);
    else
      core_handleMouseGrabbed(normx, normy);
  }
  else
    if (g_cursor.inWindow)
      core_handleMouseNormal(normx, normy);
}

static inline double clamp(double x, double min, double max)
{
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

// On some display servers normal cursor logic does not work due to the lack of
// cursor warp support. Instead, we attempt a best-effort emulation which works
// with a 1:1 mouse movement patch applied in the guest. For anything fancy, use
// capture mode.
void app_handleMouseBasic()
{
  /* do not pass mouse events to the guest if we do not have focus */
  if (!g_cursor.guest.valid || !g_state.haveSrcSize || !g_state.focused)
    return;

  if (!core_inputEnabled())
    return;

  const bool inView =
    g_cursor.pos.x >= g_state.dstRect.x                     &&
    g_cursor.pos.x <  g_state.dstRect.x + g_state.dstRect.w &&
    g_cursor.pos.y >= g_state.dstRect.y                     &&
    g_cursor.pos.y <  g_state.dstRect.y + g_state.dstRect.h;

  core_setCursorInView(inView);

  /* translate the current position to guest coordinate space */
  struct DoublePoint guest;
  util_localCurToGuest(&guest);

  int x = (int) round(clamp(guest.x, 0, g_state.srcSize.x) - g_cursor.projected.x);
  int y = (int) round(clamp(guest.y, 0, g_state.srcSize.y) - g_cursor.projected.y);

  if (!x && !y)
    return;

  g_cursor.projected.x += x;
  g_cursor.projected.y += y;

  if (!spice_mouse_motion(x, y))
    DEBUG_ERROR("failed to send mouse motion message");
}

void app_resyncMouseBasic()
{
  if (!g_cursor.guest.valid)
    return;
  g_cursor.projected.x = g_cursor.guest.x + g_cursor.guest.hx;
  g_cursor.projected.y = g_cursor.guest.y + g_cursor.guest.hy;
}

void app_updateWindowPos(int x, int y)
{
  g_state.windowPos.x = x;
  g_state.windowPos.y = y;
}

void app_handleResizeEvent(int w, int h, const struct Border border)
{
  memcpy(&g_state.border, &border, sizeof(border));

  /* don't do anything else if the window dimensions have not changed */
  if (g_state.windowW == w && g_state.windowH == h)
    return;

  g_state.windowW  = w;
  g_state.windowH  = h;
  g_state.windowCX = w / 2;
  g_state.windowCY = h / 2;
  core_updatePositionInfo();

  if (core_inputEnabled())
  {
    /* if the window is moved/resized causing a loss of focus while grabbed, it
     * makes it impossible to re-focus the window, so we quietly re-enter
     * capture if we were already in it */
    if (g_cursor.grab)
    {
      core_setGrabQuiet(false);
      core_setGrabQuiet(true);
    }
    core_alignToGuest();
  }
}

void app_handleCloseEvent(void)
{
  if (!g_params.ignoreQuit || !g_cursor.inView)
    g_state.state = APP_STATE_SHUTDOWN;
}

void app_setFullscreen(bool fs)
{
  g_state.ds->setFullscreen(fs);
}

bool app_getFullscreen(void)
{
  return g_state.ds->getFullscreen();
}

bool app_getProp(LG_DSProperty prop, void * ret)
{
  return g_state.ds->getProp(prop, ret);
}

#ifdef ENABLE_EGL
EGLDisplay app_getEGLDisplay(void)
{
  return g_state.ds->getEGLDisplay();
}

EGLNativeWindowType app_getEGLNativeWindow(void)
{
  return g_state.ds->getEGLNativeWindow();
}

void app_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
  g_state.ds->eglSwapBuffers(display, surface);
}
#endif

#ifdef ENABLE_OPENGL
LG_DSGLContext app_glCreateContext(void)
{
  return g_state.ds->glCreateContext();
}

void app_glDeleteContext(LG_DSGLContext context)
{
  g_state.ds->glDeleteContext(context);
}

void app_glMakeCurrent(LG_DSGLContext context)
{
  g_state.ds->glMakeCurrent(context);
}

void app_glSetSwapInterval(int interval)
{
  g_state.ds->glSetSwapInterval(interval);
}

void app_glSwapBuffers(void)
{
  g_state.ds->glSwapBuffers();
}
#endif

void app_alert(LG_MsgAlert type, const char * fmt, ...)
{
  if (!g_state.lgr || !g_params.showAlerts)
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

KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque, const char * description)
{
  // don't allow duplicate binds
  if (g_state.bindings[sc])
  {
    DEBUG_INFO("Key already bound");
    return NULL;
  }

  KeybindHandle handle = (KeybindHandle)malloc(sizeof(struct KeybindHandle));
  handle->sc       = sc;
  handle->callback = callback;
  handle->opaque   = opaque;

  g_state.bindings[sc] = handle;
  g_state.keyDescription[sc] = description;
  return handle;
}

void app_releaseKeybind(KeybindHandle * handle)
{
  if (!*handle)
    return;

  g_state.bindings[(*handle)->sc] = NULL;
  free(*handle);
  *handle = NULL;
}

void app_releaseAllKeybinds(void)
{
  for(int i = 0; i < KEY_MAX; ++i)
    if (g_state.bindings[i])
    {
      free(g_state.bindings[i]);
      g_state.bindings[i] = NULL;
    }
}
