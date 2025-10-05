#pragma once

#include "CWidget.h"

class CGroupBox : public CWidget
{
public:
  CGroupBox(LPCWSTR title, DWORD style, HWND parent);
};
