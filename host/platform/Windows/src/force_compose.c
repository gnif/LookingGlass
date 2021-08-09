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

#include "windows/force_compose.h"
#include "common/windebug.h"

#include <windows.h>
#include <stdbool.h>

struct ForceCompose
{
  HANDLE      event;
  HANDLE      thread;
};

static struct ForceCompose forceCompose = { 0 };

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  switch (message)
  {
    case WM_CLOSE:
      return 0; // Don't allow close

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      FillRect(hdc, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW+1));
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProc(hwnd, message, wParam, lParam);
}

static DWORD WINAPI threadProc(LPVOID lParam)
{
  WNDCLASSA wc = { 0 };

  wc.lpfnWndProc   = windowProc;
  wc.hInstance     = (HINSTANCE) GetModuleHandle(NULL);
  wc.lpszClassName = "looking-glass-force-composition";
  RegisterClass(&wc);

  HWND hwnd = CreateWindowEx(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName,
      "Looking Glass Helper Window", WS_POPUP, 0, 0, 1, 1, NULL, NULL,
      wc.hInstance, NULL
  );

  if (!hwnd)
  {
    DEBUG_ERROR("Failed to create window to force composition");
    goto exit;
  }

  SetLayeredWindowAttributes(hwnd, GetSysColor(COLOR_WINDOW), 0, LWA_COLORKEY);
  ShowWindow(hwnd, SW_SHOW);
  DEBUG_INFO("Created window to force composition");

  MSG msg;
  while (true)
  {
    switch (MsgWaitForMultipleObjects(1, &forceCompose.event, FALSE, INFINITE, QS_ALLINPUT))
    {
      case WAIT_OBJECT_0:
        DEBUG_INFO("Force composition received quit request");
        DestroyWindow(hwnd);

        // Do not wait on the event after it has been signaled.
        while (GetMessage(&msg, NULL, 0, 0) > 0)
        {
          TranslateMessage(&msg);
          DispatchMessage(&msg);
        }
        goto exit;
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
        DEBUG_WINERROR("MsgWaitForMultipleObjects failed", GetLastError());
        goto exit;
    }
  }

  exit:
  UnregisterClass(wc.lpszClassName, wc.hInstance);
  return 0;
}

void dwmForceComposition(void)
{
  if (!forceCompose.event)
  {
    forceCompose.event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!forceCompose.event)
    {
      DEBUG_WINERROR("Failed to create unforce composition event", GetLastError());
      return;
    }
  }
  forceCompose.thread = CreateThread(NULL, 0, threadProc, NULL, 0, NULL);
}

void dwmUnforceComposition(void)
{
  if (!forceCompose.event)
    return;
  SetEvent(forceCompose.event);
  WaitForSingleObject(forceCompose.thread, INFINITE);
  CloseHandle(forceCompose.thread);
  forceCompose.thread = NULL;
}
