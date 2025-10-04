#pragma once
#include "CWindow.h"
#include "CStaticWidget.h"
#include "CRegistrySettings.h"
#include <functional>
#include <memory>
#include <optional>
#include <wrl.h>
#include "UIHelpers.h"

class CListBox;
class CGroupBox;
class CEditWidget;

class CConfigWindow : public CWindow
{
  static ATOM s_atom;

  std::unique_ptr<CStaticWidget> m_version;
  std::unique_ptr<CGroupBox> m_modeGroup;
  std::unique_ptr<CListBox> m_modeBox;

  std::unique_ptr<CStaticWidget> m_widthLabel;
  std::unique_ptr<CStaticWidget> m_heightLabel;
  std::unique_ptr<CStaticWidget> m_refreshLabel;

  std::unique_ptr<CEditWidget> m_modeWidth;
  std::unique_ptr<CEditWidget> m_modeHeight;
  std::unique_ptr<CEditWidget> m_modeRefresh;

  std::function<void()> m_onDestroy;
  double m_scale;
  Microsoft::WRL::Wrappers::HandleT<FontTraits> m_font;
  CRegistrySettings m_settings;
  std::optional<std::vector<DisplayMode>> m_modes;

  void updateFont();

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onFinal() override;
  LRESULT onResize(DWORD width, DWORD height);
  LRESULT onCommand(WORD id, WORD code, HWND hwnd);

public:
  CConfigWindow();
  static bool registerClass();

  void onDestroy(std::function<void()> func) { m_onDestroy = std::move(func); }
};
