#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define WM_NOTIFY_ICON (WM_USER)

class CWindow {
  static ATOM s_atom;
  static UINT s_taskbarCreated;
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  HWND m_hwnd = NULL;
  NOTIFYICONDATA m_iconData;
  HMENU m_menu;

  LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
  LRESULT onCreate();
  LRESULT onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y);
  void registerIcon();

public:
  static bool registerClass();
  CWindow();
  ~CWindow();
  void destroy();

  HWND hwnd() { return m_hwnd; }
};
