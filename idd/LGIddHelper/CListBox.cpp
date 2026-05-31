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

#include "CListBox.h"
#include <commctrl.h>
#include <windows.h>
#include <windowsx.h>
#include <CDebug.h>

CListBox::CListBox(DWORD style, HWND parent)
{
  m_hwnd = createWindowSimple(WC_LISTBOX, nullptr, style, parent, WS_EX_CLIENTEDGE);
  if (!m_hwnd)
    DEBUG_ERROR_HR(GetLastError(), "Failed to create listbox");
}

void CListBox::initStorage(DWORD count, size_t perItem)
{
  SendMessage(m_hwnd, LB_INITSTORAGE, count, perItem);
}

int CListBox::addItem(const std::wstring &display, LPARAM data)
{
  int result = ListBox_AddString(m_hwnd, display.c_str());
  if (result == LB_ERRSPACE)
  {
    DEBUG_ERROR(L"Out of memory while adding to listbox: %s", display.c_str());
    return -1;
  }

  ListBox_SetItemData(m_hwnd, result, data);
  return result;
}

void CListBox::delItem(int index)
{
  if (!ListBox_DeleteString(m_hwnd, index))
    DEBUG_ERROR("listbox: failed to delete string at %d", index);
}

int CListBox::getSel()
{
  return ListBox_GetCurSel(m_hwnd);
}

int CListBox::getData(int index)
{
  return (int) ListBox_GetItemData(m_hwnd, index);
}

void CListBox::setSel(int index)
{
  if (!ListBox_SetCurSel(m_hwnd, index))
    DEBUG_ERROR("listbox: failed to set selection to %d", index);
}

void CListBox::clear()
{
  ListBox_ResetContent(m_hwnd);
}
