/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "common/types.h"
#include "interface/displayserver.h"

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
void app_updateCursorPos(double x, double y);
void app_updateWindowPos(int x, int y);
void app_handleResizeEvent(int w, int h, double scale, const struct Border border);

void app_handleMouseRelative(double normx, double normy,
    double rawx, double rawy);

void app_handleMouseBasic(void);
void app_resyncMouseBasic(void);

void app_handleButtonPress(int button);
void app_handleButtonRelease(int button);
void app_handleKeyPress(int scancode);
void app_handleKeyRelease(int scancode);
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

/**
 * Changes whether the help message is displayed or not.
 */
void app_showHelp(bool show);

/**
 * Changes whether the FPS is displayed or not.
 */
void app_showFPS(bool showFPS);

#endif
