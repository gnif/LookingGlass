#pragma once
#include "CWindow.h"
#include "CStaticWidget.h"
#include <functional>
#include <memory>
#include <wrl.h>
#include "UIHelpers.h"

class CConfigWindow : public CWindow
{
  static ATOM s_atom;

  std::unique_ptr<CStaticWidget> m_version;

  std::function<void()> m_onDestroy;
  double m_scale;
  Microsoft::WRL::Wrappers::HandleT<FontTraits> m_font;

  void updateFont();

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onFinal() override;
  LRESULT onResize(DWORD width, DWORD height);

public:
  CConfigWindow();
  static bool registerClass();

  void onDestroy(std::function<void()> func) { m_onDestroy = std::move(func); }
};
