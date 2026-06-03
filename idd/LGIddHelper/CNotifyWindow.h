/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

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
  void noGPUNotification();

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
