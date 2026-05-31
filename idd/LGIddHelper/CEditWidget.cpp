/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

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

bool CEditWidget::enable(bool enabled)
{
  return Edit_Enable(m_hwnd, enabled);
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
