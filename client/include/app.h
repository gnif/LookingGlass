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
bool app_cursorIsGrabbed(void);
bool app_cursorWantsRaw(void);
bool app_cursorInWindow(void);
void app_updateCursorPos(double x, double y);
void app_updateWindowPos(int x, int y);
void app_handleResizeEvent(int w, int h, const struct Border border);
void app_handleMouseGrabbed(double ex, double ey);
void app_handleMouseNormal(double ex, double ey);
void app_handleMouseBasic(void);
void app_handleButtonPress(int button);
void app_handleButtonRelease(int button);
void app_handleKeyPress(int scancode);
void app_handleKeyRelease(int scancode);
void app_handleEnterEvent(bool entered);
void app_handleFocusEvent(bool focused);
void app_handleCloseEvent(void);

void app_setFullscreen(bool fs);
bool app_getFullscreen(void);
bool app_getProp(LG_DSProperty prop, void * ret);

#ifdef ENABLE_EGL
EGLDisplay app_getEGLDisplay(void);
EGLNativeWindowType app_getEGLNativeWindow(void);
void app_eglSwapBuffers(EGLDisplay display, EGLSurface surface);
#endif

void app_glSwapBuffers(void);

void app_clipboardRelease(void);
void app_clipboardNotify(const LG_ClipboardData type, size_t size);
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
KeybindHandle app_registerKeybind(int sc, KeybindFn callback, void * opaque);

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
