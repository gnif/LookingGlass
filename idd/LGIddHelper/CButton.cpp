#include "CButton.h"
#include <commctrl.h>
#include <CDebug.h>

CButton::CButton(LPCWSTR title, DWORD style, HWND parent)
{
  m_hwnd = createWindowSimple(WC_BUTTON, title, style, parent);
  if (!m_hwnd)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create button");
}
