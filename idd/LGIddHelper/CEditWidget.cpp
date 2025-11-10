#include "CEditWidget.h"
#include <commctrl.h>
#include <windows.h>
#include <windowsx.h>
#include <CDebug.h>

CEditWidget::CEditWidget(DWORD style, HWND parent)
{
  m_hwnd = createWindowSimple(WC_EDIT, nullptr, style, parent, WS_EX_CLIENTEDGE);
  if (!m_hwnd)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create edit control");
}

std::wstring CEditWidget::getValue()
{
  std::wstring result;
  result.resize(Edit_GetTextLength(m_hwnd));
  Edit_GetText(m_hwnd, result.data(), (int) (result.size() + 1));
  return result;
}

int CEditWidget::getNumericValue()
{
  return std::stoi(getValue());
}

void CEditWidget::setValue(const std::wstring &value)
{
  if (!Edit_SetText(m_hwnd, value.c_str()))
    DEBUG_ERROR("Failed to update text for edit control");
}

void CEditWidget::setNumericValue(int value)
{
  setValue(std::to_wstring(value));
}
