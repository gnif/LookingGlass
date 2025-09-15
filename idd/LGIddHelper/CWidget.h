#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

class CWidget {
protected:
  HWND m_hwnd = NULL;

  HWND createWindowSimple(LPCWSTR cls, LPCWSTR title, DWORD style, HWND parent);

public:
  virtual ~CWidget();
  void destroy();

  HWND hwnd() { return m_hwnd; }
  operator HWND() { return m_hwnd; }
};
