#include "CStaticWidget.h"
#include <commctrl.h>
#include <CDebug.h>

CStaticWidget::CStaticWidget(LPCWSTR title, DWORD style, HWND parent)
{
  m_hwnd = createWindowSimple(WC_STATIC, title, style, parent);
  if (!m_hwnd)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create static widget");
}

void CStaticWidget::setText(LPCWSTR text)
{
  SetWindowText(m_hwnd, text);
}
