#pragma once
#include "CWindow.h"
#include <memory>

class CConfigWindow;

class CNotifyWindow : public CWindow
{
  static UINT s_taskbarCreated;
  static ATOM s_atom;

  NOTIFYICONDATA m_iconData;
  HMENU m_menu;
  bool closeRequested;
  std::unique_ptr<CConfigWindow> m_config;

  LRESULT onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y);
  void registerIcon();

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onClose() override;
  virtual LRESULT onDestroy() override;
  virtual LRESULT onFinal() override;

public:
  CNotifyWindow();
  ~CNotifyWindow() override;
  static bool registerClass();

  HWND hwndDialog();
  void close();
};
