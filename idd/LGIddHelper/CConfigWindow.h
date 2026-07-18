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
class CButton;
class CCheckbox;

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
  std::unique_ptr<CCheckbox> m_modePreferred;

  std::unique_ptr<CButton> m_modeUpdate;
  std::unique_ptr<CButton> m_modeDelete;
  std::unique_ptr<CButton> m_modeReset;

  std::unique_ptr<CStaticWidget> m_defRefreshLabel;
  std::unique_ptr<CEditWidget> m_defRefresh;
  std::unique_ptr<CStaticWidget> m_defRefreshHz;

  std::unique_ptr<CGroupBox> m_prefGroup;
  std::unique_ptr<CCheckbox> m_prefNoGPU;

  std::function<void()> m_onDestroy;
  std::function<void()> m_onSettingChange;

  double m_scale;
  Microsoft::WRL::Wrappers::HandleT<FontTraits> m_font;
  CRegistrySettings m_settings;
  std::optional<std::vector<DisplayMode>> m_modes;
  std::optional<DWORD> m_defaultRefresh;
  std::optional<bool> m_noGPU;

  void getMinimumSize(LONG &width, LONG &height);
  void updateFont();
  int updateModeList(int wanted = -1);
  void onModeListSelectChange();
  void sendSettingChange() { if (m_onSettingChange) m_onSettingChange(); }

  virtual LRESULT handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
  virtual LRESULT onCreate() override;
  virtual LRESULT onFinal() override;
  LRESULT onResize(DWORD width, DWORD height);
  LRESULT onCommand(WORD id, WORD code, HWND hwnd);

public:
  CConfigWindow();
  static bool registerClass();

  void onDestroy(std::function<void()> func) { m_onDestroy = std::move(func); }
  void onSettingChange(std::function<void()> func) { m_onSettingChange = std::move(func); }
};
