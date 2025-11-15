#pragma once
#include "CWidget.h"
#include <string>

class CEditWidget : public CWidget
{
public:
  CEditWidget(DWORD style, HWND parent);
  std::wstring getValue();
  int getNumericValue();

  bool enable(bool enabled = true);
  bool disable() { return enable(false); }
  void setValue(const std::wstring &value);
  void setNumericValue(int value);
};
