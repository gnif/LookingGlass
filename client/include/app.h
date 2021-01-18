/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2020 Geoffrey McRae <geoff@hostfission.com>
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

#include <stdbool.h>

#include "interface/displayserver.h"


SDL_Window * app_getWindow(void);

bool app_getProp(LG_DSProperty prop, void * ret);
bool app_inputEnabled(void);
bool app_cursorIsGrabbed(void);
bool app_cursorWantsRaw(void);
bool app_cursorInWindow(void);
void app_updateCursorPos(double x, double y);
void app_updateWindowPos(int x, int y);
void app_handleResizeEvent(int w, int h);
void app_handleMouseGrabbed(double ex, double ey);
void app_handleMouseNormal(double ex, double ey);
void app_handleMouseBasic(void);
void app_handleButtonPress(int button);
void app_handleButtonRelease(int button);
void app_handleKeyPress(int scancode);
void app_handleKeyRelease(int scancode);
void app_handleWindowEnter(void);
void app_handleWindowLeave(void);
void app_handleFocusEvent(bool focused);
void app_handleCloseEvent(void);

void app_clipboardRelease(void);
void app_clipboardNotify(const LG_ClipboardData type, size_t size);
void app_clipboardData(const LG_ClipboardData type, uint8_t * data, size_t size);
void app_clipboardRequest(const LG_ClipboardReplyFn replyFn, void * opaque);
