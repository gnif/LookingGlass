#pragma once

#include "CWidget.h"

class CStaticWidget : public CWidget
{
public:
  CStaticWidget(LPCWSTR title, DWORD style, HWND parent);
  void setText(LPCWSTR text);
};
