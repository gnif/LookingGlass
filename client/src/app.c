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

#include "app.h"

#include "main.h"
#include "core.h"
#include "util.h"
#include "clipboard.h"
#include "render_queue.h"

#include "kb.h"

#include "common/debug.h"
#include "common/stringutils.h"
#include "interface/overlay.h"
#include "overlays.h"

#include "cimgui.h"

#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <ctype.h>

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
  g_cursor.pos.x = x;
  g_cursor.pos.y = y;
  g_cursor.valid = true;

  if (app_isOverlayMode())
    g_state.io->MousePos = (ImVec2) { x, y };
}

void app_handleFocusEvent(bool focused)
{
  g_state.focused = focused;

  // release any imgui buttons/keys if we lost focus
  if (!focused && app_isOverlayMode())
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
          app_handleKeyRelease(key, 0);

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
  }
}

void app_clipboardRelease(void)
{
  if (!g_params.clipboardToVM)
    return;

  purespice_clipboardRelease();
}

void app_clipboardNotifyTypes(const LG_ClipboardData types[], int count)
{
  if (!g_params.clipboardToVM)
    return;

  if (count == 0)
  {
    purespice_clipboardRelease();
    return;
  }

  PSDataType conv[count];
  for(int i = 0; i < count; ++i)
    conv[i] = cb_lgTypeToSpiceType(types[i]);

  purespice_clipboardGrab(conv, count);
}

void app_clipboardNotifySize(const LG_ClipboardData type, size_t size)
{
  if (!g_params.clipboardToVM)
    return;

  if (type == LG_CLIPBOARD_DATA_NONE)
  {
    purespice_clipboardRelease();
    return;
  }

  g_state.cbType    = cb_lgTypeToSpiceType(type);
  g_state.cbChunked = size > 0;
  g_state.cbXfer    = size;

  purespice_clipboardDataStart(g_state.cbType, size);
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
    purespice_clipboardDataStart(g_state.cbType, size);

  purespice_clipboardData(g_state.cbType, data, (uint32_t)size);
  g_state.cbXfer -= size;
}

void app_clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque)
{
  if (!g_params.clipboardToLocal)
    return;

  struct CBRequest * cbr = malloc(sizeof(*cbr));
  if (!cbr)
  {
    DEBUG_ERROR("out of memory");
    return;
  }

  cbr->type    = g_state.cbType;
  cbr->replyFn = replyFn;
  cbr->opaque  = opaque;
  ll_push(g_state.cbRequestList, cbr);

  purespice_clipboardRequest(g_state.cbType);
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

  if (app_isOverlayMode())
  {
    int igButton = mapSpiceToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = true;
    return;
  }

  if (!core_inputEnabled() || !g_cursor.inView)
    return;

  if (!purespice_mousePress(button))
    DEBUG_ERROR("app_handleButtonPress: failed to send message");
}

void app_handleButtonRelease(int button)
{
  g_cursor.buttons &= ~(1U << button);

  if (app_isOverlayMode())
  {
    int igButton = mapSpiceToImGuiButton(button);
    if (igButton != -1)
      g_state.io->MouseDown[igButton] = false;
    return;
  }

  if (!core_inputEnabled())
    return;

  if (!purespice_mouseRelease(button))
    DEBUG_ERROR("app_handleButtonRelease: failed to send message");
}

void app_handleWheelMotion(double motion)
{
  if (app_isOverlayMode())
    g_state.io->MouseWheel -= motion;
}

void app_handleKeyPress(int sc, int charcode)
{
  if (!app_isOverlayMode() || !g_state.io->WantCaptureKeyboard)
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
      KeybindHandle handle;
      ll_forEachNL(g_state.bindings, item, handle)
      {
        if ((handle->sc       && handle->sc       == sc       ) ||
            (handle->charcode && handle->charcode == charcode))
        {
          handle->callback(sc, handle->opaque);
          break;
        }
      }
      return;
    }
  }

  if (app_isOverlayMode())
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

    if (purespice_keyDown(ps2))
      g_state.keyDown[sc] = true;
    else
    {
      DEBUG_ERROR("app_handleKeyPress: failed to send message");
      return;
    }
  }
}

void app_handleKeyRelease(int sc, int charcode)
{
  if (g_state.escapeActive)
  {
    if (g_state.escapeAction == -1)
    {
      if (!g_state.escapeHelp && g_params.useSpiceInput &&
          !app_isOverlayMode())
        core_setGrab(!g_cursor.grab);
    }

    if (sc == g_params.escapeKey)
      g_state.escapeActive = false;
  }

  if (app_isOverlayMode())
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

  if (purespice_keyUp(ps2))
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

  if (!purespice_keyModifiers(modifiers))
    DEBUG_ERROR("app_handleKeyboardLEDs: failed to send message");
}

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy)
{
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

// On some display servers normal cursor logic does not work due to the lack of
// cursor warp support. Instead, we attempt a best-effort emulation which works
// with a 1:1 mouse movement patch applied in the guest. For anything fancy, use
// capture mode.
void app_handleMouseBasic()
{
  /* do not pass mouse events to the guest if we do not have focus */
  if (!g_cursor.guest.valid || !g_state.haveSrcSize || !g_state.focused ||
      app_isOverlayMode())
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

  if (!purespice_mouseMotion(x, y))
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

  if (g_state.dsInitialized && g_state.jitRender && g_state.ds->stopWaitFrame)
    g_state.ds->stopWaitFrame();

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

  va_list args;
  va_start(args, fmt);
  overlayAlert_show(type, fmt, args);
  va_end(args);
}

MsgBoxHandle app_msgBox(const char * caption, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  MsgBoxHandle handle = overlayMsg_show(caption, NULL, NULL, fmt, args);
  va_end(args);

  core_updateOverlayState();

  return handle;
}

MsgBoxHandle app_confirmMsgBox(const char * caption,
    MsgBoxConfirmCallback callback, void * opaque, const char * fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  MsgBoxHandle handle = overlayMsg_show(caption, callback, opaque, fmt, args);
  va_end(args);

  core_updateOverlayState();

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

KeybindHandle app_registerKeybind(int sc, int charcode, KeybindFn callback,
    void * opaque, const char * description)
{
  if (charcode != 0 && sc != 0)
  {
    DEBUG_ERROR("invalid keybind, one of scancode or charcode must be 0");
    return NULL;
  }

  if (charcode && islower(charcode))
  {
    DEBUG_ERROR("invalid keybind, charcode must be uppercase");
    return NULL;
  }

  KeybindHandle handle;

  // don't allow duplicate binds
  ll_forEachNL(g_state.bindings, item, handle)
  {
    if ((sc       && handle->sc       == sc      ) ||
        (charcode && handle->charcode == charcode))
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
  handle->charcode    = charcode;
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
      overlay->ops = NULL;
    }
  }
  ll_unlock(g_state.overlays);
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

void app_invalidateOverlay(bool renderTwice)
{
  if (g_state.state == APP_STATE_SHUTDOWN)
    return;

  if (renderTwice)
    g_state.renderImGuiTwice = true;
  app_invalidateWindow(false);
}

bool app_guestIsLinux(void)
{
  return g_state.guestOS == KVMFR_OS_LINUX;
}

bool app_guestIsWindows(void)
{
  return g_state.guestOS == KVMFR_OS_WINDOWS;
}

bool app_guestIsOSX(void)
{
  return g_state.guestOS == KVMFR_OS_OSX;
}

bool app_guestIsBSD(void)
{
  return g_state.guestOS == KVMFR_OS_BSD;
}

bool app_guestIsOther(void)
{
  return g_state.guestOS == KVMFR_OS_OTHER;
}

void app_stopVideo(bool stop)
{
  if (g_state.stopVideo == stop)
    return;

  // do not change the state if the host app is not connected
  if (!g_state.lgHostConnected)
    return;

  g_state.stopVideo = stop;

  app_alert(
    LG_ALERT_INFO,
    stop ? "Video Stream Disabled" : "Video Stream Enabled"
  );

  if (stop)
  {
    core_stopCursorThread();
    core_stopFrameThread();
  }
  else
  {
    core_startCursorThread();
    core_startFrameThread();
  }
}

bool app_useSpiceDisplay(bool enable)
{
  static bool lastState = false;
  if (!g_params.useSpice || lastState == enable)
    return g_params.useSpice && lastState;

  // if spice is not yet ready, flag the state we want for when it is
  if (!g_state.spiceReady)
  {
    g_state.initialSpiceDisplay = enable;
    return false;
  }

  if (!purespice_hasChannel(PS_CHANNEL_DISPLAY))
    return false;

  // do not allow stopping of the host app if not connected
  if (!enable && !g_state.lgHostConnected)
    return false;

  lastState = enable;
  if (enable)
  {
    purespice_connectChannel(PS_CHANNEL_DISPLAY);
    purespice_connectChannel(PS_CHANNEL_CURSOR);
    renderQueue_spiceShow(true);
  }
  else
  {
    renderQueue_spiceShow(false);
    purespice_disconnectChannel(PS_CHANNEL_DISPLAY);
    purespice_disconnectChannel(PS_CHANNEL_CURSOR);
  }

  overlayStatus_set(LG_USER_STATUS_SPICE, enable);
  return enable;
}
