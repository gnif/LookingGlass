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

#include "windows/mousehook.h"
#include "common/windebug.h"
#include "platform.h"

#include <windows.h>
#include <stdbool.h>

struct mouseHook
{
  bool        installed;
  HHOOK       hook;
  MouseHookFn callback;
  int         x, y;
};

static struct mouseHook mouseHook = { 0 };

// forwards
static LRESULT WINAPI mouseHook_hook(int nCode, WPARAM wParam, LPARAM lParam);
static LRESULT msg_callback(WPARAM wParam, LPARAM lParam);

void mouseHook_install(MouseHookFn callback)
{
  struct MSG_CALL_FUNCTION cf;
  cf.fn     = msg_callback;
  cf.wParam = 1;
  cf.lParam = (LPARAM)callback;
  sendAppMessage(WM_CALL_FUNCTION, 0, (LPARAM)&cf);
}

void mouseHook_remove()
{
  struct MSG_CALL_FUNCTION cf;
  cf.fn     = msg_callback;
  cf.wParam = 0;
  cf.lParam = 0;
  sendAppMessage(WM_CALL_FUNCTION, 0, (LPARAM)&cf);
}

static LRESULT msg_callback(WPARAM wParam, LPARAM lParam)
{
  if (wParam)
  {
    if (mouseHook.installed)
    {
      DEBUG_WARN("Mouse hook already installed");
      return 0;
    }

    mouseHook.hook = SetWindowsHookEx(WH_MOUSE_LL, mouseHook_hook, NULL, 0);
    if (!mouseHook.hook)
    {
      DEBUG_WINERROR("Failed to install the mouse hook", GetLastError());
      return 0;
    }

    mouseHook.installed = true;
    mouseHook.callback  = (MouseHookFn)lParam;
  }
  else
  {
    if (!mouseHook.installed)
      return 0;

    UnhookWindowsHookEx(mouseHook.hook);
    mouseHook.installed = false;
  }

  return 0;
}

static LRESULT WINAPI mouseHook_hook(int nCode, WPARAM wParam, LPARAM lParam)
{
  if (nCode == HC_ACTION && wParam == WM_MOUSEMOVE)
  {
    MSLLHOOKSTRUCT *msg = (MSLLHOOKSTRUCT *)lParam;
    if (mouseHook.x != msg->pt.x || mouseHook.y != msg->pt.y)
    {
      mouseHook.x = msg->pt.x;
      mouseHook.y = msg->pt.y;
      mouseHook.callback(msg->pt.x, msg->pt.y);
    }
  }
  return CallNextHookEx(mouseHook.hook, nCode, wParam, lParam);
}
