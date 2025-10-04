#pragma once

#include "CWidget.h"

class CButton : public CWidget
{
public:
  CButton(LPCWSTR title, DWORD style, HWND parent);
};
