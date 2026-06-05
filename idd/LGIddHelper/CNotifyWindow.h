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
#include <functional>
#include <memory>
#include <optional>

class CConfigWindow;

class CNotifyWindow : public CWindow
{
  static UINT s_taskbarCreated;
  static ATOM s_atom;

  NOTIFYICONDATA m_iconData;
  bool m_iconRegistered;
  std::optional<bool> m_gpuQueue;
  HMENU m_menu;
  bool closeRequested;
  std::unique_ptr<CConfigWindow> m_config;

  std::function<void()> m_onSettingChange;

  LRESULT onNotifyIcon(UINT uEvent, WORD wIconId, int x, int y);
  void registerIcon();
  void handleGPUNotification(bool hasGPU);

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onClose() override;
  virtual LRESULT onDestroy() override;
  virtual LRESULT onFinal() override;

  CNotifyWindow();
  ~CNotifyWindow() override;

public:
  CNotifyWindow(const CNotifyWindow&) = delete;
  CNotifyWindow& operator=(const CNotifyWindow&) = delete;

  CNotifyWindow(CNotifyWindow&&) = delete;
  CNotifyWindow& operator=(CNotifyWindow&&) = delete;

  static CNotifyWindow& instance()
  {
    static CNotifyWindow window;
    return window;
  }

  static bool registerClass();

  void setGPU(bool hasGPU);

  HWND hwndDialog();
  void close();

  void onSettingChange(std::function<void()> func) { m_onSettingChange = std::move(func); }
};