#pragma once

#include <windows.h>

class CWidget {
protected:
  HWND m_hwnd = NULL;

  HWND createWindowSimple(LPCWSTR cls, LPCWSTR title, DWORD style, HWND parent);

public:
  virtual ~CWidget();
  void destroy();

  HWND hwnd() { return m_hwnd; }
  operator HWND() const { return m_hwnd; }
};
