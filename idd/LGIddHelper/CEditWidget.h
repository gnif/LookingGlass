#pragma once
#include "CWidget.h"
#include <string>

class CEditWidget : public CWidget
{
public:
  CEditWidget(DWORD style, HWND parent);
  std::wstring getValue();
  int getNumericValue();

  void setValue(const std::wstring &value);
  void setNumericValue(int value);
};
