#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#define WM_NOTIFY_ICON (WM_USER)

class CWindow {
  static ATOM s_atom;
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  HWND m_hwnd = NULL;
  LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

public:
  static bool registerClass();
  CWindow();
  ~CWindow();
};
