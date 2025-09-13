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
  LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
  LRESULT onCreate();
  void registerIcon();

public:
  static bool registerClass();
  CWindow();
  ~CWindow();

  HWND hwnd() { return m_hwnd; }
};
