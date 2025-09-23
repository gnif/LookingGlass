#include "CWidget.h"

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

HWND CWidget::createWindowSimple(LPCWSTR cls, LPCWSTR title, DWORD style, HWND parent, DWORD dwExStyle)
{
  return CreateWindowEx(dwExStyle, cls, title, style, 0, 0, 0, 0, parent,
    NULL, (HINSTANCE)GetModuleHandle(NULL), NULL);
}

CWidget::~CWidget()
{
  destroy();
}

void CWidget::destroy()
{
  if (m_hwnd)
  {
    DestroyWindow(m_hwnd);
    m_hwnd = NULL;
  }
}
