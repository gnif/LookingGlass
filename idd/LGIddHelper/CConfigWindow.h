#pragma once
#include "CWindow.h"
#include <functional>

class CConfigWindow : public CWindow
{
  static ATOM s_atom;

  std::function<void()> m_onDestroy;

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onFinal() override;

public:
  CConfigWindow();
  static bool registerClass();

  void onDestroy(std::function<void()> func) { m_onDestroy = std::move(func); }
};
