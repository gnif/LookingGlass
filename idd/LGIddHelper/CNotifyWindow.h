#pragma once
#include "CWindow.h"

#define WM_NOTIFY_ICON (WM_USER)

class CNotifyWindow : public CWindow
{
  static UINT s_taskbarCreated;
  static ATOM s_atom;

  NOTIFYICONDATA m_iconData;
  HMENU m_menu;
  bool closeRequested;

  LRESULT onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y);
  void registerIcon();

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onClose() override;

public:
  CNotifyWindow();
  ~CNotifyWindow() override;
  static bool registerClass();

  void close();
};
