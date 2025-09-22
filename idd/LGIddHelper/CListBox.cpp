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
