#include "CGroupBox.h"
#include <commctrl.h>
#include <CDebug.h>

CGroupBox::CGroupBox(LPCWSTR title, DWORD style, HWND parent)
{
  m_hwnd = createWindowSimple(WC_BUTTON, title, style | BS_GROUPBOX, parent);
  if (!m_hwnd)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create static widget");
}
