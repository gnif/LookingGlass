#pragma once
#include "CWidget.h"
#include <string>

class CListBox : public CWidget
{
public:
  CListBox(DWORD style, HWND parent);
  void initStorage(DWORD count, size_t perItem);
  int addItem(const std::wstring &display, LPARAM data);
  int getSel();
  int getSelData();
};
