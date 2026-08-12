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

#include "app_internal.h"

#include "main.h"
#include "core.h"
#include "util.h"
#include "clipboard.h"
#include "render_queue.h"
#include "evdev.h"
#include "input.h"

#include "kb.h"

#include "common/debug.h"
#include "common/rects.h"
#include "common/stringutils.h"
#include "interface/overlay.h"
#include "overlays.h"

#include "cimgui.h"

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define SHADER_MOUSE_VALID (UINT32_C(1) << 31)

#define MTRACE(fmt, ...) \
  do \
  { \
    const uint64_t seq = app_mouseSeq(); \
    if (seq) \
      app_mouseTrace(__FILE__, __LINE__, __FUNCTION__, seq, \
          "app." fmt, ##__VA_ARGS__); \
  } \
  while (0)

extern _Atomic(enum RunState) p_appState;

enum RunState app_getState(void)
{
  return atomic_load_explicit(&p_appState, memory_order_acquire);
}

void app_setState(enum RunState state)
{
  if (state == APP_STATE_SHUTDOWN)
  {
    atomic_store_explicit(&p_appState, state, memory_order_release);
    return;
  }

  enum RunState current = atomic_load_explicit(
      &p_appState, memory_order_acquire);
  while (current != APP_STATE_SHUTDOWN &&
      !atomic_compare_exchange_weak_explicit(&p_appState, &current, state,
        memory_order_acq_rel, memory_order_acquire));
}

bool app_transitionState(enum RunState from, enum RunState to)
{
  if (from == APP_STATE_SHUTDOWN)
    return false;

  return atomic_compare_exchange_strong_explicit(
      &p_appState, &from, to, memory_order_acq_rel, memory_order_acquire);
}

bool app_isRunning(void)
{
  const enum RunState state = app_getState();
  return
    state == APP_STATE_RUNNING ||
    state == APP_STATE_RESTART;
}

bool app_isCaptureMode(void)
{
  return g_cursor.grab;
}

uint64_t app_mouseSeq(void)
{
  static atomic_uint_fast64_t seq;

  if (!g_params.mouseTrace)
    return 0;

  return atomic_fetch_add_explicit(&seq, 1, memory_order_relaxed) + 1;
}

void app_mouseTrace(const char * file, unsigned int line,
    const char * function, uint64_t seq, const char * format, ...)
{
  char message[1024];
  va_list ap;
  va_start(ap, format);
  const int result = vsnprintf(message, sizeof(message), format, ap);
  va_end(ap);
  if (result < 0)
    return;

  flockfile(stderr);
  debug_info(file, line, function, "Mouse %06" PRIu64 ": %s", seq,
      message);
  funlockfile(stderr);
}

bool app_isCaptureOnlyMode(void)
{
  return g_params.captureInputOnly;
}

bool app_isFormatValid(void)
{
  return g_state.formatValid;
}

bool app_isOverlayMode(void)
{
  if (g_state.overlayInput)
    return true;

  bool result = false;
  struct Overlay * overlay;
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    if (overlay->ops->needs_overlay && overlay->ops->needs_overlay(overlay))
    {
      result = true;
      break;
    }
  }
  ll_unlock(g_state.overlays);

  return result;
}

void app_updateCursorPos(double x, double y)
{
  const bool overlay = app_isOverlayMode();
  if (overlay)
  {
    g_cursor.pos.x       = x;
    g_cursor.pos.y       = y;
    g_cursor.valid       = true;
    g_cursor.motionValid = false;
  }
  else
    core_handleMousePosition(x, y);

  MTRACE("pos pos=%.3f,%.3f inWin=%d inView=%d grab=%d",
      x, y, g_cursor.inWindow, g_cursor.inView, g_cursor.grab);

  if (overlay)
    g_state.io->MousePos = (ImVec2) { x, y };
}

void app_updateMouseState(void)
{
  LG_MouseState state = { 0 };

  if (g_cursor.guest.valid &&
      g_state.srcSize.x > 0 && g_state.srcSize.y > 0)
  {
    state.x = (float)(g_cursor.guest.x + g_cursor.guest.hx) /
      g_state.srcSize.x;
    state.y = (float)(g_cursor.guest.y + g_cursor.guest.hy) /
      g_state.srcSize.y;
    state.valid = true;
  }

  if (!state.valid)
  {
    atomic_fetch_and_explicit(&g_state.shaderMouseState,
        ~SHADER_MOUSE_VALID, memory_order_release);
    return;
  }

  uint32_t position[2];
  memcpy(position + 0, &state.x, sizeof(state.x));
  memcpy(position + 1, &state.y, sizeof(state.y));
  const uint64_t packedPosition =
    (uint64_t)position[0] | (uint64_t)position[1] << 32;

  atomic_store_explicit(&g_state.shaderMousePosition, packedPosition,
      memory_order_release);
  atomic_fetch_or_explicit(&g_state.shaderMouseState,
      SHADER_MOUSE_VALID, memory_order_release);
}

static void app_updateMouseButtons(void)
{
  const uint_least32_t buttons =
    (g_cursor.buttons >> 1) & ~SHADER_MOUSE_VALID;
  uint_least32_t current = atomic_load_explicit(&g_state.shaderMouseState,
      memory_order_relaxed);

  for (;;)
  {
    const uint_least32_t updated =
      (current & SHADER_MOUSE_VALID) | buttons;
    if (atomic_compare_exchange_weak_explicit(&g_state.shaderMouseState,
          &current, updated, memory_order_release, memory_order_relaxed))
      return;
  }
}

void app_getMouseState(LG_MouseState * state)
{
  const uint32_t packedState = atomic_load_explicit(
      &g_state.shaderMouseState, memory_order_acquire);
  const uint64_t packedPosition = atomic_load_explicit(
      &g_state.shaderMousePosition, memory_order_acquire);

  const uint32_t position[2] = {
    packedPosition,
    packedPosition >> 32,
  };

  memcpy(&state->x, position + 0, sizeof(state->x));
  memcpy(&state->y, position + 1, sizeof(state->y));
  state->buttons = packedState & ~SHADER_MOUSE_VALID;
  state->valid = !!(packedState & SHADER_MOUSE_VALID);
}

void app_handleFocusEvent(bool focused)
{
  MTRACE("focus set=%d old=%d inWin=%d inView=%d grab=%d",
      focused, g_state.focused, g_cursor.inWindow, g_cursor.inView,
      g_cursor.grab);

  if (g_state.focused == focused)
    return;

  g_state.focused       = focused;
  g_cursor.motionValid  = false;
  const bool inputEnabled = core_inputEnabled();

  // release any imgui buttons/keys if we lost focus
  if (!focused && app_isOverlayMode())
    core_resetOverlayInputState();

  if (!focused)
  {
    core_setGrabQuiet(false);
    core_setCursorInView(false);
  }

  if (!inputEnabled)
  {
    if (!focused && g_params.minimizeOnFocusLoss && app_getFullscreen())
      g_state.ds->minimize();
    return;
  }

  if (!focused)
  {
    if (g_params.releaseKeysOnFocusLoss)
      lgInput_releaseKeys();

    g_state.escapeActive = false;

    if (!g_params.showCursorDot)
      g_state.ds->setPointer(LG_POINTER_NONE);

    if (g_params.minimizeOnFocusLoss)
      g_state.ds->minimize();
  }
  else
    if (g_params.captureOnFocus)
      core_setGrab(true);

  g_cursor.realign = true;
  g_state.ds->realignPointer();
}

void app_handleEnterEvent(bool entered)
{
  MTRACE("enter set=%d inWin=%d inView=%d grab=%d",
      entered, g_cursor.inWindow, g_cursor.inView, g_cursor.grab);

  g_cursor.motionValid = false;

  if (entered)
  {
    g_cursor.inWindow = true;
    if (!core_inputEnabled())
      return;

    g_cursor.realign = true;
    if (g_cursor.grab)
      core_setCursorInView(true);
    else
      core_handleMouseAbsolute();
  }
  else
  {
    g_cursor.inWindow = false;
    core_setCursorInView(false);

    // stop the user being able to drag windows off the screen and work around
    // the mouse button release being missed due to not being in capture mode.
    if (app_isOverlayMode())
    {
      g_state.io->MouseDown[ImGuiMouseButton_Left  ] = false;
      g_state.io->MouseDown[ImGuiMouseButton_Right ] = false;
      g_state.io->MouseDown[ImGuiMouseButton_Middle] = false;
    }

    if (!core_inputEnabled())
      return;

    if (!g_params.alwaysShowCursor)
      g_cursor.draw = false;
    g_cursor.redraw = true;
    app_invalidateWindow(false);
  }
}

void app_clipboardRelease(void)
{
  lgClipboard_release();
}

void app_clipboardNotifyTypes(const LG_ClipboardData types[], int count)
{
  if (count < 0)
    return;
  lgClipboard_notifyTypes(types, (size_t)count);
}

void app_clipboardData(LG_ClipboardRequest request,
    const LG_ClipboardData type, const void * data, size_t size)
{
  lgClipboard_data(request, type, data, size);
}

void app_clipboardAbort(LG_ClipboardRequest request)
{
  lgClipboard_abort(request);
}

bool app_clipboardRequest(LG_ClipboardData type,
    LG_ClipboardReplyFn replyFn, void * opaque)
{
  return lgClipboard_request(type, replyFn, opaque);
}

static int mapInputToImGuiButton(uint32_t button)
{
  switch (button)
  {
    case 1: // left
      return ImGuiMouseButton_Left;
    case 2: // middle
      return ImGuiMouseButton_Middle;
    case 3: // right
      return ImGuiMouseButton_Right;
  }

  return -1;
}

void app_handleButtonPress(int button)
{
  g_cursor.buttons |= (1U << button);
  app_updateMouseButtons();

  MTRACE("button down=%d buttons=%u inView=%d grab=%d",
      button, g_cursor.buttons, g_cursor.inView, g_cursor.grab);

  if (app_isOverlayMode())
  {
    int igButton = mapInputToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = true;
    return;
  }

  if (!core_inputEnabled() || !g_cursor.inView || !g_cursor.viewReq)
    return;

  core_handleMouseAbsolute();
  if (!lgInput_mousePress(button))
    DEBUG_ERROR("app_handleButtonPress: failed to send message");
}

void app_handleButtonRelease(int button)
{
  g_cursor.buttons &= ~(1U << button);
  app_updateMouseButtons();

  MTRACE("button up=%d buttons=%u inView=%d grab=%d",
      button, g_cursor.buttons, g_cursor.inView, g_cursor.grab);

  if (app_isOverlayMode())
  {
    int igButton = mapInputToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = false;
    return;
  }

  if (!core_inputEnabled())
    return;

  if (!lgInput_mouseRelease(button))
    DEBUG_ERROR("app_handleButtonRelease: failed to send message");
}

void app_handleWheelMotion(double motion)
{
  if (app_isOverlayMode())
    g_state.io->MouseWheel -= motion;
}

void app_handleKeyPressInternal(int sc)
{
  if (!app_isOverlayMode() || !g_state.io->WantCaptureKeyboard)
  {
    if (sc == g_params.escapeKey && !g_state.escapeActive)
    {
      g_state.escapeActive   = true;
      g_state.escapeTime     = microtime();
      g_state.escapeAction   = -1;
      g_state.escapeKeys[sc] = true;
      return;
    }

    if (g_state.escapeActive)
    {
      g_state.escapeAction   = sc;
      g_state.escapeKeys[sc] = true;
      KeybindHandle handle;
      ll_forEachNL(g_state.bindings, item, handle)
      {
        if (handle->sc == sc)
        {
          handle->callback(sc, handle->opaque);
          break;
        }
      }
      return;
    }
  }

  g_state.escapeKeys[sc] = false;

  if (app_isOverlayMode())
  {
    if (sc == KEY_ESC)
      app_setOverlay(false);
    else
    {
      if (linux_to_imgui[sc])
        ImGuiIO_AddKeyEvent(g_state.io, linux_to_imgui[sc], true);
    }
    return;
  }

  if (!core_inputEnabled())
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  if (!lgInput_keyDown(sc))
    DEBUG_ERROR("app_handleKeyPress: failed to send message");
}

void app_handleKeyReleaseInternal(int sc)
{
  if (g_state.escapeActive && sc == g_params.escapeKey)
  {
    if (g_state.escapeAction == -1)
    {
      if (!g_state.escapeHelp && lgInput_available() &&
          !app_isOverlayMode())
        core_setGrab(!g_cursor.grab);
    }

    g_state.escapeActive = false;
  }

  if (g_state.escapeKeys[sc])
  {
    g_state.escapeKeys[sc] = false;
    return;
  }

  if (app_isOverlayMode())
  {
    if (linux_to_imgui[sc])
      ImGuiIO_AddKeyEvent(g_state.io, linux_to_imgui[sc], false);
    return;
  }

  if (!core_inputEnabled())
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  if (!lgInput_keyUp(sc))
    DEBUG_ERROR("app_handleKeyRelease: failed to send message");
}

void app_handleKeyPress(int sc)
{
  if (!evdev_isExclusive())
    app_handleKeyPressInternal(sc);
}

void app_handleKeyRelease(int sc)
{
  if (!evdev_isExclusive())
    app_handleKeyReleaseInternal(sc);
}

void app_handleKeyboardTyped(const char * typed)
{
  app_registerImGuiText(typed);
  ImGuiIO_AddInputCharactersUTF8(g_state.io, typed);
}

void app_handleKeyboardModifiers(bool ctrl, bool shift, bool alt, bool super)
{
  g_state.modCtrl  = ctrl;
  g_state.modShift = shift;
  g_state.modAlt   = alt;
  g_state.modSuper = super;
}

void app_handleKeyboardLEDs(bool numLock, bool capsLock, bool scrollLock)
{
  if (!core_inputEnabled())
    return;

  if (!lgInput_keyboardLEDs(numLock, capsLock, scrollLock))
    DEBUG_ERROR("app_handleKeyboardLEDs: failed to send message");
}

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy)
{
  MTRACE("rel delta=%.3f,%.3f raw=%.3f,%.3f inWin=%d inView=%d "
      "grab=%d", normx, normy, rawx, rawy, g_cursor.inWindow,
      g_cursor.inView, g_cursor.grab);

  if (app_isOverlayMode())
    return;

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

void app_handleGrabEvent(bool active)
{
  core_handleGrabEvent(active);
}

void app_updateWindowPos(int x, int y)
{
  g_state.windowPos.x = x;
  g_state.windowPos.y = y;
}

void app_handleResizeEvent(int w, int h, double scale, const struct Border border)
{
  MTRACE("resize win=%dx%d scale=%.3f border=%d,%d,%d,%d",
      w, h, scale, border.left, border.top, border.right, border.bottom);

  memcpy(&g_state.border, &border, sizeof(border));

  /* don't do anything else if the window dimensions have not changed */
  if (g_state.windowW == w && g_state.windowH == h && g_state.windowScale == scale)
    return;

  g_state.windowW     = w;
  g_state.windowH     = h;
  g_state.windowCX    = w / 2;
  g_state.windowCY    = h / 2;
  g_state.windowScale = scale;
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

void app_invalidateWindow(bool full)
{
  if (full)
    atomic_store(&g_state.invalidateWindow, true);

  if (g_state.dsInitialized && g_state.jitRender && g_state.ds->stopWaitFrame)
    g_state.ds->stopWaitFrame();

  lgSignalEvent(g_state.frameEvent);
}

void app_handleCloseEvent(void)
{
  if (!g_params.ignoreQuit || !g_cursor.inView)
    app_setState(APP_STATE_SHUTDOWN);
}

void app_handleRenderEvent(const uint64_t timeUs)
{
  bool invalidate = false;
  if (!g_state.escapeActive)
  {
    if (g_state.escapeHelp)
    {
      g_state.escapeHelp = false;
      invalidate = true;
    }
  }
  else
  {
    if (!g_state.escapeHelp && timeUs - g_state.escapeTime > g_params.helpMenuDelayUs)
    {
      g_state.escapeHelp = true;
      invalidate = true;
    }
  }

  if (invalidate)
    app_invalidateWindow(false);
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

bool app_getHDRDescFailed(void)
{
  return atomic_load(&g_state.hdrDescFailed);
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

bool app_eglSwapBuffers(EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count, uint64_t frameToken,
    uint64_t * swapTime, bool * presentTracked)
{
  return g_state.ds->eglSwapBuffers(display, surface, damage, count,
      frameToken, swapTime, presentTracked);
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
  overlayAlert_show(type, fmt, args);
  va_end(args);
}

MsgBoxHandle app_msgBox(const char * caption, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  MsgBoxHandle handle = overlayMsg_show(
      caption, NULL, NULL, NULL, NULL, fmt, args);
  va_end(args);

  return handle;
}

MsgBoxHandle app_msgBoxWithClose(const char * caption,
    MsgBoxCloseCallback callback, void * opaque, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  MsgBoxHandle handle = overlayMsg_show(
      caption, NULL, NULL, callback, opaque, fmt, args);
  va_end(args);

  return handle;
}

MsgBoxHandle app_confirmMsgBox(const char * caption,
    MsgBoxConfirmCallback callback, void * opaque, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  MsgBoxHandle handle = overlayMsg_show(
      caption, callback, opaque, NULL, NULL, fmt, args);
  va_end(args);

  return handle;
}

void app_msgBoxClose(MsgBoxHandle handle)
{
  if (!handle)
    return;

  overlayMsg_close(handle);
}

void app_showRecord(bool show)
{
  overlayStatus_set(LG_USER_STATUS_RECORDING, show);
}

KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque,
    const char * description)
{
  if (!callback)
  {
    DEBUG_ERROR("invalid keybind callback");
    return NULL;
  }

  if (sc <= KEY_RESERVED || sc >= KEY_MAX || !linux_to_display[sc])
  {
    DEBUG_ERROR("invalid keybind keycode: %d", sc);
    return NULL;
  }

  KeybindHandle handle;

  // don't allow duplicate binds
  ll_forEachNL(g_state.bindings, item, handle)
  {
    if (handle->sc == sc)
    {
      DEBUG_INFO("Key already bound");
      return NULL;
    }
  }

  handle = malloc(sizeof(*handle));
  if (!handle)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  handle->sc          = sc;
  handle->callback    = callback;
  handle->description = description;
  handle->opaque      = opaque;

  ll_push(g_state.bindings, handle);
  return handle;
}

void app_releaseKeybind(KeybindHandle * handle)
{
  if (!*handle)
    return;

  ll_removeData(g_state.bindings, *handle);
  free(*handle);
  *handle = NULL;
}

void app_releaseAllKeybinds(void)
{
  KeybindHandle * handle;
  while(ll_shift(g_state.bindings, (void **)&handle))
    free(handle);
}

GraphHandle app_registerGraph(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn)
{
  return overlayGraph_register(name, buffer, min, max, formatFn);
}

void app_unregisterGraph(GraphHandle handle)
{
  overlayGraph_unregister(handle);
}

void app_invalidateGraph(GraphHandle handle)
{
  overlayGraph_invalidate(handle);
}

void app_setGraphCompact(GraphHandle handle, bool compact)
{
  overlayGraph_setCompact(handle, compact);
}

void app_setFrameImportTiming(uint64_t importTime, uint64_t importWaitTime)
{
  g_state.frameImportTime     = importTime;
  g_state.frameImportWaitTime = importWaitTime;
}

void app_registerOverlay(const struct LG_OverlayOps * ops, const void * params)
{
  ASSERT_LG_OVERLAY_VALID(ops);

  struct Overlay * overlay = malloc(sizeof(*overlay));
  if (!overlay)
  {
    DEBUG_ERROR("out of ram");
    return;
  }

  overlay->ops           = ops;
  overlay->params        = params;
  overlay->udata         = NULL;
  overlay->lastRectCount = 0;
  ll_push(g_state.overlays, overlay);

  if (ops->earlyInit)
    ops->earlyInit();
}

void app_initOverlays(void)
{
  struct Overlay * overlay;
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    DEBUG_ASSERT(overlay->ops);
    if (!overlay->ops->init(&overlay->udata, overlay->params))
    {
      DEBUG_ERROR("Overlay `%s` failed to initialize", overlay->ops->name);
      overlay->ops->free(overlay->udata);
      ll_removeNL(g_state.overlays, item);
      free(item);
      free(overlay);
    }
  }
  ll_unlock(g_state.overlays);

  /* Do not seed ImGui's clock with absolute host uptime. ImGui stores the
   * background and foreground draw-list activity time as a float. */
  g_state.lastImGuiFrame = nanotime();
}

static inline void mergeRect(struct Rect * dest, const struct Rect * a, const struct Rect * b)
{
  int x2 = max(a->x + a->w, b->x + b->w);
  int y2 = max(a->y + a->h, b->y + b->h);

  dest->x = min(a->x, b->x);
  dest->y = min(a->y, b->y);
  dest->w = x2 - dest->x;
  dest->h = y2 - dest->y;
}

static inline LG_DSPointer mapImGuiCursor(ImGuiMouseCursor cursor)
{
  switch (cursor)
  {
    case ImGuiMouseCursor_None:
      return LG_POINTER_NONE;
    case ImGuiMouseCursor_Arrow:
      return LG_POINTER_ARROW;
    case ImGuiMouseCursor_TextInput:
      return LG_POINTER_INPUT;
    case ImGuiMouseCursor_ResizeAll:
      return LG_POINTER_MOVE;
    case ImGuiMouseCursor_ResizeNS:
      return LG_POINTER_RESIZE_NS;
    case ImGuiMouseCursor_ResizeEW:
      return LG_POINTER_RESIZE_EW;
    case ImGuiMouseCursor_ResizeNESW:
      return LG_POINTER_RESIZE_NESW;
    case ImGuiMouseCursor_ResizeNWSE:
      return LG_POINTER_RESIZE_NWSE;
    case ImGuiMouseCursor_Hand:
      return LG_POINTER_HAND;
    case ImGuiMouseCursor_NotAllowed:
      return LG_POINTER_NOT_ALLOWED;
    default:
      return LG_POINTER_ARROW;
  }
}

bool app_overlayNeedsRender(void)
{
  if (app_isOverlayMode())
    return true;

  bool result = false;
  struct Overlay * overlay;
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    if (!overlay->ops->needs_render)
      continue;

    if (overlay->ops->needs_render(overlay->udata, false))
    {
      result = true;
      break;
    }
  }
  ll_unlock(g_state.overlays);

  return result;
}

bool app_overlayNeedsFullRender(void)
{
  bool result = false;
  struct Overlay * overlay;
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    if (!overlay->ops->needs_full_render)
      continue;

    if (overlay->ops->needs_full_render(overlay->udata))
    {
      result = true;
      break;
    }
  }
  ll_unlock(g_state.overlays);

  return result;
}

int app_renderOverlay(struct Rect * rects, int maxRects)
{
  int  totalRects  = 0;
  bool totalDamage = false;
  struct Overlay * overlay;
  struct Rect buffer[MAX_OVERLAY_RECTS];

  g_state.io->KeyCtrl  = g_state.modCtrl;
  g_state.io->KeyShift = g_state.modShift;
  g_state.io->KeyAlt   = g_state.modAlt;
  g_state.io->KeySuper = g_state.modSuper;

  uint64_t now = nanotime();
  g_state.io->DeltaTime  = (now - g_state.lastImGuiFrame) * 1e-9f;
  g_state.lastImGuiFrame = now;

render_again:

  igNewFrame();

  const bool overlayMode = app_isOverlayMode();
  if (overlayMode && g_params.overlayDim)
  {
    totalDamage = true;
    ImDrawList_AddRectFilled(igGetBackgroundDrawList_Nil(), (ImVec2) { 0.0f , 0.0f },
      g_state.io->DisplaySize,
      igGetColorU32_Col(ImGuiCol_ModalWindowDimBg, 1.0f),
      0, 0);
  }

  const bool msgModal = overlayMsg_modal();

  // render the overlays
  ll_lock(g_state.overlays);
  ll_forEachNL(g_state.overlays, item, overlay)
  {
    if (msgModal && overlay->ops != &LGOverlayMsg)
      continue;

    const int written =
      overlay->ops->render(overlay->udata, overlayMode,
          buffer, MAX_OVERLAY_RECTS);

    for (int i = 0; i < written; ++i)
      rectScaleOutward(buffer + i, g_state.windowScale);

    // It is an error to run out of rectangles, because we will not be able to
    // correctly calculate the damage of the next frame.
    DEBUG_ASSERT(written >= 0);

    const int toAdd = max(written, overlay->lastRectCount);
    totalDamage |= toAdd > maxRects;

    if (!totalDamage && toAdd)
    {
      int i = 0;
      for (; i < overlay->lastRectCount && i < written; ++i)
        mergeRect(rects + i, buffer + i, overlay->lastRects + i);

      // only one of the following memcpys will copy non-zero bytes.
      memcpy(rects + i, buffer + i, (written - i) * sizeof(struct Rect));
      memcpy(rects + i, overlay->lastRects + i, (overlay->lastRectCount - i) * sizeof(struct Rect));

      rects      += toAdd;
      totalRects += toAdd;
      maxRects   -= toAdd;
    }

    memcpy(overlay->lastRects, buffer, sizeof(struct Rect) * written);
    overlay->lastRectCount = written;
  }
  ll_unlock(g_state.overlays);

  if (overlayMode)
  {
    ImGuiMouseCursor cursor = igGetMouseCursor();
    if (cursor != g_state.cursorLast)
    {
      g_state.ds->setPointer(mapImGuiCursor(cursor));
      g_state.cursorLast = cursor;
    }
  }

  igRender();

  /* imgui requires two passes to calculate the bounding box of auto sized
   * windows, this is by design
   * ref: https://github.com/ocornut/imgui/issues/2158#issuecomment-434223618
   */
  if (g_state.renderImGuiTwice)
  {
    g_state.renderImGuiTwice = false;
    goto render_again;
  }

  return totalDamage ? -1 : totalRects;
}

void app_freeOverlays(void)
{
  struct Overlay * overlay;
  while(ll_shift(g_state.overlays, (void **)&overlay))
  {
    overlay->ops->free(overlay->udata);
    free(overlay);
  }
}

void app_setOverlay(bool enable)
{
  if (g_state.overlayInput == enable)
    return;

  g_state.overlayInput = enable;
  core_updateOverlayState();
}

void app_overlayConfigRegister(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  overlayConfig_register(title, callback, udata);
}

void app_overlayConfigRegisterTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata)
{
  overlayConfig_registerTab(title, callback, udata);
}

void app_registerImGuiText(const char * text)
{
  if (!util_uiFontAddText(text))
    return;

  atomic_store(&g_state.fontDirty, true);
  app_invalidateOverlay(false);
}

void app_invalidateOverlay(bool renderTwice)
{
  if (app_getState() == APP_STATE_SHUTDOWN)
    return;

  if (renderTwice)
    g_state.renderImGuiTwice = true;
  app_invalidateWindow(false);
}

bool app_guestIsLinux(void)
{
  return g_state.guestOS == LG_TRANSPORT_OS_LINUX;
}

bool app_guestIsWindows(void)
{
  return g_state.guestOS == LG_TRANSPORT_OS_WINDOWS;
}

bool app_guestIsOSX(void)
{
  return g_state.guestOS == LG_TRANSPORT_OS_OSX;
}

bool app_guestIsBSD(void)
{
  return g_state.guestOS == LG_TRANSPORT_OS_BSD;
}

bool app_guestIsOther(void)
{
  return g_state.guestOS == LG_TRANSPORT_OS_OTHER;
}

void app_stopVideo(bool stop)
{
  if (atomic_load_explicit(&g_state.stopVideo, memory_order_acquire) == stop)
    return;

  // do not change the state if the host app is not connected
  if (!atomic_load_explicit(&g_state.lgHostConnected, memory_order_acquire))
    return;

  atomic_store_explicit(&g_state.stopVideo, stop, memory_order_release);

  app_alert(
    LG_ALERT_INFO,
    stop ? "Video Stream Disabled" : "Video Stream Enabled"
  );

  if (stop)
  {
    if (g_state.videoOps->type == LG_VIDEO_TYPE_FRAME)
      core_stopVideoThreads();
    else
      g_state.videoOps->swSurface->setActive(
          g_state.transport.handle, false);
  }
  else
  {
    if (g_state.videoOps->type == LG_VIDEO_TYPE_FRAME)
    {
      if (!core_startVideoThreads())
      {
        atomic_store_explicit(
            &g_state.stopVideo, true, memory_order_release);
        app_alert(LG_ALERT_ERROR, "Failed to enable the video stream");
      }
    }
    else
    {
      g_state.videoOps->swSurface->setActive(
          g_state.transport.handle, true);
      app_useVideoSource(LG_VIDEO_SOURCE_PRIMARY);
    }
  }
}

static RenderQueueSource renderSource(LG_VideoSource source)
{
  switch(source)
  {
    case LG_VIDEO_SOURCE_NONE:
      return RENDER_QUEUE_SOURCE_NONE;

    case LG_VIDEO_SOURCE_PRIMARY:
      return RENDER_QUEUE_SOURCE_PRIMARY;

    case LG_VIDEO_SOURCE_FALLBACK:
      return RENDER_QUEUE_SOURCE_FALLBACK;

    case LG_VIDEO_SOURCE_COUNT:
      break;
  }

  return RENDER_QUEUE_SOURCE_NONE;
}

static bool useVideoSourceLocked(LG_VideoSource source,
    LG_VideoSource previousRequested, bool inputAvailable)
{
  const RenderQueueSource queueSource = renderSource(source);
  if (source == LG_VIDEO_SOURCE_NONE)
    return renderQueue_sourceTransition(
        queueSource, 0, false, NULL) != 0;

  struct VideoSourceState * state = &g_state.videoSource[source];
  if (previousRequested != source)
  {
    atomic_store_explicit(
        &state->transitionSerial, 0, memory_order_release);
    atomic_store_explicit(
        &state->transitionPending, false, memory_order_release);
  }

  const uint64_t generation = atomic_load_explicit(
      &state->generation, memory_order_acquire);
  if (!generation || !atomic_load_explicit(
        &state->ready, memory_order_acquire))
    return false;

  const LG_VideoSource applied = atomic_load_explicit(
      &g_state.videoSourceApplied, memory_order_acquire);
  const uint64_t appliedGeneration = atomic_load_explicit(
      &g_state.videoSourceAppliedGeneration, memory_order_acquire);
  const int width = atomic_load_explicit(
      &state->width, memory_order_relaxed);
  const int height = atomic_load_explicit(
      &state->height, memory_order_relaxed);
  const LG_RendererRotate rotate = atomic_load_explicit(
      &state->rotate, memory_order_relaxed);
  if (width <= 0 || height <= 0)
    return false;

  const LG_RendererRotate appliedRotate = atomic_load_explicit(
      &state->appliedRotate, memory_order_acquire);
  const bool geometryApplied =
    atomic_load_explicit(&state->appliedWidth, memory_order_relaxed) ==
      (unsigned int)width &&
    atomic_load_explicit(&state->appliedHeight, memory_order_relaxed) ==
      (unsigned int)height &&
    appliedRotate == rotate;
  if (applied == source && appliedGeneration == generation &&
      previousRequested == source && geometryApplied)
    return true;

  if (previousRequested == source && atomic_load_explicit(
        &state->transitionPending, memory_order_acquire))
    return true;

  if (state->cursorStateValid)
    renderQueue_sourceCursorState(queueSource, generation,
        state->cursorVisible &&
          (source == LG_VIDEO_SOURCE_FALLBACK ||
            g_cursor.draw || !inputAvailable),
        state->cursorX, state->cursorY,
        state->cursorHX, state->cursorHY);

  const bool configure = state->swSurface && state->configurePending;
  atomic_store_explicit(
      &state->transitionPending, true, memory_order_release);
  const uint64_t serial = configure ?
    renderQueue_sourceSwSurfaceConfigureTransition(
        queueSource, generation, width, height,
        &state->transitionSerial) :
    renderQueue_sourceTransition(
        queueSource, generation, state->swSurface,
        &state->transitionSerial);
  if (!serial)
  {
    atomic_store_explicit(
        &state->transitionPending, false, memory_order_release);
    return false;
  }
  return true;
}

bool app_useVideoSource(LG_VideoSource source)
{
  if (source < LG_VIDEO_SOURCE_NONE || source >= LG_VIDEO_SOURCE_COUNT)
    return false;

  if (source == LG_VIDEO_SOURCE_PRIMARY && !atomic_load_explicit(
        &g_state.lgHostConnected, memory_order_acquire))
    return false;

  if (source == LG_VIDEO_SOURCE_FALLBACK && !g_state.fallback)
    return false;

  const bool inputAvailable = lgInput_available();
  LG_LOCK(g_state.videoSourceLock);
  const LG_VideoSource previousRequested = atomic_load_explicit(
      &g_state.videoSourceRequested, memory_order_acquire);
  atomic_store_explicit(
      &g_state.videoSourceRequested, source, memory_order_release);
  if (source == LG_VIDEO_SOURCE_FALLBACK)
    lgTransportFallback_requestVideoActive(g_state.fallback, true);
  const bool result =
    useVideoSourceLocked(source, previousRequested, inputAvailable);
  LG_UNLOCK(g_state.videoSourceLock);
  return result;
}

void app_refreshVideoSource(void)
{
  const bool inputAvailable = lgInput_available();
  LG_LOCK(g_state.videoSourceLock);
  const LG_VideoSource source = atomic_load_explicit(
      &g_state.videoSourceRequested, memory_order_acquire);
  if (source != LG_VIDEO_SOURCE_NONE &&
      (source != LG_VIDEO_SOURCE_PRIMARY || atomic_load_explicit(
        &g_state.lgHostConnected, memory_order_acquire)) &&
      (source != LG_VIDEO_SOURCE_FALLBACK || g_state.fallback))
    useVideoSourceLocked(source, source, inputAvailable);
  LG_UNLOCK(g_state.videoSourceLock);
}
