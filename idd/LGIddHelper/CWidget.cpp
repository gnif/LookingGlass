#include "CWidget.h"

HWND CWidget::createWindowSimple(LPCWSTR cls, LPCWSTR title, DWORD style, HWND parent)
{
  return CreateWindow(cls, title, style, 0, 0, 0, 0, parent,
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
