#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

class CWindow {
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  virtual LRESULT onCreate();
  virtual LRESULT onClose();
  virtual LRESULT onDestroy();

protected:
  static HINSTANCE hInstance;
  static void populateWindowClass(WNDCLASSEX &wx);

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

  HWND m_hwnd = NULL;

public:
  virtual ~CWindow();
  void destroy();

  HWND hwnd() { return m_hwnd; }
};
