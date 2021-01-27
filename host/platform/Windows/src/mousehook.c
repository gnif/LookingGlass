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
  HANDLE      event;
  HANDLE      thread;
};

static struct mouseHook mouseHook = { 0 };

// forwards
static LRESULT WINAPI mouseHook_hook(int nCode, WPARAM wParam, LPARAM lParam);

static bool switchDesktopAndHook(void)
{
  HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
  if (!desk)
  {
    DEBUG_WINERROR("Failed to OpenInputDesktop", GetLastError());
    return false;
  }

  if (!SetThreadDesktop(desk))
  {
    DEBUG_WINERROR("Failed to SetThreadDesktop", GetLastError());
    CloseDesktop(desk);
    return false;
  }
  CloseDesktop(desk);

  POINT position;
  GetCursorPos(&position);

  mouseHook.x = position.x;
  mouseHook.y = position.y;
  mouseHook.callback(position.x, position.y);

  mouseHook.hook = SetWindowsHookEx(WH_MOUSE_LL, mouseHook_hook, NULL, 0);
  if (!mouseHook.hook)
  {
    DEBUG_WINERROR("Failed to install the mouse hook", GetLastError());
    return false;
  }
  return true;
}

static VOID WINAPI winEventProc(HWINEVENTHOOK hWinEventHook, DWORD event,
    HWND hwnd, LONG idObject, LONG idChild, DWORD idEventThread, DWORD dwmsEventTime)
{
  switch (event)
  {
    case EVENT_SYSTEM_DESKTOPSWITCH:
      UnhookWindowsHookEx(mouseHook.hook);
      switchDesktopAndHook();
      break;
  }
}

static DWORD WINAPI threadProc(LPVOID lParam) {
  if (mouseHook.installed)
  {
    DEBUG_WARN("Mouse hook already installed");
    return 0;
  }

  mouseHook.callback = (MouseHookFn)lParam;
  if (!switchDesktopAndHook())
    return 0;

  mouseHook.installed = true;

  HWINEVENTHOOK eventHook = SetWinEventHook(
      EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH, NULL,
      winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT
  );
  if (!eventHook)
  {
    DEBUG_WINERROR("Failed to SetWinEventHook", GetLastError());
    goto exit;
  }

  MSG msg;
  while (true) {
    switch (MsgWaitForMultipleObjects(1, &mouseHook.event, FALSE, INFINITE, QS_ALLINPUT)) {
      case WAIT_OBJECT_0:
        DEBUG_INFO("Mouse hook thread received quit request");
        PostQuitMessage(0);
        break;
      case WAIT_OBJECT_0 + 1:
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
          if (msg.message == WM_QUIT)
            goto exit;
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
        break;
      default:
        goto exit;
    }
  }

  exit:
  if (eventHook) UnhookWinEvent(eventHook);
  UnhookWindowsHookEx(mouseHook.hook);
  mouseHook.installed = false;
  return 0;
}

void mouseHook_install(MouseHookFn callback)
{
  if (!mouseHook.event)
  {
    mouseHook.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!mouseHook.event)
    {
      DEBUG_WINERROR("Failed to create mouse hook uninstall event", GetLastError());
      return;
    }
  }
  mouseHook.thread = CreateThread(NULL, 0, threadProc, callback, 0, NULL);
}

void mouseHook_remove(void)
{
  if (!mouseHook.event)
    return;

  SetEvent(mouseHook.event);
  WaitForSingleObject(mouseHook.thread, INFINITE);
  CloseHandle(mouseHook.thread);
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
