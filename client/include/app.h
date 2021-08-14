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

#ifndef _H_LG_APP_
#define _H_LG_APP_

#include <stdbool.h>
#include <linux/input.h>

#include "common/ringbuffer.h"
#include "common/types.h"
#include "interface/displayserver.h"
#include "interface/overlay.h"

typedef enum LG_MsgAlert
{
  LG_ALERT_INFO   ,
  LG_ALERT_SUCCESS,
  LG_ALERT_WARNING,
  LG_ALERT_ERROR
}
LG_MsgAlert;

bool app_isRunning(void);
bool app_inputEnabled(void);
bool app_isCaptureMode(void);
bool app_isCaptureOnlyMode(void);
bool app_isFormatValid(void);
bool app_isOverlayMode(void);
void app_updateCursorPos(double x, double y);
void app_updateWindowPos(int x, int y);
void app_handleResizeEvent(int w, int h, double scale, const struct Border border);
void app_invalidateWindow(bool full);

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy);

void app_handleMouseBasic(void);
void app_resyncMouseBasic(void);

void app_handleButtonPress(int button);
void app_handleButtonRelease(int button);
void app_handleWheelMotion(double motion);
void app_handleKeyPress(int scancode);
void app_handleKeyRelease(int scancode);
void app_handleKeyboardTyped(const char * typed);
void app_handleKeyboardModifiers(bool ctrl, bool shift, bool alt, bool super);
void app_handleKeyboardLEDs(bool numLock, bool capsLock, bool scrollLock);
void app_handleEnterEvent(bool entered);
void app_handleFocusEvent(bool focused);
void app_handleCloseEvent(void);
void app_handleRenderEvent(const uint64_t timeUs);

void app_setFullscreen(bool fs);
bool app_getFullscreen(void);
bool app_getProp(LG_DSProperty prop, void * ret);

#ifdef ENABLE_EGL
EGLDisplay app_getEGLDisplay(void);
EGLNativeWindowType app_getEGLNativeWindow(void);
void app_eglSwapBuffers(EGLDisplay display, EGLSurface surface, const struct Rect * damage, int count);
#endif

#ifdef ENABLE_OPENGL
LG_DSGLContext app_glCreateContext(void);
void app_glDeleteContext(LG_DSGLContext context);
void app_glMakeCurrent(LG_DSGLContext context);
void app_glSetSwapInterval(int interval);
void app_glSwapBuffers(void);
#endif

#define MAX_OVERLAY_RECTS 10
void app_registerOverlay(const struct LG_OverlayOps * ops, const void * params);
void app_initOverlays(void);
void app_setOverlay(bool enable);
bool app_overlayNeedsRender(void);
/**
 * render the overlay
 * returns:
 *   -1 for full output damage
 *    0 for no overlay
 *   >0 number of rects written into rects
 */
int app_renderOverlay(struct Rect * rects, int maxRects);

void app_freeOverlays(void);

struct OverlayGraph;
typedef struct OverlayGraph * GraphHandle;

GraphHandle app_registerGraph(const char * name, RingBuffer buffer, float min, float max);
void app_unregisterGraph(GraphHandle handle);

void app_overlayConfigRegister(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

void app_overlayConfigRegisterTab(const char * title,
    void (*callback)(void * udata, int * id), void * udata);

void app_clipboardRelease(void);
void app_clipboardNotifyTypes(const LG_ClipboardData types[], int count);
void app_clipboardNotifySize(const LG_ClipboardData type, size_t size);
void app_clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size);
void app_clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque);

/**
 * Show an alert on screen
 * @param type The alert type
 * param  fmt  The alert message format
 @ param  ...  formatted message values
 */
void app_alert(LG_MsgAlert type, const char * fmt, ...);

typedef struct KeybindHandle * KeybindHandle;
typedef void (*KeybindFn)(int sc, void * opaque);

/**
 * Register a handler for the <super>+<key> combination
 * @param sc       The scancode to register
 * @param callback The function to be called when the combination is pressed
 * @param opaque   A pointer to be passed to the callback, may be NULL
 * @retval A handle for the binding or NULL on failure.
 *         The caller is required to release the handle via `app_releaseKeybind` when it is no longer required
 */
KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque, const char * description);

/**
 * Release an existing key binding
 * @param handle A pointer to the keybind handle to release, may be NULL
 */
void app_releaseKeybind(KeybindHandle * handle);

/**
 * Release all keybindings
 */
void app_releaseAllKeybinds(void);

#endif
