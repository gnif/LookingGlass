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

#include <windows.h>
#include <windowsx.h>

#undef SW_MAX

#include "interface/displayserver.h"

#include "input_event.h"
#include "platform.h"
#include "window_size.h"

#include "app.h"
#include "common/debug.h"
#include "common/event.h"
#include "common/thread.h"
#include "common/time.h"
#include "common/win32.h"
#include "common/windebug.h"
#include "egl_dynprocs.h"
#include "eglutil.h"
#include "input/keycodes.h"
#include "kb.h"

#undef SW_MAX

#ifdef ENABLE_EGL
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#endif

#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "clipboard.h"

#define LG_WINDOW_CLASS L"LookingGlassClientWindow"

enum WindowsMessage
{
  WM_LG_SHUTDOWN = WM_APP + 1,
  WM_LG_SET_POINTER,
  WM_LG_GRAB_POINTER,
  WM_LG_GRAB_KEYBOARD,
  WM_LG_WARP_POINTER,
  WM_LG_GUEST_POINTER,
  WM_LG_SET_SIZE,
  WM_LG_SET_FULLSCREEN,
  WM_LG_MINIMIZE,
  WM_LG_ACTIVATE,
  WM_LG_INHIBIT_IDLE,
  WM_LG_SET_HANDLER,
  WM_LG_EVENT_SOURCE,
};

struct WindowsPointMessage
{
  int  x;
  int  y;
  bool exiting;
};

struct WindowsGuestPointerMessage
{
  double localX;
  double localY;
};

struct WindowsHandlerMessage
{
  WindowsMessageHandler handler;
  void                * opaque;
};

struct WindowsDSState
{
  HINSTANCE instance;
  HWND      window;
  DWORD     threadID;
  HHOOK     keyboardHook;
  HCURSOR   cursors[LG_POINTER_COUNT];

  LGThread * eventThread;
  LGEvent  * initEvent;

  WindowsInput input;

  LG_DSEventSource      eventSource;
  bool                  eventSourceSubscribed;
  WindowsMessageHandler messageHandler;
  void                * messageOpaque;

  WindowsWindowSize windowSize;

  wchar_t * title;
  int       x;
  int       y;
  bool      center;
  bool      resizable;
  bool      borderless;
  bool      maximize;
  bool      initialFullscreen;
  bool      initOK;
  bool      dispatchReady;
  bool      idleInhibited;

  DWORD           windowedStyle;
  DWORD           windowedExStyle;
  WINDOWPLACEMENT windowedPlacement;

  UINT    dpi;
  double  scale;
  int     rawAbsoluteX;
  int     rawAbsoluteY;
  bool    rawAbsoluteValid;
  bool    mouseLeaveTracked;
  wchar_t highSurrogate;

  _Atomic(int)  pointer;
  _Atomic(bool) pointerGrabbed;
  _Atomic(bool) pointerCaptured;
  _Atomic(bool) keyboardGrabbed;
  _Atomic(bool) fullscreen;
  _Atomic(bool) stopping;
};

static struct WindowsDSState windows;

static LRESULT CALLBACK windowProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam);
static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam,
    LPARAM lParam);
static void armPointerLeave(void);
static void requestWindowSize(int width, int height);
static void applyPendingWindowSize(void);

static UINT queryWindowDPI(HWND window)
{
  typedef UINT (WINAPI * GetDpiForWindowFn)(HWND window);

  const HMODULE     user32          = GetModuleHandleW(L"user32.dll");
  const FARPROC     procedure       = user32 ?
    GetProcAddress(user32, "GetDpiForWindow") : NULL;
  GetDpiForWindowFn getDpiForWindow = NULL;
  memcpy(&getDpiForWindow, &procedure, sizeof(getDpiForWindow));
  if (getDpiForWindow)
  {
    const UINT dpi = getDpiForWindow(window);
    if (dpi)
      return dpi;
  }

  HDC dc = GetDC(window);
  if (!dc)
    return 96;

  const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
  ReleaseDC(window, dc);
  return dpi > 0 ? (UINT)dpi : 96;
}

static double logicalScale(void)
{
  return windows.scale > 0.0 ? windows.scale : 1.0;
}

static int toLogical(int value)
{
  return (int)(value / logicalScale() + 0.5);
}

static int toPhysical(int value)
{
  return (int)(value * logicalScale() + 0.5);
}

static void updateWindowGeometry(void)
{
  RECT client;
  if (!GetClientRect(windows.window, &client))
    return;

  POINT origin = { 0, 0 };
  ClientToScreen(windows.window, &origin);

  windows.dpi   = queryWindowDPI(windows.window);
  windows.scale = (double)windows.dpi / 96.0;

  const int width  = client.right  - client.left;
  const int height = client.bottom - client.top;
  app_updateWindowPos(toLogical(origin.x), toLogical(origin.y));
  app_handleResizeEvent(toLogical(width), toLogical(height), windows.scale,
      (struct Border) { 0, 0, 0, 0 });
  app_invalidateWindow(true);
}

static bool cursorInClient(POINT point)
{
  RECT client;
  if (!GetClientRect(windows.window, &client))
    return false;

  return point.x >= client.left && point.x < client.right &&
    point.y >= client.top && point.y < client.bottom;
}

static void updatePointerClip(void)
{
  if (!windowsInputIsPointerGrabbed(&windows.input) ||
      GetForegroundWindow() != windows.window)
  {
    ClipCursor(NULL);
    return;
  }

  RECT clip;
  if (!GetClientRect(windows.window, &clip))
    return;

  MapWindowPoints(windows.window, NULL, (POINT *)&clip, 2);
  ClipCursor(&clip);
}

static void emitInputState(void)
{
  const bool ctrl   = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift  = (GetAsyncKeyState(VK_SHIFT  ) & 0x8000) != 0;
  const bool alt    = (GetAsyncKeyState(VK_MENU   ) & 0x8000) != 0;
  const bool super  = (GetAsyncKeyState(VK_LWIN   ) & 0x8000) != 0 ||
    (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
  const bool num    = (GetKeyState(VK_NUMLOCK) & 1) != 0;
  const bool caps   = (GetKeyState(VK_CAPITAL) & 1) != 0;
  const bool scroll = (GetKeyState(VK_SCROLL ) & 1) != 0;

  windowsInputState(&windows.input, ctrl, shift, alt, super,
      num, caps, scroll);
}

static int keyCode(WPARAM virtualKey, uint8_t scanCode, bool extended,
    bool altDown, bool pressed)
{
  if (virtualKey == VK_PAUSE)
    return LG_KEY_PAUSE;
  if (virtualKey == VK_SNAPSHOT)
    return windowsInputMapPrintScreenEvent(
        &windows.input, altDown, pressed);

  return windowsInputMapScanCode(scanCode, extended);
}

static bool dispatchKey(WPARAM virtualKey, uint8_t scanCode,
    bool extended, bool altDown, bool pressed, bool grabbedEvent)
{
  const int  key     = keyCode(
      virtualKey, scanCode, extended, altDown, pressed);
  const bool handled = windowsInputKeyCode(&windows.input, key,
      pressed, grabbedEvent);
  if (handled)
  {
    if (grabbedEvent)
      windowsInputTrackedState(&windows.input);
    else
      emitInputState();
  }
  return handled;
}

static bool virtualKeyHeld(UINT virtualKey)
{
  return (GetAsyncKeyState((int)virtualKey) & 0x8000) != 0;
}

static bool mouseVirtualKey(UINT virtualKey)
{
  return virtualKey == VK_LBUTTON  || virtualKey == VK_RBUTTON ||
    virtualKey == VK_MBUTTON || virtualKey == VK_XBUTTON1 ||
    virtualKey == VK_XBUTTON2;
}

static bool heldKeyAlreadyAdded(const int * keys, size_t count, int key)
{
  for (size_t i = 0; i < count; ++i)
    if (keys[i] == key)
      return true;

  return false;
}

static size_t queryHeldKeys(int * keys, size_t capacity)
{
  const bool altDown = virtualKeyHeld(VK_MENU);
  size_t     count   = 0;

  for (UINT virtualKey = 1; virtualKey < 0xFF && count < capacity;
       ++virtualKey)
  {
    if (!virtualKeyHeld(virtualKey) ||
        mouseVirtualKey(virtualKey) ||
        virtualKey == VK_SHIFT ||
        virtualKey == VK_CONTROL ||
        virtualKey == VK_MENU)
      continue;

    const UINT scanCodeEx = MapVirtualKeyW(
        virtualKey, MAPVK_VK_TO_VSC_EX);
    const bool extended   = (scanCodeEx & 0xFF00U) == 0xE000U;
    const int  key        = keyCode(virtualKey,
        (uint8_t)(scanCodeEx & 0xFFU), extended, altDown, true);
    if (key <= LG_KEY_RESERVED || key >= LG_KEY_MAX ||
        heldKeyAlreadyAdded(keys, count, key))
      continue;

    keys[count++] = key;
  }

  return count;
}

static void syncHeldKeys(void)
{
  int          keys[256];
  const size_t count = queryHeldKeys(keys, sizeof(keys) / sizeof(*keys));
  windowsInputSyncHeldKeys(&windows.input, keys, count,
      windowsInputIsKeyboardGrabbed(&windows.input));
}

static void emitCodepoint(uint32_t codepoint)
{
  char text[5] = { 0 };
  if (codepoint <= 0x7F)
    text[0] = (char)codepoint;
  else if (codepoint <= 0x7FF)
  {
    text[0] = (char)(0xC0 | codepoint >> 6);
    text[1] = (char)(0x80 | (codepoint & 0x3F));
  }
  else if (codepoint <= 0xFFFF)
  {
    text[0] = (char)(0xE0 | codepoint >> 12);
    text[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    text[2] = (char)(0x80 | (codepoint & 0x3F));
  }
  else if (codepoint <= 0x10FFFF)
  {
    text[0] = (char)(0xF0 | codepoint >> 18);
    text[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    text[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    text[3] = (char)(0x80 | (codepoint & 0x3F));
  }

  if (text[0])
    windowsInputText(&windows.input, text);
}

static void handleUTF16(wchar_t value)
{
  if (value >= 0xD800 && value <= 0xDBFF)
  {
    windows.highSurrogate = value;
    return;
  }

  uint32_t codepoint = value;
  if (value >= 0xDC00 && value <= 0xDFFF &&
      windows.highSurrogate >= 0xD800 &&
      windows.highSurrogate <= 0xDBFF)
  {
    codepoint = UINT32_C(0x10000) +
      ((uint32_t)(windows.highSurrogate - 0xD800) << 10) +
      (uint32_t)(value - 0xDC00);
  }

  windows.highSurrogate = 0;
  if (codepoint < 0xD800 || codepoint > 0xDFFF)
    emitCodepoint(codepoint);
}

static unsigned int mouseButton(UINT message, WPARAM wParam)
{
  switch (message)
  {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      return 1;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      return 2;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      return 3;

    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      return GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? 6 : 7;
  }

  return 0;
}

static bool mouseButtonPressed(UINT message)
{
  return message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN ||
    message == WM_RBUTTONDOWN || message == WM_XBUTTONDOWN;
}

static void checkPointerLeave(void)
{
  POINT point;
  if (!GetCursorPos(&point))
    return;
  ScreenToClient(windows.window, &point);
  if (cursorInClient(point))
  {
    armPointerLeave();
    windowsInputPointerEnter(&windows.input,
        point.x / logicalScale(), point.y / logicalScale());
  }
  else
    windowsInputPointerLeave(&windows.input);
}

static void armPointerLeave(void)
{
  if (windows.mouseLeaveTracked)
    return;

  TRACKMOUSEEVENT track =
  {
    .cbSize      = sizeof(track),
    .dwFlags     = TME_LEAVE,
    .hwndTrack   = windows.window,
    .dwHoverTime = HOVER_DEFAULT,
  };
  windows.mouseLeaveTracked = TrackMouseEvent(&track) != FALSE;
}

static void handleMouseButton(UINT message, WPARAM wParam)
{
  const unsigned int button  = mouseButton(message, wParam);
  const bool         pressed = mouseButtonPressed(message);
  if (!windows.input.entered)
  {
    POINT point;
    if (GetCursorPos(&point))
    {
      ScreenToClient(windows.window, &point);
      if (cursorInClient(point))
      {
        armPointerLeave();
        windowsInputPointerEnter(&windows.input,
            point.x / logicalScale(), point.y / logicalScale());
      }
    }
  }

  if (!windowsInputPointerButton(&windows.input, button, pressed))
    return;

  if (pressed)
    SetCapture(windows.window);
  else if (!windows.input.buttons &&
      !windowsInputIsPointerGrabbed(&windows.input))
  {
    ReleaseCapture();
    checkPointerLeave();
  }
}

static void handleMouseMove(LPARAM lParam)
{
  const int   x     = GET_X_LPARAM(lParam);
  const int   y     = GET_Y_LPARAM(lParam);
  const POINT point = { .x = x, .y = y };
  if (!cursorInClient(point))
  {
    windowsInputPointerLeave(&windows.input);
    return;
  }

  armPointerLeave();
  if (!windowsInputPointerEnter(&windows.input,
        x / logicalScale(), y / logicalScale()))
    windowsInputPointerMotion(&windows.input,
        x / logicalScale(), y / logicalScale());
}

static void handleRawMouse(const RAWMOUSE * mouse)
{
  double x;
  double y;
  if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE)
  {
    const bool virtualDesktop =
      (mouse->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
    const int  originX        = virtualDesktop ?
      GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
    const int  originY        = virtualDesktop ?
      GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
    const int  width          = GetSystemMetrics(
        virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
    const int  height         = GetSystemMetrics(
        virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
    const int  currentX       = originX +
      (int)((uint64_t)(uint32_t)mouse->lLastX * (uint32_t)width / 65535U);
    const int  currentY       = originY +
      (int)((uint64_t)(uint32_t)mouse->lLastY * (uint32_t)height / 65535U);

    if (!windows.rawAbsoluteValid)
    {
      windows.rawAbsoluteX     = currentX;
      windows.rawAbsoluteY     = currentY;
      windows.rawAbsoluteValid = true;
      return;
    }

    x                    = currentX - windows.rawAbsoluteX;
    y                    = currentY - windows.rawAbsoluteY;
    windows.rawAbsoluteX = currentX;
    windows.rawAbsoluteY = currentY;
  }
  else
  {
    windows.rawAbsoluteValid = false;

    x = mouse->lLastX;
    y = mouse->lLastY;
  }

  if (x != 0.0 || y != 0.0)
    windowsInputRelativeMotion(&windows.input, x, y);
}

static void handleRawInput(HRAWINPUT handle)
{
  if (!windows.input.focused ||
      !windowsInputIsPointerGrabbed(&windows.input) ||
      GetForegroundWindow() != windows.window)
  {
    windows.rawAbsoluteValid = false;
    return;
  }

  RAWINPUT input;
  UINT     size = sizeof(input);
  if (GetRawInputData(handle, RID_INPUT, &input, &size,
        sizeof(RAWINPUTHEADER)) != size)
    return;

  if (input.header.dwType == RIM_TYPEMOUSE)
    handleRawMouse(&input.data.mouse);
}

static bool installKeyboardHook(void)
{
  if (windows.keyboardHook)
    return true;

  windows.keyboardHook = SetWindowsHookExW(
      WH_KEYBOARD_LL, keyboardHookProc, windows.instance, 0);
  if (!windows.keyboardHook)
  {
    DEBUG_WINERROR("SetWindowsHookExW failed", GetLastError());
    return false;
  }

  return true;
}

static void removeKeyboardHook(void)
{
  if (!windows.keyboardHook)
    return;

  UnhookWindowsHookEx(windows.keyboardHook);
  windows.keyboardHook = NULL;
}

static void setKeyboardGrab(bool grab)
{
  if (grab == windowsInputIsKeyboardGrabbed(&windows.input))
    return;

  if (grab && !installKeyboardHook())
    return;

  if (!grab)
    removeKeyboardHook();

  windowsInputSetKeyboardGrabbed(&windows.input, grab);
  atomic_store_explicit(&windows.keyboardGrabbed, grab,
      memory_order_release);
}

static void setPointerGrab(bool grab, bool captured)
{
  const bool old = windowsInputIsPointerGrabbed(&windows.input);
  if (grab)
  {
    windowsInputSetPointerGrabbed(&windows.input, true);
    windows.rawAbsoluteValid = false;
    SetCapture(windows.window);
    updatePointerClip();
  }
  else
  {
    ClipCursor(NULL);
    windowsInputSetPointerGrabbed(&windows.input, false);
    if (!windows.input.buttons)
      ReleaseCapture();
  }

  atomic_store_explicit(&windows.pointerGrabbed, grab,
      memory_order_release);
  atomic_store_explicit(&windows.pointerCaptured, grab && captured,
      memory_order_release);
  if (old != grab)
    app_handleGrabEvent(grab);
  if (!grab)
    checkPointerLeave();
}

static void releasePointerState(bool releaseCapture)
{
  windowsInputReleaseButtons(&windows.input);
  const bool captured = atomic_load_explicit(
      &windows.pointerCaptured, memory_order_acquire);
  if (windowsInputIsPointerGrabbed(&windows.input))
  {
    ClipCursor(NULL);
    windowsInputSetPointerGrabbed(&windows.input, false);
    atomic_store_explicit(&windows.pointerGrabbed, false,
        memory_order_release);
    atomic_store_explicit(&windows.pointerCaptured, false,
        memory_order_release);
    if (releaseCapture)
      ReleaseCapture();

    if (captured)
      app_handleCaptureLost();
    else
      app_handleGrabEvent(false);
  }
  else if (releaseCapture)
    ReleaseCapture();

  checkPointerLeave();
}

static void setFullscreen(bool fullscreen)
{
  const bool old = atomic_load_explicit(
      &windows.fullscreen, memory_order_acquire);
  if (old == fullscreen)
    return;

  if (fullscreen)
  {
    atomic_store_explicit(&windows.fullscreen, true,
        memory_order_release);
    windows.windowedStyle     = (DWORD)GetWindowLongPtrW(
        windows.window, GWL_STYLE);
    windows.windowedExStyle   = (DWORD)GetWindowLongPtrW(
        windows.window, GWL_EXSTYLE);
    windows.windowedPlacement = (WINDOWPLACEMENT)
    {
      .length = sizeof(windows.windowedPlacement),
    };
    GetWindowPlacement(windows.window, &windows.windowedPlacement);

    const HMONITOR monitor = MonitorFromWindow(
        windows.window, MONITOR_DEFAULTTONEAREST);
    MONITORINFO    info    = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);

    SetWindowLongPtrW(windows.window, GWL_STYLE,
        WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrW(windows.window, GWL_EXSTYLE,
        windows.windowedExStyle & ~(DWORD)WS_EX_WINDOWEDGE);
    SetWindowPos(windows.window, HWND_TOP,
        info.rcMonitor.left, info.rcMonitor.top,
        info.rcMonitor.right  - info.rcMonitor.left,
        info.rcMonitor.bottom - info.rcMonitor.top,
        SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  }
  else
  {
    SetWindowLongPtrW(windows.window, GWL_STYLE,
        windows.windowedStyle);
    SetWindowLongPtrW(windows.window, GWL_EXSTYLE,
        windows.windowedExStyle);
    SetWindowPlacement(windows.window, &windows.windowedPlacement);
    SetWindowPos(windows.window, NULL, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
        SWP_NOOWNERZORDER | SWP_NOZORDER);
    atomic_store_explicit(&windows.fullscreen, false,
        memory_order_release);
    applyPendingWindowSize();
  }
}

static bool deferWindowResize(void)
{
  return atomic_load_explicit(
      &windows.fullscreen, memory_order_acquire) ||
    IsZoomed(windows.window);
}

static void applyWindowSize(int width, int height)
{
  RECT rect =
  {
    .left   = 0,
    .top    = 0,
    .right  = toPhysical(width),
    .bottom = toPhysical(height),
  };
  const DWORD style   = (DWORD)GetWindowLongPtrW(
      windows.window, GWL_STYLE);
  const DWORD exStyle = (DWORD)GetWindowLongPtrW(
      windows.window, GWL_EXSTYLE);
  AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, windows.dpi);
  SetWindowPos(windows.window, NULL, 0, 0,
      rect.right - rect.left, rect.bottom - rect.top,
      SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOZORDER);
}

static void requestWindowSize(int width, int height)
{
  if (windowsWindowSizeRequest(
        &windows.windowSize, width, height, deferWindowResize()))
    applyWindowSize(width, height);
}

static void applyPendingWindowSize(void)
{
  int width;
  int height;
  if (windowsWindowSizeTakePending(&windows.windowSize,
        deferWindowResize(), &width, &height))
    applyWindowSize(width, height);
}

static void centerWindow(void)
{
  const HMONITOR monitor = MonitorFromWindow(
      windows.window, MONITOR_DEFAULTTONEAREST);
  MONITORINFO    info    = { .cbSize = sizeof(info) };
  RECT           window;
  if (!GetMonitorInfoW(monitor, &info) ||
      !GetWindowRect(windows.window, &window))
    return;

  const int width  = window.right  - window.left;
  const int height = window.bottom - window.top;
  const int x      = info.rcWork.left +
    (info.rcWork.right - info.rcWork.left - width) / 2;
  const int y      = info.rcWork.top +
    (info.rcWork.bottom - info.rcWork.top - height) / 2;
  SetWindowPos(windows.window, NULL, x, y, 0, 0,
      SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOZORDER);
}

static void setPointerPosition(const struct WindowsPointMessage * point)
{
  POINT position =
  {
    .x = toPhysical(point->x),
    .y = toPhysical(point->y),
  };
  ClientToScreen(windows.window, &position);
  SetCursorPos(position.x, position.y);
}

static void setGuestPointerPosition(
    const struct WindowsGuestPointerMessage * point)
{
  if (atomic_load_explicit(
        &windows.pointerCaptured, memory_order_acquire) ||
      !windows.input.entered)
    return;

  struct WindowsPointMessage position =
  {
    .x = (int)(point->localX + 0.5),
    .y = (int)(point->localY + 0.5),
  };
  setPointerPosition(&position);
}

static void setCursorShape(LG_DSPointer pointer)
{
  const int index = (int)pointer;
  if (index < LG_POINTER_NONE || index >= LG_POINTER_COUNT)
    pointer = LG_POINTER_ARROW;

  atomic_store_explicit(&windows.pointer, pointer, memory_order_release);
  POINT point;
  if (windowsInputIsPointerGrabbed(&windows.input) ||
      (GetCursorPos(&point) &&
       ScreenToClient(windows.window, &point) && cursorInClient(point)))
    SetCursor(windows.cursors[pointer]);
}

static bool eventSourceWake(void * opaque)
{
  const HWND window = (HWND)opaque;
  if (PostMessageW(window, WM_LG_EVENT_SOURCE, 0, 0))
    return true;

  DEBUG_WINERROR("Failed to signal the display event source",
      GetLastError());
  return false;
}

static void eventSourceFree(void)
{
  if (!windows.eventSourceSubscribed)
    return;

  windows.eventSource.unsubscribe(windows.eventSource.opaque);
  windows.eventSourceSubscribed = false;
}

static LRESULT handleCustomMessage(UINT message, WPARAM wParam,
    LPARAM lParam)
{
  switch (message)
  {
    case WM_LG_SHUTDOWN:
      windowsCBEventShutdown();
      eventSourceFree();
      setKeyboardGrab(false);
      setPointerGrab(false, false);
      if (windows.idleInhibited)
        SetThreadExecutionState(ES_CONTINUOUS);
      DestroyWindow(windows.window);
      return 0;

    case WM_LG_SET_POINTER:
      setCursorShape((LG_DSPointer)wParam);
      return 0;

    case WM_LG_GRAB_POINTER:
      setPointerGrab(wParam != 0, lParam != 0);
      return 0;

    case WM_LG_GRAB_KEYBOARD:
      setKeyboardGrab(wParam != 0);
      return 0;

    case WM_LG_WARP_POINTER:
      setPointerPosition((const struct WindowsPointMessage *)lParam);
      return 0;

    case WM_LG_GUEST_POINTER:
      setGuestPointerPosition(
          (const struct WindowsGuestPointerMessage *)lParam);
      return 0;

    case WM_LG_SET_SIZE:
      requestWindowSize((int)wParam, (int)lParam);
      return 0;

    case WM_LG_SET_FULLSCREEN:
      setFullscreen(wParam != 0);
      return 0;

    case WM_LG_MINIMIZE:
      ShowWindow(windows.window, SW_MINIMIZE);
      return 0;

    case WM_LG_ACTIVATE:
    {
      FLASHWINFO flash =
      {
        .cbSize  = sizeof(flash),
        .hwnd    = windows.window,
        .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG,
        .uCount  = 3,
      };
      SetForegroundWindow(windows.window);
      FlashWindowEx(&flash);
      return 0;
    }

    case WM_LG_INHIBIT_IDLE:
      windows.idleInhibited = wParam != 0;
      SetThreadExecutionState(ES_CONTINUOUS |
          (windows.idleInhibited ? ES_DISPLAY_REQUIRED : 0));
      return 0;

    case WM_LG_SET_HANDLER:
    {
      const struct WindowsHandlerMessage * handler =
        (const struct WindowsHandlerMessage *)lParam;
      windows.messageHandler = handler->handler;
      windows.messageOpaque  = handler->opaque;
      return 0;
    }

    case WM_LG_EVENT_SOURCE:
      windows.eventSource.dispatch(windows.eventSource.opaque);
      return 0;
  }

  return 0;
}

static LRESULT CALLBACK windowProc(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
  LRESULT clipboardResult;
  if (windowsCBHandleMessage(
        window, message, wParam, lParam, &clipboardResult))
    return clipboardResult;

  if (message >= WM_LG_SHUTDOWN && message <= WM_LG_EVENT_SOURCE)
    return handleCustomMessage(message, wParam, lParam);

  if (windows.dispatchReady && windows.messageHandler)
  {
    LRESULT result = 0;
    if (windows.messageHandler(windows.messageOpaque, window,
          message, wParam, lParam, &result))
      return result;
  }

  switch (message)
  {
    case WM_CLOSE:
      app_handleCloseEvent();
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_NCHITTEST:
      if (windows.borderless && windows.resizable &&
          !atomic_load_explicit(&windows.fullscreen, memory_order_acquire))
      {
        RECT bounds;
        if (!GetWindowRect(window, &bounds))
          break;

        const int x      = GET_X_LPARAM(lParam);
        const int y      = GET_Y_LPARAM(lParam);
        const int margin = toPhysical(8);
        const bool left   = x < bounds.left + margin;
        const bool right  = x >= bounds.right - margin;
        const bool top    = y < bounds.top + margin;
        const bool bottom = y >= bounds.bottom - margin;

        if (top && left)
          return HTTOPLEFT;
        if (top && right)
          return HTTOPRIGHT;
        if (bottom && left)
          return HTBOTTOMLEFT;
        if (bottom && right)
          return HTBOTTOMRIGHT;
        if (left)
          return HTLEFT;
        if (right)
          return HTRIGHT;
        if (top)
          return HTTOP;
        if (bottom)
          return HTBOTTOM;
      }
      break;

    case WM_PAINT:
    {
      PAINTSTRUCT paint;
      BeginPaint(window, &paint);
      EndPaint(window, &paint);
      app_invalidateWindow(true);
      return 0;
    }

    case WM_MOVE:
      if (windows.dispatchReady && !IsIconic(window))
      {
        updateWindowGeometry();
        updatePointerClip();
      }
      return 0;

    case WM_SIZE:
      if (windows.dispatchReady && wParam != SIZE_MINIMIZED)
      {
        updateWindowGeometry();
        updatePointerClip();
        if (wParam == SIZE_RESTORED)
          applyPendingWindowSize();
      }
      return 0;

    case WM_DPICHANGED:
      if (windows.dispatchReady)
      {
        const RECT * suggested = (const RECT *)lParam;
        SetWindowPos(window, NULL,
            suggested->left, suggested->top,
            suggested->right  - suggested->left,
            suggested->bottom - suggested->top,
            SWP_NOOWNERZORDER | SWP_NOZORDER);
        updateWindowGeometry();
      }
      return 0;

    case WM_SETFOCUS:
      if (windows.dispatchReady)
      {
        const bool changed = windowsInputFocus(&windows.input, true);
        if (changed)
          syncHeldKeys();
        emitInputState();
        updatePointerClip();
      }
      return 0;

    case WM_KILLFOCUS:
      if (windows.dispatchReady)
      {
        releasePointerState(true);
        setKeyboardGrab(false);
        windowsInputFocus(&windows.input, false);
        windows.rawAbsoluteValid = false;
      }
      return 0;

    case WM_MOUSEMOVE:
      if (windows.dispatchReady)
        handleMouseMove(lParam);
      return 0;

    case WM_MOUSELEAVE:
      if (windows.dispatchReady)
      {
        windows.mouseLeaveTracked = false;
        windowsInputPointerLeave(&windows.input);
      }
      return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      if (windows.dispatchReady)
        handleMouseButton(message, wParam);
      return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP ?
        TRUE : 0;

    case WM_MOUSEWHEEL:
      if (windows.dispatchReady)
        windowsInputPointerWheel(&windows.input,
            (double)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
      return 0;

    case WM_INPUT:
      if (windows.dispatchReady)
        handleRawInput((HRAWINPUT)lParam);
      return DefWindowProcW(window, message, wParam, lParam);

    case WM_CAPTURECHANGED:
      if (windows.dispatchReady && (HWND)lParam != windows.window)
        releasePointerState(false);
      return 0;

    case WM_CANCELMODE:
      if (windows.dispatchReady)
        releasePointerState(true);
      return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if (windows.dispatchReady)
      {
        const bool pressed = message == WM_KEYDOWN ||
          message == WM_SYSKEYDOWN;
        dispatchKey(wParam, (uint8_t)((lParam >> 16) & 0xFF),
            (lParam & (UINT32_C(1) << 24)) != 0,
            (lParam & (UINT32_C(1) << 29)) != 0, pressed, false);
      }
      return 0;

    case WM_CHAR:
    case WM_SYSCHAR:
      if (windows.dispatchReady)
        handleUTF16((wchar_t)wParam);
      return 0;

    case WM_UNICHAR:
      if (wParam == UNICODE_NOCHAR)
        return TRUE;
      if (windows.dispatchReady)
        emitCodepoint((uint32_t)wParam);
      return 0;

    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT)
      {
        const int pointer = atomic_load_explicit(
            &windows.pointer, memory_order_acquire);
        SetCursor(windows.cursors[pointer]);
        return TRUE;
      }
      break;
  }

  return DefWindowProcW(window, message, wParam, lParam);
}

static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam,
    LPARAM lParam)
{
  if (code == HC_ACTION &&
      windowsInputIsKeyboardGrabbed(&windows.input) &&
      windows.input.focused &&
      GetForegroundWindow() == windows.window)
  {
    const KBDLLHOOKSTRUCT * event    = (const KBDLLHOOKSTRUCT *)lParam;
    const bool              pressed  = wParam == WM_KEYDOWN ||
      wParam == WM_SYSKEYDOWN;
    const bool              released = wParam == WM_KEYUP ||
      wParam == WM_SYSKEYUP;
    if (pressed || released)
    {
      dispatchKey(event->vkCode, (uint8_t)event->scanCode,
          (event->flags & LLKHF_EXTENDED) != 0,
          (event->flags & LLKHF_ALTDOWN) != 0, pressed, true);
      return 1;
    }
  }

  return CallNextHookEx(windows.keyboardHook, code, wParam, lParam);
}

static HCURSOR createSquareCursor(bool large)
{
  const int width  = GetSystemMetrics(SM_CXCURSOR);
  const int height = GetSystemMetrics(SM_CYCURSOR);
  if (width <= 0 || height <= 0)
    return NULL;

  const size_t stride = ((size_t)width + 15) / 16 * 2;
  if ((size_t)height > SIZE_MAX / stride)
    return NULL;

  const size_t size    = stride * (size_t)height;
  uint8_t    * andMask = malloc(size);
  uint8_t    * xorMask = calloc(size, 1);
  if (!andMask || !xorMask)
  {
    free(andMask);
    free(xorMask);
    return NULL;
  }

  memset(andMask, 0xFF, size);
  int dotSize = large ? 9 : 5;
  if (dotSize > width)
    dotSize = width;
  if (dotSize > height)
    dotSize = height;

  const int left = (width  - dotSize) / 2;
  const int top  = (height - dotSize) / 2;
  for (int y = top; y < top + dotSize; ++y)
    for (int x = left; x < left + dotSize; ++x)
      andMask[(size_t)y * stride + (size_t)x / 8] &=
        (uint8_t)~(UINT8_C(0x80) >> (x & 7));

  HCURSOR cursor = CreateCursor(windows.instance,
      width / 2, height / 2, width, height, andMask, xorMask);
  free(andMask);
  free(xorMask);
  return cursor;
}

static bool loadCursors(bool largeCursorDot)
{
  windows.cursors[LG_POINTER_NONE       ] = NULL;
  windows.cursors[LG_POINTER_SQUARE     ] = createSquareCursor(largeCursorDot);
  windows.cursors[LG_POINTER_ARROW      ] = LoadCursorW(NULL, IDC_ARROW);
  windows.cursors[LG_POINTER_INPUT      ] = LoadCursorW(NULL, IDC_IBEAM);
  windows.cursors[LG_POINTER_MOVE       ] = LoadCursorW(NULL, IDC_SIZEALL);
  windows.cursors[LG_POINTER_RESIZE_NS  ] = LoadCursorW(NULL, IDC_SIZENS);
  windows.cursors[LG_POINTER_RESIZE_EW  ] = LoadCursorW(NULL, IDC_SIZEWE);
  windows.cursors[LG_POINTER_RESIZE_NESW] = LoadCursorW(NULL, IDC_SIZENESW);
  windows.cursors[LG_POINTER_RESIZE_NWSE] = LoadCursorW(NULL, IDC_SIZENWSE);
  windows.cursors[LG_POINTER_HAND       ] = LoadCursorW(NULL, IDC_HAND);
  windows.cursors[LG_POINTER_NOT_ALLOWED] = LoadCursorW(NULL, IDC_NO);
  if (!windows.cursors[LG_POINTER_SQUARE])
  {
    DEBUG_WINERROR("CreateCursor failed", GetLastError());
    return false;
  }

  return true;
}

static void freeCursors(void)
{
  if (!windows.cursors[LG_POINTER_SQUARE])
    return;

  DestroyCursor(windows.cursors[LG_POINTER_SQUARE]);
  windows.cursors[LG_POINTER_SQUARE] = NULL;
}

static DWORD initialStyle(void)
{
  if (windows.borderless)
    return WS_POPUP;

  DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
    WS_MINIMIZEBOX;
  if (windows.resizable)
    style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
  return style;
}

static int eventThread(void * opaque)
{
  (void)opaque;
  windows.threadID = GetCurrentThreadId();

  HICON icon = LoadIconW(windows.instance, IDI_APPLICATION);
  if (!icon)
    icon = LoadIconW(NULL, IDI_APPLICATION);

  WNDCLASSEXW windowClass =
  {
    .cbSize        = sizeof(windowClass),
    .style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
    .lpfnWndProc   = windowProc,
    .hInstance     = windows.instance,
    .hIcon         = icon,
    .hCursor       = windows.cursors[LG_POINTER_ARROW],
    .hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH),
    .lpszClassName = LG_WINDOW_CLASS,
    .hIconSm       = icon,
  };
  if (!RegisterClassExW(&windowClass))
  {
    DEBUG_WINERROR("RegisterClassExW failed", GetLastError());
    lgSignalEvent(windows.initEvent);
    return 0;
  }

  windows.dpi   = 96;
  windows.scale = 1.0;
  RECT rect =
  {
    .left   = 0,
    .top    = 0,
    .right  = windows.windowSize.width,
    .bottom = windows.windowSize.height,
  };
  const DWORD style   = initialStyle();
  const DWORD exStyle = windows.borderless ? 0 : WS_EX_APPWINDOW;
  AdjustWindowRectEx(&rect, style, FALSE, exStyle);

  int x = windows.x;
  int y = windows.y;
  if (windows.center)
  {
    const HMONITOR monitor = MonitorFromPoint(
        (POINT) { windows.x, windows.y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO    info    = { .cbSize = sizeof(info) };
    GetMonitorInfoW(monitor, &info);
    x = info.rcWork.left +
      (info.rcWork.right - info.rcWork.left - (rect.right - rect.left)) / 2;
    y = info.rcWork.top +
      (info.rcWork.bottom - info.rcWork.top - (rect.bottom - rect.top)) / 2;
  }

  windows.window = CreateWindowExW(exStyle, LG_WINDOW_CLASS, windows.title,
      style, x, y, rect.right - rect.left, rect.bottom - rect.top,
      NULL, NULL, windows.instance, NULL);
  if (!windows.window)
  {
    DEBUG_WINERROR("CreateWindowExW failed", GetLastError());
    UnregisterClassW(LG_WINDOW_CLASS, windows.instance);
    lgSignalEvent(windows.initEvent);
    return 0;
  }

  windows.dpi   = queryWindowDPI(windows.window);
  windows.scale = (double)windows.dpi / 96.0;
  requestWindowSize(windows.windowSize.width, windows.windowSize.height);
  if (windows.center)
    centerWindow();

  RAWINPUTDEVICE mouse =
  {
    .usUsagePage = 0x01,
    .usUsage     = 0x02,
    .dwFlags     = 0,
    .hwndTarget  = windows.window,
  };
  if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)))
  {
    DEBUG_WINERROR("RegisterRawInputDevices failed", GetLastError());
    DestroyWindow(windows.window);
    windows.window = NULL;
    UnregisterClassW(LG_WINDOW_CLASS, windows.instance);
    lgSignalEvent(windows.initEvent);
    return 0;
  }

  if (lgEventSourceValid(&windows.eventSource) &&
      !windows.eventSource.subscribe(windows.eventSource.opaque,
        eventSourceWake, windows.window))
  {
    DEBUG_ERROR("Failed to subscribe to the display event source");
    DestroyWindow(windows.window);
    windows.window = NULL;
    UnregisterClassW(LG_WINDOW_CLASS, windows.instance);
    lgSignalEvent(windows.initEvent);
    return 0;
  }
  windows.eventSourceSubscribed = lgEventSourceValid(&windows.eventSource);

  windows.dispatchReady = true;
  windows.initOK        = true;
  updateWindowGeometry();
  ShowWindow(windows.window, windows.maximize ? SW_MAXIMIZE : SW_SHOW);
  UpdateWindow(windows.window);
  if (windows.initialFullscreen)
    setFullscreen(true);
  lgSignalEvent(windows.initEvent);

  MSG message;
  while (GetMessageW(&message, NULL, 0, 0) > 0)
  {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  eventSourceFree();
  windows.dispatchReady = false;
  windows.window        = NULL;
  removeKeyboardHook();
  ClipCursor(NULL);
  UnregisterClassW(LG_WINDOW_CLASS, windows.instance);
  return 0;
}

static void inputPosition(void * opaque, double x, double y)
{
  (void)opaque;
  app_updateCursorPos(x, y);
}

static void inputRelative(void * opaque, double x, double y,
    double rawX, double rawY)
{
  (void)opaque;
  app_handleMouseRelative(x, y, rawX, rawY);
}

static void inputButton(void * opaque, unsigned int button, bool pressed)
{
  (void)opaque;
  if (pressed)
    app_handleButtonPress(button);
  else
    app_handleButtonRelease(button);
}

static void inputWheel(void * opaque, double motion)
{
  (void)opaque;
  app_handleWheelMotion(motion);
}

static void inputKey(void * opaque, int scancode, bool pressed)
{
  (void)opaque;
  if (pressed)
    app_handleKeyPress(scancode);
  else
    app_handleKeyRelease(scancode);
}

static void inputText(void * opaque, const char * text)
{
  (void)opaque;
  app_handleKeyboardTyped(text);
}

static void inputModifiers(void * opaque, bool ctrl, bool shift,
    bool alt, bool super)
{
  (void)opaque;
  app_handleKeyboardModifiers(ctrl, shift, alt, super);
}

static void inputLEDs(void * opaque, bool numLock, bool capsLock,
    bool scrollLock)
{
  (void)opaque;
  app_handleKeyboardLEDs(numLock, capsLock, scrollLock);
}

static void inputEnter(void * opaque, bool entered)
{
  (void)opaque;
  app_handleEnterEvent(entered);
}

static void inputFocus(void * opaque, bool focused)
{
  (void)opaque;
  app_handleFocusEvent(focused);
}

static const LG_DSInputSink inputSink =
{
  .position  = inputPosition,
  .relative  = inputRelative,
  .button    = inputButton,
  .wheel     = inputWheel,
  .key       = inputKey,
  .text      = inputText,
  .modifiers = inputModifiers,
  .leds      = inputLEDs,
  .enter     = inputEnter,
  .focus     = inputFocus,
};

static void windowsSetup(void)
{
}

static bool windowsProbe(void)
{
  return true;
}

static bool windowsEarlyInit(void)
{
  typedef BOOL (WINAPI * SetProcessDpiAwarenessContextFn)(HANDLE value);

  const HMODULE                   user32          =
    GetModuleHandleW(L"user32.dll");
  const FARPROC                   procedure       = user32 ? GetProcAddress(
      user32, "SetProcessDpiAwarenessContext") : NULL;
  SetProcessDpiAwarenessContextFn setDPIAwareness = NULL;
  memcpy(&setDPIAwareness, &procedure, sizeof(setDPIAwareness));
  if (setDPIAwareness)
    setDPIAwareness((HANDLE)(intptr_t)-4);
  return true;
}

static bool windowsInit(const LG_DSInitParams params)
{
  memset(&windows, 0, sizeof(windows));
  windowsInputInit(&windows.input, &inputSink, NULL);

  windows.instance          = GetModuleHandleW(NULL);
  windows.eventSource       = params.eventSource;
  windows.title             = lgUtf8ToWide(params.title);
  windows.x                 = params.x;
  windows.y                 = params.y;
  windows.center            = params.center;
  windows.resizable         = params.resizable;
  windows.borderless        = params.borderless;
  windows.maximize          = params.maximize;
  windows.initialFullscreen = params.fullscreen;
  windowsWindowSizeInit(&windows.windowSize, params.w, params.h);

  windows.windowedPlacement.length = sizeof(windows.windowedPlacement);

  atomic_init(&windows.pointer, LG_POINTER_SQUARE);
  atomic_init(&windows.pointerGrabbed, false);
  atomic_init(&windows.pointerCaptured, false);
  atomic_init(&windows.keyboardGrabbed, false);
  atomic_init(&windows.fullscreen, false);
  atomic_init(&windows.stopping, false);

  if (!windows.title)
  {
    DEBUG_ERROR("Failed to convert the window title to UTF-16");
    return false;
  }

  if (!loadCursors(params.largeCursorDot))
  {
    free(windows.title);
    windows.title = NULL;
    return false;
  }

  windows.initEvent = lgCreateEvent(true, 0);
  if (!windows.initEvent)
  {
    DEBUG_ERROR("Failed to create the Win32 initialization event");
    freeCursors();
    free(windows.title);
    windows.title = NULL;
    return false;
  }

  if (!lgCreateThread("Win32EventThread", eventThread, NULL,
        &windows.eventThread))
  {
    DEBUG_ERROR("Failed to create the Win32 event thread");
    lgFreeEvent(windows.initEvent);
    freeCursors();
    free(windows.title);
    windows.initEvent = NULL;
    windows.title     = NULL;
    return false;
  }

  lgWaitEvent(windows.initEvent, TIMEOUT_INFINITE);
  if (!windows.initOK)
  {
    lgJoinThread(windows.eventThread, NULL);
    windows.eventThread = NULL;
    lgFreeEvent(windows.initEvent);
    freeCursors();
    free(windows.title);
    windows.initEvent = NULL;
    windows.title     = NULL;
    return false;
  }

  return true;
}

static void windowsStartup(void)
{
}

static void windowsShutdown(void)
{
  if (!windows.eventThread || atomic_exchange_explicit(
        &windows.stopping, true, memory_order_acq_rel))
    return;

  if (windows.window)
    SendMessageW(windows.window, WM_LG_SHUTDOWN, 0, 0);
  else if (windows.threadID)
    PostThreadMessageW(windows.threadID, WM_QUIT, 0, 0);

  lgJoinThread(windows.eventThread, NULL);
  windows.eventThread = NULL;
}

static void windowsFree(void)
{
  windowsShutdown();
  if (windows.initEvent)
  {
    lgFreeEvent(windows.initEvent);
    windows.initEvent = NULL;
  }
  free(windows.title);
  windows.title = NULL;
  freeCursors();
}

static bool windowsGetProp(LG_DSProperty prop, void * ret)
{
  switch (prop)
  {
    case LG_DS_WARP_SUPPORT:
      *(enum LG_DSWarpSupport *)ret = LG_DS_WARP_SCREEN;
      return true;

    case LG_DS_NATIVE_HDR:
      *(bool *)ret = false;
      return true;

    case LG_DS_MAX_MULTISAMPLE:
    case LG_DS_HDR_WHITE_LEVELS:
      return false;
  }

  return false;
}

#ifdef ENABLE_EGL
static EGLDisplay windowsGetEGLDisplay(void)
{
  const EGLAttrib coreAttributes[] =
  {
    EGL_PLATFORM_ANGLE_TYPE_ANGLE,
    EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
    EGL_NONE,
  };
  const EGLint    extAttributes [] =
  {
    EGL_PLATFORM_ANGLE_TYPE_ANGLE,
    EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
    EGL_NONE,
  };

  if (g_egl_dynProcs.eglGetPlatformDisplay)
    return g_egl_dynProcs.eglGetPlatformDisplay(
        EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, coreAttributes);

  if (g_egl_dynProcs.eglGetPlatformDisplayEXT)
    return g_egl_dynProcs.eglGetPlatformDisplayEXT(
        EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, extAttributes);

  return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLNativeWindowType windowsGetEGLNativeWindow(void)
{
  return (EGLNativeWindowType)windows.window;
}

static bool windowsEGLSwapBuffers(EGLDisplay display, EGLSurface surface,
    const struct Rect * damage, int count, uint64_t frameToken,
    uint64_t * swapTime, bool * presentTracked)
{
  static struct SwapWithDamageData swap = { 0 };
  if (!swap.init)
    swapWithDamageInit(&swap, display);

  (void)frameToken;
  *presentTracked       = false;
  const uint64_t start  = nanotime();
  const bool     result = swapWithDamage(
      &swap, display, surface, damage, count);
  *swapTime             = nanotime() - start;
  return result;
}
#endif

static bool windowsGetFramePeriod(uint64_t * period)
{
  const HMONITOR monitor = MonitorFromWindow(
      windows.window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info    = { .cbSize = sizeof(info) };
  if (!GetMonitorInfoW(monitor, (MONITORINFO *)&info))
    return false;

  DEVMODEW mode = { .dmSize = sizeof(mode) };
  if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) ||
      mode.dmDisplayFrequency <= 1)
    return false;

  *period = UINT64_C(1000000000) / mode.dmDisplayFrequency;
  return true;
}

static void windowsGuestPointerUpdated(double x, double y,
    double localX, double localY)
{
  (void)x;
  (void)y;
  const struct WindowsGuestPointerMessage point =
  {
    .localX = localX,
    .localY = localY,
  };
  SendMessageW(windows.window, WM_LG_GUEST_POINTER, 0, (LPARAM)&point);
}

static void windowsSetPointer(LG_DSPointer pointer)
{
  SendMessageW(windows.window, WM_LG_SET_POINTER, pointer, 0);
}

static void windowsGrabKeyboard(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_KEYBOARD, true, 0);
}

static void windowsUngrabKeyboard(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_KEYBOARD, false, 0);
}

static void windowsGrabPointer(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_POINTER, true, false);
}

static void windowsUngrabPointer(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_POINTER, false, false);
}

static bool windowsIsPointerGrabbed(void)
{
  return atomic_load_explicit(
      &windows.pointerGrabbed, memory_order_acquire);
}

static void windowsCapturePointer(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_POINTER, true, true);
}

static void windowsUncapturePointer(void)
{
  SendMessageW(windows.window, WM_LG_GRAB_POINTER, false, false);
}

static bool windowsIsPointerCaptured(void)
{
  return atomic_load_explicit(
      &windows.pointerCaptured, memory_order_acquire);
}

static bool windowsGetKeyLabel(int scancode, char * label, size_t size)
{
  if (scancode < 0 || scancode >= LG_KEY_MAX || !label ||
      size < 2 || size > INT_MAX)
    return false;

  const uint32_t ps2 = lg_keycode_to_ps2[scancode];
  if (!ps2)
    return false;

  LONG keyNameParam = (LONG)((ps2 & 0xFF) << 16);
  if ((ps2 & 0xFF00) == 0xE000)
    keyNameParam |= 1L << 24;

  wchar_t   wide[64];
  const int wideLength = GetKeyNameTextW(
      keyNameParam, wide, (int)(sizeof(wide) / sizeof(*wide)));
  if (wideLength <= 0)
    return false;

  const int length = WideCharToMultiByte(CP_UTF8, 0,
      wide, wideLength, label, (int)size - 1, NULL, NULL);
  if (length <= 0)
    return false;

  label[length] = '\0';
  return true;
}

static void windowsWarpPointer(int x, int y, bool exiting)
{
  const struct WindowsPointMessage point =
  {
    .x       = x,
    .y       = y,
    .exiting = exiting,
  };
  SendMessageW(windows.window, WM_LG_WARP_POINTER, 0, (LPARAM)&point);
}

static void windowsRealignPointer(void)
{
  app_handleMouseRelative(0.0, 0.0, 0.0, 0.0);
}

static bool windowsIsValidPointerPos(int x, int y)
{
  const POINT point =
  {
    .x = toPhysical(x),
    .y = toPhysical(y),
  };
  return MonitorFromPoint(point, MONITOR_DEFAULTTONULL) != NULL;
}

static void windowsInhibitIdle(void)
{
  SendMessageW(windows.window, WM_LG_INHIBIT_IDLE, true, 0);
}

static void windowsUninhibitIdle(void)
{
  SendMessageW(windows.window, WM_LG_INHIBIT_IDLE, false, 0);
}

static void windowsRequestActivation(void)
{
  PostMessageW(windows.window, WM_LG_ACTIVATE, 0, 0);
}

static void windowsWait(unsigned int time)
{
  Sleep(time);
}

static void windowsSetWindowSize(int width, int height)
{
  SendMessageW(windows.window, WM_LG_SET_SIZE, width, height);
}

static bool windowsGetFullscreen(void)
{
  return atomic_load_explicit(&windows.fullscreen, memory_order_acquire);
}

static void windowsSetFullscreen(bool fullscreen)
{
  SendMessageW(windows.window, WM_LG_SET_FULLSCREEN, fullscreen, 0);
}

static void windowsMinimize(void)
{
  PostMessageW(windows.window, WM_LG_MINIMIZE, 0, 0);
}

HWND windowsGetWindow(void)
{
  return windows.window;
}

void windowsSetMessageHandler(WindowsMessageHandler handler, void * opaque)
{
  const struct WindowsHandlerMessage message =
  {
    .handler = handler,
    .opaque  = opaque,
  };
  SendMessageW(windows.window, WM_LG_SET_HANDLER, 0, (LPARAM)&message);
}

struct LG_DisplayServerOps LGDS_Windows =
{
  .name                = "Windows",
  .setup               = windowsSetup,
  .probe               = windowsProbe,
  .earlyInit           = windowsEarlyInit,
  .init                = windowsInit,
  .startup             = windowsStartup,
  .shutdown            = windowsShutdown,
  .free                = windowsFree,
  .getProp             = windowsGetProp,
#ifdef ENABLE_EGL
  .getEGLDisplay       = windowsGetEGLDisplay,
  .getEGLNativeWindow  = windowsGetEGLNativeWindow,
  .eglSwapBuffers      = windowsEGLSwapBuffers,
#endif
  .getFramePeriod      = windowsGetFramePeriod,
  .guestPointerUpdated = windowsGuestPointerUpdated,
  .setPointer          = windowsSetPointer,
  .grabKeyboard        = windowsGrabKeyboard,
  .ungrabKeyboard      = windowsUngrabKeyboard,
  .grabPointer         = windowsGrabPointer,
  .ungrabPointer       = windowsUngrabPointer,
  .isPointerGrabbed    = windowsIsPointerGrabbed,
  .capturePointer      = windowsCapturePointer,
  .uncapturePointer    = windowsUncapturePointer,
  .isPointerCaptured   = windowsIsPointerCaptured,
  .getKeyLabel         = windowsGetKeyLabel,
  .warpPointer         = windowsWarpPointer,
  .realignPointer      = windowsRealignPointer,
  .isValidPointerPos   = windowsIsValidPointerPos,
  .inhibitIdle         = windowsInhibitIdle,
  .uninhibitIdle       = windowsUninhibitIdle,
  .requestActivation   = windowsRequestActivation,
  .wait                = windowsWait,
  .setWindowSize       = windowsSetWindowSize,
  .getFullscreen       = windowsGetFullscreen,
  .setFullscreen       = windowsSetFullscreen,
  .minimize            = windowsMinimize,
  .cbInit              = windowsCBInit,
  .cbNotice            = windowsCBNotice,
  .cbRelease           = windowsCBRelease,
  .cbRequest           = windowsCBRequest,
  .cbRequestReady      = windowsCBRequestReady,
  .cbRequestCancel     = windowsCBRequestCancel,
};
