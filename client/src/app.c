/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "app.h"

#include "main.h"
#include "core.h"
#include "util.h"
#include "clipboard.h"

#include "ll.h"
#include "kb.h"

#include "common/debug.h"
#include "common/stringutils.h"
#include "interface/overlay.h"
#include "overlays.h"

#include "cimgui.h"

#include <stdarg.h>
#include <math.h>
#include <string.h>

#define ALERT_TIMEOUT 2000000

bool app_isRunning(void)
{
  return
    g_state.state == APP_STATE_RUNNING ||
    g_state.state == APP_STATE_RESTART;
}

bool app_isCaptureMode(void)
{
  return g_cursor.grab;
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
  return g_state.overlayInput;
}

void app_updateCursorPos(double x, double y)
{
  g_cursor.pos.x = x;
  g_cursor.pos.y = y;
  g_cursor.valid = true;

  if (g_state.overlayInput)
    g_state.io->MousePos = (ImVec2) { x, y };
}

void app_handleFocusEvent(bool focused)
{
  g_state.focused = focused;

  // release any imgui buttons/keys if we lost focus
  if (!focused && g_state.overlayInput)
    core_resetOverlayInputState();

  if (!core_inputEnabled())
  {
    if (!focused && g_params.minimizeOnFocusLoss && app_getFullscreen())
      g_state.ds->minimize();
    return;
  }

  if (!focused)
  {
    core_setGrabQuiet(false);
    core_setCursorInView(false);

    if (g_params.releaseKeysOnFocusLoss)
      for (int key = 0; key < KEY_MAX; key++)
        if (g_state.keyDown[key])
          app_handleKeyRelease(key);

    g_state.escapeActive = false;

    if (!g_params.showCursorDot)
      g_state.ds->setPointer(LG_POINTER_NONE);

    if (g_params.minimizeOnFocusLoss)
      g_state.ds->minimize();
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

    // stop the user being able to drag windows off the screen and work around
    // the mouse button release being missed due to not being in capture mode.
    if (g_state.overlayInput)
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
  }
}

void app_clipboardRelease(void)
{
  if (!g_params.clipboardToVM)
    return;

  spice_clipboard_release();
}

void app_clipboardNotifyTypes(const LG_ClipboardData types[], int count)
{
  if (!g_params.clipboardToVM)
    return;

  if (count == 0)
  {
    spice_clipboard_release();
    return;
  }

  SpiceDataType conv[count];
  for(int i = 0; i < count; ++i)
    conv[i] = cb_lgTypeToSpiceType(types[i]);

  spice_clipboard_grab(conv, count);
}

void app_clipboardNotifySize(const LG_ClipboardData type, size_t size)
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

  struct CBRequest * cbr = malloc(sizeof(*cbr));

  cbr->type    = g_state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(g_state.cbRequestList, cbr);

  spice_clipboard_request(g_state.cbType);
}

static int mapSpiceToImGuiButton(uint32_t button)
{
  switch (button)
  {
    case 1:  // SPICE_MOUSE_BUTTON_LEFT
      return ImGuiMouseButton_Left;
    case 2:  // SPICE_MOUSE_BUTTON_MIDDLE
      return ImGuiMouseButton_Middle;
    case 3:  // SPICE_MOUSE_BUTTON_RIGHT
      return ImGuiMouseButton_Right;
  }

  return -1;
}

void app_handleButtonPress(int button)
{
  g_cursor.buttons |= (1U << button);

  if (g_state.overlayInput)
  {
    int igButton = mapSpiceToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = true;
    return;
  }

  if (!core_inputEnabled() || !g_cursor.inView)
    return;

  if (!spice_mouse_press(button))
    DEBUG_ERROR("app_handleButtonPress: failed to send message");
}

void app_handleButtonRelease(int button)
{
  g_cursor.buttons &= ~(1U << button);

  if (g_state.overlayInput)
  {
    int igButton = mapSpiceToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = false;
    return;
  }

  if (!core_inputEnabled())
    return;

  if (!spice_mouse_release(button))
    DEBUG_ERROR("app_handleButtonRelease: failed to send message");
}

void app_handleWheelMotion(double motion)
{
  if (g_state.overlayInput)
    g_state.io->MouseWheel -= motion;
}

void app_handleKeyPress(int sc)
{
  if (!g_state.overlayInput || !g_state.io->WantCaptureKeyboard)
  {
    if (sc == g_params.escapeKey && !g_state.escapeActive)
    {
      g_state.escapeActive = true;
      g_state.escapeTime   = microtime();
      g_state.escapeAction = -1;
      return;
    }

    if (g_state.escapeActive)
    {
      g_state.escapeAction = sc;
      return;
    }
  }

  if (g_state.overlayInput)
  {
    if (sc == KEY_ESC)
      app_setOverlay(false);
    else
      g_state.io->KeysDown[sc] = true;
    return;
  }

  if (!core_inputEnabled())
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  if (!g_state.keyDown[sc])
  {
    uint32_t ps2 = linux_to_ps2[sc];
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
      if (!g_state.escapeHelp && g_params.useSpiceInput && !g_state.overlayInput)
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

  if (g_state.overlayInput)
  {
    g_state.io->KeysDown[sc] = false;
    return;
  }

  if (!core_inputEnabled())
    return;

  // avoid sending key up events when we didn't send a down
  if (!g_state.keyDown[sc])
    return;

  if (g_params.ignoreWindowsKeys && (sc == KEY_LEFTMETA || sc == KEY_RIGHTMETA))
    return;

  uint32_t ps2 = linux_to_ps2[sc];
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

void app_handleKeyboardTyped(const char * typed)
{
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

  uint32_t modifiers =
    (scrollLock ? 1 /* SPICE_SCROLL_LOCK_MODIFIER */ : 0) |
    (numLock    ? 2 /* SPICE_NUM_LOCK_MODIFIER    */ : 0) |
    (capsLock   ? 4 /* SPICE_CAPS_LOCK_MODIFIER   */ : 0);

  if (!spice_key_modifiers(modifiers))
    DEBUG_ERROR("app_handleKeyboardLEDs: failed to send message");
}

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy)
{
  if (g_state.overlayInput)
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

// On some display servers normal cursor logic does not work due to the lack of
// cursor warp support. Instead, we attempt a best-effort emulation which works
// with a 1:1 mouse movement patch applied in the guest. For anything fancy, use
// capture mode.
void app_handleMouseBasic()
{
  /* do not pass mouse events to the guest if we do not have focus */
  if (!g_cursor.guest.valid || !g_state.haveSrcSize || !g_state.focused || g_state.overlayInput)
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

  int x = (int) round(util_clamp(guest.x, 0, g_state.srcSize.x) -
      g_cursor.projected.x);
  int y = (int) round(util_clamp(guest.y, 0, g_state.srcSize.y) -
      g_cursor.projected.y);

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

void app_handleResizeEvent(int w, int h, double scale, const struct Border border)
{
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
  lgSignalEvent(g_state.frameEvent);
}

void app_handleCloseEvent(void)
{
  if (!g_params.ignoreQuit || !g_cursor.inView)
    g_state.state = APP_STATE_SHUTDOWN;
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

  if (g_state.alertShow)
    if (g_state.alertTimeout < timeUs)
    {
      g_state.alertShow = false;
      free(g_state.alertMessage);
      g_state.alertMessage = NULL;
      invalidate = true;
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

#ifdef ENABLE_EGL
EGLDisplay app_getEGLDisplay(void)
{
  return g_state.ds->getEGLDisplay();
}

EGLNativeWindowType app_getEGLNativeWindow(void)
{
  return g_state.ds->getEGLNativeWindow();
}

void app_eglSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count)
{
  g_state.ds->eglSwapBuffers(display, surface, damage, count);
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

  char * buffer;

  va_list args;
  va_start(args, fmt);
  valloc_sprintf(&buffer, fmt, args);
  va_end(args);

  free(g_state.alertMessage);
  g_state.alertMessage = buffer;
  g_state.alertTimeout = microtime() + ALERT_TIMEOUT;
  g_state.alertType    = type;
  g_state.alertShow    = true;
  app_invalidateWindow(false);
}

KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque, const char * description)
{
  // don't allow duplicate binds
  if (g_state.bindings[sc])
  {
    DEBUG_INFO("Key already bound");
    return NULL;
  }

  KeybindHandle handle = malloc(sizeof(*handle));
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

GraphHandle app_registerGraph(const char * name, RingBuffer buffer, float min, float max)
{
  return overlayGraph_register(name, buffer, min, max);
}

void app_unregisterGraph(GraphHandle handle)
{
  overlayGraph_unregister(handle);
}

struct Overlay
{
  const struct LG_OverlayOps * ops;
  const void * params;
  void * udata;
  int lastRectCount;
  struct Rect lastRects[MAX_OVERLAY_RECTS];
};

void app_registerOverlay(const struct LG_OverlayOps * ops, const void * params)
{
  ASSERT_LG_OVERLAY_VALID(ops);

  struct Overlay * overlay = malloc(sizeof(*overlay));
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
  for (ll_reset(g_state.overlays);
      ll_walk(g_state.overlays, (void **)&overlay); )
  {
    if (!overlay->ops->init(&overlay->udata, overlay->params))
    {
      DEBUG_ERROR("Overlay `%s` failed to initialize", overlay->ops->name);
      overlay->ops = NULL;
    }
  }
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
  struct Overlay * overlay;

  if (g_state.overlayInput)
    return true;

  for (ll_reset(g_state.overlays);
      ll_walk(g_state.overlays, (void **)&overlay); )
  {
    if (!overlay->ops->needs_render)
      continue;

    if (overlay->ops->needs_render(overlay->udata, g_state.overlayInput))
      return true;
  }

  return false;
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

  igNewFrame();

  if (g_state.overlayInput)
  {
    totalDamage = true;
    ImDrawList_AddRectFilled(igGetBackgroundDrawListNil(), (ImVec2) { 0.0f , 0.0f },
      g_state.io->DisplaySize, igGetColorU32Col(ImGuiCol_ModalWindowDimBg, 1.0f), 0, 0);

//    bool test;
//    igShowDemoWindow(&test);
  }

  // render the overlays
  for (ll_reset(g_state.overlays);
      ll_walk(g_state.overlays, (void **)&overlay); )
  {
    const int written =
      overlay->ops->render(overlay->udata, g_state.overlayInput,
          buffer, MAX_OVERLAY_RECTS);

    for (int i = 0; i < written; ++i)
    {
      buffer[i].x *= g_state.windowScale;
      buffer[i].y *= g_state.windowScale;
      buffer[i].w *= g_state.windowScale;
      buffer[i].h *= g_state.windowScale;
    }

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

  if (g_state.overlayInput)
  {
    ImGuiMouseCursor cursor = igGetMouseCursor();
    if (cursor != g_state.cursorLast)
    {
      g_state.ds->setPointer(mapImGuiCursor(cursor));
      g_state.cursorLast = cursor;
    }
  }

  igRender();

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
  static bool wasGrabbed = false;

  if (g_state.overlayInput == enable)
    return;

  g_state.overlayInput = enable;
  g_state.cursorLast   = -2;

  if (g_state.overlayInput)
  {
    wasGrabbed = g_cursor.grab;

    g_state.io->ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    g_state.io->MousePos = (ImVec2) { g_cursor.pos.x, g_cursor.pos.y };

    core_setGrabQuiet(false);
    core_setCursorInView(false);
  }
  else
  {
    g_state.io->ConfigFlags |= ImGuiConfigFlags_NoMouse;
    core_resetOverlayInputState();
    core_setGrabQuiet(wasGrabbed);
    app_invalidateWindow(false);
  }
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
